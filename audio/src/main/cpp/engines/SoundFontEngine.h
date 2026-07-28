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
     * @brief Render audio for all active notes into buffer
     *
     * AUDIO THREAD ONLY. Drains the event queue first, then renders.
     */
    void render(float* buffer, int32_t numFrames) {
        tsf* sf = mSFManager ? mSFManager->getActiveSF() : nullptr;
        if (!sf) {
            std::fill_n(buffer, numFrames * 2, 0.0f);
            return;
        }

        // 1. Check for preset change (atomic, deferred from setPreset)
        int newPreset = mPendingPreset.load(std::memory_order_acquire);
        if (newPreset != mActivePreset) {
            tsf_note_off_all(sf);
            for (auto& t : mTouches) t.active = false;
            mActivePreset = newPreset;
        }

        // 2. Drain event queue — all tsf calls happen HERE on audio thread
        drainEvents(sf);

        // 3. Render all active tsf voices
        tsf_render_float(sf, buffer, numFrames, 0);

        // 4. Apply expression gain
        float gain = smoothParam(PARAM_EXPRESSION);
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
        NOTE_OFF_ALL_EXCEPT
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
                            tsf_note_off(sf, mActivePreset, touch.midiNote);
                        }
                        // Start new note
                        if (!touch.active || touch.midiNote != note) {
                            tsf_note_on(sf, mActivePreset, note, event.velocity);
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
                            tsf_note_off(sf, mActivePreset, touch.midiNote);
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
                            tsf_note_off(sf, mActivePreset, touch.midiNote);
                            touch.active = false;
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
    };

    SoundFontManager* mSFManager = nullptr;

    // Preset: written from any thread, consumed on audio thread in render()
    std::atomic<int> mPendingPreset{0};
    int mActivePreset = 0; // Audio-thread-only shadow

    // Touch state: audio-thread-only (modified in drainEvents)
    std::array<TouchState, MAX_TOUCHES> mTouches{};

    // Lock-free SPSC ring buffer for note events
    std::array<NoteEvent, EVENT_QUEUE_CAPACITY> mEventQueue{};
    std::atomic<int> mWritePos{0}; // Written by producer (any thread)
    std::atomic<int> mReadPos{0};  // Written by consumer (audio thread)
};
