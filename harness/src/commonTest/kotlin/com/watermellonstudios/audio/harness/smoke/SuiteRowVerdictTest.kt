package com.watermellonstudios.audio.harness.smoke

import com.watermellonstudios.audio.harness.smoke.SuiteRowVerdict.FAIL
import com.watermellonstudios.audio.harness.smoke.SuiteRowVerdict.NOT_APPLICABLE
import com.watermellonstudios.audio.harness.smoke.SuiteRowVerdict.PASS
import kotlin.test.Test
import kotlin.test.assertEquals

/** REQ-050 S3 — el veredicto del harness sobre cada fila de la suite USB (AC-050.8, AC-050.10). */
class SuiteRowVerdictTest {

    private fun row(
        passed: Boolean = true,
        notApplicable: Boolean = false,
        first: Long? = 100,
        last: Long? = 900,
        measured: Float? = 0f,
        row: Int = 96_000,
        stream: Int = 96_000,
        restored: Boolean = true,
        stallMs: Long = 100,
    ) = SuiteRowVerdict.of(passed, notApplicable, first, last, measured, row, stream, restored, stallMs)

    /** El caso sano: la fila de 96 k se midió a 96 k, con tráfico y el stream restaurado. */
    @Test
    fun aRowMeasuredAtItsRateWithTrafficPasses() {
        assertEquals(PASS, row())
        assertEquals(PASS, row(measured = 96_000.3f))
        assertEquals(PASS, row(measured = null))
    }

    /**
     * Bug que atrapa: MINI-039 otra vez — el PASSED de la librería sobre una fila de 96 k que se
     * midió con el stream de 48 k. Antes era NO-MEDIDO; ahora que el runner aplica el rate, es FAIL.
     */
    @Test
    fun aRowWhoseStreamRanAtAnotherRateFailsEvenIfTheLibrarySaysPassed() {
        assertEquals(FAIL, row(stream = 48_000))
        assertEquals(FAIL, row(stream = 0))
    }

    /** Bug que atrapa (D5): un NOT_APPLICABLE de la librería contado como pasado o como fallo. */
    @Test
    fun aRowTheDeviceDoesNotOfferIsNotApplicable() {
        assertEquals(NOT_APPLICABLE, row(passed = false, notApplicable = true, first = null, last = null, stream = 0))
    }

    /** Bug que atrapa: 96 k pedido, el device reporta 48 k por feedback — un rate mal negociado. */
    @Test
    fun aMisnegotiatedRateFails() {
        assertEquals(FAIL, row(measured = 48_000f))
    }

    /** Bug que atrapa (AC-050.9): un stream muerto que la librería diera por pasado. */
    @Test
    fun aRowWithoutTrafficFails() {
        assertEquals(FAIL, row(first = null, last = null))
        assertEquals(FAIL, row(first = 500, last = 500))
    }

    /** Bug que atrapa (D18): una fila que pasó pero dejó el stream del consumidor roto. */
    @Test
    fun aRowThatCouldNotRestoreTheStreamFails() {
        assertEquals(FAIL, row(restored = false))
    }

    /** Una fila que la librería dio por fallida sigue siendo fallida. */
    @Test
    fun aFailingRowIsStillAFailure() {
        assertEquals(FAIL, row(passed = false))
    }

    /**
     * Bug que atrapa (review de S3): un stream que se traba a los 0,5 s de una fila de 5 s deja
     * "último > primero" en verdadero y congela todo lo demás. Sin este chequeo pasaba.
     */
    @Test
    fun aRowWhoseStreamStalledHalfWayFails() {
        assertEquals(FAIL, row(stallMs = 4_500))
        assertEquals(FAIL, row(stallMs = SuiteRowVerdict.STALL_LIMIT_MS))
    }

    /** El tramo sin crecer se mide sobre las muestras, también el que va hasta la última. */
    @Test
    fun theLongestStallIsMeasuredOverTheSamples() {
        assertEquals(0L, SuiteRowVerdict.longestStallMs(listOf(0L to 10L)))
        assertEquals(100L, SuiteRowVerdict.longestStallMs(listOf(0L to 10L, 100L to 20L, 200L to 30L)))
        // Sube al principio y se queda: 0 -> 100 crece, de 100 a 5000 no.
        assertEquals(4_900L, SuiteRowVerdict.longestStallMs(listOf(0L to 10L, 100L to 20L, 2_000L to 20L, 5_000L to 20L)))
    }
}
