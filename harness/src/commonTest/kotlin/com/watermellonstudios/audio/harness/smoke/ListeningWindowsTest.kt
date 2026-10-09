package com.watermellonstudios.audio.harness.smoke

import com.watermellonstudios.audio.domain.AudioBackendType
import com.watermellonstudios.audio.harness.soundfont.SoundFontCheck
import com.watermellonstudios.audio.harness.soundfont.SoundFontPort
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * REQ-053 S3 (AC-053.8, AC-053.10) — las ventanas de escucha: el estímulo declarado y sus controles.
 *
 * Fija el formato que lee `scripts/smoke-device.sh`: `step=escuchar` (el aviso, CIEGO: no dice si la
 * ventana es estímulo o control), y al cerrar `step=estimulo` / `step=control` con lo que el motor
 * rindió en la ventana. Cada test nombra el bug que atrapa.
 */
class ListeningWindowsTest {

    private class FakePort : SoundFontPort {
        var loaded = true
        var engineStarts = true
        var currentType = 3
        val typesSet = mutableListOf<Int>()
        val presetsSet = mutableListOf<Int>()
        var noteOns = 0
        var noteOffs = 0
        var noteIsOn = false
        var peakSilent = 0f
        var peakNote = 0.3f
        var frame = 0L
        var framesPerRead = 4_800L
        var backendValue = AudioBackendType.OBOE

        override fun load(path: String) = true
        override fun isLoaded() = loaded
        override fun unload() { loaded = false }
        override fun presetCount() = 1
        override fun presetName(index: Int) = "WMA fixture"
        override fun bankProgram(index: Int) = intArrayOf(0, 0)
        override fun setPreset(index: Int) { presetsSet += index }
        override suspend fun ensureEngineRunning() = engineStarts
        override fun engineType() = currentType
        override fun setEngineType(type: Int) { typesSet += type; currentType = type }
        override fun noteOn(midiNote: Int, velocity: Float) { noteOns++; noteIsOn = true }
        override fun noteOff() { noteOffs++; noteIsOn = false }
        override fun outputPeak() = if (noteIsOn) peakNote else peakSilent
        override fun playFrame(): Long { frame += framesPerRead; return frame }
        override fun backend() = backendValue
    }

    /** Líneas y pausas, en el orden en que pasaron. */
    private val events = mutableListOf<String>()
    private val lines get() = events.filter { it.startsWith("HARNESS-SMOKE ") }
    private val reporter = SmokeReporter({ events += it }, run = "t")

    private fun windows(port: SoundFontPort) = ListeningWindows(port, pause = { events += "pausa=$it" })

    private fun step(line: String) = line.substringAfter(" step=").substringBefore(' ')
    private fun field(line: String, key: String): String? =
        line.split(' ').firstOrNull { it.startsWith("$key=") }?.substringAfter('=')
    private fun single(step: String) = lines.single { step(it) == step }

    private val stimulusOnly = listOf(WindowKind.STIMULUS)
    private val controlOnly = listOf(WindowKind.CONTROL)

    /**
     * Bug que atrapa: un `step=estimulo` sin alguno de los datos que AC-053.8 pide (qué sonó, por
     * dónde, cuánto, lo que rindió el motor) — el juez y el sensor no tendrían qué cruzar.
     */
    @Test
    fun aStimulusWindowSaysWhatSoundedWhereForHowLongAndWhatTheEngineRendered() = runTest {
        assertTrue(windows(FakePort()).run(reporter, "sf2", Route.SYSTEM, stimulusOnly, "wma-fixture.sf2"))
        val l = single("estimulo")
        assertEquals("true", field(l, "ok"))
        assertEquals("1", field(l, "n"))
        assertEquals("1", field(l, "de"))
        assertEquals("A4", field(l, "sono"))
        assertEquals("440", field(l, "hz"))
        assertEquals("69", field(l, "nota"))
        assertEquals("wma-fixture.sf2", field(l, "archivo"))
        assertEquals("0", field(l, "preset"))
        assertEquals("sistema", field(l, "ruta"))
        assertEquals("OBOE", field(l, "backend"))
        assertEquals(ListeningWindows.WINDOW_MS.toString(), field(l, "ventana-ms"))
        assertTrue((field(l, "frames")?.toLong() ?: 0) > 0)
        assertEquals("0.3000", field(l, "pico"))
        assertEquals(ListeningWindows.ANSWER_PAUSE_MS.toString(), field(l, "pausa-ms"))
        assertEquals(null, field(l, "motivo"))
    }

    /** Bug que atrapa (AC-053.8): dar el estímulo por rendido con el render parado (pico viejo). */
    @Test
    fun aStimulusWhoseFramesStoodStillWasNotRendered() = runTest {
        val port = FakePort().apply { framesPerRead = 0 }
        assertFalse(windows(port).run(reporter, "sf2", Route.SYSTEM, stimulusOnly, "f.sf2"))
        assertEquals("false", field(single("estimulo"), "ok"))
        assertEquals("0", field(single("estimulo"), "frames"))
        assertEquals("frames-quietos", field(single("estimulo"), "motivo"))
    }

    /** Bug que atrapa (AC-053.8): dar el estímulo por rendido porque el render avanza en silencio. */
    @Test
    fun aStimulusWithoutSignalWasNotRendered() = runTest {
        val port = FakePort().apply { peakNote = 0f }
        assertFalse(windows(port).run(reporter, "sf3", Route.SYSTEM, stimulusOnly, "f.sf3"))
        assertEquals("false", field(single("estimulo"), "ok"))
        assertEquals("0.0000", field(single("estimulo"), "pico"))
        assertEquals("sin-senal", field(single("estimulo"), "motivo"))
    }

    /** Bug que atrapa (AC-053.8, "por la ruta declarada"): el A4 de USB que en realidad sale por Oboe. */
    @Test
    fun aStimulusOverAnotherRouteThanTheDeclaredOneIsNotTheStimulus() = runTest {
        val port = FakePort().apply { backendValue = AudioBackendType.OBOE }
        assertFalse(windows(port).run(reporter, "usb", Route.LIBUSB, stimulusOnly, "f.sf2"))
        assertEquals("ruta-equivocada", field(single("estimulo"), "motivo"))
        events.clear()
        port.backendValue = AudioBackendType.LIBUSB
        assertFalse(windows(port).run(reporter, "sf2", Route.SYSTEM, stimulusOnly, "f.sf2"))
        assertEquals("ruta-equivocada", field(single("estimulo"), "motivo"))
        events.clear()
        assertTrue(windows(port).run(reporter, "usb", Route.LIBUSB, stimulusOnly, "f.sf2"))
        assertEquals("LIBUSB", field(single("estimulo"), "backend"))
    }

    /** Bug que atrapa (AC-053.10): un control que toca la nota no es un control. */
    @Test
    fun aControlWindowIsSilenceRenderedByTheEngineAndSaysSo() = runTest {
        val port = FakePort()
        assertTrue(windows(port).run(reporter, "sf2", Route.SYSTEM, controlOnly, "f.sf2"))
        val l = single("control")
        assertEquals("true", field(l, "ok"))
        assertEquals("silencio", field(l, "tipo"))
        assertEquals("sistema", field(l, "ruta"))
        assertTrue((field(l, "frames")?.toLong() ?: 0) > 0)
        assertEquals("0.0000", field(l, "pico"))
        assertEquals(null, field(l, "sono"))
        assertEquals(0, port.noteOns)
    }

    /** Bug que atrapa: un "silencio" que suena (cola, ruido) haría pasar a un sensor que dice "sí". */
    @Test
    fun aControlThatSoundsIsNotAValidControl() = runTest {
        val port = FakePort().apply { peakSilent = 0.2f }
        assertFalse(windows(port).run(reporter, "sf2", Route.SYSTEM, controlOnly, "f.sf2"))
        assertEquals("false", field(single("control"), "ok"))
        assertEquals("el-control-sono", field(single("control"), "motivo"))
    }

    /** Bug que atrapa: un control con el render parado se da por silencio "medido". */
    @Test
    fun aControlWhoseFramesStoodStillIsNotAValidControl() = runTest {
        val port = FakePort().apply { framesPerRead = 0 }
        assertFalse(windows(port).run(reporter, "sf2", Route.SYSTEM, controlOnly, "f.sf2"))
        assertEquals("frames-quietos", field(single("control"), "motivo"))
    }

    /**
     * Bug que atrapa (D7, controles CIEGOS): un aviso que dice qué tipo de ventana viene. El aviso de
     * un estímulo y el de un control son la MISMA línea salvo el número.
     */
    @Test
    fun theAnnouncementIsBlindToTheKindOfWindow() = runTest {
        windows(FakePort()).run(reporter, "sf2", Route.SYSTEM, listOf(WindowKind.STIMULUS, WindowKind.CONTROL), "f.sf2")
        val announcements = lines.filter { step(it) == "escuchar" }
        assertEquals(2, announcements.size)
        val withoutN = announcements.map { l -> l.split(' ').filterNot { it.startsWith("n=") }.joinToString(" ") }
        assertEquals(withoutN[0], withoutN[1])
        val a = announcements[0]
        assertEquals("A4-440Hz", field(a, "estimulo"))
        assertEquals("sistema", field(a, "ruta"))
        assertEquals("2", field(a, "de"))
        assertEquals(ListeningWindows.PRE_ROLL_MS.toString(), field(a, "en-ms"))
        assertEquals(ListeningWindows.WINDOW_MS.toString(), field(a, "ventana-ms"))
        assertEquals(null, field(a, "tipo"))
        assertEquals(null, field(a, "sono"))
    }

    /** Bug que atrapa (AC-053.10): las ventanas en otro orden que el pedido, o mal numeradas. */
    @Test
    fun theWindowsFollowTheGivenOrderEachAfterItsAnnouncement() = runTest {
        windows(FakePort()).run(reporter, "sf3", Route.SYSTEM, listOf(WindowKind.CONTROL, WindowKind.STIMULUS), "f.sf3")
        assertEquals(
            listOf("escuchar" to "1", "control" to "1", "escuchar" to "2", "estimulo" to "2"),
            lines.map { step(it) to field(it, "n") },
        )
    }

    /**
     * Bug que atrapa: lo que la línea DECLARA (`en-ms`, `ventana-ms`, `pausa-ms`) no es lo que pasó. El
     * script decide cuánto espera la respuesta humana con `pausa-ms`.
     */
    @Test
    fun thePausesAreTheDeclaredOnes() = runTest {
        windows(FakePort()).run(reporter, "sf2", Route.SYSTEM, stimulusOnly, "f.sf2")
        val listen = events.indexOfFirst { it.startsWith("HARNESS-SMOKE ") && step(it) == "escuchar" }
        val close = events.indexOfFirst { it.startsWith("HARNESS-SMOKE ") && step(it) == "estimulo" }
        assertEquals("pausa=${ListeningWindows.PRE_ROLL_MS}", events[listen + 1])
        val inside = events.subList(listen + 2, close).map { it.removePrefix("pausa=").toLong() }
        assertEquals(ListeningWindows.WINDOW_MS, inside.sum())
        assertEquals("pausa=${ListeningWindows.ANSWER_PAUSE_MS}", events[close + 1])
    }

    /**
     * Bug que atrapa (D7, medido en el g42 el 2026-10-09): el primer aviso pegado a lo que sonó antes
     * del panel (la `nota` de sf2/sf3, la suite de usb). El oyente oye un A4 junto al aviso y, si la
     * ventana 1 es un control, contesta "presente" sobre un silencio que el motor midió en 0.
     */
    @Test
    fun theFirstNoticeComesAfterASilentLeadIn() = runTest {
        val port = FakePort()
        windows(port).run(reporter, "sf3", Route.SYSTEM, listOf(WindowKind.CONTROL, WindowKind.STIMULUS), "f.sf3")
        val listen = events.indexOfFirst { it.startsWith("HARNESS-SMOKE ") && step(it) == "escuchar" }
        assertEquals(listOf("pausa=${ListeningWindows.LEAD_IN_MS}"), events.subList(0, listen))
        assertTrue(ListeningWindows.LEAD_IN_MS >= 3000L)
    }

    /** Bug que atrapa: la nota suena fuera de su ventana, queda colgada o deja el motor en SOUNDFONT. */
    @Test
    fun theNoteSoundsOnlyInTheStimulusWindowAndTheEngineIsLeftAsItWas() = runTest {
        val port = FakePort().apply { currentType = 3 }
        windows(port).run(reporter, "sf2", Route.SYSTEM, listOf(WindowKind.CONTROL, WindowKind.STIMULUS), "f.sf2")
        assertEquals(1, port.noteOns)
        assertFalse(port.noteIsOn)
        assertEquals(listOf(SoundFontCheck.ENGINE_SOUNDFONT, 3), port.typesSet)
        assertEquals(listOf(0), port.presetsSet)
    }

    /**
     * Bug que atrapa (AC-053.8): sin motor no hay estímulo, y una ventana sin aviso no se puede
     * escuchar — no se anuncia, y cada ventana pedida sale `ok=false` con su motivo.
     */
    @Test
    fun withoutTheEngineNoWindowIsAnnouncedAndEachOneFails() = runTest {
        val port = FakePort().apply { engineStarts = false }
        assertFalse(windows(port).run(reporter, "usb", Route.LIBUSB, listOf(WindowKind.CONTROL, WindowKind.STIMULUS), "f.sf2"))
        assertEquals(listOf("control", "estimulo"), lines.map { step(it) })
        assertTrue(lines.all { field(it, "ok") == "false" && field(it, "motivo") == "motor-no-arranca" && field(it, "frames") == "0" })
        assertEquals(0, port.noteOns)
    }

    @Test
    fun withoutTheSoundFontNoWindowIsAnnouncedAndEachOneFails() = runTest {
        val port = FakePort().apply { loaded = false }
        assertFalse(windows(port).run(reporter, "usb", Route.LIBUSB, listOf(WindowKind.STIMULUS, WindowKind.CONTROL), "f.sf2"))
        assertEquals(listOf("estimulo", "control"), lines.map { step(it) })
        assertTrue(lines.all { field(it, "motivo") == "sin-soundfont" })
    }

    /**
     * Bug que atrapa (AC-053.10): el orden no sale de la semilla, o sale distinto en el script. Estos
     * vectores son LOS MISMOS que fija `smoke-device.sh --self-test` sobre su implementación: si una
     * de las dos cambia, su test se pone rojo.
     */
    @Test
    fun theOrderComesFromTheSeedWithTheSharedVectors() {
        val e = WindowKind.STIMULUS
        val c = WindowKind.CONTROL
        assertEquals(listOf(e, c), WindowOrder.of(0, "sf2"))
        assertEquals(listOf(c, e), WindowOrder.of(1, "sf2"))
        assertEquals(listOf(e, c), WindowOrder.of(42, "usb"))
        assertEquals(listOf(c, e), WindowOrder.of(12345, "sf3"))
        assertEquals(listOf(c, e), WindowOrder.of(2147483647, "sf2"))
        assertEquals(listOf(e, c), WindowOrder.of(123456789, "usb"))
    }

    /** Bug que atrapa: un orden fijo (siempre el estímulo primero) que el oyente aprende. */
    @Test
    fun bothOrdersHappenAndTheSameSeedGivesTheSameOrder() {
        val orders = (0L until 64L).map { WindowOrder.of(it, "sf2") }.toSet()
        assertEquals(2, orders.size)
        assertEquals(WindowOrder.of(777, "usb"), WindowOrder.of(777, "usb"))
        assertTrue(orders.all { it.size == 2 && it.toSet() == setOf(WindowKind.STIMULUS, WindowKind.CONTROL) })
    }
}
