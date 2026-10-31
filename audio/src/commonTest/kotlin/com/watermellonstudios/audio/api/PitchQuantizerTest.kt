package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.internal.util.ScaleQuantizer
import kotlin.test.AfterTest
import kotlin.test.BeforeTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotEquals
import kotlin.test.assertTrue

/**
 * REQ-055 S15 — [PitchQuantizer]: una instancia por trabajo de re-render, con el MISMO algoritmo que
 * el pad en vivo (`ScaleQuantizer`, que ahora delega en una instancia por defecto).
 */
class PitchQuantizerTest {

    private val major = listOf(0, 2, 4, 5, 7, 9, 11)
    private val pentatonic = listOf(0, 3, 5, 7, 10)
    private val free = emptyList<Int>()

    /** Una configuración completa: lo que el pad en vivo le fija al quantizer. */
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

    private val quarterToneId = 12

    private val configs = listOf(
        Config("mayor raíz C 3 octavas", major, 0, 0, 3, false),
        Config("pentatónica raíz G 1 octava", pentatonic, 0, 7, 1, false),
        Config("mayor raíz A 5 octavas", major, 0, 9, 5, false),
        Config("mayor con rango de engine angosto", major, 0, 2, 4, false, engineMin = 110f, engineMax = 880f),
        Config("cuarto de tono", major, quarterToneId, 0, 3, false),
        Config("modo libre", free, 0, 4, 2, false),
        Config("equal spacing mayor", major, 0, 5, 3, true),
        Config("equal spacing pentatónica raíz 11", pentatonic, 0, 11, 2, true),
    )

    private fun PitchQuantizer.configure(c: Config) {
        setScaleModeId(c.scaleModeId)
        setRootNote(c.root)
        setOctaveRange(c.octaves)
        setEngineFrequencyRange(c.engineMin, c.engineMax)
        equalNoteSpacing = c.equalSpacing
        resetHysteresis()
    }

    private fun configureGlobal(c: Config) {
        ScaleQuantizer.setScaleModeId(c.scaleModeId)
        ScaleQuantizer.setRootNote(c.root)
        ScaleQuantizer.setOctaveRange(c.octaves)
        ScaleQuantizer.setEngineFrequencyRange(c.engineMin, c.engineMax)
        ScaleQuantizer.equalNoteSpacing = c.equalSpacing
        ScaleQuantizer.resetHysteresis()
    }

    /** Ida y vuelta: la vuelta es la que ejerce la histéresis (cada nota se alcanza desde los dos lados). */
    private fun sweep(): List<Float> {
        val steps = 600
        val up = List(steps + 1) { it / steps.toFloat() }
        // jitter alrededor de cada punto: ejerce la zona de histéresis sin cambiar de nota
        val jitter = up.map { (it + if ((it * steps).toInt() % 2 == 0) 0.0007f else -0.0007f).coerceIn(0f, 1f) }
        return up + up.reversed() + jitter
    }

    @BeforeTest
    @AfterTest
    fun resetGlobal() {
        configureGlobal(Config("defaults", major, 0, 9, 3, false))
    }

    // Bug plausible: la extracción cambió el orden de operaciones en float o perdió un estado: el
    // re-render cantaría otras notas que el pad en vivo (la regla es una, no dos copias que derivan).
    @Test
    fun `AC-055_15 una instancia da la misma secuencia que ScaleQuantizer en todas las configuraciones`() {
        for (c in configs) {
            val q = PitchQuantizer().also { it.configure(c) }
            configureGlobal(c)
            var changes = 0
            for ((i, x) in sweep().withIndex()) {
                val expected = ScaleQuantizer.quantizeFrequencyWithResult(x, c.intervals)
                val actual = q.quantizeFrequencyWithResult(x, c.intervals)
                val at = "${c.name} paso $i x=$x"
                assertEquals(expected.frequency.toRawBits(), actual.frequency.toRawBits(), "frecuencia, $at")
                assertEquals(expected.changed, actual.changed, "changed, $at")
                assertEquals(ScaleQuantizer.currentMidiNote, q.currentMidiNote, "currentMidiNote, $at")
                assertEquals(ScaleQuantizer.isQuarterTone, q.isQuarterTone, "isQuarterTone, $at")
                if (actual.changed) changes++
            }
            assertTrue(changes > 0, "${c.name}: el barrido nunca cambió de nota, no ejerció nada")
        }
    }

    // Bug plausible: las funciones sin estado leen otro estado que las con estado (rango, raíz) o la
    // escala generada difiere: el desplegable de notas del pad no coincidiría con lo que suena.
    @Test
    fun `AC-055_15 las funciones sin estado y la escala generada coinciden con ScaleQuantizer`() {
        for (c in configs) {
            val q = PitchQuantizer().also { it.configure(c) }
            configureGlobal(c)
            for (i in 0..200) {
                val x = i / 200f
                assertEquals(
                    ScaleQuantizer.quantizeFrequencyStateless(x, c.intervals).toRawBits(),
                    q.quantizeFrequencyStateless(x, c.intervals).toRawBits(),
                    "${c.name} stateless x=$x",
                )
                assertEquals(
                    ScaleQuantizer.equalSpacingMapStateless(x, c.intervals).toRawBits(),
                    q.equalSpacingMapStateless(x, c.intervals).toRawBits(),
                    "${c.name} equalSpacingMapStateless x=$x",
                )
            }
            assertEquals(ScaleQuantizer.generateScaleNotes(c.intervals), q.generateScaleNotes(c.intervals), c.name)
            for (hz in listOf(55f, 261.63f, 440f, 1760f)) {
                assertEquals(ScaleQuantizer.getNoteName(hz), q.getNoteName(hz), "$hz Hz")
            }
        }
        assertEquals("A4", PitchQuantizer().getNoteName(440f))
    }

    // Bug plausible: el estado quedó en un companion/object compartido: dos trabajos de re-render a la
    // vez (o un re-render con el pad sonando) se pisan la histéresis y la configuración.
    @Test
    fun `AC-055_15 dos instancias no comparten histeresis ni configuracion`() {
        val a = PitchQuantizer().also { it.configure(configs[0]) }
        val b = PitchQuantizer().also { it.configure(configs[1]) }
        assertEquals(-1, a.currentMidiNote)
        assertEquals(-1, b.currentMidiNote)

        val fa = a.quantizeFrequencyWithResult(0.5f, major)
        assertEquals(-1, b.currentMidiNote, "b no se enteró de a")
        val fb = b.quantizeFrequencyWithResult(0.5f, major)
        assertNotEquals(fa.frequency.toRawBits(), fb.frequency.toRawBits(), "raíz y rango distintos dan otra nota")

        // misma entrada en la zona de histéresis: a la conserva, b recién resetea y la reevalúa
        b.resetHysteresis()
        val stay = 0.5f + 0.0002f
        assertEquals(false, a.quantizeFrequencyWithResult(stay, major).changed, "a conserva su nota")
        assertEquals(true, b.quantizeFrequencyWithResult(stay, major).changed, "resetear b no resetea a, y a la inversa")
        a.setRootNote(5)
        assertEquals(
            PitchQuantizer().also { it.configure(configs[1]) }.quantizeFrequencyStateless(0.3f, major).toRawBits(),
            b.quantizeFrequencyStateless(0.3f, major).toRawBits(),
            "cambiar la raíz de a no movió a b",
        )
    }

    // Bug plausible: la instancia escribe en el ScaleQuantizer global (flow, histéresis o rango): un
    // re-render en segundo plano movería las notas del pad que el usuario está tocando.
    @Test
    fun `AC-055_15 usar una instancia no mueve el ScaleQuantizer global`() {
        configureGlobal(configs[0])
        val twin = PitchQuantizer().also { it.configure(configs[0]) }
        for (x in listOf(0.2f, 0.45f, 0.61f)) {
            ScaleQuantizer.quantizeFrequencyWithResult(x, major)
            twin.quantizeFrequencyWithResult(x, major)
        }
        val midiBefore = ScaleQuantizer.currentMidiNote
        val flowBefore = ScaleQuantizer.currentMidiNoteFlow.value
        val quarterBefore = ScaleQuantizer.isQuarterTone

        // el re-render usa otra configuración entera, incluido cuarto de tono y modo libre
        val job = PitchQuantizer()
        for (c in configs.drop(1)) {
            job.configure(c)
            for (x in sweep().take(120)) job.quantizeFrequencyWithResult(x, c.intervals)
        }

        assertEquals(midiBefore, ScaleQuantizer.currentMidiNote)
        assertEquals(flowBefore, ScaleQuantizer.currentMidiNoteFlow.value)
        assertEquals(quarterBefore, ScaleQuantizer.isQuarterTone)
        // y su histéresis y su configuración siguen siendo las suyas: la próxima secuencia coincide con la del gemelo
        for (x in sweep().take(200)) {
            val g = ScaleQuantizer.quantizeFrequencyWithResult(x, major)
            val t = twin.quantizeFrequencyWithResult(x, major)
            assertEquals(t.frequency.toRawBits(), g.frequency.toRawBits(), "x=$x")
            assertEquals(t.changed, g.changed, "x=$x")
        }
    }

    // Bug plausible: la instancia nueva arranca con otro estado que el del pad (raíz, rango, modo): el
    // re-render sin configurar daría otras notas que el pad recién abierto.
    @Test
    fun `AC-055_15 una instancia nueva arranca con los defaults del pad`() {
        val q = PitchQuantizer()
        assertEquals(-1, q.currentMidiNote)
        assertEquals(false, q.isQuarterTone)
        assertEquals(false, q.equalNoteSpacing)
        // los defaults del singleton recién reseteado son raíz A, 3 octavas, rango 20..2000, modo libre
        ScaleQuantizer.setScaleModeId(0)
        ScaleQuantizer.setRootNote(9)
        ScaleQuantizer.setOctaveRange(3)
        ScaleQuantizer.setEngineFrequencyRange(20f, 2000f)
        for (i in 0..50) {
            val x = i / 50f
            assertEquals(
                ScaleQuantizer.quantizeFrequencyStateless(x, major).toRawBits(),
                q.quantizeFrequencyStateless(x, major).toRawBits(),
                "x=$x",
            )
        }
    }
}
