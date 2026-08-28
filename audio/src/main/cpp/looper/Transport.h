#pragma once

#include "AudioLooper.h"
#include <algorithm>
#include <atomic>
#include <cmath>

/**
 * @class Transport
 * @brief Musical transport for tempo/BPM-driven scheduling.
 *
 * Owns the global musical clock: BPM, beats-per-bar, sample rate. Provides:
 *  - frame-precise quantization helpers (frames-per-beat, frames-per-bar)
 *  - RT-safe metronome scheduler that emits N clicks at beat intervals
 *    independent of UI thread responsiveness.
 *
 * The UI calls `startMetronome(beats, withDownbeat)` once and the audio thread
 * delivers clicks from the audio callback. This eliminates the "click sometimes
 * plays, sometimes doesn't" symptom caused by UI-driven scheduling jank.
 *
 * Threading:
 *  - setBpm/setBeatsPerBar/setSampleRate/startMetronome/stopMetronome → UI thread
 *  - tick() → audio thread (RT-safe, lock-free)
 *  - All cross-thread state via std::atomic
 *
 * Future extensions (Sprint 2+):
 *  - Bar/beat quantization helpers for loop length
 *  - "Armed → wait for downbeat → record" state machine
 *  - Ableton Link / MIDI clock sync
 */
class Transport {
public:
    static constexpr float MIN_BPM = 20.0f;
    static constexpr float MAX_BPM = 300.0f;
    static constexpr int MIN_BEATS_PER_BAR = 1;
    static constexpr int MAX_BEATS_PER_BAR = 16;

    Transport() { recompute(); }

    // ========== Configuration (UI thread) ==========

    void setSampleRate(int sampleRate) {
        if (sampleRate <= 0) return;
        mSampleRate.store(sampleRate, std::memory_order_release);
        recompute();
    }

    void setBpm(float bpm) {
        bpm = std::clamp(bpm, MIN_BPM, MAX_BPM);
        mBpm.store(bpm, std::memory_order_release);
        recompute();
    }

    void setBeatsPerBar(int beats) {
        beats = std::clamp(beats, MIN_BEATS_PER_BAR, MAX_BEATS_PER_BAR);
        mBeatsPerBar.store(beats, std::memory_order_release);
    }

    int getSampleRate()   const { return mSampleRate.load(std::memory_order_acquire); }
    float getBpm()        const { return mBpm.load(std::memory_order_acquire); }
    int getBeatsPerBar()  const { return mBeatsPerBar.load(std::memory_order_acquire); }

    // ========== Quantization helpers (any thread) ==========

    /** Frames per beat at current BPM/SR. */
    int framesPerBeat() const {
        return mFramesPerBeat.load(std::memory_order_acquire);
    }

    /** Frames in `bars` complete bars at current BPM/SR/beats-per-bar. */
    int framesPerBar(int bars) const {
        if (bars <= 0) return 0;
        return mFramesPerBeat.load(std::memory_order_acquire)
             * mBeatsPerBar.load(std::memory_order_acquire)
             * bars;
    }

    // ========== Metronome scheduling (UI thread) ==========

    /**
     * @brief Schedule N clicks at beat intervals starting "now".
     *        First click fires on the next audio callback; subsequent clicks
     *        fire in the callback block where the next beat falls.
     * @param beats Number of clicks to emit (e.g. 4 for a one-bar pre-count).
     * @param firstIsDownbeat If true, the first click is a downbeat (higher pitch).
     * @param everyBeatDownbeatPattern If true, clicks N where N % beatsPerBar == 0
     *        are downbeats; otherwise only the first click is a downbeat.
     */
    void startMetronome(int beats, bool firstIsDownbeat = true,
                        bool everyBeatDownbeatPattern = false) {
        if (beats <= 0) {
            stopMetronome();
            return;
        }
        // Configure click pattern atomically — audio thread will pick this up
        // on the next tick().
        mContinuous.store(false, std::memory_order_relaxed);
        mPatternMode.store(everyBeatDownbeatPattern ? 1 : 0, std::memory_order_relaxed);
        mFirstIsDownbeat.store(firstIsDownbeat, std::memory_order_relaxed);
        mClickIndex.store(0, std::memory_order_relaxed);
        mFramesUntilNextClick.store(0, std::memory_order_relaxed);  // fire immediately
        mBeatsRemaining.store(beats, std::memory_order_release);    // arms scheduler
    }

    /**
     * @brief Run the metronome continuously until stopMetronome() is called.
     *        Intended for the "click as reference during recording" mode — the
     *        scheduler does NOT decrement beatsRemaining, so clicks fire forever
     *        at framesPerBeat intervals, quantized to the callback block.
     * @param everyBeatDownbeatPattern If true, clicks where idx % beatsPerBar == 0
     *        are downbeats (recommended for in-take reference).
     */
    /**
     * @brief Arma un schedule CONTINUO.
     *
     * 🔴 **Armar no es sonar** (issue #229). Lo que hace sonar los clicks es `tick()`,
     * que se llama **desde el camino de render**: con el callback de audio parado esto
     * no produce nada y no lo dice — y `isMetronomeRunning()` contesta `true` igual.
     * Para verificar que de verdad suena, mira `getBeatsElapsed()`.
     */
    void startMetronomeContinuous(bool everyBeatDownbeatPattern = true) {
        mPatternMode.store(everyBeatDownbeatPattern ? 1 : 0, std::memory_order_relaxed);
        mFirstIsDownbeat.store(true, std::memory_order_relaxed);
        mClickIndex.store(0, std::memory_order_relaxed);
        mFramesUntilNextClick.store(0, std::memory_order_relaxed);
        mContinuous.store(true, std::memory_order_release);
        // Set a non-zero beatsRemaining sentinel so the tick() guard arms the
        // scheduler. The actual value is irrelevant in continuous mode because
        // the tick loop will not decrement it.
        mBeatsRemaining.store(1, std::memory_order_release);
    }

    /** Cancel any in-flight metronome schedule. Currently-sounding click decays naturally. */
    void stopMetronome() {
        mContinuous.store(false, std::memory_order_release);
        mBeatsRemaining.store(0, std::memory_order_release);
    }

    bool isMetronomeContinuous() const {
        return mContinuous.load(std::memory_order_acquire);
    }

    bool isMetronomeRunning() const {
        return mBeatsRemaining.load(std::memory_order_acquire) > 0;
    }

    int getRemainingBeats() const {
        return mBeatsRemaining.load(std::memory_order_acquire);
    }

    /**
     * @brief Cuantos beats EMITIO la grilla desde que se armo el metronomo.
     *
     * 🔴 Es el unico observable que se mueve **solo si el bucle de `tick()` corrio**,
     * y existe por eso (REQ-020). Los otros tres mienten, cada uno a su manera:
     *
     *  - `isMetronomeRunning()` dice **armado**, no sonando: es `mBeatsRemaining > 0`,
     *    o sea que contesta `true` para siempre con el render apagado (issue #229).
     *  - `getRemainingBeats()` en modo CONTINUO es el centinela `1` fijo — mirar
     *    `startMetronomeContinuous()`, que lo pone a proposito y nunca lo decrementa.
     *  - `getPlayFrame()` avanza **incondicionalmente**, antes de toda guarda, asi que
     *    dice si el RENDER corre pero no si el metronomo suena.
     *
     * El par que discrimina es (`isMetronomeRunning()`, `getBeatsElapsed()`):
     *
     *   | armado | elapsed avanza | que esta pasando                              |
     *   |--------|----------------|-----------------------------------------------|
     *   | true   | si             | suena: la grilla emite clicks Y eventos Beat  |
     *   | true   | no             | armado y NADIE tickea — el render no corre    |
     *   | false  | no             | parado                                        |
     *
     * ⚠️ Un click audible NO implica que esto se haya movido: `AudioLooper::triggerClick`
     * tiene un segundo llamador que no viene de la grilla (`wma_looper_trigger_click`).
     * Esa distincion es justamente el discriminador del issue #228.
     *
     * Coincide exactamente con la cantidad de eventos `Beat` que se empujaron al
     * dispatcher: los dos salen de la misma iteracion del mismo bucle.
     *
     * Lock-free; seguro desde cualquier thread. Se resetea a 0 al armar.
     */
    int getBeatsElapsed() const {
        return mClickIndex.load(std::memory_order_acquire);
    }

    // ========== Play position (musical clock) ==========
    //
    // The transport maintains a monotonically increasing frame counter while
    // the metronome is running. This counter is used by the looper to align
    // armed-recording starts with bar boundaries.

    /** Reset the play frame counter. Called when transport is restarted. */
    void resetPlayPosition() {
        mPlayFrameCounter.store(0, std::memory_order_release);
    }

    /**
     * @brief Current play position in frames since last resetPlayPosition().
     *        Lock-free; safe from any thread.
     *
     * 🔴 **Es tambien LA senal de liveness del render, y por accidente de diseño**
     * (issue #229): `tick()` avanza este contador **incondicionalmente**, antes de
     * toda guarda, asi que si dos lecturas separadas en el tiempo dan el mismo
     * numero, el callback de audio NO esta corriendo — y entonces nada de lo que
     * armes va a sonar, aunque `isMetronomeRunning()` diga `true`.
     *
     * La API lo presenta como una posicion musical, asi que esa lectura hay que
     * DEDUCIRLA. Queda dicha aca para no tener que deducirla de nuevo.
     *
     * Ojo con el reparto: esto dice si el **render** vive; `getBeatsElapsed()` dice
     * si el **metronomo** suena. No son la misma pregunta.
     */
    int64_t getPlayFrame() const {
        return mPlayFrameCounter.load(std::memory_order_acquire);
    }

    /**
     * @brief Frame index of the next bar boundary at or after `fromFrame`.
     *        Returns the smallest multiple of framesPerBar(1) that is >= fromFrame.
     *        If transport is not ready, returns fromFrame.
     */
    int64_t nextBarBoundary(int64_t fromFrame) const {
        const int64_t fpb = static_cast<int64_t>(framesPerBar(1));
        if (fpb <= 0) return fromFrame;
        const int64_t mod = fromFrame % fpb;
        return (mod == 0) ? fromFrame : fromFrame + (fpb - mod);
    }

    // ========== Audio thread tick (RT-safe) ==========

    /**
     * @brief Advance the transport by `numFrames` frames. Emits any clicks that
     *        fall within this audio block via `looper.triggerClick(...)`.
     *        RT-safe: lock-free, no allocations, no syscalls.
     *
     * Note: Click is fired at the audio block granularity, not sample-accurate
     *       to within the block. Jitter is bounded by `numFrames` (typically
     *       <10 ms at default Oboe burst sizes). For sample-accurate triggering,
     *       a future revision can split the block at the click frame.
     */
    void tick(int numFrames, AudioLooper& looper) {
        // Advance the global play frame counter unconditionally — the looper
        // uses this for armed-recording downbeat alignment whether or not the
        // metronome is running.
        // El valor DESPUES del avance: el frame en que termina este bloque
        // (exclusivo). Es la escala en la que se expresa el ancla del beat
        // (REQ-017) y la misma que devuelve getPlayFrame().
        const int64_t playFrameEnd =
            mPlayFrameCounter.fetch_add(static_cast<int64_t>(numFrames),
                                        std::memory_order_release)
            + static_cast<int64_t>(numFrames);

        int beatsLeft = mBeatsRemaining.load(std::memory_order_acquire);
        if (beatsLeft <= 0) return;

        const int fpb = mFramesPerBeat.load(std::memory_order_relaxed);
        if (fpb <= 0) return;

        int next = mFramesUntilNextClick.load(std::memory_order_relaxed);
        next -= numFrames;

        const int beatsPerBar = mBeatsPerBar.load(std::memory_order_relaxed);
        const int patternMode = mPatternMode.load(std::memory_order_relaxed);
        const bool firstDown = mFirstIsDownbeat.load(std::memory_order_relaxed);
        const bool continuous = mContinuous.load(std::memory_order_relaxed);
        int idx = mClickIndex.load(std::memory_order_relaxed);

        // Emit all clicks whose target frame fell inside this block. Tight bounded
        // loop — at most 1 iteration in practice (numFrames <= ~1024, fpb >= 1600).
        // In continuous mode we don't decrement beatsLeft so the scheduler fires
        // forever until stopMetronome() flips the flag.
        //
        // `next < 0`, not `<= 0`. After the subtraction above, `next` is the
        // frames remaining once THIS block has been consumed, so next == 0 means
        // the beat lands on the first sample of the NEXT block — it has not
        // happened yet. Firing on `<= 0` emitted it one block early and, because
        // the countdown then restarts from that early instant, shifted the whole
        // click train by one block for good. It only bites when framesPerBeat is
        // an exact multiple of the callback size, which is not exotic: 120 BPM at
        // 48 kHz is 24000 frames, and 24000 / 192 (a common Oboe burst) = 125.
        while (next < 0 && beatsLeft > 0) {
            const bool isDown = (patternMode == 1)
                ? (idx % beatsPerBar) == 0
                : (idx == 0 && firstDown);
            looper.triggerClick(isDown);
            const int beatIndex = idx;
            idx++;
            if (!continuous) beatsLeft--;
            next += fpb;
            // REQ-017 — co-emision. Tres cosas que parecen detalle y no lo son:
            //
            //  1. Va ADENTRO del bucle, no despues. El evento tiene que salir de
            //     la misma iteracion que el click para ser tan preciso como el —
            //     ni mas ni menos. Sacarlo afuera lo cuantiza al bloque en vez de
            //     al beat, y con framesPerBeat multiplo del bloque eso ademas
            //     colapsa dos beats de un mismo bloque en uno solo.
            //  2. Va DESPUES de `next += fpb`. El ancla es el frame del PROXIMO
            //     beat, no el residuo del que acaba de sonar: antes de la suma,
            //     `next` es negativo y apunta al beat ya ido.
            //  3. `playFrameEnd + next` es el frame absoluto del proximo beat.
            //     `next` se mide desde el FIN del bloque, que es exactamente lo
            //     que vale `playFrameEnd`.
            //
            // El consumidor necesita el ancla ABSOLUTA, no un delta: no conoce el
            // frame de emision, asi que un delta lo atrasaria justo lo que tardo
            // el evento en llegar — el defecto que REQ-017 viene a sacar.
            looper.emitBeat(beatIndex, playFrameEnd + static_cast<int64_t>(next));
        }

        mClickIndex.store(idx, std::memory_order_relaxed);
        mFramesUntilNextClick.store(next, std::memory_order_relaxed);
        if (!continuous) {
            mBeatsRemaining.store(beatsLeft, std::memory_order_release);
        }
    }

private:
    void recompute() {
        const int sr = mSampleRate.load(std::memory_order_relaxed);
        const float bpm = mBpm.load(std::memory_order_relaxed);
        if (sr <= 0 || bpm <= 0.0f) return;
        const float fpb = (60.0f / bpm) * static_cast<float>(sr);
        mFramesPerBeat.store(static_cast<int>(std::round(fpb)),
                             std::memory_order_release);
    }

    std::atomic<int>   mSampleRate{48000};
    std::atomic<float> mBpm{120.0f};
    std::atomic<int>   mBeatsPerBar{4};
    std::atomic<int>   mFramesPerBeat{24000};   // = 60/120 * 48000

    // Metronome scheduler state
    std::atomic<int>  mBeatsRemaining{0};        // 0 = idle
    std::atomic<int>  mFramesUntilNextClick{0};  // can go negative briefly
    std::atomic<int>  mClickIndex{0};            // 0..N-1 within current schedule
    std::atomic<int>  mPatternMode{0};           // 0 = "first is down", 1 = "every bar"
    std::atomic<bool> mFirstIsDownbeat{true};
    std::atomic<bool> mContinuous{false};         // true → run metronome forever

    // Monotonic frame counter advanced on every tick(). Used by the looper for
    // armed-recording downbeat alignment.
    std::atomic<int64_t> mPlayFrameCounter{0};
};
