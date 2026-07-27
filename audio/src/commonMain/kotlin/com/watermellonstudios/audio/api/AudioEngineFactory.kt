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
 *
 * // Custom configuration, pero dejando que el dispositivo recorte lo que no aguanta
 * val engine = AudioEngineFactory.create(
 *     AudioEngineConfig.tunedFor(currentDeviceCapabilities(), config)
 * )
 * ```
 */
object AudioEngineFactory {

    /**
     * Creates a new [AudioEngine] instance.
     *
     * Sin argumentos, la config por defecto sale **ajustada al dispositivo** (WA-1.2):
     * [AudioEngineConfig.DEFAULT] pasado por [AudioEngineConfig.tunedFor] con
     * [currentDeviceCapabilities]. Un teléfono de gama baja arranca con la cadena de
     * efectos recortada en vez de con el mismo 12 que un buque insignia.
     *
     * Con una config explícita **no se toca nada**: lo que se pasa es lo que se usa.
     * La regla es que la librería adivina sólo cuando no le dijeron; en cuanto el
     * consumidor abre la boca, manda él. Para pedir ambas cosas —config propia y
     * ajuste por dispositivo— está [AudioEngineConfig.tunedFor], que es explícito.
     *
     * @param config Configuration for the audio engine (default: [AudioEngineConfig.DEFAULT]
     *   ajustado a [currentDeviceCapabilities])
     * @return A new [AudioEngine] instance ready to use
     */
    fun create(
        config: AudioEngineConfig = AudioEngineConfig.tunedFor(currentDeviceCapabilities()),
    ): AudioEngine {
        return AudioEngineImpl(config)
    }
}
