package com.watermellonstudios.audio.harness.smoke

import com.watermellonstudios.audio.api.AudioEngine
import com.watermellonstudios.audio.api.AudioInput
import com.watermellonstudios.audio.harness.soundfont.Fixtures
import com.watermellonstudios.audio.harness.soundfont.SoundFontCheck
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay

/**
 * MINI-038 — la corrida automática que dispara `scripts/smoke-device.sh` por adb.
 *
 * Corre los paneles del [SmokePlan] en el orden de [Panel] y emite sus líneas. Nada acá simula un
 * gesto humano: lo que lo necesita (el permiso USB) lo resuelve el panel de la plataforma, que
 * emite `step=esperando-humano` y espera.
 *
 * Pasos, por panel (el contrato que `smoke-device.sh` espera):
 * - `salida`: `start`, `stream`, `frames`.
 * - `captura`: `start`, `nivel`, `stop`.
 * - `sf2`: `fixture`, `carga`, `preset`, `nota`, `descarga`, y el rechazo: `fixture`
 *   (el archivo trucho) + `no-soundfont`.
 * - `sf3`: `fixture`, `carga`, `preset`, `nota`, `descarga`.
 * - `usb`: `motor-parado` (acá) y los que emita el panel de la plataforma (ver `UsbHarness` en
 *   androidMain). Sin USB en la plataforma, `usb step=disponible ok=false motivo=no-aplica`.
 * - siempre: `plan inicio` al principio y `plan fin` al final, con los paneles que fallaron.
 */
class SmokePlanRunner(
    private val engine: AudioEngine,
    private val input: AudioInput,
    private val playFrame: () -> Long,
    /** `getEngineState()` del puente: el estado NATIVO, que es el que decide si se puede reconfigurar. */
    private val engineState: () -> Int,
    private val soundFont: SoundFontCheck,
    private val fixtures: Fixtures,
    private val usb: (suspend (SmokeReporter) -> Boolean)?,
    private val pause: suspend (Long) -> Unit = { delay(it) },
) {

    /**
     * @param requestProblem un defecto del pedido que el shell detectó al leerlo (p. ej. un extra
     *   con un número inválido). Si lo hay, la corrida NO arranca: se reporta, no se adivina.
     */
    suspend fun run(raw: String, reporter: SmokeReporter, requestProblem: String? = null): Boolean {
        if (requestProblem != null) {
            reporter.report(PLAN, "inicio", false, "plan" to raw, "motivo" to requestProblem)
            reporter.report(PLAN, "fin", false, "fallidos" to PLAN)
            return false
        }
        val plan = SmokePlan.parse(raw)
        if (plan is SmokePlan.Invalid) {
            reporter.report(PLAN, "inicio", false, "plan" to raw, "motivo" to plan.reason)
            reporter.report(PLAN, "fin", false, "fallidos" to PLAN)
            return false
        }
        val panels = (plan as SmokePlan.Valid).panels
        reporter.report(PLAN, "inicio", true, "plan" to panels.joinToString(",") { it.id })

        val failed = mutableListOf<String>()
        for (panel in panels) {
            val ok = try {
                runPanel(panel, reporter)
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                // Un panel que tira no puede tapar a los demás ni desaparecer del registro.
                reporter.report(panel.id, "excepcion", false, "error" to (e.message ?: e::class.simpleName))
            }
            if (!ok) failed += panel.id
        }

        // "Detenido" es el ESTADO final, no el Result de stop(): con `--plan usb` suelto el motor
        // nunca arrancó y stop() sobre un motor parado no es un fallo.
        val stopped = stopEngineAndWait(engine, engineState, pause).stopped
        return reporter.report(
            PLAN, "fin", failed.isEmpty() && stopped,
            "fallidos" to failed.joinToString(",").ifEmpty { "-" }, "motor-detenido" to stopped,
        )
    }

    private suspend fun runPanel(panel: Panel, r: SmokeReporter): Boolean = when (panel) {
        Panel.SALIDA -> output(r)
        Panel.CAPTURA -> capture(r)
        Panel.SF2 -> sf2(r)
        Panel.SF3 -> sf3(r)
        Panel.USB -> usb?.let { runUsb ->
            // El test tone de USB y el motor no pueden pelearse por el device: el motor se para
            // antes, igual que hace NoisyPad antes de `startStreaming`. Y si no para, se dice.
            val stopped = stopEngineForUsb(engine, engineState, r, pause)
            runUsb(r) && stopped
        } ?: r.report(panel.id, "disponible", false, "motivo" to "no-aplica")
    }

    private suspend fun output(r: SmokeReporter): Boolean {
        val p = Panel.SALIDA.id
        val result = if (engine.isRunning) Result.success(Unit) else engine.start()
        val started = r.report(
            p, "start", result.isSuccess && engine.isRunning,
            "lifecycle" to engine.state.value.lifecycle, "error" to result.exceptionOrNull()?.message,
        )
        val info = engine.state.value.streamInfo
        val stream = r.report(
            p, "stream", started && info != null && info.sampleRate > 0,
            "rate" to info?.sampleRate, "buffer" to info?.bufferSizeInFrames, "canales" to info?.channelCount,
            "latencia-ms" to info?.latencyMillis, "motivo" to if (!started) "motor-no-arranco" else null,
        )
        val f0 = playFrame()
        pause(FRAMES_WINDOW_MS)
        val delta = playFrame() - f0
        val frames = r.report(
            p, "frames", started && delta > 0,
            "delta" to delta, "ventana-ms" to FRAMES_WINDOW_MS,
            "esperado-aprox" to info?.sampleRate?.let { it.toLong() * FRAMES_WINDOW_MS / 1000 },
        )
        return started && stream && frames
    }

    private suspend fun capture(r: SmokeReporter): Boolean {
        val p = Panel.CAPTURA.id
        val accepted = input.start()
        var waited = 0L
        while (accepted && input.isStarting && waited < CAPTURE_OPEN_DEADLINE_MS) {
            pause(POLL_MS)
            waited += POLL_MS
        }
        val running = input.isRunning
        val started = r.report(
            p, "start", accepted && running,
            "aceptado" to accepted, "corriendo" to running, "espera-ms" to waited,
            "motivo" to when {
                !accepted -> "start-rechazado"
                !running -> "no-quedo-corriendo"
                else -> null
            },
        )
        pause(METERING_SETTLE_MS)
        val first = if (started) input.metering() else null
        pause(METERING_SECOND_READ_MS)
        val m = if (started) input.metering() else null
        // `metering()` no-nulo sólo dice que EXISTE el nodo de entrada. Lo que dice que llegó audio
        // es un pico distinto de cero: un micrófono real siempre tiene piso de ruido (−32 dB en el
        // g42), y un stream "corriendo" sin callbacks o todo en cero da exactamente 0.
        val heard = listOfNotNull(first, m).any { it.peakLinear > 0f }
        val level = r.report(
            p, "nivel", m != null && heard,
            "l-db" to m?.levelDbLeft, "r-db" to m?.levelDbRight, "pico" to m?.peakLinear,
            "latencia-ms" to m?.latencyMs,
            "motivo" to when {
                !started -> "sin-stream"
                m == null -> "sin-medicion"
                !heard -> "silencio-digital"
                else -> null
            },
        )
        input.stop()
        val stopped = r.report(p, "stop", !input.isRunning)
        return started && level && stopped
    }

    private suspend fun sf2(r: SmokeReporter): Boolean {
        val p = Panel.SF2.id
        val fixture = fixtures.materialize(r, p, Fixtures.SF2)
        val played = fixture != null && soundFont.runFixture(r, p, fixture, Fixtures.SF2)
        val junk = fixtures.notASoundFont(r, p)
        val rejected = junk != null && soundFont.loadRejects(r, p, junk, Fixtures.NOT_A_SOUNDFONT)
        return played && rejected
    }

    private suspend fun sf3(r: SmokeReporter): Boolean {
        val p = Panel.SF3.id
        val fixture = fixtures.materialize(r, p, Fixtures.SF3) ?: return false
        return soundFont.runFixture(r, p, fixture, Fixtures.SF3)
    }

    companion object {
        const val PLAN = "plan"
        const val FRAMES_WINDOW_MS = 500L
        const val CAPTURE_OPEN_DEADLINE_MS = 3000L
        const val METERING_SETTLE_MS = 500L
        const val METERING_SECOND_READ_MS = 200L
        const val POLL_MS = 50L
    }
}

/** El resultado de [stopEngineAndWait]. */
class EngineStop(val stopped: Boolean, val waitedMs: Long, val nativeState: Int, val error: String?)

/** `EngineState::Stopped` de `core/AudioEngine.h`: lo que devuelve `getEngineState()`. */
const val ENGINE_STATE_STOPPED = 0

/**
 * Para el motor y ESPERA a que el estado NATIVO sea Stopped. Medido en el g42 (30/09): `stop()`
 * vuelve —y el estado de Kotlin dice parado— antes de que termine el fade-out nativo; el motor
 * nativo seguía en Running ~70 ms después, y el paso USB siguiente lo encontraba corriendo. Se
 * decide por el estado nativo y no por `isRunning`, que es el de Kotlin.
 */
suspend fun stopEngineAndWait(
    engine: AudioEngine,
    engineState: () -> Int,
    pause: suspend (Long) -> Unit,
    deadlineMs: Long = 3000,
): EngineStop {
    var error: String? = null
    if (engine.isRunning || engineState() != ENGINE_STATE_STOPPED) {
        error = engine.stop().exceptionOrNull()?.message
    }
    var waited = 0L
    while (engineState() != ENGINE_STATE_STOPPED && waited < deadlineMs) {
        pause(50)
        waited += 50
    }
    val state = engineState()
    return EngineStop(state == ENGINE_STATE_STOPPED && !engine.isRunning, waited, state, error)
}

/**
 * Para el motor antes de tocar USB y lo reporta (`panel=usb step=motor-parado`). Lo usan el plan y
 * el panel USB de la UI: los dos caminos cumplen la misma invariante (I5: no se reconfigura el
 * backend con el stream vivo).
 */
suspend fun stopEngineForUsb(
    engine: AudioEngine,
    engineState: () -> Int,
    r: SmokeReporter,
    pause: suspend (Long) -> Unit = { delay(it) },
): Boolean {
    val result = stopEngineAndWait(engine, engineState, pause)
    return r.report(
        "usb", "motor-parado", result.stopped,
        "estado-motor" to result.nativeState, "espera-ms" to result.waitedMs, "error" to result.error,
    )
}
