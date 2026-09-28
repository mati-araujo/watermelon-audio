package com.watermellonstudios.audio.domain.state

/**
 * Information about the active audio stream.
 *
 * ## `channelCount` e `isLowLatency` son MEDIDOS o AUSENTES (REQ-045, D10)
 *
 * Los dos son nullables, y el `null` no es comodidad: hasta el 2026-09-28
 * [fromNativeArray] los rellenaba con `2` y `true` mientras el nativo le pasaba tres
 * números, así que un consumidor leía **dos valores inventados** como si fueran del
 * stream. Un default plausible es peor que una ausencia — este repo ya shippeó dos
 * stubs cuyos ceros derrotaron los fallbacks elvis de sus propios llamadores.
 *
 * `isLowLatency` puede ser `null` **con stream abierto**: Oboe sabe contestarlo
 * (`getPerformanceMode()` del stream negociado) y Core Audio no tiene un modo
 * análogo. Derivarlo del buffer sería volver a inventarlo, una capa más abajo.
 *
 * @property sampleRate Sample rate in Hz (e.g., 48000)
 * @property bufferSizeInFrames Buffer size in frames
 * @property channelCount Channels of the open stream, or `null` if the platform did
 *   not report them. **Nunca un default.**
 * @property latencyMillis Estimated latency in milliseconds
 * @property isLowLatency Whether the open stream is in the platform's low-latency
 *   mode, or `null` when the platform cannot answer. **Nunca un default.**
 */
data class StreamInfo(
    val sampleRate: Int = 48000,
    val bufferSizeInFrames: Int = 192,
    val channelCount: Int? = null,
    val latencyMillis: Double = 4.0,
    val isLowLatency: Boolean? = null
) {
    companion object {
        val EMPTY = StreamInfo()

        /**
         * Índices del array que cruza la frontera. Los tres primeros son los de
         * siempre: los dos nuevos van **al final** para que un lector viejo siga
         * leyendo lo mismo en 0..2.
         */
        private const val I_SAMPLE_RATE = 0
        private const val I_BUFFER_SIZE = 1
        private const val I_LATENCY_MS = 2
        private const val I_CHANNELS = 3
        private const val I_LOW_LATENCY = 4

        /** El tri-estado de `isLowLatency` tal como lo manda la C API. */
        private const val LOW_LATENCY_UNKNOWN = -1f
        private const val LOW_LATENCY_YES = 1f

        fun fromNativeArray(array: FloatArray?): StreamInfo? {
            if (array == null || array.size < 3) return null
            return StreamInfo(
                sampleRate = array[I_SAMPLE_RATE].toInt(),
                bufferSizeInFrames = array[I_BUFFER_SIZE].toInt(),
                latencyMillis = array[I_LATENCY_MS].toDouble(),
                // Un array corto es un nativo que no los sabe reportar: ausentes, no 2
                // y true. Y un conteo de canales en 0 es "no lo sé" del lado nativo,
                // no un stream mudo.
                channelCount = array.getOrNull(I_CHANNELS)?.toInt()?.takeIf { it > 0 },
                isLowLatency = array.getOrNull(I_LOW_LATENCY)
                    ?.takeIf { it != LOW_LATENCY_UNKNOWN }
                    ?.let { it == LOW_LATENCY_YES },
            )
        }
    }
}
