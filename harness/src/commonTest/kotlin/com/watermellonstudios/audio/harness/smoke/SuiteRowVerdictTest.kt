package com.watermellonstudios.audio.harness.smoke

import com.watermellonstudios.audio.harness.smoke.SuiteRowVerdict.FAIL
import com.watermellonstudios.audio.harness.smoke.SuiteRowVerdict.NOT_MEASURED
import com.watermellonstudios.audio.harness.smoke.SuiteRowVerdict.PASS
import kotlin.test.Test
import kotlin.test.assertEquals

/** MINI-038, D11 — el veredicto de cada fila de la suite USB. */
class SuiteRowVerdictTest {

    @Test
    fun aPassedRowAtItsOwnRateWithTrafficPasses() {
        assertEquals(PASS, SuiteRowVerdict.of(true, 100, 900, 48_000.4f, 48_000))
    }

    /** Bug que atrapa (D11): el PASSED de la librería sobre una fila de 96 k que midió el stream de 48 k. */
    @Test
    fun aRowWhoseRateWasNotAppliedIsNotMeasuredEvenIfTheLibrarySaysPassed() {
        assertEquals(NOT_MEASURED, SuiteRowVerdict.of(true, 100, 900, 48_000f, 96_000))
        assertEquals(NOT_MEASURED, SuiteRowVerdict.of(true, 100, 900, 48_000f, 44_100))
    }

    /** Bug que atrapa: una fila de 48 k que falla tapada como "no medida". */
    @Test
    fun aFailingRowAtItsOwnRateIsStillAFailure() {
        assertEquals(FAIL, SuiteRowVerdict.of(false, 100, 900, 48_000f, 48_000))
        assertEquals(FAIL, SuiteRowVerdict.of(true, 900, 900, 48_000f, 48_000))
    }

    /** Bug que atrapa: un stream muerto (sin stats) escondido como "no medido" en vez de FAIL. */
    @Test
    fun aRowWithoutStatsIsAFailureNotUnmeasured() {
        assertEquals(FAIL, SuiteRowVerdict.of(true, null, null, null, 96_000))
    }
}
