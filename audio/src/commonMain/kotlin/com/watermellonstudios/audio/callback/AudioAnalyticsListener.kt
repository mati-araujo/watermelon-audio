package com.watermellonstudios.audio.callback

import com.watermellonstudios.audio.domain.effect.EffectType
import com.watermellonstudios.audio.domain.modulator.ModulatorType
import com.watermellonstudios.audio.domain.oscillator.OscillatorType
import com.watermellonstudios.audio.domain.scale.ScaleMode
import com.watermellonstudios.audio.domain.state.AudioError
import com.watermellonstudios.audio.domain.state.StreamInfo

/**
 * Interface for external analytics integration.
 *
 * Implement this interface in the app module to connect
 * to your analytics framework (Firebase Analytics, etc.)
 *
 * Example implementation:
 * ```kotlin
 * class FirebaseAudioAnalytics : AudioAnalyticsListener {
 *     override fun onSessionStarted(streamInfo: StreamInfo) {
 *         Firebase.analytics.logEvent("audio_session_started") {
 *             param("sample_rate", streamInfo.sampleRate.toLong())
 *         }
 *     }
 *     // ... other methods
 * }
 * ```
 */
interface AudioAnalyticsListener {
    /** Called when an audio session starts */
    fun onSessionStarted(streamInfo: StreamInfo)

    /** Called when an audio session ends */
    fun onSessionEnded(durationMs: Long)

    /** Called when oscillator type changes */
    fun onOscillatorChanged(type: OscillatorType, previous: OscillatorType)

    /** Called when modulator type changes */
    fun onModulatorChanged(type: ModulatorType, previous: ModulatorType)

    /** Called when an effect is added */
    fun onEffectAdded(type: EffectType, index: Int)

    /** Called when an effect is removed */
    fun onEffectRemoved(type: EffectType, index: Int)

    /** Called when scale mode changes */
    fun onScaleModeChanged(mode: ScaleMode, previous: ScaleMode)

    /** Called when an error occurs */
    fun onError(error: AudioError)

    /** Called for custom events */
    fun onCustomEvent(name: String, params: Map<String, Any> = emptyMap())
}

/**
 * Default no-op analytics implementation.
 * Used when no analytics listener is provided.
 */
object NoOpAudioAnalytics : AudioAnalyticsListener {
    override fun onSessionStarted(streamInfo: StreamInfo) {}
    override fun onSessionEnded(durationMs: Long) {}
    override fun onOscillatorChanged(type: OscillatorType, previous: OscillatorType) {}
    override fun onModulatorChanged(type: ModulatorType, previous: ModulatorType) {}
    override fun onEffectAdded(type: EffectType, index: Int) {}
    override fun onEffectRemoved(type: EffectType, index: Int) {}
    override fun onScaleModeChanged(mode: ScaleMode, previous: ScaleMode) {}
    override fun onError(error: AudioError) {}
    override fun onCustomEvent(name: String, params: Map<String, Any>) {}
}
