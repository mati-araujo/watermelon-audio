package com.watermellonstudios.audio.harness

import com.watermellonstudios.audio.api.TunerReading
import com.watermellonstudios.audio.domain.tuner.TunerSnapshot
import com.watermellonstudios.audio.domain.tuner.TunerState
import kotlin.test.Test
import kotlin.test.assertEquals

/** MINI-053 — la línea `TUNER-READ` del panel del afinador. Cada test nombra el bug que atrapa. */
class TunerReadLineTest {

    /** Bug que atrapa: un campo que cambia de nombre, de orden o de formato y rompe el parser del Mac. */
    @Test
    fun fullReadingWritesEveryFieldInOrder() {
        val s = snapshot(
            rate = 48000, levelRms = 0.1f, analyzed = 123456789012L, dropped = 37, count = 4,
            seam = true, state = TunerState.CONVERGED, hz = 110.04f, cents = -3.27f, locked = 5,
        )
        assertEquals(
            "TUNER-READ t-ms=80 analizados=123456789012 perdidos=37 discontinuidades=4 costura=1 " +
                "rate=48000 estado-motor=CONVERGED nivel-dbfs=-20.0 hz=110.0 cents=-3.3 cuerda=5",
            tunerReadLine(80, TunerReading(null, s)),
        )
    }

    /**
     * Bug que atrapa: una ausencia escrita como un cero plausible (`rate=0`, `hz=0.0`, `cuerda=0`)
     * que el análisis leería como medición; y la costura apagada escrita como algo que no es `0`.
     */
    @Test
    fun absencesAreDashesNotZeros() {
        val s = snapshot(
            rate = 0, levelRms = 0f, analyzed = 0, dropped = 0, count = 0,
            seam = false, state = TunerState.NO_SIGNAL, hz = null, cents = null, locked = null,
        )
        assertEquals(
            "TUNER-READ t-ms=0 analizados=0 perdidos=0 discontinuidades=0 costura=0 " +
                "rate=- estado-motor=NO_SIGNAL nivel-dbfs=-inf hz=- cents=- cuerda=-",
            tunerReadLine(0, TunerReading(null, s)),
        )
    }

    /** Bug que atrapa: un `NaN` del motor que sale como "NaN" (o con coma de locale) en vez de `-`. */
    @Test
    fun nanLevelIsADash() {
        val line = tunerReadLine(
            1, TunerReading(null, snapshot(48000, Float.NaN, 1, 0, 0, false, TunerState.NO_LOCK, null, null, null)),
        )
        assertEquals("nivel-dbfs=-", line.split(' ').single { it.startsWith("nivel-dbfs=") })
    }

    /** Bug que atrapa: `reading() == null` colapsado con una lectura en cero, o perdido de la serie. */
    @Test
    fun noReadingIsItsOwnLineWithTime() {
        assertEquals("TUNER-READ t-ms=1600 sin-lectura", tunerReadLine(1600, null))
    }

    private fun snapshot(
        rate: Int, levelRms: Float, analyzed: Long, dropped: Long, count: Long, seam: Boolean,
        state: TunerState, hz: Float?, cents: Float?, locked: Int?,
    ) = TunerSnapshot(
        captureSampleRate = rate, levelRms = levelRms, framesAnalyzed = analyzed, droppedFrames = dropped,
        state = state, cents = cents, phaseAngle = null, uncertainty = null, detectedHz = hz,
        detectionClarity = 0.9f, inharmonicityB = null, lockedString = locked, fastModeState = 0,
        usableRangeCents = null, inputDiscontinuity = seam, discontinuityCount = count, spectralSupport = null,
    )
}
