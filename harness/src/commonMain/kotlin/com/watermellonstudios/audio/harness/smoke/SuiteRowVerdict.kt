package com.watermellonstudios.audio.harness.smoke

import kotlin.math.abs

/**
 * MINI-038, D11 (humano) — el veredicto de una fila de la suite de `UsbAudioTestRunner`.
 *
 * Tres valores, no dos:
 * - [PASS]: la librería dijo PASSED **y** hubo tráfico medido (paquetes completados que crecen
 *   durante el test) **y** el stream medido corre al rate de la fila.
 * - [FAIL]: el rate de la fila SÍ se aplicó (o no hay stats para saberlo) y algo falló.
 * - [NOT_MEASURED]: el stream medido corre a OTRO rate que el de la fila. `runPlaybackTest` ignora
 *   `config.sampleRate`, así que las filas de 44,1 k y 96 k de `STANDARD_SUITE` miden el stream de
 *   48 k que ya estaba corriendo: su PASSED no dice nada de esos rates. No es PASS ni FAIL, y no
 *   cuenta como cobertura. El arreglo del runner es de `:audio` y va a otro MINI.
 *
 * Una fila sin stats (`realRateHz == null`) NO es "no medida": es un FAIL por falta de tráfico. Si
 * no, un stream muerto se escondería como "no medido".
 */
enum class SuiteRowVerdict {
    PASS,
    FAIL,
    NOT_MEASURED;

    companion object {
        /** Tolerancia del rate medido contra el de la fila: 1 %, holgada para el drift de un reloj USB. */
        const val RATE_TOLERANCE = 0.01f

        fun of(
            libraryPassed: Boolean,
            firstCompleted: Long?,
            lastCompleted: Long?,
            realRateHz: Float?,
            configRateHz: Int,
        ): SuiteRowVerdict {
            if (realRateHz != null && !rateApplied(realRateHz, configRateHz)) return NOT_MEASURED
            val traffic = firstCompleted != null && lastCompleted != null && lastCompleted > firstCompleted
            return if (libraryPassed && traffic && realRateHz != null) PASS else FAIL
        }

        fun rateApplied(realRateHz: Float, configRateHz: Int): Boolean =
            abs(realRateHz - configRateHz) <= configRateHz * RATE_TOLERANCE
    }
}
