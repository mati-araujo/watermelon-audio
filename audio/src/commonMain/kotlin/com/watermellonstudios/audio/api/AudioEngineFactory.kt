package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.api.config.AudioEngineConfig
import com.watermellonstudios.audio.internal.engine.AudioEngineImpl

/**
 * Factory for creating [AudioEngine] instances.
 *
 * Usage:
 * ```kotlin
 * // Simple creation with defaults
 * val engine = AudioEngineFactory.create()
 *
 * // With custom configuration
 * val config = AudioEngineConfig.builder()
 *     .sampleRate(48000)
 *     .enableLowLatency(true)
 *     .logger(myLogger)
 *     .build()
 * val engine = AudioEngineFactory.create(config)
 * ```
 */
object AudioEngineFactory {

    /**
     * Creates a new [AudioEngine] instance.
     *
     * @param config Configuration for the audio engine (default: [AudioEngineConfig.DEFAULT])
     * @return A new [AudioEngine] instance ready to use
     */
    fun create(config: AudioEngineConfig = AudioEngineConfig.DEFAULT): AudioEngine {
        return AudioEngineImpl(config)
    }
}
