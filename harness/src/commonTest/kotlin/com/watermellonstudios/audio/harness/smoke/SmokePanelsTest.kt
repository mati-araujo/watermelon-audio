package com.watermellonstudios.audio.harness.smoke

import com.watermellonstudios.audio.domain.AudioBackendType
import com.watermellonstudios.audio.harness.soundfont.SoundFontPort
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * REQ-053 S3 — el CABLEADO de las ventanas en el plan (review de S3): qué semilla, qué ruta y qué
 * panel recibe cada una, y el orden del panel USB (cargar, parar el motor, la plataforma con las
 * ventanas por libusb, parar el motor otra vez, descargar). Sin esto, `ListeningWindowsTest` prueba la
 * clase y nadie prueba que el plan la use bien: los errores sólo se verían en un teléfono.
 */
class SmokePanelsTest {

    private class Port(var backendValue: AudioBackendType) : SoundFontPort {
        var noteIsOn = false
        var frame = 0L
        override fun load(path: String) = true
        override fun isLoaded() = true
        override fun unload() {}
        override fun presetCount() = 1
        override fun presetName(index: Int) = "p"
        override fun bankProgram(index: Int) = intArrayOf(0, 0)
        override fun setPreset(index: Int) {}
        override suspend fun ensureEngineRunning() = true
        override fun engineType() = 0
        override fun setEngineType(type: Int) {}
        override fun noteOn(midiNote: Int, velocity: Float) { noteIsOn = true }
        override fun noteOff() { noteIsOn = false }
        override fun outputPeak() = if (noteIsOn) 0.3f else 0f
        override fun playFrame(): Long { frame += 480; return frame }
        override fun backend() = backendValue
    }

    private val lines = mutableListOf<String>()
    private val reporter = SmokeReporter({ lines += it }, run = "t")
    private fun field(line: String, key: String) =
        line.split(' ').firstOrNull { it.startsWith("$key=") }?.substringAfter('=')
    private fun closings() = lines.filter { it.contains(" step=estimulo ") || it.contains(" step=control ") }

    /** Bug que atrapa (AC-053.10): el plan ignora la semilla que mandó el script. */
    @Test
    fun theSeedIsTheScriptsWhenItCameAndOtherwiseOneThePlanPicksAndSaysSo() {
        assertEquals(SeedChoice(12345L, "script"), SeedChoice.of(12345L) { 7L })
        assertEquals(SeedChoice(7L, "app"), SeedChoice.of(null) { 7L })
    }

    /**
     * Bug que atrapa: el A4 de sf2/sf3 por otra ruta que el sistema, o con el orden de otro panel (con
     * la semilla 12345, sf2 va estímulo-control y sf3 control-estímulo).
     */
    @Test
    fun overTheSystemEachPanelUsesItsOwnOrderAndTheSystemRoute() = runTest {
        val seeded = SeededWindows(ListeningWindows(Port(AudioBackendType.OBOE), pause = {}), seed = 12345L)
        assertTrue(seeded.overSystem(reporter, "sf3", "f.sf3"))
        assertEquals(WindowOrder.of(12345L, "sf3").map { it.step }, closings().map { it.substringAfter(" step=").substringBefore(' ') })
        assertEquals(listOf("control", "estimulo"), closings().map { it.substringAfter(" step=").substringBefore(' ') })
        assertTrue(lines.all { field(it, "panel") == "sf3" && field(it, "ruta") == "sistema" })
    }

    /** Bug que atrapa (D5, AC-053.8): el A4 del plan USB declarado por la salida del sistema. */
    @Test
    fun overUsbTheWindowsArePanelUsbByLibusbWithTheUsbOrder() = runTest {
        val seeded = SeededWindows(ListeningWindows(Port(AudioBackendType.LIBUSB), pause = {}), seed = 12345L)
        assertTrue(seeded.overUsb(reporter, "wma-fixture.sf2"))
        assertEquals(WindowOrder.of(12345L, "usb").map { it.step }, closings().map { it.substringAfter(" step=").substringBefore(' ') })
        assertTrue(lines.all { field(it, "panel") == "usb" && field(it, "ruta") == "libusb" })
        assertEquals("wma-fixture.sf2", field(closings().single { " step=estimulo " in it }, "archivo"))
    }

    private val calls = mutableListOf<String>()

    private suspend fun usbPanel(
        connects: Boolean = true,
        platformThrows: Boolean = false,
        windowsThrow: Boolean = false,
    ): Boolean = runUsbPanel(
        reporter,
        loadFixture = { calls += "cargar"; true },
        stopEngineForUsb = { calls += "parar-para-usb"; true },
        runUsb = { r, listen ->
            calls += "conectar"
            if (platformThrows) throw IllegalStateException("plataforma")
            val ok = if (connects) listen(r) else false
            calls += "desconectar"
            ok
        },
        listen = { calls += "ventanas"; if (windowsThrow) throw IllegalStateException("ventanas"); true },
        stopEngine = { calls += "parar-motor" },
        unload = { calls += "descargar" },
    )

    /**
     * Bug que atrapa: el font descargado antes de las ventanas, el motor andando cuando la plataforma
     * desconecta (`backend-restaurado` exige el motor parado), o el motor sin parar antes del USB.
     */
    @Test
    fun theUsbPanelLoadsStopsPlaysOverUsbStopsAgainAndUnloads() = runTest {
        assertTrue(usbPanel())
        assertEquals(
            listOf("cargar", "parar-para-usb", "conectar", "ventanas", "parar-motor", "desconectar", "descargar"),
            calls,
        )
    }

    /** Bug que atrapa: sin conexión (permiso pendiente) el font queda cargado para el panel siguiente. */
    @Test
    fun withoutAConnectionThereAreNoWindowsAndTheFixtureIsStillUnloaded() = runTest {
        assertFalse(usbPanel(connects = false))
        assertEquals(listOf("cargar", "parar-para-usb", "conectar", "desconectar", "descargar"), calls)
    }

    /** Bug que atrapa: una excepción en las ventanas deja el motor andando sobre libusb al desconectar. */
    @Test
    fun theEngineStopsAndTheFixtureUnloadsEvenIfSomethingThrows() = runTest {
        assertFailsWith<IllegalStateException> { usbPanel(windowsThrow = true) }
        assertEquals(listOf("cargar", "parar-para-usb", "conectar", "ventanas", "parar-motor", "descargar"), calls)
        calls.clear()
        assertFailsWith<IllegalStateException> { usbPanel(platformThrows = true) }
        assertEquals(listOf("cargar", "parar-para-usb", "conectar", "descargar"), calls)
    }

    /** Bug que atrapa: un panel USB que da verde con el fixture sin cargar o el motor sin parar. */
    @Test
    fun theUsbPanelIsGreenOnlyIfTheFixtureLoadedAndTheEngineStopped() = runTest {
        val bad = runUsbPanel(
            reporter, loadFixture = { false }, stopEngineForUsb = { true },
            runUsb = { r, listen -> listen(r) }, listen = { true }, stopEngine = {}, unload = {},
        )
        assertFalse(bad)
        val notStopped = runUsbPanel(
            reporter, loadFixture = { true }, stopEngineForUsb = { false },
            runUsb = { r, listen -> listen(r) }, listen = { true }, stopEngine = {}, unload = {},
        )
        assertFalse(notStopped)
    }
}
