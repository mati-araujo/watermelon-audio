package com.watermellonstudios.audio.internal.util

import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.runTest
import kotlin.test.AfterTest
import kotlin.test.BeforeTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/**
 * AUD-3 coverage: [ScaleQuantizer.currentMidiNoteFlow] must emit only when the
 * quantized MIDI note actually changes, not on every X update inside the same
 * note/hysteresis bucket.
 */
class ScaleQuantizerFlowTest {

    private val majorIntervals = listOf(0, 2, 4, 5, 7, 9, 11)

    @BeforeTest
    fun setup() {
        // ScaleQuantizer is a process-wide singleton; reset before each test
        // so prior test state can't leak into hysteresis or the StateFlow.
        ScaleQuantizer.setScaleModeId(0) // FREE/scale mode (not quarter-tone)
        ScaleQuantizer.setRootNote(0)    // C
        ScaleQuantizer.setOctaveRange(3)
        ScaleQuantizer.setEngineFrequencyRange(20f, 2000f)
        ScaleQuantizer.equalNoteSpacing = false
        ScaleQuantizer.resetHysteresis()
        // Drain the flow back to a known starting value by writing into it
        // through a free-mode call we'll overwrite shortly.
    }

    @AfterTest
    fun teardown() {
        ScaleQuantizer.resetHysteresis()
    }

    @Test
    fun flow_value_matches_currentMidiNote_after_quantization() = runTest {
        ScaleQuantizer.quantizeFrequency(0.5f, majorIntervals)
        assertEquals(ScaleQuantizer.currentMidiNote, ScaleQuantizer.currentMidiNoteFlow.value)
        assertTrue(ScaleQuantizer.currentMidiNoteFlow.value >= 0)
    }

    @Test
    fun flow_does_not_re_emit_for_same_quantized_note() = runTest {
        // Establish a baseline note.
        ScaleQuantizer.quantizeFrequency(0.5f, majorIntervals)
        val baseline = ScaleQuantizer.currentMidiNoteFlow.value

        // Many micro-jitter X updates inside the hysteresis zone — quantized
        // note must not change, so the StateFlow must stay at `baseline`.
        repeat(60) { i ->
            val x = 0.5f + (i % 3 - 1) * 0.0005f
            ScaleQuantizer.quantizeFrequency(x, majorIntervals)
        }

        assertEquals(baseline, ScaleQuantizer.currentMidiNoteFlow.value)
    }

    @Test
    fun flow_emits_when_note_changes() = runTest {
        ScaleQuantizer.quantizeFrequency(0.1f, majorIntervals)
        val low = ScaleQuantizer.currentMidiNoteFlow.value

        ScaleQuantizer.quantizeFrequency(0.9f, majorIntervals)
        val high = ScaleQuantizer.currentMidiNoteFlow.value

        assertTrue(high > low, "high X must quantize above low X (got low=$low, high=$high)")
        // first { it == high } would deadlock if the flow never published — by
        // the time we get here .value is already `high`, confirming emission.
        assertEquals(high, ScaleQuantizer.currentMidiNoteFlow.first())
    }
}
