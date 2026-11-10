package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.internal.util.ScaleQuantizer
import kotlin.test.AfterTest
import kotlin.test.BeforeTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/**
 * REQ-055 S15 — golden de [PitchQuantizer] contra el `ScaleQuantizer` PRE-extraccion.
 *
 * Los valores de [PITCH_QUANTIZER_GOLDEN_2_27_0] salieron del commit 31451966 (master, 2.27.0), antes
 * de extraer la clase: este test la compara con el codigo VIEJO, no con ella misma. El barrido es
 * determinista (ida, vuelta y jitter; 8 configuraciones, histeresis y reset entre ellas).
 */
class PitchQuantizerGoldenTest {

    private val major = listOf(0, 2, 4, 5, 7, 9, 11)
    private val pentatonic = listOf(0, 3, 5, 7, 10)
    private val free = emptyList<Int>()

    private data class Config(
        val name: String,
        val intervals: List<Int>,
        val scaleModeId: Int,
        val root: Int,
        val octaves: Int,
        val equalSpacing: Boolean,
        val engineMin: Float = 20f,
        val engineMax: Float = 2000f,
    )

    // Mismas configuraciones, mismo orden y mismos nombres que el generador del golden.
    private val configs = listOf(
        Config("mayor raiz C 3 oct", major, 0, 0, 3, false),
        Config("pentatonica raiz G 1 oct", pentatonic, 0, 7, 1, false),
        Config("mayor raiz A 5 oct", major, 0, 9, 5, false),
        Config("mayor engine angosto", major, 0, 2, 4, false, 110f, 880f),
        Config("cuarto de tono", major, 12, 0, 3, false),
        Config("modo libre", free, 0, 4, 2, false),
        Config("equal spacing mayor", major, 0, 5, 3, true),
        Config("equal spacing pentatonica raiz 11", pentatonic, 0, 11, 2, true),
    )

    private fun sweep(): List<Float> {
        val steps = 60
        val up = List(steps + 1) { it / steps.toFloat() }
        val jitter = up.map { (it + if ((it * steps).toInt() % 2 == 0) 0.0007f else -0.0007f).coerceIn(0f, 1f) }
        return up + up.reversed() + jitter
    }

    private class Step(val bits: Int, val changed: Boolean, val midi: Int)

    private val golden: Map<String, List<Step>> by lazy {
        val out = LinkedHashMap<String, MutableList<Step>>()
        var current: MutableList<Step>? = null
        for (line in PITCH_QUANTIZER_GOLDEN_2_27_0.lines().filter { it.isNotBlank() }) {
            if (line.startsWith("CFG ")) {
                current = mutableListOf<Step>().also { out[line.removePrefix("CFG ")] = it }
            } else {
                val (bits, changed, midi) = line.split(',')
                current!!.add(Step(bits.toInt(), changed == "1", midi.toInt()))
            }
        }
        out
    }

    private fun resetGlobal() {
        ScaleQuantizer.setScaleModeId(0)
        ScaleQuantizer.setRootNote(9)
        ScaleQuantizer.setOctaveRange(3)
        ScaleQuantizer.setEngineFrequencyRange(20f, 2000f)
        ScaleQuantizer.equalNoteSpacing = false
        ScaleQuantizer.resetHysteresis()
    }

    @BeforeTest
    @AfterTest
    fun reset() = resetGlobal()

    // Bug plausible: la extraccion cambio el orden de operaciones en float, un redondeo, la
    // histeresis o el rango: PitchQuantizer o el delegado cantan otras notas que el ScaleQuantizer de
    // 2.27.0. Igualdad de bits, no de tolerancia.
    @Test
    fun `AC-055_15 PitchQuantizer y el delegado ScaleQuantizer dan los bits exactos de 2_27_0`() {
        assertEquals(configs.map { it.name }, golden.keys.toList(), "el golden cubre las configuraciones del test")
        val xs = sweep()
        for (c in configs) {
            val expected = golden.getValue(c.name)
            assertEquals(xs.size, expected.size, "${c.name}: largo del barrido")

            val q = PitchQuantizer()
            q.setScaleModeId(c.scaleModeId)
            q.setRootNote(c.root)
            q.setOctaveRange(c.octaves)
            q.setEngineFrequencyRange(c.engineMin, c.engineMax)
            q.equalNoteSpacing = c.equalSpacing
            q.resetHysteresis()

            resetGlobal()
            ScaleQuantizer.setScaleModeId(c.scaleModeId)
            ScaleQuantizer.setRootNote(c.root)
            ScaleQuantizer.setOctaveRange(c.octaves)
            ScaleQuantizer.setEngineFrequencyRange(c.engineMin, c.engineMax)
            ScaleQuantizer.equalNoteSpacing = c.equalSpacing
            ScaleQuantizer.resetHysteresis()

            var changes = 0
            for ((i, x) in xs.withIndex()) {
                val e = expected[i]
                val at = "${c.name} paso $i x=$x"
                val r = q.quantizeFrequencyWithResult(x, c.intervals)
                assertEquals(e.bits, r.frequency.toRawBits(), "PitchQuantizer frecuencia, $at")
                assertEquals(e.changed, r.changed, "PitchQuantizer changed, $at")
                assertEquals(e.midi, q.currentMidiNote, "PitchQuantizer currentMidiNote, $at")

                val d = ScaleQuantizer.quantizeFrequencyWithResult(x, c.intervals)
                assertEquals(e.bits, d.frequency.toRawBits(), "ScaleQuantizer frecuencia, $at")
                assertEquals(e.changed, d.changed, "ScaleQuantizer changed, $at")
                assertEquals(e.midi, ScaleQuantizer.currentMidiNote, "ScaleQuantizer currentMidiNote, $at")
                if (e.changed) changes++
            }
            assertTrue(changes > 0, "${c.name}: el golden nunca cambio de nota, no ejerce nada")
        }
    }
}
