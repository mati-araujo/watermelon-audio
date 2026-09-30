package com.watermellonstudios.audio.harness.soundfont

import com.watermellonstudios.audio.harness.smoke.SmokeReporter
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * MINI-038, AC-3 — los veredictos del SoundFont. El harness NO puede decir "cargó" o "sonó" por algo
 * que no midió: cada test nombra el reporte falso que atrapa.
 */
class SoundFontCheckTest {

    private class FakePort : SoundFontPort {
        var loadResult = true
        var presets = 1
        var loaded = false
        var engineStarts = true
        var currentType = 0
        val typesSet = mutableListOf<Int>()
        var noteOffs = 0
        var unloads = 0
        var peakBefore = 0f
        var peakDuring = 0.3f
        var frame = 0L
        var framesPerRead = 10_000L
        var noteIsOn = false

        override fun load(path: String): Boolean {
            loaded = loadResult
            return loadResult
        }
        override fun isLoaded() = loaded
        override fun unload() { unloads++; loaded = false }
        override fun presetCount() = if (loaded) presets else 0
        override fun presetName(index: Int) = if (index < presetCount()) "WMA fixture" else null
        override fun bankProgram(index: Int) = if (index < presetCount()) intArrayOf(0, 0) else null
        override fun setPreset(index: Int) = Unit
        override suspend fun ensureEngineRunning() = engineStarts
        override fun engineType() = currentType
        override fun setEngineType(type: Int) { typesSet += type; currentType = type }
        override fun noteOn(midiNote: Int, velocity: Float) { noteIsOn = true }
        override fun noteOff() { noteOffs++; noteIsOn = false }
        override fun outputPeak() = if (noteIsOn) peakDuring else peakBefore
        override fun playFrame(): Long { frame += framesPerRead; return frame }
    }

    private val lines = mutableListOf<String>()
    private val reporter = SmokeReporter({ lines += it }, run = "t")
    private fun check(port: SoundFontPort) = SoundFontCheck(port, pause = {})
    private fun line(step: String) = lines.single { " step=$step " in it }
    private fun field(step: String, key: String) =
        line(step).split(' ').single { it.startsWith("$key=") }.substringAfter('=')

    /** Bug que atrapa: reportar "cargó" con un `true` que dejó CERO presets (nada que tocar). */
    @Test
    fun aLoadWithZeroPresetsIsNotALoad() {
        val port = FakePort().apply { presets = 0 }
        assertFalse(check(port).load(reporter, "sf2", "/x.sf2", "x.sf2"))
        assertTrue(line("carga").contains("ok=false") && line("carga").contains("presets=0"))
        assertTrue(lines.none { " step=preset " in it })
    }

    @Test
    fun aFailedLoadIsReportedAsFailed() {
        val port = FakePort().apply { loadResult = false }
        assertFalse(check(port).load(reporter, "sf3", "/x.sf3", "x.sf3"))
        assertEquals("false", field("carga", "ok"))
        assertEquals("false", field("carga", "cargado"))
    }

    @Test
    fun aGoodLoadReportsThePresetWithBankAndProgram() {
        assertTrue(check(FakePort()).load(reporter, "sf2", "/x.sf2", "x.sf2"))
        assertTrue(line("carga").contains("ok=true"))
        assertTrue(line("preset").contains("ok=true indice=0 nombre=WMA_fixture bank=0 program=0"))
    }

    @Test
    fun aNoteThatSoundsPassesAndLeavesTheEngineAsItFoundIt() = runTest {
        val port = FakePort().apply { loaded = true; currentType = 3 }
        assertTrue(check(port).playNote(reporter, "sf2"))
        assertTrue(line("nota").contains("ok=true"))
        assertEquals(listOf(SoundFontCheck.ENGINE_SOUNDFONT, 3), port.typesSet)
        assertEquals(1, port.noteOffs)
    }

    /** Bug que atrapa: dar "sonó" por un pico viejo con el render parado (los frames no se mueven). */
    @Test
    fun aNoteWithFramesStandingStillFails() = runTest {
        val port = FakePort().apply { loaded = true; framesPerRead = 0 }
        assertFalse(check(port).playNote(reporter, "sf2"))
        assertTrue(line("nota").contains("motivo=frames-quietos"))
        assertEquals(1, port.noteOffs)
    }

    /** Bug que atrapa: dar "sonó" porque el render avanza, aunque avance en silencio. */
    @Test
    fun aSilentNoteFails() = runTest {
        val port = FakePort().apply { loaded = true; peakDuring = 0.001f }
        assertFalse(check(port).playNote(reporter, "sf3"))
        assertTrue(line("nota").contains("motivo=sin-senal"))
    }

    /** Bug que atrapa: atribuirle a la nota una señal que ya estaba antes de tocarla. */
    @Test
    fun aNoisyControlMakesTheNoteUnjudgeable() = runTest {
        val port = FakePort().apply { loaded = true; peakBefore = 0.2f }
        assertFalse(check(port).playNote(reporter, "sf2"))
        assertTrue(line("nota").contains("motivo=control-no-silencioso"))
    }

    @Test
    fun noEngineNoNoteAndTheEngineTypeIsNotTouched() = runTest {
        val port = FakePort().apply { loaded = true; engineStarts = false }
        assertFalse(check(port).playNote(reporter, "sf2"))
        assertTrue(line("nota").contains("motivo=motor-no-arranca"))
        assertTrue(port.typesSet.isEmpty())
    }

    @Test
    fun noSoundFontNoNote() = runTest {
        assertFalse(check(FakePort()).playNote(reporter, "sf2"))
        assertTrue(line("nota").contains("motivo=sin-soundfont"))
    }

    /** Bug que atrapa (AC-3): un archivo que no es SoundFont "cargado" se reporta como rechazo. */
    @Test
    fun aNonSoundFontThatLoadsIsAFailureAndGetsUnloaded() {
        val port = FakePort().apply { loadResult = true }
        assertFalse(check(port).loadRejects(reporter, "sf2", "/junk", "junk"))
        assertEquals("false", field("no-soundfont", "ok"))
        assertEquals("true", field("no-soundfont", "cargado"))
        assertEquals(1, port.unloads)
    }

    @Test
    fun aNonSoundFontThatIsRejectedPasses() {
        val port = FakePort().apply { loadResult = false }
        assertTrue(check(port).loadRejects(reporter, "sf2", "/junk", "junk"))
        assertEquals("true", field("no-soundfont", "ok"))
        assertEquals("false", field("no-soundfont", "cargado"))
    }

    /** Bug que atrapa: una nota que falla deja el font cargado y la corrida siguiente mide sobre él. */
    @Test
    fun theFixtureIsUnloadedEvenWhenTheNoteFails() = runTest {
        val port = FakePort().apply { peakDuring = 0f }
        assertFalse(check(port).runFixture(reporter, "sf2", "/x.sf2", "x.sf2"))
        assertEquals(listOf("carga", "preset", "nota", "descarga"), lines.map { it.substringAfter(" step=").substringBefore(' ') })
        assertTrue(line("descarga").contains("ok=true"))
        assertFalse(port.loaded)
    }

    @Test
    fun peakIsTheLargestMagnitudeWithinTheReadCount() {
        assertEquals(0.5f, SoundFontCheck.peak(floatArrayOf(0.1f, -0.5f, 0.3f, 0.9f), 3))
        assertEquals(0f, SoundFontCheck.peak(floatArrayOf(0.4f), 0))
    }
}
