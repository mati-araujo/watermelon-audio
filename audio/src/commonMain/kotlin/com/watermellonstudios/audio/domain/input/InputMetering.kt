package com.watermellonstudios.audio.domain.input

/**
 * Una lectura completa del medidor de entrada, tomada de una sola vez.
 *
 * **Por qué existe como snapshot y no como siete getters.** Un medidor de UI
 * lee estos siete valores por frame; uno por uno son siete cruces de frontera
 * por tick, ~420/s a 60 fps. `wma_input_get_metering_snapshot()` los devuelve
 * en una sola llamada y esta clase es su forma en Kotlin.
 *
 * Y hay una razón más fuerte que el costo: leídos de a uno **no son
 * coherentes entre sí**. El thread de audio corre entre lectura y lectura, así
 * que un medidor podría mostrar el pico de un bloque y el flag de clipping de
 * otro. El snapshot es una foto.
 *
 * @property levelDbLeft   Pico del canal 0 en dBFS (negativo; -inf ≈ silencio)
 * @property levelDbRight  Pico del canal 1 en dBFS
 * @property levelLeft     Pico del canal 0, lineal 0..1
 * @property levelRight    Pico del canal 1, lineal 0..1
 * @property isClipping    El input pasó por 0 dBFS desde la última lectura
 * @property isNoiseGateOpen  La compuerta de ruido está dejando pasar señal
 * @property latencyMs     Latencia de entrada reportada por el backend
 */
data class InputMetering(
    val levelDbLeft: Float,
    val levelDbRight: Float,
    val levelLeft: Float,
    val levelRight: Float,
    val isClipping: Boolean,
    val isNoiseGateOpen: Boolean,
    val latencyMs: Float,
) {
    /** El más alto de los dos canales, lineal — lo que suele dibujar una barra. */
    val peakLinear: Float get() = if (levelLeft > levelRight) levelLeft else levelRight

    companion object {
        /** Cantidad de floats del snapshot nativo. Espeja `WMA_INPUT_METERING_VALUES`. */
        const val VALUE_COUNT: Int = 7

        /**
         * Estado cuando no hay nodo de entrada.
         *
         * **No es lo mismo que silencio y no hay que dibujarlo como tal.** La C
         * API devuelve `false` y **deja el buffer intacto** justamente para que
         * nadie lea ceros como si fueran una medición; acá eso se representa
         * devolviendo `null` desde el bridge, y este valor es sólo para quien
         * quiera un placeholder explícito.
         */
        val SILENT = InputMetering(
            levelDbLeft = -160f,
            levelDbRight = -160f,
            levelLeft = 0f,
            levelRight = 0f,
            isClipping = false,
            isNoiseGateOpen = false,
            latencyMs = 0f,
        )

        /**
         * Arma el snapshot desde los 7 floats nativos, en el orden que documenta
         * `wma_input_get_metering_snapshot`.
         *
         * @return null si el array no tiene exactamente [VALUE_COUNT] valores —
         *   un array corto significa que el contrato con la capa nativa se
         *   rompió, y devolver datos a medias sería peor que no devolver nada.
         */
        fun fromNative(values: FloatArray): InputMetering? {
            if (values.size < VALUE_COUNT) return null
            return InputMetering(
                levelDbLeft = values[0],
                levelDbRight = values[1],
                levelLeft = values[2],
                levelRight = values[3],
                isClipping = values[4] != 0f,
                isNoiseGateOpen = values[5] != 0f,
                latencyMs = values[6],
            )
        }
    }
}
