package com.watermellonstudios.audio.api

/**
 * Push-based notifications of looper track state changes from native code.
 *
 * Replaces the per-track polling pattern previously used by consumers
 * (e.g. NoisyPad's `LooperViewModel` polled `getTrackProgress`,
 * `isTrackPlaying`, and `getTrackPeakLevel` every 33 ms across 8 tracks,
 * generating ~800 JNI calls/sec and visible UI lag with multiple tracks).
 *
 * ## Threading
 * Callbacks arrive on a **single background worker thread** owned by the
 * native audio engine. They are NOT on the main thread. Implementations
 * must marshal to the UI thread themselves (e.g. via `Dispatchers.Main`
 * or `viewModelScope.launch(Dispatchers.Main)`).
 *
 * ## Coalescing
 * The native side suppresses redundant updates:
 *   - `onTrackProgress` fires when the playhead has moved >= 2048 frames
 *     (~43 ms @ 48 kHz) since the last emission.
 *   - `onTrackPeakChanged` fires when the peak level has changed by
 *     >= 0.5 dB.
 *   - `onTrackPlayingChanged` fires only on state transitions.
 *
 * ## Lifecycle
 * Register **after** the audio engine has been initialized. The listener
 * stays installed until [unregister][com.watermellonstudios.audio.internal.bridge]
 * is called or the engine is destroyed. Only one listener can be installed
 * at a time — registering replaces any previous listener.
 */
interface LooperStateListener {
    /**
     * @param trackIndex 0..7
     * @param progress   Normalized playhead position [0, 1].
     */
    fun onTrackProgress(trackIndex: Int, progress: Float)

    /**
     * @param trackIndex 0..7
     * @param isPlaying  Current play state after the transition.
     */
    fun onTrackPlayingChanged(trackIndex: Int, isPlaying: Boolean)

    /**
     * @param trackIndex 0..7
     * @param peakLevel  Linear peak amplitude in [0, 1].
     */
    fun onTrackPeakChanged(trackIndex: Int, peakLevel: Float)

    /**
     * Recording progress of the track currently being captured (QW-5). Push
     * replacement for polling `getRecordProgress()` on a timer.
     *
     * Fires when the record progress changes by >= ~0.02 (about 50 updates over
     * a full take). A **negative** [progress] (sentinel -1.0) signals that the
     * recording on [trackIndex] has ENDED (loop finalized, free-take cap reached,
     * or stopped) — use it to clear any "recording…" UI state without polling
     * `isRecording()`.
     *
     * Has a no-op default so existing implementations keep compiling; override
     * it to retire the record-progress polling loop.
     *
     * @param trackIndex 0..7
     * @param progress   Record progress in [0, 1], or < 0 when recording ended.
     */
    fun onTrackRecordProgress(trackIndex: Int, progress: Float) {}

    /**
     * The track finished its finite play count (see `setTrackPlayCount`) and
     * auto-stopped (F3.4). Distinct from [onTrackPlayingChanged] with
     * `isPlaying = false` so the UI can tell "completed N plays" apart from a
     * user-initiated stop (e.g. to advance a song section or trigger the next clip).
     *
     * Has a no-op default so existing implementations keep compiling.
     *
     * @param trackIndex 0..15
     */
    fun onTrackCompleted(trackIndex: Int) {}

    /**
     * Un beat de la grilla acaba de sonar (REQ-017). Es el ÚNICO callback global de
     * esta interfaz: no habla de una pista.
     *
     * Se co-emite desde la misma decisión que dispara el click, así que es
     * exactamente tan preciso como el click — que está cuantizado al bloque de
     * audio por diseño, no al sample.
     *
     * ## Cómo se usa el ancla, que es el punto entero
     *
     * NO sabés cuándo se emitió esto: llegó después de una cola, un poll del worker
     * (~15 ms) y un salto de thread. Por eso [nextBeatFrame] es un frame ABSOLUTO y
     * no "frames que faltan": restale
     * [IAudioNativeBridge.transportGetPlayFrame] cuando lo recibís y obtenés los
     * frames que faltan **de verdad**, sea cual sea el atraso.
     *
     * ```kotlin
     * override fun onBeat(beatIndex: Int, nextBeatFrame: Int) {
     *     val faltan = nextBeatFrame - bridge.transportGetPlayFrame()
     *     val segundos = faltan.toDouble() / sampleRate
     *     // …programar la animación para que llegue A TIEMPO, no para reaccionar tarde
     * }
     * ```
     *
     * El compás se deriva acá, no viaja: `beatIndex / beatsPerBar` es el bar y
     * `beatIndex % beatsPerBar == 0` es el downbeat (`transportGetBeatsPerBar()`).
     *
     * ## Dos límites, los dos a propósito
     *
     * - **Sin metrónomo armado no hay beat.** Manda el tren de clicks porque es lo
     *   que hace que el pulso esté clavado a lo que se ESCUCHA: derivar de
     *   playFrame/framesPerBeat seguiría latiendo, pero pierde la fase contra el
     *   click en el primer cambio de BPM en vuelo.
     * - [nextBeatFrame] es un `Int`, así que es exacto por 2^31 frames de transport
     *   corrido sin resetear la posición: **12,4 h a 48 kHz y 6,2 h a 96 kHz**.
     *
     * Tiene default no-op para que las implementaciones existentes sigan compilando;
     * sobrescribilo para animar el pulso desde el push en vez de con un reloj propio.
     *
     * @param beatIndex     Índice del beat que acaba de sonar, monótono desde que se
     *                      armó el metrónomo.
     * @param nextBeatFrame Frame ABSOLUTO del PRÓXIMO beat, en la escala de
     *                      [IAudioNativeBridge.transportGetPlayFrame].
     */
    fun onBeat(beatIndex: Int, nextBeatFrame: Int) {}
}
