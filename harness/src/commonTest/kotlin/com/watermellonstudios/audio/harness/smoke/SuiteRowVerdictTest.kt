package com.watermellonstudios.audio.harness.smoke

import com.watermellonstudios.audio.harness.smoke.SuiteRowVerdict.FAIL
import com.watermellonstudios.audio.harness.smoke.SuiteRowVerdict.NOT_MEASURED
import com.watermellonstudios.audio.harness.smoke.SuiteRowVerdict.PASS
import kotlin.test.Test
import kotlin.test.assertEquals

/** MINI-038, D11 — el veredicto de cada fila de la suite USB. El stream se abre a 48 k. */
class SuiteRowVerdictTest {

    private fun row(passed: Boolean = true, first: Long? = 100, last: Long? = 900, measured: Float?, row: Int) =
        SuiteRowVerdict.of(passed, first, last, measured, row, streamRateHz = 48_000)

    @Test
    fun aPassedRowAtTheStreamRateWithTrafficPasses() {
        assertEquals(PASS, row(measured = 48_000.4f, row = 48_000))
    }

    /** Bug que atrapa (N1): un device sin feedback (rate medido 0) no puede dejar sin medir la fila de 48 k. */
    @Test
    fun withoutFeedbackTheStreamRateRowStillPasses() {
        assertEquals(PASS, row(measured = 0f, row = 48_000))
        assertEquals(PASS, row(measured = null, row = 48_000))
    }

    /** Bug que atrapa (D11): el PASSED de la librería sobre una fila de 96 k que midió el stream de 48 k. */
    @Test
    fun aRowAtAnotherRateThanTheStreamIsNotMeasuredEvenIfTheLibrarySaysPassed() {
        assertEquals(NOT_MEASURED, row(measured = 48_000f, row = 96_000))
        assertEquals(NOT_MEASURED, row(measured = 0f, row = 44_100))
    }

    /** Bug que atrapa (N1): 48 k pedido, el device quedó en 44,1 k — un bug de negociación que no puede pasar. */
    @Test
    fun aMisnegotiatedRateFailsTheStreamRowAndDoesNotPassTheOtherOne() {
        assertEquals(FAIL, row(measured = 44_100f, row = 48_000))
        assertEquals(NOT_MEASURED, row(measured = 44_100f, row = 44_100))
    }

    /** Bug que atrapa: una fila de 48 k que falla tapada como "no medida". */
    @Test
    fun aFailingRowAtTheStreamRateIsStillAFailure() {
        assertEquals(FAIL, row(passed = false, measured = 48_000f, row = 48_000))
        assertEquals(FAIL, row(first = 900, last = 900, measured = 48_000f, row = 48_000))
    }

    /** Bug que atrapa: un stream muerto (sin tráfico) escondido como "no medido" en una fila de otro rate. */
    @Test
    fun aRowWithoutTrafficIsAFailureEvenAtAnotherRate() {
        assertEquals(FAIL, row(first = null, last = null, measured = null, row = 96_000))
        assertEquals(FAIL, row(first = 500, last = 500, measured = 0f, row = 44_100))
    }
}
