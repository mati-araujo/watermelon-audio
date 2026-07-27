package com.watermellonstudios.audio.api.config

import com.watermellonstudios.audio.callback.AudioAnalyticsListener
import com.watermellonstudios.audio.callback.AudioLogger
import com.watermellonstudios.audio.callback.NoOpAudioAnalytics
import com.watermellonstudios.audio.callback.NoOpAudioLogger
import com.watermellonstudios.audio.domain.device.DeviceCapabilities
import com.watermellonstudios.audio.domain.effect.EffectType
import com.watermellonstudios.audio.domain.oscillator.OscillatorType

/**
 * Configuration for the audio engine.
 *
 * Use [AudioEngineConfig.Builder] for convenient construction:
 * ```kotlin
 * val config = AudioEngineConfig.builder()
 *     .sampleRate(48000)
 *     .enableLowLatency(true)
 *     .logger(myLogger)
 *     .build()
 * ```
 */
data class AudioEngineConfig(
    /** Target sample rate in Hz */
    val sampleRate: Int = 48000,

    /** Preferred buffer size in frames (0 = auto) */
    val bufferSize: Int = 0,

    /** Enable low-latency mode */
    val enableLowLatency: Boolean = true,

    /** Maximum effects in chain */
    val maxEffects: Int = 12,

    /** Default oscillator type */
    val defaultOscillator: OscillatorType = OscillatorType.SAW,

    /** Default effects to add on startup */
    val defaultEffects: List<EffectType> = emptyList(),

    /** Default fade time in milliseconds */
    val defaultFadeMs: Int = 500,

    /** Logger for debug output */
    val logger: AudioLogger = NoOpAudioLogger,

    /** Analytics listener */
    val analyticsListener: AudioAnalyticsListener = NoOpAudioAnalytics
) {
    class Builder {
        private var sampleRate: Int = 48000
        private var bufferSize: Int = 0
        private var enableLowLatency: Boolean = true
        private var maxEffects: Int = 12
        private var defaultOscillator: OscillatorType = OscillatorType.SAW
        private var defaultEffects: List<EffectType> = emptyList()
        private var defaultFadeMs: Int = 500
        private var logger: AudioLogger = NoOpAudioLogger
        private var analyticsListener: AudioAnalyticsListener = NoOpAudioAnalytics

        fun sampleRate(rate: Int) = apply { this.sampleRate = rate }
        fun bufferSize(size: Int) = apply { this.bufferSize = size }
        fun enableLowLatency(enable: Boolean) = apply { this.enableLowLatency = enable }
        fun maxEffects(max: Int) = apply { this.maxEffects = max }
        fun defaultOscillator(type: OscillatorType) = apply { this.defaultOscillator = type }
        fun defaultEffects(effects: List<EffectType>) = apply { this.defaultEffects = effects }
        fun defaultFadeMs(ms: Int) = apply { this.defaultFadeMs = ms }
        fun logger(logger: AudioLogger) = apply { this.logger = logger }
        fun analyticsListener(listener: AudioAnalyticsListener) = apply { this.analyticsListener = listener }

        fun build() = AudioEngineConfig(
            sampleRate = sampleRate,
            bufferSize = bufferSize,
            enableLowLatency = enableLowLatency,
            maxEffects = maxEffects,
            defaultOscillator = defaultOscillator,
            defaultEffects = defaultEffects,
            defaultFadeMs = defaultFadeMs,
            logger = logger,
            analyticsListener = analyticsListener
        )
    }

    companion object {
        /** Default configuration */
        val DEFAULT = AudioEngineConfig()

        /** Builder for convenient construction */
        fun builder() = Builder()

        /**
         * Cadena de efectos recortada para un dispositivo de gama baja.
         *
         * El valor viene del `DeviceCapabilities` de androidMain, que ya aplicaba este
         * recorte desde antes de WA-1.2.
         */
        const val LOW_END_MAX_EFFECTS = 6

        /**
         * Ajusta [base] a lo que el dispositivo puede sostener (WA-1.2).
         *
         * Acá vive la **política**; [DeviceCapabilities] sólo tiene los hechos. La
         * separación importa porque el umbral de "gama baja" es una heurística por
         * plataforma que va a cambiar, y no debería arrastrar consigo la detección.
         *
         * Sólo toca lo que el dispositivo puede desmentir:
         * - [maxEffects] se **recorta** —nunca se sube— a [LOW_END_MAX_EFFECTS] en gama
         *   baja. Recortar, y no fijar, es lo que hace que un consumidor que pidió 4
         *   efectos siga teniendo 4 y no 6.
         * - [enableLowLatency] se apaga si la plataforma no ofrece el path. Hoy ninguna
         *   de las dos lo niega (AAudio desde API 26, Core Audio siempre), así que en la
         *   práctica esto no cambia nada — está para que el día que aparezca un target
         *   que sí lo niegue, el motor no pida algo que no existe.
         *
         * Todo lo demás de [base] se respeta tal cual: el logger, los efectos por
         * defecto y el sample rate son decisiones del consumidor, no del hardware.
         */
        fun tunedFor(
            capabilities: DeviceCapabilities,
            base: AudioEngineConfig = DEFAULT,
        ): AudioEngineConfig = base.copy(
            maxEffects = if (capabilities.isLowEndDevice) {
                minOf(base.maxEffects, LOW_END_MAX_EFFECTS)
            } else {
                base.maxEffects
            },
            enableLowLatency = base.enableLowLatency && capabilities.supportsLowLatencyAudio,
        )
    }
}
