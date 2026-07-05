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
}
