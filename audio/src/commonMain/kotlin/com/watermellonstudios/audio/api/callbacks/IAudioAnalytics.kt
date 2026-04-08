package com.watermellonstudios.audio.api.callbacks

import com.watermellonstudios.audio.domain.oscillator.OscillatorType
import com.watermellonstudios.audio.domain.scale.ScaleMode

/**
 * Interface for audio analytics integration.
 *
 * This abstraction allows the audio module to log usage metrics without
 * depending on Firebase Analytics or any specific analytics implementation.
 *
 * Implementations should be provided by the app module via dependency injection.
 *
 * @see NoOpAudioAnalytics for a default no-op implementation
 */
interface IAudioAnalytics {

    // ========== SESSION EVENTS ==========

    /**
     * Log when an audio session starts.
     *
     * @param fadeTimeMs The fade-in time in milliseconds
     */
    fun logAudioSessionStarted(fadeTimeMs: Int)

    /**
     * Log when an audio session stops.
     *
     * @param sessionDurationMs Duration of the session in milliseconds
     * @param fadeTimeMs The fade-out time in milliseconds
     */
    fun logAudioSessionStopped(sessionDurationMs: Long, fadeTimeMs: Int)

    // ========== AUDIO METRICS ==========

    /**
     * Log audio latency measurement.
     *
     * @param latencyMs Measured latency in milliseconds
     * @param sampleRate Sample rate in Hz
     * @param bufferSize Buffer size in frames
     */
    fun logAudioLatency(latencyMs: Double, sampleRate: Int, bufferSize: Int)

    /**
     * Log a stream error.
     *
     * @param errorCode Error code from the audio engine
     * @param errorMessage Human-readable error message
     * @param isRecoverable Whether the error can be recovered from
     */
    fun logStreamError(errorCode: Int, errorMessage: String, isRecoverable: Boolean)

    /**
     * Log successful stream recovery.
     *
     * @param errorCode The original error code that was recovered from
     * @param recoveryTimeMs Time it took to recover in milliseconds
     */
    fun logStreamRecovery(errorCode: Int, recoveryTimeMs: Long)

    // ========== USER INTERACTIONS ==========

    /**
     * Log oscillator type change.
     *
     * @param oscillatorType The new oscillator type
     * @param fromType The previous oscillator type (null if first selection)
     */
    fun logOscillatorChanged(oscillatorType: OscillatorType, fromType: OscillatorType?)

    /**
     * Log scale mode change.
     *
     * @param scaleMode The new scale mode
     * @param previousMode The previous scale mode (null if first selection)
     */
    fun logScaleModeChanged(scaleMode: ScaleMode, previousMode: ScaleMode?)

    /**
     * Log dual touch usage.
     *
     * @param touchCount Number of active touches
     * @param distance Distance between touches
     * @param angle Angle between touches
     * @param mode The dual touch mode (active, passive, mix)
     */
    fun logDualTouchUsed(touchCount: Int, distance: Float, angle: Float, mode: String)
}

/**
 * No-op implementation of IAudioAnalytics.
 *
 * Use this when analytics is not needed or not available.
 */
object NoOpAudioAnalytics : IAudioAnalytics {
    override fun logAudioSessionStarted(fadeTimeMs: Int) = Unit
    override fun logAudioSessionStopped(sessionDurationMs: Long, fadeTimeMs: Int) = Unit
    override fun logAudioLatency(latencyMs: Double, sampleRate: Int, bufferSize: Int) = Unit
    override fun logStreamError(errorCode: Int, errorMessage: String, isRecoverable: Boolean) = Unit
    override fun logStreamRecovery(errorCode: Int, recoveryTimeMs: Long) = Unit
    override fun logOscillatorChanged(oscillatorType: OscillatorType, fromType: OscillatorType?) = Unit
    override fun logScaleModeChanged(scaleMode: ScaleMode, previousMode: ScaleMode?) = Unit
    override fun logDualTouchUsed(touchCount: Int, distance: Float, angle: Float, mode: String) = Unit
}
