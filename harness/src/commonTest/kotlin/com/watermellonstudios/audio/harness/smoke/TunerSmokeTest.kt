package com.watermellonstudios.audio.harness.smoke

import com.watermellonstudios.audio.api.ITuner
import com.watermellonstudios.audio.api.TunerReading
import com.watermellonstudios.audio.domain.AudioBackendType
import com.watermellonstudios.audio.domain.tuner.TunerSnapshot
import com.watermellonstudios.audio.domain.tuner.TunerState
import com.watermellonstudios.audio.domain.tuning.StringTarget
import com.watermellonstudios.audio.domain.tuning.Tuning
import com.watermellonstudios.audio.domain.tuning.TuningConfiguration
import com.watermellonstudios.audio.harness.soundfont.SoundFontPort
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * REQ-053 S4 (AC-053.12, AC-053.13, D17) — el plan `afinador`: el estado del afinador por tick, sus
 * dos fases (reproducción con la configuración del panel, juicio con el A4) y la ventana de silencio
 * de cada una, que es el control y la medición del piso.
 *
 * Fija el formato que lee `scripts/smoke-device.sh`. El juez decide con los ticks, no con lo que la
 * app resume: por eso cada campo del tick tiene su test. Cada test nombra el bug que atrapa.
 */
class TunerSmokeTest {

    // --- Fakes -------------------------------------------------------------------------------

    private class Port : SoundFontPort {
        var loaded = true
        var engineStarts = true
        var currentType = 3
        var noteOns = 0
        var noteIsOn = false
        var peakNote = 0.3f
        var peakSilent = 0f
        var frame = 0L
        var framesPerRead = 480L
        var backendValue = AudioBackendType.OBOE
        override fun load(path: String) = true
        override fun isLoaded() = loaded
        override fun unload() { loaded = false }
        override fun presetCount() = 1
        override fun presetName(index: Int) = "p"
        override fun bankProgram(index: Int) = intArrayOf(0, 0)
        override fun setPreset(index: Int) {}
        override suspend fun ensureEngineRunning() = engineStarts
        override fun engineType() = currentType
        override fun setEngineType(type: Int) { currentType = type }
        override fun noteOn(midiNote: Int, velocity: Float) { noteOns++; noteIsOn = true }
        override fun noteOff() { noteIsOn = false }
        override fun outputPeak() = if (noteIsOn) peakNote else peakSilent
        override fun playFrame(): Long { frame += framesPerRead; return frame }
        override fun backend() = backendValue
    }

    /** El motor del afinador es uno: su objetivo lo empuja el último que asignó (TunerFactory). */
    private class Engine {
        var targetHz = 0f
    }

    /**
     * Un afinador que "oye" lo que toca el port: con la nota sonando y un objetivo en el motor,
     * engancha; sin objetivo, mide sin enganche; en silencio, mide sin enganche.
     */
    private inner class FakeTuner(
        override var configuration: TuningConfiguration,
        private val engine: Engine,
    ) : ITuner {
        var running = false
        override val targets: List<StringTarget> get() = configuration.targets()
        override var selectedString: Int? = null
            set(value) {
                field = value
                engine.targetHz = value?.let { configuration.targetForString(it)?.frequency?.hz?.toFloat() } ?: 0f
                created += "cuerda=$value"
            }
        override val isRunning: Boolean get() = running
        override fun start(): Boolean {
            created += "start"
            running = startAccepts
            // Como `wma_tuner_start`: abre el stream de captura propio. `stop()` NO lo cierra.
            if (startAccepts) captureOpen = true
            return startAccepts
        }
        override fun stop() {
            created += "stop"
            running = false
        }
        override fun reading(): TunerReading? = behavior(port.noteIsOn, selectedString?.let { configuration.targetForString(it) }, engine.targetHz)
    }

    private val port = Port()
    private val engine = Engine()
    private val created = mutableListOf<String>()
    private val configurations = mutableListOf<TuningConfiguration>()
    private var startAccepts = true
    private var behavior: (noteOn: Boolean, target: StringTarget?, engineHz: Float) -> TunerReading? =
        { noteOn, target, engineHz ->
            when {
                noteOn && engineHz > 0f -> TunerReading(target, snapshot(cents = 1.25f, hz = 440.1f, rms = tunerRms()))
                noteOn -> TunerReading(target, snapshot(cents = null, hz = 440.1f, rms = tunerRms()))
                else -> TunerReading(target, snapshot(cents = null, hz = null, rms = tunerRms()))
            }
        }
    /**
     * El RMS de lo que analiza el afinador (`nivel-dbfs`, con el que se mide el piso): 0,18 (≈ −15 dBFS,
     * el A4 medido en el g42) con la nota, 10^(−58/20) (−58 dBFS) en silencio.
     */
    private var tunerRms: () -> Float = { if (port.noteIsOn) 0.18f else SILENCE_PEAK }

    /**
     * El pico LINEAL de la entrada (`entrada-dbfs`, informativo): RETIENE el pico, como el medidor de
     * `:audio` medido en el g42 (decae ~0,3 dB/s): −4 dBFS también en el silencio que sigue al A4.
     */
    private var inputLevel: () -> Float? = { 0.63f }
    private var captureOpen = false

    private val probe = object : TunerProbe {
        override fun inputPeak(): Float? = inputLevel()
        override fun engineTargetHz(): Float? = engine.targetHz
        override fun inputRunning(): Boolean = captureOpen
        override fun stopInput() {
            created += "cerrar-entrada"
            captureOpen = false
        }
    }

    private val events = mutableListOf<String>()
    private val lines get() = events.filter { it.startsWith("HARNESS-SMOKE ") }
    private val reporter = SmokeReporter({ events += it }, run = "t")

    private fun windows(threshold: Float? = -30f) = TunerWindows(
        port = port,
        newTuner = { config -> configurations += config; FakeTuner(config, engine) },
        probe = probe,
        floorThresholdDbfs = threshold,
        pause = { events += "pausa=$it" },
    )

    private fun step(line: String) = line.substringAfter(" step=").substringBefore(' ')
    private fun field(line: String, key: String): String? =
        line.split(' ').firstOrNull { it.startsWith("$key=") }?.substringAfter('=')
    private fun closes(phase: String) = lines.filter { step(it) == "$phase-estimulo" || step(it) == "$phase-control" }
    private fun ticks() = lines.filter { step(it) == TunerWindows.TICK_STEP }
    private fun preconditions(id: String) = lines.filter { step(it) == HarnessSmoke.STEP_PRECONDITION && field(it, "id") == id }

    // --- AC-053.12: los tres estados ----------------------------------------------------------

    /**
     * Bug que atrapa: colapsar "sin lectura" con "midiendo" (o con 0 cents). Es el modo de falla
     * caro del KDoc de TunerControl: una aguja clavada en 0,0 que se lee como "afinado exacto".
     */
    @Test
    fun theReadoutHasThreeStatesAndZeroCentsIsALockNotAnAbsence() {
        assertEquals(TunerReadout.NO_READING, TunerReadout.of(null))
        assertEquals(TunerReadout.MEASURING, TunerReadout.of(TunerReading(a4(), snapshot(cents = null, hz = 440f))))
        assertEquals(TunerReadout.LOCKED, TunerReadout.of(TunerReading(a4(), snapshot(cents = 0f, hz = 440f))))
        assertEquals(listOf("sin-lectura", "midiendo", "enganchado"), TunerReadout.entries.map { it.id })
    }

    /**
     * Bug que atrapa: un enganche SIN objetivo (cents contra lo que el motor tenía de antes) que no se
     * ve. El panel lo muestra como "X cents · sin objetivo": es una reacción, y en la fase de juicio es
     * un enganche en otra nota. La línea lo dice con `nota=-`.
     */
    @Test
    fun aLockWithoutATargetIsStillALockAndTheTickSaysNoNote() {
        val reading = TunerReading(null, snapshot(cents = -3.26f, hz = 439.2f))
        assertEquals(TunerReadout.LOCKED, TunerReadout.of(reading))
        val line = tickLine(reading)
        assertEquals("enganchado", field(line, "estado"))
        assertEquals("-", field(line, "nota"))
        assertEquals("-3.3", field(line, "cents"))
        assertEquals("false", field(line, "convergido"))
    }

    // --- 4.2: el formato del tick --------------------------------------------------------------

    /**
     * Bug que atrapa: un tick sin alguno de los datos que el juez cruza (fase, ventana, estado, Hz,
     * nota) o que el diagnóstico de AC-053.14 necesita (cents, convergencia, rate de captura,
     * perdidos, el estado del motor y los niveles).
     */
    @Test
    fun aTickCarriesPhaseWindowStateHzNoteCentsConvergedRateAndDropped() {
        val reading = TunerReading(a4(), snapshot(cents = 1.25f, hz = 440.06f, state = TunerState.CONVERGED, rate = 48000, dropped = 7, rms = 0.1f))
        val line = tickLine(reading, phase = TunerPhase.JUDGMENT, n = 2, tMs = 480, input = 0.25f)
        assertEquals(
            "HARNESS-SMOKE v=1 run=t panel=afinador step=afinador ok=true fase=juicio n=2 t-ms=480 " +
                "estado=enganchado hz=440.1 nota=A4 cents=1.3 convergido=true rate=48000 perdidos=7 " +
                "estado-motor=CONVERGED nivel-dbfs=-20.0 entrada-dbfs=-12.0",
            line,
        )
        // Silencio digital en la entrada: −inf, no un número plausible.
        assertEquals("-inf", field(tickLine(reading, input = 0f), "entrada-dbfs"))
    }

    /**
     * Bug que atrapa: un tick sin lectura que inventa ceros (`hz=0`, `rate=0`, `perdidos=0`). Cero es
     * un dato plausible; la ausencia se escribe `-`, como en el resto del formato.
     */
    @Test
    fun aTickWithoutAReadingSaysSoAndInventsNoZeros() {
        val line = tickLine(null, input = null)
        assertEquals("sin-lectura", field(line, "estado"))
        for (key in listOf("hz", "nota", "cents", "rate", "perdidos", "estado-motor", "nivel-dbfs", "entrada-dbfs")) {
            assertEquals("-", field(line, key), key)
        }
        assertEquals("false", field(line, "convergido"))
    }

    /**
     * Bug que atrapa: perder el signo de un número chico (−0,4 cents impreso "0.4": una cuerda baja
     * que se lee alta) o depender del locale (coma decimal). Es lo que hace el `oneDecimal` del panel.
     */
    @Test
    fun numbersHaveOneDecimalTheirSignAndNoLocale() {
        assertEquals("-0.4", decimal1(-0.4f))
        assertEquals("-3.3", decimal1(-3.26f))
        assertEquals("440.0", decimal1(439.96f))
        assertEquals("0.0", decimal1(0f))
        assertEquals("-", decimal1(null))
        assertEquals("-", decimal1(Float.NaN))
        assertEquals("-inf", decimal1(Float.NEGATIVE_INFINITY))
    }

    // --- 4.2 / D17: las dos fases y sus ventanas ----------------------------------------------

    /**
     * Bug que atrapa (D17): reproducir con una cuerda elegida (eso ya sería el arreglo) o juzgar con
     * otra configuración que la del A4. La reproducción es la configuración del PANEL, la misma
     * constante que usa TunerControl.
     */
    @Test
    fun theReproductionIsThePanelsConfigurationWithoutAStringAndTheJudgmentIsTheA4() = runTest {
        windows().run(reporter, seed = 12345L, fixture = "wma-fixture.sf2")
        assertEquals(listOf(PANEL_TUNING, TuningConfiguration(Tuning.UKULELE_HIGH_G)), configurations)
        assertEquals(TuningConfiguration(Tuning.GUITAR_STANDARD), PANEL_TUNING)
        assertEquals("A4", TunerPhase.JUDGMENT.configuration.targetForString(TunerPhase.JUDGMENT.selectedString!!)?.note?.name)
        assertNull(TunerPhase.REPRODUCTION.selectedString)
        // La reproducción nunca toca la cuerda; el juicio la elige antes de arrancar y la suelta al final.
        assertEquals(
            listOf("start", "stop", "cerrar-entrada", "cuerda=1", "start", "cuerda=null", "stop", "cerrar-entrada"),
            created,
        )
    }

    /**
     * Bug que atrapa (review de S4): el micrófono abierto después del afinador. `stop()` del afinador
     * no cierra el stream de captura que abrió `start()`; en `todo` lo que sigue es `usb`, y el stream
     * de Oboe seguiría escribiendo en el nodo de entrada junto con el motor por libusb. Cada fase cierra
     * la entrada, y el panel lo AFIRMA con `step=parada`.
     */
    @Test
    fun theCaptureIsClosedAfterEachPhaseAndThePanelSaysSo() = runTest {
        windows().run(reporter, seed = 12345L, fixture = "f.sf2")
        assertFalse(captureOpen)
        val stop = lines.single { step(it) == "parada" }
        assertEquals("true", field(stop, "ok"))
        assertEquals("false", field(stop, "entrada-corriendo"))
        assertTrue(lines.indexOf(stop) > lines.indexOfLast { step(it).startsWith("juicio-") })
        // Si la entrada no se cierra (un stop que no anda), la parada lo dice.
        events.clear()
        val stuck = TunerWindows(
            port = port,
            newTuner = { FakeTuner(it, engine) },
            probe = object : TunerProbe by probe {
                override fun stopInput() {}
            },
            floorThresholdDbfs = -30f,
            pause = {},
        )
        assertFalse(stuck.run(reporter, seed = 12345L, fixture = "f.sf2"))
        assertEquals("false", field(lines.single { step(it) == "parada" }, "ok"))
    }

    /** Bug que atrapa: ventanas que no son múltiplo del período del tick (podrían quedar sin ticks). */
    @Test
    fun theWindowsAreWholeTicks() {
        assertFailsWith<IllegalArgumentException> {
            TunerWindows(port, { FakeTuner(it, engine) }, probe, -30f, pause = {}, stimulusMs = 4010L)
        }
        assertFailsWith<IllegalArgumentException> {
            TunerWindows(port, { FakeTuner(it, engine) }, probe, -30f, pause = {}, silenceMs = 30L)
        }
    }

    /**
     * Bug que atrapa (AC-053.13): ventanas en un orden fijo. Con el silencio siempre primero, un
     * afinador que se queda enganchado después del estímulo nunca se vería en el control. El orden de
     * cada fase sale de la semilla, con la cuenta que el juez recalcula (`afinador-<fase>`).
     */
    @Test
    fun eachPhaseRunsItsTwoWindowsInTheSeedsOrderReproductionFirst() = runTest {
        windows().run(reporter, seed = 12345L, fixture = "f.sf2")
        for (phase in TunerPhase.entries) {
            val want = WindowOrder.of(12345L, "afinador-${phase.id}").map { "${phase.id}-${it.step}" }
            assertEquals(want, closes(phase.id).map { step(it) }, phase.id)
            assertEquals(listOf("1", "2"), closes(phase.id).map { field(it, "n") })
        }
        val first = lines.indexOfFirst { step(it).startsWith("juicio-") }
        assertTrue(lines.indexOfLast { step(it).startsWith("reproduccion-") } < first)
        // Las dos fases no tienen el mismo orden con esta semilla: el test ve si se usa otra clave.
        assertTrue(WindowOrder.of(12345L, "afinador-reproduccion") != WindowOrder.of(12345L, "afinador-juicio"))
    }

    /**
     * Bug que atrapa: un estímulo que no es el de D5 (otra nota, sin el fixture, por otra ruta) o un
     * silencio en el que suena algo. La ventana de estímulo toca el A4 del fixture por el parlante.
     */
    @Test
    fun theStimulusWindowPlaysTheA4OverTheSystemAndTheSilenceWindowPlaysNothing() = runTest {
        assertTrue(windows().run(reporter, seed = 12345L, fixture = "wma-fixture.sf2"))
        val stimulus = lines.single { step(it) == "juicio-estimulo" }
        assertEquals("true", field(stimulus, "ok"))
        assertEquals("A4", field(stimulus, "sono"))
        assertEquals("69", field(stimulus, "nota"))
        assertEquals("wma-fixture.sf2", field(stimulus, "archivo"))
        assertEquals("sistema", field(stimulus, "ruta"))
        assertEquals("OBOE", field(stimulus, "backend"))
        assertEquals(TunerWindows.STIMULUS_MS.toString(), field(stimulus, "ventana-ms"))
        assertTrue((field(stimulus, "frames")?.toLong() ?: 0) > 0)
        assertEquals("0.3000", field(stimulus, "pico"))
        val silence = lines.single { step(it) == "juicio-control" }
        assertEquals("true", field(silence, "ok"))
        assertEquals("silencio", field(silence, "tipo"))
        assertEquals(TunerWindows.SILENCE_MS.toString(), field(silence, "ventana-ms"))
        assertEquals("0.0000", field(silence, "pico"))
        assertNull(field(silence, "sono"))
        assertEquals(2, port.noteOns)
        assertFalse(port.noteIsOn)
    }

    /**
     * Bug que atrapa: ticks fuera de su ventana o con el número de otra (el juez asocia cada tick a
     * su ventana por fase y `n`: un tick mal numerado mueve un enganche del estímulo al control).
     */
    @Test
    fun ticksComeEveryPollInsideEachWindowWithItsPhaseAndNumber() = runTest {
        windows().run(reporter, seed = 12345L, fixture = "f.sf2")
        for (phase in TunerPhase.entries) {
            for (close in closes(phase.id)) {
                val n = field(close, "n")
                val mine = ticks().filter { field(it, "fase") == phase.id && field(it, "n") == n }
                val windowMs = field(close, "ventana-ms")!!.toLong()
                assertEquals(windowMs / TUNER_POLL_MS, mine.size.toLong(), "${phase.id}/$n")
                assertEquals(mine.size.toString(), field(close, "ticks"))
                val times = mine.map { field(it, "t-ms")!!.toLong() }
                assertEquals((1..mine.size).map { it * TUNER_POLL_MS }, times)
                // Cada tick de la ventana de estímulo engancha (el fake oye la nota) sólo en juicio.
                val states = mine.map { field(it, "estado") }.toSet()
                val stimulus = step(close).endsWith("-estimulo")
                val want = if (stimulus && phase == TunerPhase.JUDGMENT) setOf("enganchado") else setOf("midiendo")
                assertEquals(want, states, "${phase.id}/$n")
                // Y el tick se emite ANTES del cierre de su ventana: cierre = fin de la ventana.
                assertTrue(lines.indexOf(mine.last()) < lines.indexOf(close))
            }
        }
        assertEquals(TUNER_POLL_MS, 80L)
    }

    /** Bug que atrapa (AC-053.8): dar el A4 por rendido con el render parado o sin señal. */
    @Test
    fun aStimulusNotRenderedSaysWhy() = runTest {
        port.framesPerRead = 0
        assertFalse(windows().run(reporter, seed = 1L, fixture = "f.sf2"))
        assertEquals("false", field(lines.first { step(it) == "juicio-estimulo" }, "ok"))
        assertEquals("frames-quietos", field(lines.first { step(it) == "juicio-estimulo" }, "motivo"))
        events.clear()
        port.framesPerRead = 480
        port.peakNote = 0f
        assertFalse(windows().run(reporter, seed = 1L, fixture = "f.sf2"))
        assertEquals("sin-senal", field(lines.first { step(it) == "juicio-estimulo" }, "motivo"))
    }

    /** Bug que atrapa: una ventana de silencio en la que el motor rinde algo, juzgada como control. */
    @Test
    fun aSilenceWindowThatSoundsIsNotSilence() = runTest {
        port.peakSilent = 0.2f
        assertFalse(windows().run(reporter, seed = 1L, fixture = "f.sf2"))
        val silence = lines.first { step(it) == "reproduccion-control" }
        assertEquals("false", field(silence, "ok"))
        assertEquals("el-control-sono", field(silence, "motivo"))
    }

    /** Bug que atrapa: sin motor o sin el font, ventanas "corridas" sin estímulo posible. */
    @Test
    fun withoutTheEngineOrTheFontNoWindowRunsAndEachClosesFalse() = runTest {
        port.engineStarts = false
        assertFalse(windows().run(reporter, seed = 1L, fixture = "f.sf2"))
        assertTrue(ticks().isEmpty())
        assertEquals(0, port.noteOns)
        assertEquals(4, lines.count { step(it).endsWith("-estimulo") || step(it).endsWith("-control") })
        assertTrue(lines.filter { step(it).endsWith("-estimulo") }.all { field(it, "motivo") == "motor-no-arranca" && field(it, "ok") == "false" })
        events.clear()
        port.engineStarts = true
        port.loaded = false
        assertFalse(windows().run(reporter, seed = 1L, fixture = "f.sf2"))
        assertTrue(lines.filter { step(it).endsWith("-control") }.all { field(it, "motivo") == "sin-soundfont" })
    }

    /**
     * Bug que atrapa (AC-053.14): el "por qué" que no queda escrito. El arranque dice contra qué
     * configuración, qué cuerda, qué objetivo cree la app y contra qué Hz mide el MOTOR — la hipótesis
     * de D17 (sin cuerda no hay objetivo) se lee ahí, no se supone.
     */
    @Test
    fun theStartSaysTheConfigurationTheStringAndWhatTheEngineMeasuresAgainst() = runTest {
        windows().run(reporter, seed = 12345L, fixture = "f.sf2")
        val repro = lines.single { step(it) == "reproduccion-arranque" }
        assertEquals("true", field(repro, "ok"))
        assertEquals("guitar_standard", field(repro, "config"))
        assertEquals("-", field(repro, "cuerda"))
        assertEquals("-", field(repro, "objetivo"))
        assertEquals("0.0", field(repro, "objetivo-motor-hz"))
        val judge = lines.single { step(it) == "juicio-arranque" }
        assertEquals("ukulele_high_g", field(judge, "config"))
        assertEquals("1", field(judge, "cuerda"))
        assertEquals("A4", field(judge, "objetivo"))
        assertEquals("440.0", field(judge, "objetivo-motor-hz"))
    }

    /**
     * Bug que atrapa: abandonar la observación cuando `start()` falla (el full-duplex del g42 es
     * justamente un riesgo a MEDIR, D17). El arranque falla y `mic-abre` no se cumple, pero las ventanas
     * corren igual y los ticks dicen lo que hubo.
     */
    @Test
    fun aRejectedStartFailsTheStartAndTheMicButTheWindowsStillRun() = runTest {
        startAccepts = false
        behavior = { _, _, _ -> null }
        assertFalse(windows().run(reporter, seed = 1L, fixture = "f.sf2"))
        val start = lines.first { step(it) == "reproduccion-arranque" }
        assertEquals("false", field(start, "ok"))
        assertEquals("start-rechazado", field(start, "motivo"))
        val mic = preconditions(SmokePreconditions.MIC_OPENS)
        assertEquals(2, mic.size)
        assertTrue(mic.all { field(it, "cumplida") == "false" })
        assertEquals("fase:reproduccion,aceptado:false,corriendo:false,espera-ms:0", field(mic.first(), "evidencia"))
        assertEquals(4, lines.count { step(it).endsWith("-estimulo") || step(it).endsWith("-control") })
        assertTrue(ticks().isNotEmpty())
        assertTrue(ticks().all { field(it, "estado") == "sin-lectura" })
    }

    /**
     * Bug que atrapa: dejar el afinador corriendo, o el motor midiendo contra el A4, después del plan
     * (el panel de la UI, sin cuerda, quedaría midiendo contra 440). También si algo tira.
     */
    @Test
    fun eachPhaseStopsTheTunerAndClearsTheStringEvenIfSomethingThrows() = runTest {
        behavior = { _, _, _ -> throw IllegalStateException("puente") }
        assertFailsWith<IllegalStateException> { windows().run(reporter, seed = 1L, fixture = "f.sf2") }
        assertEquals(listOf("start", "stop", "cerrar-entrada"), created)
        assertFalse(port.noteIsOn)
        assertEquals(3, port.currentType)
        created.clear()
        configurations.clear()
        behavior = { noteOn, target, _ -> if (noteOn && target != null) throw IllegalStateException("puente") else null }
        assertFailsWith<IllegalStateException> { windows().run(reporter, seed = 1L, fixture = "f.sf2") }
        assertEquals(
            listOf("start", "stop", "cerrar-entrada", "cuerda=1", "start", "cuerda=null", "stop", "cerrar-entrada"),
            created,
        )
        assertEquals(0f, engine.targetHz)
        assertFalse(port.noteIsOn)
    }

    // --- 4.2: la precondición del piso ---------------------------------------------------------

    /**
     * Bug que atrapa: un piso "cumplido" a la par del umbral, o una evidencia sin el medidor y los dos
     * números (el juez verifica que la app midió con el medidor y contra el umbral de la FICHA, y que lo
     * que dice cuadra con ellos).
     */
    @Test
    fun theFloorIsMetOnlyBelowTheThresholdAndTheEvidenceCarriesTheMeterAndBothNumbers() {
        val met = assertNotNull(silenceFloor(TunerPhase.JUDGMENT, maxLevelDbfs = -50f, readings = 10, thresholdDbfs = -25f))
        assertTrue(met.met)
        assertEquals("fase:juicio,medidor:nivel-afinador,nivel-max-dbfs:-50.0,umbral-dbfs:-25.0,lecturas:10", met.evidence)
        assertFalse(assertNotNull(silenceFloor(TunerPhase.JUDGMENT, -20f, 10, -25f)).met)
        assertFalse(assertNotNull(silenceFloor(TunerPhase.JUDGMENT, -25f, 10, -25f)).met)
        assertTrue(assertNotNull(silenceFloor(TunerPhase.REPRODUCTION, -90f, 3, -25f)).met)
    }

    /**
     * Bug que atrapa (review de S4): la app decide con el número crudo y escribe el redondeado. Con
     * −25,04 dBFS contra −25: crudo, "cumplida"; escrito, `-25.0` contra `-25.0`, y el juez (que sólo
     * ve lo escrito) la da incoherente. La app decide con lo que escribe.
     */
    @Test
    fun theFloorDecidesWithTheNumberItWrites() {
        val r = assertNotNull(silenceFloor(TunerPhase.JUDGMENT, -25.04f, 5, -25f))
        assertFalse(r.met)
        assertTrue("nivel-max-dbfs:-25.0,umbral-dbfs:-25.0" in r.evidence)
        assertTrue(assertNotNull(silenceFloor(TunerPhase.JUDGMENT, -25.06f, 5, -25f)).met)
    }

    /**
     * Bug que atrapa (AC-053.3): un piso dado por cumplido sin umbral o sin medición. Sin las dos
     * cosas la app NO emite la línea, y el juez la da no-verificable: nunca cumplida por defecto.
     */
    @Test
    fun withoutAThresholdOrAMeasurementTheFloorIsNotClaimed() {
        assertNull(silenceFloor(TunerPhase.JUDGMENT, maxLevelDbfs = -50f, readings = 10, thresholdDbfs = null))
        assertNull(silenceFloor(TunerPhase.JUDGMENT, maxLevelDbfs = null, readings = 0, thresholdDbfs = -25f))
    }

    /**
     * Bug que atrapa: medir el piso con el estímulo sonando (sale incumplido siempre) o una sola vez
     * por corrida. Es una línea por fase, medida en SU ventana de silencio, con el nivel del afinador.
     */
    @Test
    fun theFloorIsMeasuredInEachPhasesSilenceWindowOnly() = runTest {
        windows(threshold = -30f).run(reporter, seed = 12345L, fixture = "f.sf2")
        val floor = preconditions(SmokePreconditions.SILENCE_FLOOR)
        assertEquals(listOf("reproduccion", "juicio"), floor.map { field(it, "evidencia")!!.substringAfter("fase:").substringBefore(',') })
        assertTrue(floor.all { field(it, "cumplida") == "true" && field(it, "ok") == "true" })
        assertTrue(floor.all { "medidor:nivel-afinador,nivel-max-dbfs:-58.0,umbral-dbfs:-30.0,lecturas:40" in field(it, "evidencia")!! })
        // Cada línea va después del cierre de su ventana de silencio.
        for (phase in TunerPhase.entries) {
            val close = lines.indexOfFirst { step(it) == "${phase.id}-control" }
            val line = lines.indexOfFirst { field(it, "id") == SmokePreconditions.SILENCE_FLOOR && "fase:${phase.id}" in it }
            assertTrue(close in 0 until line, phase.id)
        }
        val silence = lines.first { step(it) == "juicio-control" }
        assertEquals("-58.0", field(silence, "nivel-max-dbfs"))
        assertEquals("40", field(silence, "nivel-lecturas"))
        assertEquals("-30.0", field(silence, "umbral-dbfs"))
    }

    /**
     * Bug que atrapa (corrida 4.6 en el g42): medir el piso con el medidor de ENTRADA, que retiene el
     * pico y decae ~0,3 dB/s. El silencio que sigue al A4 midió −4,1 dBFS con la sala callada (un
     * BLOQUEADO falso); el nivel del afinador dio −43,4. La entrada se sigue emitiendo (tick y cierre),
     * pero no juzga nada: con la entrada en −4 dBFS y el nivel en −58, el piso se cumple.
     */
    @Test
    fun theFloorIsTheTunersLevelNotTheInputMetersRetainedPeak() = runTest {
        windows(threshold = -30f).run(reporter, seed = 12345L, fixture = "f.sf2")
        val silence = lines.first { step(it) == "juicio-control" }
        assertEquals("-4.0", field(silence, "entrada-max-dbfs"))
        assertTrue(ticks().all { field(it, "entrada-dbfs") == "-4.0" })
        val floor = preconditions(SmokePreconditions.SILENCE_FLOOR)
        assertEquals(2, floor.size)
        assertTrue(floor.all { field(it, "cumplida") == "true" })
        assertTrue(floor.none { "entrada" in field(it, "evidencia")!! })
    }

    /** Bug que atrapa: un piso "medido" sin umbral que el script mandó (una corrida a mano). */
    @Test
    fun withoutTheScriptsThresholdTheFloorLineIsNotEmitted() = runTest {
        windows(threshold = null).run(reporter, seed = 12345L, fixture = "f.sf2")
        assertTrue(preconditions(SmokePreconditions.SILENCE_FLOOR).isEmpty())
        assertEquals("-", field(lines.first { step(it) == "juicio-control" }, "umbral-dbfs"))
    }

    /** Bug que atrapa: un piso sin lecturas del afinador (sin snapshot, o NaN) dado por cumplido. */
    @Test
    fun aFloorWithoutTunerReadingsIsNotClaimed() = runTest {
        behavior = { noteOn, target, _ -> if (noteOn) TunerReading(target, snapshot(cents = null, hz = 440f, rms = 0.18f)) else null }
        windows(threshold = -30f).run(reporter, seed = 12345L, fixture = "f.sf2")
        assertTrue(preconditions(SmokePreconditions.SILENCE_FLOOR).isEmpty())
        assertEquals("-", field(lines.first { step(it) == "juicio-control" }, "nivel-max-dbfs"))
        assertEquals("0", field(lines.first { step(it) == "juicio-control" }, "nivel-lecturas"))
        events.clear()
        tunerRms = { if (port.noteIsOn) 0.18f else Float.NaN }
        behavior = { _, target, _ -> TunerReading(target, snapshot(cents = null, hz = null, rms = tunerRms())) }
        windows(threshold = -30f).run(reporter, seed = 12345L, fixture = "f.sf2")
        assertTrue(preconditions(SmokePreconditions.SILENCE_FLOOR).isEmpty())
    }

    /**
     * Bug que atrapa (review de S4): un silencio DIGITAL (RMS 0 en cada lectura: el mic silenciado por
     * el control de privacidad, o una captura que "corre" sin entregar) dado por sala en silencio. Sin
     * una sola lectura con señal, la app no afirma el piso (el juez lo da no-verificable).
     */
    @Test
    fun aDigitalSilenceIsNotAQuietRoom() = runTest {
        tunerRms = { if (port.noteIsOn) 0.18f else 0f }
        windows(threshold = -30f).run(reporter, seed = 12345L, fixture = "f.sf2")
        assertTrue(preconditions(SmokePreconditions.SILENCE_FLOOR).isEmpty())
        val silence = lines.first { step(it) == "juicio-control" }
        assertEquals("0", field(silence, "nivel-lecturas"))
        assertEquals("-", field(silence, "nivel-max-dbfs"))
    }

    /**
     * Bug que atrapa: un umbral inválido que cae callado a un default, o uno fuera de dBFS. Presente e
     * inválido, la corrida no arranca (igual que la semilla).
     */
    @Test
    fun theThresholdIsAFiniteDbfsOrTheRequestIsInvalid() {
        assertEquals(ThresholdRequest(null, null), ThresholdRequest.parse(null))
        assertEquals(ThresholdRequest(-25f, null), ThresholdRequest.parse("-25"))
        assertEquals(ThresholdRequest(-42.5f, null), ThresholdRequest.parse("-42.5"))
        assertEquals(ThresholdRequest(null, "umbral-dbfs-invalido:abc"), ThresholdRequest.parse("abc"))
        assertEquals(ThresholdRequest(null, "umbral-dbfs-invalido:5"), ThresholdRequest.parse("5"))
        assertEquals(ThresholdRequest(null, "umbral-dbfs-invalido:NaN"), ThresholdRequest.parse("NaN"))
        assertEquals(ThresholdRequest(null, "umbral-dbfs-invalido:-200"), ThresholdRequest.parse("-200"))
        assertEquals(ThresholdRequest(null, "umbral-dbfs-invalido:-"), ThresholdRequest.parse(""))
    }

    // --- El panel del plan ---------------------------------------------------------------------

    private val calls = mutableListOf<String>()

    /** Bug que atrapa: el fixture que queda cargado si las ventanas tiran (el panel siguiente lo hereda). */
    @Test
    fun theTunerPanelLoadsRunsTheWindowsAndAlwaysUnloads() = runTest {
        assertTrue(runTunerPanel(loadFixture = { calls += "cargar"; true }, windows = { calls += "ventanas"; true }, unload = { calls += "descargar"; true }))
        assertEquals(listOf("cargar", "ventanas", "descargar"), calls)
        calls.clear()
        assertFailsWith<IllegalStateException> {
            runTunerPanel(loadFixture = { true }, windows = { calls += "ventanas"; throw IllegalStateException("x") }, unload = { calls += "descargar"; true })
        }
        assertEquals(listOf("ventanas", "descargar"), calls)
    }

    /** Bug que atrapa: un panel verde sin fixture (sin estímulo posible) o que no descargó. */
    @Test
    fun theTunerPanelIsGreenOnlyIfTheFixtureLoadedTheWindowsRanAndItUnloaded() = runTest {
        assertFalse(runTunerPanel(loadFixture = { false }, windows = { calls += "ventanas"; true }, unload = { true }))
        assertEquals(emptyList(), calls)
        assertFalse(runTunerPanel(loadFixture = { true }, windows = { false }, unload = { true }))
        assertFalse(runTunerPanel(loadFixture = { true }, windows = { true }, unload = { false }))
    }

    // --- helpers -----------------------------------------------------------------------------

    private fun a4(): StringTarget = TunerPhase.JUDGMENT.configuration.targetForString(1)!!

    private fun tickLine(
        reading: TunerReading?,
        phase: TunerPhase = TunerPhase.REPRODUCTION,
        n: Int = 1,
        tMs: Long = 80,
        input: Float? = 0.01f,
    ): String {
        reportTunerTick(reporter, phase, n, tMs, reading, input)
        return lines.last()
    }

    private companion object {
        /** 10^(−58/20): el pico lineal de −58 dBFS. */
        const val SILENCE_PEAK = 0.0012589254f
    }

    private fun snapshot(
        cents: Float?,
        hz: Float?,
        state: TunerState = TunerState.MEASURING,
        rate: Int = 48000,
        dropped: Long = 0,
        rms: Float = 0.01f,
    ) = TunerSnapshot(
        captureSampleRate = rate, levelRms = rms, framesAnalyzed = 4800, droppedFrames = dropped, state = state,
        cents = cents, phaseAngle = null, uncertainty = null, detectedHz = hz, detectionClarity = 0.9f,
        inharmonicityB = null, lockedString = null, fastModeState = 0, usableRangeCents = null,
        inputDiscontinuity = false, discontinuityCount = 0, spectralSupport = null,
    )
}
