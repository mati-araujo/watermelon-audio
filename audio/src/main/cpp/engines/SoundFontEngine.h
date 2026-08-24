#pragma once

#include "SynthEngine.h"
#include "SoundFontManager.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

/**
 * @class SoundFontEngine
 * @brief SynthEngine with multi-note polyphony via TinySoundFont
 *
 * All tsf* access is confined to the audio thread (inside render()).
 * External calls (noteOn/noteOff/setPreset from UI/JNI threads) push events
 * into a lock-free SPSC ring buffer, consumed at the start of each render().
 *
 * Thread safety guarantee:
 *   - noteOn()/noteOff()/noteOffAll()/setPreset(): ANY thread (lock-free queue push)
 *   - render(): ONLY audio thread (drains queue, calls tsf)
 *   - setSoundFontManager(): setup only (before audio starts)
 *
 * Parameters:
 *   0 - Expression: amplitude scaling (CC11-style)
 */
class SoundFontEngine : public SynthEngine {
public:
    static constexpr int PARAM_EXPRESSION = 0;
    static constexpr int MAX_TOUCHES = 16; // 0-3 touch, 4-9 reserved, 10-15 chord

    // Event queue capacity — power of 2 for fast modulo
    static constexpr int EVENT_QUEUE_CAPACITY = 64;
    static constexpr int EVENT_QUEUE_MASK = EVENT_QUEUE_CAPACITY - 1;

    SoundFontEngine() {
        mParams[PARAM_EXPRESSION].store(1.0f, std::memory_order_relaxed);
        for (auto& t : mTouches) t = TouchState{};
    }

    ~SoundFontEngine() override = default;

    // ========== Lifecycle ==========

    /**
     * @brief Prepare for a stream at [sampleRate].
     *
     * Además de lo del engine base, **re-configura la tasa de salida del
     * SoundFont**. El font se configura a la tasa de salida cuando se carga, y
     * el stream puede abrir —o reabrir— a otra: sin esto el font se quedaba con
     * la tasa vieja y sonaba desafinado en silencio. Ver
     * `SoundFontManager::setOutputSampleRate()` para por qué reemplaza en vez
     * de mutar, y para la nota de que no swapea si la tasa no cambió.
     *
     * NOT RT-safe: `setOutputSampleRate()` aloca. Es correcto porque `prepare()`
     * corre en el hilo de control —`AudioEngine::start()` y
     * `configureComponentsWithSampleRate()`, vía `SynthEngineDispatcher`—, nunca
     * desde el callback de audio.
     */
    void prepare(int32_t sampleRate, int32_t maxBlockSize) override {
        SynthEngine::prepare(sampleRate, maxBlockSize);
        for (auto& sm : mTouchExprSmoothers) {
            sm.setSmoothingTime(kParamSmoothingMs, static_cast<float>(sampleRate));
            sm.reset(1.0f);   // el neutro, sembrado — ver el KDoc de mTouchExprSmoothers
        }
        if (mSFManager) {
            mSFManager->setOutputSampleRate(sampleRate);
        }
    }

    void reset() override {
        // Queue a reset event — will be processed on audio thread.
        // mTouches is audio-thread-only; NOTE_OFF_ALL clears it in drainEvents().
        pushEvent(NoteEvent::makeNoteOffAll());
    }

    // ========== SynthEngine interface (unused in SOUNDFONT mode) ==========

    void process(float* buffer, int32_t numFrames,
                 float /* frequency */, float /* amplitude */) override {
        // SOUNDFONT branch in onAudioReady calls render() directly.
        // This fallback just renders whatever notes are active.
        render(buffer, numFrames);
    }

    // ========== Note event API (thread-safe, lock-free) ==========
    // These can be called from ANY thread. Events are queued and
    // processed on the audio thread at the start of render().

    void noteOn(int touchId, int midiNote, float velocity) {
        if (touchId < 0 || touchId >= MAX_TOUCHES) return;
        pushEvent(NoteEvent::makeNoteOn(touchId, midiNote, velocity));
    }

    void noteOff(int touchId) {
        if (touchId < 0 || touchId >= MAX_TOUCHES) return;
        pushEvent(NoteEvent::makeNoteOff(touchId));
    }

    void noteOffAll() {
        pushEvent(NoteEvent::makeNoteOffAll());
    }

    /**
     * @brief Release every active touch except @p keepTouchId.
     *
     * Replaces the per-frame cleanup pattern of calling noteOff() for every
     * "other" touchId from the UI thread (which costs one JNI call per slot).
     * The actual scan over mTouches happens on the audio thread inside
     * drainEvents(), so this is a single lock-free enqueue regardless of how
     * many touches are active.
     */
    void noteOffAllExcept(int keepTouchId) {
        pushEvent(NoteEvent::makeNoteOffAllExcept(keepTouchId));
    }

    /**
     * @brief REQ-008 — expresion continua de un toque que YA esta sonando.
     *
     * Cambia el nivel de la nota **sin volver a atacarla**: no hay `tsf_*note_on` en este
     * camino. Multiplicador con neutro en `1.0`; multiplica contra [PARAM_EXPRESSION], que
     * sigue siendo la perilla global del usuario.
     *
     * Un toque INACTIVO se ignora sin efecto (AC-008.5). No se guarda "para cuando ataque":
     * el `NOTE_ON` reinicia la expresion a 1,0 igual (AC-008.3), asi que guardarla seria
     * estado que nadie va a leer.
     *
     * 🔑 Nota para el consumidor: si el gesto arranca desde la Y del dedo, mandar el primer
     * valor absoluto ATENUA EL ATAQUE DOS VECES —por la velocity y por esto—. La expresion
     * tiene que ser **relativa al punto donde aterrizo el dedo**. Ver la spec de REQ-008.
     *
     * Thread-safe y lock-free, como `noteOn`/`noteOff`.
     */
    void setTouchExpression(int touchId, float expression) {
        if (touchId < 0 || touchId >= MAX_TOUCHES) return;
        pushEvent(NoteEvent::makeSetExpression(touchId, expression));
    }

    /**
     * @brief Render audio for all active notes into buffer
     *
     * AUDIO THREAD ONLY. Drains the event queue first, then renders.
     */
    void render(float* buffer, int32_t numFrames) {
        // El hazard pointer se baja SIEMPRE, por cualquier salida. Mientras esté
        // arriba, el hilo de control tiene prohibido liberar este `tsf`; si se
        // quedara arriba, el font no se liberaría nunca. Ver
        // SoundFontManager::acquireActive().
        struct ActiveGuard {
            SoundFontManager* mgr;
            ~ActiveGuard() { if (mgr) mgr->releaseActive(); }
        } guard{mSFManager};

        tsf* sf = mSFManager ? mSFManager->acquireActive() : nullptr;
        if (!sf) {
            std::fill_n(buffer, numFrames * 2, 0.0f);
            return;
        }

        // 1. Check for preset change (atomic, deferred from setPreset)
        int newPreset = mPendingPreset.load(std::memory_order_acquire);
        const bool presetChanged = (newPreset != mActivePreset);
        // REQ-008 — con el API por canal el preset vive EN CADA CANAL, no en el llamado a
        // `tsf_note_on`. Hay que (re)configurar los 16 en tres casos, y los tres importan:
        //   · el preset cambio;
        //   · es el primer render (arranca en 0 y `presetChanged` seria false, asi que sin
        //     esto los canales nunca se configuran y NO SUENA NADA);
        //   · cambio la fuente — `mSFManager` puede swapear el `tsf*`, y la config de
        //     canales vive adentro de esa instancia, asi que se va con ella.
        if (presetChanged || sf != mConfiguredFor) {
            if (presetChanged) {
                tsf_note_off_all(sf);
                for (auto& t : mTouches) t.active = false;
                mActivePreset = newPreset;
            }
            for (int ch = 0; ch < MAX_TOUCHES; ++ch) {
                tsf_channel_set_presetindex(sf, ch, mActivePreset);
                // El canal arranca en el neutro, y el estado espejado con el.
                tsf_channel_set_volume(sf, ch, 1.0f);
                mTouches[ch].expression = 1.0f;
                mTouchExprSmoothers[ch].reset(1.0f);
            }
            mConfiguredFor = sf;
        }

        // 2. Drain event queue — all tsf calls happen HERE on audio thread
        drainEvents(sf);

        // 2b. REQ-008 — la expresion por toque, suavizada POR BLOQUE.
        //
        // Va aca y no en el evento porque el suavizado tiene que existir aunque no llegue
        // ningun evento: el smoother sigue caminando hacia su objetivo bloque a bloque.
        // `processBlock` avanza el coeficiente POR MUESTRA (`numFrames` de una vez), asi
        // que los 5 ms son 5 ms de verdad con cualquier tamaño de bloque — que es lo que
        // pide AC-008.7 y lo que el propio `smoothParam` ya garantiza.
        for (int tid = 0; tid < MAX_TOUCHES; ++tid) {
            auto& touch = mTouches[tid];
            if (!touch.active) continue;
            const float smoothed =
                mTouchExprSmoothers[tid].processBlock(touch.expression, static_cast<int>(numFrames));
            // `tsf_channel_set_volume` early-returna si el valor no cambio (tsf.h:1877),
            // asi que el toque quieto no paga nada.
            tsf_channel_set_volume(sf, tid, smoothed);
        }

        // 3. Render all active tsf voices
        tsf_render_float(sf, buffer, numFrames, 0);

        // 4. Apply expression gain
        float gain = smoothParam(PARAM_EXPRESSION, numFrames);
        if (gain < 0.999f) {
            for (int i = 0; i < numFrames * 2; ++i) {
                buffer[i] *= gain;
            }
        }
    }

    int getActiveTouchCount() const {
        int count = 0;
        for (const auto& t : mTouches) {
            if (t.active) ++count;
        }
        return count;
    }

    // ========== Metadata ==========

    const char* getName() const override { return "SoundFont"; }
    int getParameterCount() const override { return 1; }

    EngineParameterDef getParameterDef(int paramId) const override {
        if (paramId == PARAM_EXPRESSION) {
            return {"Expression", "EXPR", 0.0f, 1.0f, 1.0f};
        }
        return {"Unknown", "???", 0.0f, 1.0f, 0.5f};
    }

    // ========== SoundFont-specific ==========

    void setSoundFontManager(SoundFontManager* manager) {
        mSFManager = manager;
    }

    /**
     * @brief Set preset (thread-safe, deferred to audio thread)
     *
     * Stores the new preset atomically. The actual tsf_note_off_all + preset
     * switch happens at the start of the next render() on the audio thread.
     */
    void setPreset(int presetIndex) {
        mPendingPreset.store(presetIndex, std::memory_order_release);
    }

    int getPreset() const {
        return mPendingPreset.load(std::memory_order_acquire);
    }

    static int frequencyToMidi(float freq) {
        if (freq <= 0.0f) return 60;
        int note = static_cast<int>(std::round(69.0f + 12.0f * std::log2(freq / 440.0f)));
        return std::clamp(note, 0, 127);
    }

private:
    // ========== Lock-free SPSC note event queue ==========

    enum class EventType : uint8_t {
        NOTE_ON,
        NOTE_OFF,
        NOTE_OFF_ALL,
        NOTE_OFF_ALL_EXCEPT,
        SET_EXPRESSION      // REQ-008
    };

    struct NoteEvent {
        EventType type;
        int8_t touchId;
        int8_t midiNote;
        float velocity;

        static NoteEvent makeNoteOn(int touchId, int midiNote, float velocity) {
            return {EventType::NOTE_ON, static_cast<int8_t>(touchId),
                    static_cast<int8_t>(midiNote), velocity};
        }
        static NoteEvent makeNoteOff(int touchId) {
            return {EventType::NOTE_OFF, static_cast<int8_t>(touchId), 0, 0.0f};
        }
        static NoteEvent makeNoteOffAll() {
            return {EventType::NOTE_OFF_ALL, 0, 0, 0.0f};
        }
        static NoteEvent makeNoteOffAllExcept(int keepTouchId) {
            return {EventType::NOTE_OFF_ALL_EXCEPT,
                    static_cast<int8_t>(keepTouchId), 0, 0.0f};
        }
        /// REQ-008 — el escalar viaja en `velocity`, que en este evento ES el valor.
        static NoteEvent makeSetExpression(int touchId, float expression) {
            return {EventType::SET_EXPRESSION, static_cast<int8_t>(touchId), 0, expression};
        }
    };

    void pushEvent(NoteEvent event) {
        int write = mWritePos.load(std::memory_order_relaxed);
        int read = mReadPos.load(std::memory_order_acquire);
        int nextWrite = (write + 1) & EVENT_QUEUE_MASK;

        if (nextWrite == read) {
            // Queue full — drop newest event to preserve SPSC invariant.
            // Only the consumer (audio thread) may advance mReadPos.
            return;
        }

        mEventQueue[write] = event;
        mWritePos.store(nextWrite, std::memory_order_release);
    }

    void drainEvents(tsf* sf) {
        int read = mReadPos.load(std::memory_order_relaxed);
        int write = mWritePos.load(std::memory_order_acquire);

        while (read != write) {
            const auto& event = mEventQueue[read];

            switch (event.type) {
                case EventType::NOTE_ON: {
                    int tid = event.touchId;
                    int note = event.midiNote;
                    if (tid >= 0 && tid < MAX_TOUCHES) {
                        auto& touch = mTouches[tid];
                        // Release previous note if different
                        if (touch.active && touch.midiNote != note) {
                            tsf_channel_note_off(sf, tid, touch.midiNote);
                        }
                        // REQ-008 (AC-008.3) — la expresion vuelve al neutro, y tiene que ser
                        // ANTES del note-on: `tsf_channel_set_volume` le suma el delta a las
                        // voces VIVAS y guarda el gain del canal, que las voces nuevas recogen
                        // al nacer (tsf.h:1752/1880). Al reves, la nota nueva nacia con el
                        // nivel del gesto anterior y el reset llegaba tarde.
                        touch.expression = 1.0f;
                        // `reset` y no `processBlock`: el neutro tiene que valer YA. Si el
                        // smoother rampeara, la nota nueva arrancaria en el nivel del gesto
                        // anterior y treparia hasta 1,0 — que es exactamente lo que AC-008.3
                        // prohibe, sonando ademas como un fade-in que nadie pidio.
                        mTouchExprSmoothers[tid].reset(1.0f);
                        tsf_channel_set_volume(sf, tid, 1.0f);
                        // Start new note
                        if (!touch.active || touch.midiNote != note) {
                            tsf_channel_note_on(sf, tid, note, event.velocity);
                        }
                        touch.active = true;
                        touch.midiNote = note;
                        touch.velocity = event.velocity;
                    }
                    break;
                }
                case EventType::NOTE_OFF: {
                    int tid = event.touchId;
                    if (tid >= 0 && tid < MAX_TOUCHES) {
                        auto& touch = mTouches[tid];
                        if (touch.active) {
                            tsf_channel_note_off(sf, tid, touch.midiNote);
                            touch.active = false;
                        }
                    }
                    break;
                }
                case EventType::NOTE_OFF_ALL: {
                    tsf_note_off_all(sf);
                    for (auto& t : mTouches) t.active = false;
                    break;
                }
                case EventType::NOTE_OFF_ALL_EXCEPT: {
                    int keep = event.touchId;
                    for (int i = 0; i < MAX_TOUCHES; ++i) {
                        if (i == keep) continue;
                        auto& touch = mTouches[i];
                        if (touch.active) {
                            tsf_channel_note_off(sf, i, touch.midiNote);
                            touch.active = false;
                        }
                    }
                    break;
                }
                case EventType::SET_EXPRESSION: {
                    int tid = event.touchId;
                    if (tid >= 0 && tid < MAX_TOUCHES) {
                        auto& touch = mTouches[tid];
                        // AC-008.5 — un toque inactivo se ignora sin efecto.
                        if (touch.active) {
                            // Solo se guarda el OBJETIVO. Aplicarlo es del render, que lo
                            // pasa por el smoother: hacerlo aca seria un escalon, y un salto
                            // grande de un gesto brusco si clickea (AC-008.2).
                            touch.expression = event.velocity;
                        }
                    }
                    break;
                }
            }

            read = (read + 1) & EVENT_QUEUE_MASK;
        }

        mReadPos.store(read, std::memory_order_release);
    }

    // ========== State ==========

    struct TouchState {
        bool active = false;
        int midiNote = -1;
        float velocity = 0.0f;
        /**
         * REQ-008 — expresion continua de ESTE toque, multiplicador con neutro en 1,0.
         *
         * Ortogonal a [PARAM_EXPRESSION]: aquella es global al engine y es la perilla del
         * usuario; esta es del dedo. Se componen porque entran por caminos distintos —esta
         * como ganancia de canal ANTES del render, aquella sobre el buffer DESPUES—, asi
         * que el producto sale solo y en el neutro el camino es exactamente el de antes.
         */
        float expression = 1.0f;
    };

    SoundFontManager* mSFManager = nullptr;

    // Preset: written from any thread, consumed on audio thread in render()
    std::atomic<int> mPendingPreset{0};
    int mActivePreset = 0; // Audio-thread-only shadow

    /**
     * REQ-008 — para que instancia de `tsf` se configuraron los canales.
     *
     * Audio-thread-only. Se compara por PUNTERO y no se desreferencia: sirve para detectar
     * que `SoundFontManager` swapeo la fuente (cambio de font o de tasa de salida), porque
     * la configuracion de canales vive dentro de la instancia y se va con ella.
     */
    tsf* mConfiguredFor = nullptr;

    /**
     * REQ-008 — un suavizador por toque, el MISMO `ParameterSmoother` que usan los
     * parametros del engine. Audio-thread-only.
     *
     * Se SIEMBRA en 1,0 (el neutro) por el mismo motivo que documenta `SynthEngine::prepare`
     * para los de parametros: sin sembrar arrancan en CERO y trepan hacia su valor, o sea que
     * el primer toque entraria con un fade-in que nadie pidio.
     */
    std::array<ParameterSmoother, MAX_TOUCHES> mTouchExprSmoothers;

    // Touch state: audio-thread-only (modified in drainEvents)
    std::array<TouchState, MAX_TOUCHES> mTouches{};

    // Lock-free SPSC ring buffer for note events
    std::array<NoteEvent, EVENT_QUEUE_CAPACITY> mEventQueue{};
    std::atomic<int> mWritePos{0}; // Written by producer (any thread)
    std::atomic<int> mReadPos{0};  // Written by consumer (audio thread)
};
