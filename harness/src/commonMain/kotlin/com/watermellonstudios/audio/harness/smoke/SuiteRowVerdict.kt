package com.watermellonstudios.audio.harness.smoke

import kotlin.math.abs

/**
 * El veredicto de una fila de la suite de `UsbAudioTestRunner`, del lado del harness.
 *
 * Desde REQ-050 S3 el runner mide cada fila al rate que declara (AC-050.8), así que el viejo
 * NO-MEDIDO de MINI-038 (D11: "el runner ignora el rate de la fila") ya no existe. Quedan tres:
 *
 * - [NOT_APPLICABLE]: la librería devolvió `NOT_APPLICABLE` — el device no ofrece la config de la
 *   fila y no se midió. No es PASS ni FAIL y no cuenta como cobertura (D5).
 * - [FAIL]: cualquiera de estas, en este orden:
 *   - sin TRÁFICO (paquetes completados que crecen durante la fila): el stream está muerto;
 *   - el stream se TRABÓ a mitad de la fila ([STALL_LIMIT_MS] sin un paquete completado): "el
 *     último es mayor que el primero" lo da por vivo con una sola subida al principio;
 *   - el stream corrió a un rate distinto del de la fila (`streamSampleRateHz`): la fila no se midió
 *     a lo que declara, diga lo que diga la librería;
 *   - el device reporta un rate medido (feedback) y no es el de la fila;
 *   - la librería no pudo restaurar el stream del consumidor después de la fila (D18);
 *   - la librería no dijo PASSED.
 * - [PASS]: lo demás.
 *
 * El harness NO repite el juicio de la librería sobre la latencia y el éxito (techo declarado,
 * paquetes en vuelo): lo afirma `UsbAudioTestRunnerTest`. Acá se re-verifica lo que una regresión
 * del runner podría esconder detrás de un PASSED: tráfico, rate y restauración.
 */
enum class SuiteRowVerdict {
    PASS,
    FAIL,
    NOT_APPLICABLE;

    companion object {
        /** Tolerancia del rate medido contra el pedido: 1 %, holgada para el drift de un reloj USB. */
        const val RATE_TOLERANCE = 0.01f

        /** Medio segundo sin un paquete completado es un stream muerto (completan cada 1-8 ms). */
        const val STALL_LIMIT_MS = 500L

        /**
         * El tramo más largo sin crecer los completados, en ms, sobre muestras (instante, completados)
         * en orden. Con menos de dos muestras no hay tramo que medir: 0.
         */
        fun longestStallMs(points: List<Pair<Long, Long>>): Long {
            if (points.size < 2) return 0
            var lastGrowthAt = points.first().first
            var lastCompleted = points.first().second
            var longest = 0L
            for ((at, completed) in points.drop(1)) {
                longest = maxOf(longest, at - lastGrowthAt)
                if (completed > lastCompleted) {
                    lastCompleted = completed
                    lastGrowthAt = at
                }
            }
            return longest
        }

        fun of(
            libraryPassed: Boolean,
            libraryNotApplicable: Boolean,
            firstCompleted: Long?,
            lastCompleted: Long?,
            measuredRateHz: Float?,
            rowRateHz: Int,
            streamRateHz: Int,
            restored: Boolean,
            longestStallMs: Long,
        ): SuiteRowVerdict {
            if (libraryNotApplicable) return NOT_APPLICABLE
            if (failureReason(libraryPassed, firstCompleted, lastCompleted, measuredRateHz, rowRateHz, streamRateHz, restored, longestStallMs) != null) {
                return FAIL
            }
            return PASS
        }

        /** El motivo del FAIL, para la línea del smoke; null si la fila pasa. */
        fun failureReason(
            libraryPassed: Boolean,
            firstCompleted: Long?,
            lastCompleted: Long?,
            measuredRateHz: Float?,
            rowRateHz: Int,
            streamRateHz: Int,
            restored: Boolean,
            longestStallMs: Long,
        ): String? = when {
            firstCompleted == null || lastCompleted == null || lastCompleted <= firstCompleted -> "sin-trafico"
            longestStallMs >= STALL_LIMIT_MS -> "el-stream-se-trabo-${longestStallMs}ms"
            streamRateHz != rowRateHz -> "el-stream-corrio-a-$streamRateHz-no-a-$rowRateHz"
            rateContradicts(measuredRateHz, rowRateHz) -> "rate-real-distinto-del-pedido"
            !restored -> "no-restauro-el-stream"
            !libraryPassed -> "estado-de-la-libreria"
            else -> null
        }

        /** `true` si el device reporta un rate medido (> 0) y NO es el pedido. Sin feedback no contradice nada. */
        fun rateContradicts(measuredRateHz: Float?, requestedRateHz: Int): Boolean =
            measuredRateHz != null && measuredRateHz > 0f &&
                abs(measuredRateHz - requestedRateHz) > requestedRateHz * RATE_TOLERANCE
    }
}
