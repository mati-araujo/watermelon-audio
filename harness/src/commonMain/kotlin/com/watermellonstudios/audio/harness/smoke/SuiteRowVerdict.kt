package com.watermellonstudios.audio.harness.smoke

import kotlin.math.abs

/**
 * MINI-038, D11 (humano) — el veredicto de una fila de la suite de `UsbAudioTestRunner`.
 *
 * Tres valores, no dos:
 * - [FAIL] primero: sin TRÁFICO medido (paquetes completados que crecen durante el test) el stream
 *   está muerto, y eso es un fallo sea cual sea la fila — no puede esconderse como "no medido".
 * - [NOT_MEASURED]: la fila pide un rate distinto del rate al que el harness ABRIÓ el stream.
 *   `runPlaybackTest` ignora `config.sampleRate` y mide el stream que ya corre, así que las filas de
 *   44,1 k y 96 k de `STANDARD_SUITE` no dicen nada de esos rates. No es PASS ni FAIL y no cuenta
 *   como cobertura. El arreglo del runner es de `:audio` y va a otro MINI.
 * - [PASS]: la librería dijo PASSED, hubo tráfico y, SI el device reporta un rate medido, coincide
 *   con el pedido.
 *
 * 🔴 La clasificación se decide por lo que se sabe DE ANTEMANO (el rate pedido vs el de la fila), no
 * por el rate medido. `currentSampleRateHz` sólo existe con feedback (endpoint async o implícito por
 * captura): en un device adaptativo o síncrono vale 0, y clasificar por él dejaba TODAS las filas en
 * "no medido" — incluida la de 48 k, que sí midió — y, con feedback, un rate mal negociado (48 k
 * pedido, 44,1 k real) hacía PASAR la fila de 44,1 k. El rate medido sólo VERIFICA: si es > 0 y no
 * coincide con el pedido, la fila es FAIL (review de MINI-038, N1).
 */
enum class SuiteRowVerdict {
    PASS,
    FAIL,
    NOT_MEASURED;

    companion object {
        /** Tolerancia del rate medido contra el pedido: 1 %, holgada para el drift de un reloj USB. */
        const val RATE_TOLERANCE = 0.01f

        fun of(
            libraryPassed: Boolean,
            firstCompleted: Long?,
            lastCompleted: Long?,
            measuredRateHz: Float?,
            rowRateHz: Int,
            streamRateHz: Int,
        ): SuiteRowVerdict {
            val traffic = firstCompleted != null && lastCompleted != null && lastCompleted > firstCompleted
            if (!traffic) return FAIL
            if (rowRateHz != streamRateHz) return NOT_MEASURED
            if (rateContradicts(measuredRateHz, streamRateHz)) return FAIL
            return if (libraryPassed) PASS else FAIL
        }

        /** `true` si el device reporta un rate medido (> 0) y NO es el pedido. Sin feedback no contradice nada. */
        fun rateContradicts(measuredRateHz: Float?, requestedRateHz: Int): Boolean =
            measuredRateHz != null && measuredRateHz > 0f &&
                abs(measuredRateHz - requestedRateHz) > requestedRateHz * RATE_TOLERANCE
    }
}
