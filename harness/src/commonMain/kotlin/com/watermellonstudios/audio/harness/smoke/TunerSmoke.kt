package com.watermellonstudios.audio.harness.smoke

import com.watermellonstudios.audio.api.ITuner
import com.watermellonstudios.audio.api.TunerReading
import com.watermellonstudios.audio.domain.tuning.Tuning
import com.watermellonstudios.audio.domain.tuning.TuningConfiguration
import com.watermellonstudios.audio.harness.soundfont.SoundFontCheck
import com.watermellonstudios.audio.harness.soundfont.SoundFontPort
import com.watermellonstudios.audio.harness.soundfont.formatPeak
import kotlinx.coroutines.delay
import kotlin.math.abs
import kotlin.math.log10
import kotlin.math.roundToLong

/**
 * REQ-053 S4 — el estado del afinador, tal como lo dibuja el panel (`TunerControl`) y lo emite el
 * plan por tick. Tres estados y no dos (ver el KDoc de TunerControl):
 *
 * - [NO_READING] `sin-lectura`: `reading()` es `null`, el motor no publicó nada.
 * - [MEASURING] `midiendo`: hay snapshot y los cents son `null` (sin enganche).
 * - [LOCKED] `enganchado`: hay cents. Con o SIN objetivo: el panel los muestra igual
 *   ("X cents · sin objetivo"), así que para quien mira es una reacción. La nota contra la que
 *   enganchó la dice el tick (`nota`, `-` sin objetivo); el juez sólo da PASS con la del estímulo.
 */
enum class TunerReadout(val id: String) {
    NO_READING("sin-lectura"),
    MEASURING("midiendo"),
    LOCKED("enganchado"),
    ;

    companion object {
        fun of(reading: TunerReading?): TunerReadout = when {
            reading == null -> NO_READING
            reading.cents == null -> MEASURING
            else -> LOCKED
        }
    }
}

/** Cada cuánto el panel lee el afinador, y cada cuánto el plan emite un tick: el mismo número. */
const val TUNER_POLL_MS = 80L

/**
 * La configuración del panel de la UI (`TunerControl`). La fase de reproducción usa ESTA constante,
 * no una copia: reproducir con otra configuración que la que vio el humano no reproduce nada (D17).
 */
val PANEL_TUNING: TuningConfiguration = TuningConfiguration(Tuning.GUITAR_STANDARD)

/**
 * D17 — las dos fases del plan `afinador`.
 *
 * - [REPRODUCTION] (AC-053.14): el panel tal como lo usó el humano el 05/10, [PANEL_TUNING] y sin
 *   cuerda elegida. Lo que pase con el A4 se OBSERVA: no es PASS ni FAIL de AC-053.13.
 * - [JUDGMENT] (AC-053.13): ukelele high-G con la cuerda 1, que es A4. El motor afina alrededor de
 *   un objetivo y no lo busca (KDoc de `ITuner`), y A4 no es cuerda de la guitarra estándar.
 */
enum class TunerPhase(val id: String, val configuration: TuningConfiguration, val selectedString: Int?) {
    REPRODUCTION("reproduccion", PANEL_TUNING, null),
    JUDGMENT("juicio", TuningConfiguration(Tuning.UKULELE_HIGH_G), 1),
    ;

    /** La clave del orden de sus ventanas (la cuenta de [WindowOrder], la misma que el juez). */
    val orderKey: String get() = "${Panel.AFINADOR.id}-$id"
}

/**
 * Lo que el plan necesita de la entrada y del motor además del afinador: el nivel de la ENTRADA (con
 * él se mide el piso, independiente de lo que publique el afinador), contra qué Hz mide el motor (el
 * objetivo global que empujó el último afinador, ver TunerFactory) y cerrar la captura.
 *
 * Cerrarla es parte del contrato porque `ITuner.stop()` NO cierra el stream de captura que abrió
 * `start()` (review de S4): sin esto el micrófono queda abierto para el panel siguiente (`usb`), con el
 * stream de Oboe escribiendo en el nodo de entrada a la par del motor.
 */
interface TunerProbe {
    /** El pico LINEAL de la entrada (el mayor de los dos canales), o `null` sin nodo de entrada. */
    fun inputPeak(): Float?

    /** La frecuencia objetivo vigente en el motor (0 = sin objetivo), o `null` si no se puede leer. */
    fun engineTargetHz(): Float?

    /** Si el stream de captura sigue abierto. */
    fun inputRunning(): Boolean

    /** Cierra el stream de captura (sin soltar el nodo: eso es `release`). */
    fun stopInput()
}

/**
 * El umbral del piso de silencio, tal como llega por el extra `harness.smoke.umbral-dbfs.<id>` (el
 * script lo saca de la ficha: la app no la lee). Ausente no es un defecto: la app no puede afirmar el
 * piso y no emite la línea (el juez la da no-verificable). Presente e inválido SÍ lo es: la corrida
 * no arranca, como con la semilla.
 */
data class ThresholdRequest(val dbfs: Float?, val problem: String?) {
    companion object {
        fun parse(raw: String?): ThresholdRequest {
            if (raw == null) return ThresholdRequest(null, null)
            val v = raw.toFloatOrNull()?.takeIf { it.isFinite() && it in MIN_DBFS..0f }
                ?: return ThresholdRequest(null, "umbral-dbfs-invalido:" + HarnessSmoke.value(raw))
            return ThresholdRequest(v, null)
        }

        /** El piso del medidor de entrada (`InputMetering.SILENT`): por debajo no hay dBFS. */
        const val MIN_DBFS = -160f
    }
}

/**
 * `piso-silencio` de una fase: la entrada no pasó del umbral en la ventana de silencio. `null`
 * (no se emite línea ⇒ no-verificable, AC-053.3) sin umbral o sin una sola lectura CON SEÑAL de la
 * entrada: un piso no medido nunca es cumplido, y un silencio digital no es una sala en silencio.
 *
 * Decide con el número que ESCRIBE (un decimal), no con el crudo: el juez sólo ve lo escrito y
 * verifica que lo afirmado cuadre con eso.
 */
fun silenceFloor(phase: TunerPhase, maxInputDbfs: Float?, readings: Int, thresholdDbfs: Float?): PreconditionReading? {
    if (thresholdDbfs == null || maxInputDbfs == null || maxInputDbfs.isNaN()) return null
    val shown = decimal1(maxInputDbfs)
    val threshold = decimal1(thresholdDbfs)
    return PreconditionReading(
        tenths(maxInputDbfs) < tenths(thresholdDbfs),
        "fase:${phase.id},entrada-max-dbfs:$shown,umbral-dbfs:$threshold,lecturas:$readings",
    )
}

/** Un valor en décimas, como lo escribe [decimal1] (los infinitos quedan en los extremos). */
private fun tenths(x: Float): Long = when {
    x == Float.NEGATIVE_INFINITY -> Long.MIN_VALUE
    x == Float.POSITIVE_INFINITY -> Long.MAX_VALUE
    else -> (x * 10f).roundToLong()
}

/** dBFS de un pico lineal: −inf para el silencio digital, `null` para la ausencia o NaN. */
fun peakDbfs(linear: Float?): Float? = when {
    linear == null || linear.isNaN() -> null
    linear <= 0f -> Float.NEGATIVE_INFINITY
    else -> 20f * log10(linear)
}

/**
 * Un tick: `step=afinador` en el panel `afinador`. **No es un veredicto** (como `escuchar`): lo que
 * el afinador leía en ese instante. El juez decide con los ticks de cada ventana.
 *
 * `fase n t-ms estado hz nota cents convergido rate perdidos estado-motor nivel-dbfs entrada-dbfs`.
 * Una ausencia se escribe `-`, nunca un cero plausible.
 */
fun reportTunerTick(r: SmokeReporter, phase: TunerPhase, n: Int, tMs: Long, reading: TunerReading?, inputPeak: Float?) {
    val s = reading?.snapshot
    r.report(
        Panel.AFINADOR.id, TunerWindows.TICK_STEP, true,
        "fase" to phase.id, "n" to n, "t-ms" to tMs,
        "estado" to TunerReadout.of(reading).id,
        "hz" to decimal1(s?.detectedHz),
        "nota" to reading?.target?.note?.name,
        "cents" to decimal1(reading?.cents),
        "convergido" to (reading?.isConverged ?: false),
        "rate" to s?.captureSampleRate?.takeIf { it > 0 },
        "perdidos" to s?.droppedFrames,
        "estado-motor" to s?.state,
        "nivel-dbfs" to decimal1(s?.levelRms?.let { if (it > 0f) 20f * log10(it) else Float.NEGATIVE_INFINITY }),
        "entrada-dbfs" to decimal1(peakDbfs(inputPeak)),
    )
}

/**
 * Un decimal, con su signo y sin locale (lo lee el script). `-` para la ausencia y para NaN;
 * `-inf`/`inf` para los infinitos (un silencio digital en dBFS).
 */
fun decimal1(x: Float?): String {
    if (x == null || x.isNaN()) return "-"
    if (x.isInfinite()) return if (x < 0) "-inf" else "inf"
    val scaled = (x * 10f).roundToLong()
    val sign = if (scaled < 0) "-" else ""
    val a = abs(scaled)
    return "$sign${a / 10}.${a % 10}"
}

/**
 * REQ-053 S4 — las ventanas del plan `afinador` (D9, D17): en cada fase, una ventana de ESTÍMULO (el
 * A4 del fixture `.sf2`, por el parlante, D5) y una de SILENCIO (el control de AC-053.13 y donde se
 * mide el piso), en el orden que da la semilla con la clave de la fase.
 *
 * Por fase:
 * 1. Un afinador nuevo con la configuración de la fase (`TunerFactory`, una vista viva por vez); en
 *    juicio, la cuerda se elige ANTES de arrancar.
 * 2. `step=precondicion id=mic-abre` y `step=<fase>-arranque` (la misma observación: el arranque no
 *    se bloquea por el mic, falla).
 * 3. [leadInMs] para que la captura arranque. Después, cada ventana: [gapMs] de pausa (el release
 *    de la nota anterior no cae en el silencio), la ventana, y su cierre `step=<fase>-estimulo` /
 *    `step=<fase>-control` con lo que rindió el motor. Durante la ventana, un tick cada
 *    [TUNER_POLL_MS]; el pico de salida se lee cada [PEAK_POLL_MS] (la captura del motor guarda ~43 ms).
 * 4. Al cerrar el silencio, `step=precondicion id=piso-silencio` con la entrada medida y el umbral
 *    que mandó el script (sin umbral o sin una lectura con señal, no se emite: no-verificable).
 * 5. Pase lo que pase, la cuerda vuelve a `null` (el motor queda sin objetivo), el afinador se para
 *    y la captura se cierra ([TunerProbe.stopInput]). Al final del panel, `step=parada` afirma que la
 *    entrada quedó cerrada.
 *
 * `t-ms` es el desplazamiento NOMINAL dentro de la ventana (la suma de las pausas pedidas), no un
 * reloj: con el teléfono cargado la ventana real dura algo más. Ninguna regla del juez depende de él.
 *
 * El juicio sobre lo que LEYÓ el afinador lo hace el script con los ticks: acá no se decide si
 * enganchó. Lo que devuelve [run] es si el plan corrió como se declaró (arranques y ventanas).
 */
class TunerWindows(
    private val port: SoundFontPort,
    private val newTuner: (TuningConfiguration) -> ITuner,
    private val probe: TunerProbe,
    private val floorThresholdDbfs: Float?,
    private val pause: suspend (Long) -> Unit = { delay(it) },
    private val leadInMs: Long = LEAD_IN_MS,
    private val gapMs: Long = GAP_MS,
    private val stimulusMs: Long = STIMULUS_MS,
    private val silenceMs: Long = SILENCE_MS,
) {
    init {
        // Ventanas de ticks enteros: con otra duración, la última lectura caería fuera (o ninguna).
        require(stimulusMs > 0 && stimulusMs % TUNER_POLL_MS == 0L) { "stimulusMs no es multiplo de $TUNER_POLL_MS" }
        require(silenceMs > 0 && silenceMs % TUNER_POLL_MS == 0L) { "silenceMs no es multiplo de $TUNER_POLL_MS" }
        require(TUNER_POLL_MS % PEAK_POLL_MS == 0L)
    }

    suspend fun run(r: SmokeReporter, seed: Long, fixture: String): Boolean {
        val unavailable = when {
            !port.isLoaded() -> "sin-soundfont"
            !port.ensureEngineRunning() -> "motor-no-arranca"
            else -> null
        }
        if (unavailable != null) {
            for (phase in TunerPhase.entries) {
                WindowOrder.of(seed, phase.orderKey).forEachIndexed { i, kind ->
                    r.report(
                        PANEL, "${phase.id}-${kind.step}", false,
                        "n" to i + 1, "de" to 2, "ruta" to Route.SYSTEM.id, "backend" to port.backend(),
                        "ventana-ms" to 0, "frames" to 0, "pico" to formatPeak(0f), "ticks" to 0, "motivo" to unavailable,
                    )
                }
            }
            stopped(r)
            return false
        }
        val previousType = port.engineType()
        var all = true
        try {
            port.setEngineType(SoundFontCheck.ENGINE_SOUNDFONT)
            port.setPreset(ListeningWindows.PRESET)
            for (phase in TunerPhase.entries) {
                all = phase(r, phase, seed, fixture) && all
            }
        } finally {
            port.noteOff()
            port.setEngineType(previousType)
        }
        return stopped(r) && all
    }

    /** `step=parada`: la captura que abrió el afinador quedó cerrada. */
    private fun stopped(r: SmokeReporter): Boolean {
        val running = probe.inputRunning()
        return r.report(PANEL, "parada", !running, "entrada-corriendo" to running, *reason(if (running) "entrada-abierta" else null))
    }

    private suspend fun phase(r: SmokeReporter, phase: TunerPhase, seed: Long, fixture: String): Boolean {
        val tuner = newTuner(phase.configuration)
        try {
            if (phase.selectedString != null) tuner.selectedString = phase.selectedString
            val accepted = tuner.start()
            val running = tuner.isRunning
            val mic = micOpens(accepted, running, 0)
            r.precondition(PANEL, SmokePreconditions.MIC_OPENS, mic.met, "fase:${phase.id},${mic.evidence}")
            val target = phase.selectedString?.let { tuner.targets.getOrNull(it - 1) }
            val started = r.report(
                PANEL, "${phase.id}-arranque", accepted && running,
                "config" to phase.configuration.tuning.id, "cuerda" to phase.selectedString,
                "objetivo" to target?.note?.name, "objetivo-motor-hz" to decimal1(probe.engineTargetHz()),
                "aceptado" to accepted, "corriendo" to running,
                "motivo" to when {
                    !accepted -> "start-rechazado"
                    !running -> "no-quedo-corriendo"
                    else -> null
                },
            )
            pause(leadInMs)
            var windowsOk = true
            val order = WindowOrder.of(seed, phase.orderKey)
            order.forEachIndexed { i, kind ->
                windowsOk = window(r, tuner, phase, kind, i + 1, order.size, fixture) && windowsOk
            }
            return started && windowsOk
        } finally {
            // El motor queda sin objetivo: el panel de la UI (sin cuerda) no hereda el A4 del plan.
            if (phase.selectedString != null) tuner.selectedString = null
            tuner.stop()
            // `stop()` no cierra la captura que abrió `start()`: se cierra acá (review de S4).
            probe.stopInput()
        }
    }

    private suspend fun window(
        r: SmokeReporter,
        tuner: ITuner,
        phase: TunerPhase,
        kind: WindowKind,
        n: Int,
        of: Int,
        fixture: String,
    ): Boolean {
        pause(gapMs)
        val windowMs = if (kind == WindowKind.STIMULUS) stimulusMs else silenceMs
        val backend = port.backend()
        val frame0 = port.playFrame()
        if (kind == WindowKind.STIMULUS) port.noteOn(SoundFontCheck.NOTE, SoundFontCheck.VELOCITY)
        var peak = 0f
        var elapsed = 0L
        var ticks = 0
        var maxInput: Float? = null
        var inputReadings = 0   // las lecturas de la entrada CON señal (pico > 0)
        try {
            while (elapsed < windowMs) {
                val step = minOf(PEAK_POLL_MS, windowMs - elapsed)
                pause(step)
                elapsed += step
                peak = maxOf(peak, port.outputPeak())
                if (elapsed % TUNER_POLL_MS == 0L) {
                    val input = probe.inputPeak()?.takeIf { !it.isNaN() }
                    reportTunerTick(r, phase, n, elapsed, tuner.reading(), input)
                    ticks++
                    // Un pico de 0 no es una medición del piso: es silencio digital (ver silenceFloor).
                    if (input != null && input > 0f) {
                        inputReadings++
                        val db = 20f * log10(input)
                        maxInput = maxOf(maxInput ?: db, db)
                    }
                }
            }
        } finally {
            if (kind == WindowKind.STIMULUS) port.noteOff()
        }
        val frames = port.playFrame() - frame0
        val onRoute = Route.SYSTEM.carries(backend)
        val step = "${phase.id}-${kind.step}"
        if (kind == WindowKind.STIMULUS) {
            val motivo = when {
                !onRoute -> "ruta-equivocada"
                frames <= 0 -> "frames-quietos"
                peak < SoundFontCheck.MIN_PEAK -> "sin-senal"
                else -> null
            }
            return r.report(
                PANEL, step, motivo == null,
                "n" to n, "de" to of, "sono" to "A4", "hz" to 440, "nota" to SoundFontCheck.NOTE,
                "archivo" to fixture, "preset" to ListeningWindows.PRESET, "ruta" to Route.SYSTEM.id, "backend" to backend,
                "ventana-ms" to windowMs, "frames" to frames, "pico" to formatPeak(peak), "ticks" to ticks,
                "objetivo-motor-hz" to decimal1(probe.engineTargetHz()),
                *reason(motivo),
            )
        }
        val motivo = when {
            !onRoute -> "ruta-equivocada"
            frames <= 0 -> "frames-quietos"
            peak >= SoundFontCheck.MIN_PEAK -> "el-control-sono"
            else -> null
        }
        val ok = r.report(
            PANEL, step, motivo == null,
            "n" to n, "de" to of, "tipo" to "silencio", "ruta" to Route.SYSTEM.id, "backend" to backend,
            "ventana-ms" to windowMs, "frames" to frames, "pico" to formatPeak(peak), "ticks" to ticks,
            "entrada-max-dbfs" to decimal1(maxInput), "entrada-lecturas" to inputReadings,
            "umbral-dbfs" to decimal1(floorThresholdDbfs),
            *reason(motivo),
        )
        // El piso, medido en ESTA ventana de silencio (D9, AC-053.12).
        silenceFloor(phase, maxInput, inputReadings, floorThresholdDbfs)?.let {
            r.precondition(PANEL, SmokePreconditions.SILENCE_FLOOR, it.met, it.evidence)
        }
        return ok
    }

    private fun reason(motivo: String?): Array<Pair<String, Any?>> =
        if (motivo == null) emptyArray() else arrayOf("motivo" to motivo)

    companion object {
        private val PANEL = Panel.AFINADOR.id

        /** El paso de un tick (no es un veredicto). */
        const val TICK_STEP = "afinador"

        /** Lo que tarda la captura en entregar después de `start()`, antes de la primera ventana. */
        const val LEAD_IN_MS = 1500L

        /** La pausa antes de cada ventana: el release del A4 y la reverberación no caen en el silencio. */
        const val GAP_MS = 1500L

        /** El A4 sostenido: el afinador tiene que pasar a `enganchado` dentro de esta ventana. */
        const val STIMULUS_MS = 4000L
        const val SILENCE_MS = 3200L

        /** Cada cuánto se lee el pico de salida (la captura del motor guarda ~2048 muestras, ~43 ms). */
        const val PEAK_POLL_MS = 40L
    }
}

/**
 * El panel `afinador` del plan: el fixture `.sf2` (el A4 de D5), las ventanas, y la descarga pase lo
 * que pase. Sin el fixture no hay estímulo posible y no se corre ninguna ventana.
 */
suspend fun runTunerPanel(
    loadFixture: suspend () -> Boolean,
    windows: suspend () -> Boolean,
    unload: () -> Boolean,
): Boolean {
    var ran = false
    var unloaded = false
    try {
        if (loadFixture()) ran = windows()
    } finally {
        unloaded = unload()
    }
    return ran && unloaded
}
