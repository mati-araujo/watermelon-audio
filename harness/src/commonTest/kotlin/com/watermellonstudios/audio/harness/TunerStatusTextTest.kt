package com.watermellonstudios.audio.harness

import com.watermellonstudios.audio.api.TunerReading
import com.watermellonstudios.audio.domain.tuner.TunerSnapshot
import com.watermellonstudios.audio.domain.tuner.TunerState
import com.watermellonstudios.audio.domain.tuning.StringTarget
import com.watermellonstudios.audio.harness.smoke.TunerPhase
import kotlin.test.Test
import kotlin.test.assertEquals

/** MINI-045 — el texto de estado del panel del afinador. Cada test nombra el bug que atrapa. */
class TunerStatusTextTest {

    /** Bug que atrapa: detenido y "el motor no publicó nada" confundidos. */
    @Test
    fun noReadingDistinguishesRunningFromStopped() {
        assertEquals("detenido", tunerStatusText(null, running = false, hasSelectedString = true))
        assertEquals(
            "sin lectura (el motor no publicó nada)",
            tunerStatusText(null, running = true, hasSelectedString = true),
        )
    }

    /** Bug que atrapa: el "sin reacción" ilegible — el motor mide y la pantalla no lo dice. */
    @Test
    fun measuringShowsTheHzTheEngineMeasures() {
        assertEquals(
            "midiendo · 440.0 Hz · sin enganche",
            tunerStatusText(TunerReading(a4(), snapshot(cents = null, hz = 440f)), running = true, hasSelectedString = true),
        )
    }

    /** Bug que atrapa: un "0.0 Hz" inventado cuando el motor no encontró nota. */
    @Test
    fun measuringWithoutHzKeepsTheOldText() {
        assertEquals(
            "midiendo · sin enganche",
            tunerStatusText(TunerReading(a4(), snapshot(cents = null, hz = null)), running = true, hasSelectedString = true),
        )
    }

    /** Bug que atrapa: sin cuerda el motor nunca engancha y el panel no lo dice. */
    @Test
    fun measuringWithoutStringSaysSoWithAndWithoutHz() {
        assertEquals(
            "midiendo · 440.0 Hz · sin enganche · sin cuerda elegida: tocá una cuerda abajo",
            tunerStatusText(TunerReading(null, snapshot(cents = null, hz = 440f)), running = true, hasSelectedString = false),
        )
        assertEquals(
            "midiendo · sin enganche · sin cuerda elegida: tocá una cuerda abajo",
            tunerStatusText(TunerReading(null, snapshot(cents = null, hz = null)), running = true, hasSelectedString = false),
        )
    }

    /** Bug que atrapa: tocar el texto del estado enganchado (el smoke lo reproduce). */
    @Test
    fun lockedTextIsUnchanged() {
        val target = a4()
        assertEquals(
            "1.2 cents · objetivo ${target.note.name}",
            tunerStatusText(
                TunerReading(target, snapshot(cents = 1.25f, hz = 440f)),
                running = true,
                hasSelectedString = true,
            ),
        )
        assertEquals(
            "1.2 cents · sin objetivo",
            tunerStatusText(
                TunerReading(null, snapshot(cents = 1.25f, hz = 440f)),
                running = true,
                hasSelectedString = false,
            ),
        )
    }

    private fun a4(): StringTarget = TunerPhase.JUDGMENT.configuration.targetForString(1)!!

    private fun snapshot(cents: Float?, hz: Float?, state: TunerState = TunerState.MEASURING) = TunerSnapshot(
        captureSampleRate = 48000, levelRms = 0.01f, framesAnalyzed = 4800, droppedFrames = 0, state = state,
        cents = cents, phaseAngle = null, uncertainty = null, detectedHz = hz, detectionClarity = 0.9f,
        inharmonicityB = null, lockedString = null, fastModeState = 0, usableRangeCents = null,
        inputDiscontinuity = false, discontinuityCount = 0, spectralSupport = null,
    )
}
