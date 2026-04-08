package com.watermellonstudios.audio.domain.state

import com.watermellonstudios.audio.domain.effect.EffectChainState
import com.watermellonstudios.audio.domain.modulator.ModulatorType
import com.watermellonstudios.audio.domain.oscillator.OscillatorType

/**
 * Complete state of the audio engine.
 * Immutable data class for reactive state management.
 */
data class AudioState(
    // Lifecycle
    val lifecycle: EngineLifecycle = EngineLifecycle.STOPPED,
    val isPaused: Boolean = false,

    // Volume & Fade
    val masterVolume: Float = 1.0f,
    val currentFadeVolume: Float = 1.0f,
    val targetFadeVolume: Float = 1.0f,
    val isFading: Boolean = false,
    val fadeProgress: Float = 0.0f,

    // Oscillator
    val oscillator: OscillatorType = OscillatorType.SAW,
    val frequency: Float = 440f,
    val amplitude: Float = 0.5f,
    val xPosition: Float = 0.5f,
    val yPosition: Float = 0.5f,

    // Modulator
    val modulator: ModulatorType = ModulatorType.NONE,

    // Effects
    val effectChain: EffectChainState = EffectChainState(),

    // Stream
    val streamInfo: StreamInfo? = null,

    // Error
    val error: AudioError? = null
) {
    val isRunning: Boolean get() = lifecycle == EngineLifecycle.RUNNING
    val isPlaying: Boolean get() = isRunning && !isPaused
}

/**
 * Represents an audio engine error.
 */
data class AudioError(
    val code: Int,
    val message: String,
    val isRecoverable: Boolean = true
) {
    companion object {
        fun fromStreamError(code: Int): AudioError {
            val message = when (code) {
                -899 -> "Audio device disconnected"
                -898 -> "Audio permission denied"
                -897 -> "Audio device is busy"
                -896 -> "Audio stream closed unexpectedly"
                else -> "Unknown audio error"
            }
            val isRecoverable = code == -899 // Only disconnection is auto-recoverable
            return AudioError(code, message, isRecoverable)
        }
    }
}
