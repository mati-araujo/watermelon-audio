package com.watermellonstudios.audio.harness.smoke

import com.watermellonstudios.audio.domain.AudioBackendType
import com.watermellonstudios.audio.harness.soundfont.SoundFontCheck
import com.watermellonstudios.audio.harness.soundfont.SoundFontPort
import com.watermellonstudios.audio.harness.soundfont.formatPeak
import kotlinx.coroutines.delay

/**
 * REQ-053 S3 — las ventanas de ESCUCHA: el estímulo declarado (D5) y sus controles ciegos (D7).
 *
 * El harness toca el A4 del fixture con el MOTOR (D5: `:audio` no cambia, no hay tono de prueba en
 * la librería) durante una ventana declarada, por la ruta declarada, y mide lo que el motor rindió
 * en esa ventana. Intercala ventanas de control (silencio) en el orden que fija la semilla.
 *
 * El protocolo con `scripts/smoke-device.sh` (que juzga, y le pregunta al sensor):
 * 1. `step=escuchar`: el AVISO. Lleva `n`, `de`, `estimulo` (qué hay que detectar), `ruta`, `en-ms`
 *    (cuándo arranca) y `ventana-ms`. Es CIEGO: el aviso de un estímulo y el de un control son la
 *    misma línea salvo `n`, así que ni el script ni el oyente saben cuál es cuál.
 * 2. [preRollMs] de espera, para que el aviso llegue al oyente antes de que suene nada.
 * 3. La ventana: [windowMs]. En la de estímulo suena la nota; en la de control, nada.
 * 4. `step=estimulo` / `step=control`: el CIERRE, con lo que el motor rindió (`frames`, `pico`), la
 *    ruta medida (`backend`) y `pausa-ms`. Un estímulo vale si avanzó el render, hubo señal y salió
 *    por la ruta declarada; un control, si avanzó el render en SILENCIO por esa ruta.
 * 5. [answerPauseMs] de pausa: el tiempo que tiene el sensor para contestar antes del aviso siguiente.
 *
 * Nadie sincroniza relojes: la app anuncia y el script sigue el orden del log (ver `seguir` en el
 * script). Sin motor o sin el font cargado no hay estímulo posible: no se anuncia ninguna ventana y
 * cada una cierra `ok=false` con su motivo — y el script no le pregunta nada al sensor.
 */
class ListeningWindows(
    private val port: SoundFontPort,
    private val pause: suspend (Long) -> Unit = { delay(it) },
    private val preRollMs: Long = PRE_ROLL_MS,
    private val windowMs: Long = WINDOW_MS,
    private val answerPauseMs: Long = ANSWER_PAUSE_MS,
) {

    /** Toca las ventanas de [order] en [panel] por [route]. `true` si todas salieron como se declararon. */
    suspend fun run(r: SmokeReporter, panel: String, route: Route, order: List<WindowKind>, fixture: String): Boolean {
        val unavailable = when {
            !port.isLoaded() -> "sin-soundfont"
            !port.ensureEngineRunning() -> "motor-no-arranca"
            else -> null
        }
        if (unavailable != null) {
            order.forEachIndexed { i, kind ->
                r.report(
                    panel, kind.step, false,
                    "n" to i + 1, "de" to order.size, "ruta" to route.id, "backend" to port.backend(),
                    "ventana-ms" to 0, "frames" to 0, "pico" to formatPeak(0f), "pausa-ms" to 0, "motivo" to unavailable,
                )
            }
            return false
        }
        val previousType = port.engineType()
        var all = true
        try {
            port.setEngineType(SoundFontCheck.ENGINE_SOUNDFONT)
            port.setPreset(PRESET)
            order.forEachIndexed { i, kind ->
                all = window(r, panel, route, kind, i + 1, order.size, fixture) && all
            }
        } finally {
            port.noteOff()
            port.setEngineType(previousType)
        }
        return all
    }

    private suspend fun window(
        r: SmokeReporter,
        panel: String,
        route: Route,
        kind: WindowKind,
        n: Int,
        of: Int,
        fixture: String,
    ): Boolean {
        r.report(
            panel, HarnessSmoke.STEP_LISTEN, true,
            "n" to n, "de" to of, "estimulo" to STIMULUS, "ruta" to route.id,
            "en-ms" to preRollMs, "ventana-ms" to windowMs,
        )
        pause(preRollMs)
        val backend = port.backend()
        val frame0 = port.playFrame()
        if (kind == WindowKind.STIMULUS) port.noteOn(SoundFontCheck.NOTE, SoundFontCheck.VELOCITY)
        var peak = 0f
        var elapsed = 0L
        try {
            // El pico se lee DURANTE la ventana: la captura del motor guarda sólo el último bloque.
            while (elapsed < windowMs) {
                val step = minOf(POLL_MS, windowMs - elapsed)
                pause(step)
                elapsed += step
                peak = maxOf(peak, port.outputPeak())
            }
        } finally {
            if (kind == WindowKind.STIMULUS) port.noteOff()
        }
        val frames = port.playFrame() - frame0
        val onRoute = route.carries(backend)
        val ok: Boolean
        val motivo: String?
        if (kind == WindowKind.STIMULUS) {
            motivo = when {
                !onRoute -> "ruta-equivocada"
                frames <= 0 -> "frames-quietos"
                peak < SoundFontCheck.MIN_PEAK -> "sin-senal"
                else -> null
            }
            ok = motivo == null
            r.report(
                panel, kind.step, ok,
                "n" to n, "de" to of, "sono" to "A4", "hz" to 440, "nota" to SoundFontCheck.NOTE,
                "archivo" to fixture, "preset" to PRESET, "ruta" to route.id, "backend" to backend,
                "ventana-ms" to windowMs, "frames" to frames, "pico" to formatPeak(peak), "pausa-ms" to answerPauseMs,
                *reason(motivo),
            )
        } else {
            motivo = when {
                !onRoute -> "ruta-equivocada"
                frames <= 0 -> "frames-quietos"
                peak >= SoundFontCheck.MIN_PEAK -> "el-control-sono"
                else -> null
            }
            ok = motivo == null
            r.report(
                panel, kind.step, ok,
                "n" to n, "de" to of, "tipo" to "silencio", "ruta" to route.id, "backend" to backend,
                "ventana-ms" to windowMs, "frames" to frames, "pico" to formatPeak(peak), "pausa-ms" to answerPauseMs,
                *reason(motivo),
            )
        }
        pause(answerPauseMs)
        return ok
    }

    /** `motivo` sólo cuando hay uno: una ventana que salió bien no lleva `motivo=-`. */
    private fun reason(motivo: String?): Array<Pair<String, Any?>> =
        if (motivo == null) emptyArray() else arrayOf("motivo" to motivo)

    companion object {
        /** Lo que el sensor tiene que detectar: el A4 del fixture (un seno a 440 Hz). */
        const val STIMULUS = "A4-440Hz"
        const val PRESET = 0

        /** El aviso llega al oyente con latencia (logcat por Wi-Fi + el sondeo del script, 1 s). */
        const val PRE_ROLL_MS = 4000L
        const val WINDOW_MS = 2000L

        /** Lo que el sensor tiene para contestar: el script espera la respuesta 3 s menos que esto. */
        const val ANSWER_PAUSE_MS = 12000L

        /** Cada cuánto se lee el pico: la captura del motor guarda ~2048 muestras (~43 ms a 48 kHz). */
        const val POLL_MS = 40L
    }
}

/** El tipo de una ventana, con el paso que la cierra. */
enum class WindowKind(val step: String) {
    STIMULUS("estimulo"),
    CONTROL("control"),
}

/**
 * La ruta declarada de un estímulo, y cómo se verifica con el backend que el motor REPORTA.
 * El contrato no supone Android: [SYSTEM] es el backend del sistema de cada plataforma.
 */
enum class Route(val id: String) {
    /**
     * La salida del sistema (Oboe en Android, CoreAudio en iOS): cualquier cosa que no sea libusb.
     * Acepta [AudioBackendType.NONE] a propósito: el camino directo de Oboe (el que shippea en
     * Android) no pasa por `BackendManager`, que entonces no reporta ningún backend.
     */
    SYSTEM("sistema"),

    /** La placa USB por libusb (sólo Android). */
    LIBUSB("libusb"),
    ;

    fun carries(backend: AudioBackendType): Boolean = when (this) {
        SYSTEM -> backend != AudioBackendType.LIBUSB
        LIBUSB -> backend == AudioBackendType.LIBUSB
    }
}

/**
 * La semilla de la corrida (AC-053.10): la que mandó el script, o una que elige el plan — y lo dice
 * en `semilla-origen`, para que el juez sepa contra cuál verificar el orden.
 */
data class SeedChoice(val seed: Long, val origin: String) {
    companion object {
        fun of(requested: Long?, pick: () -> Long): SeedChoice =
            if (requested != null) SeedChoice(requested, "script") else SeedChoice(pick(), "app")
    }
}

/**
 * Las ventanas de cada panel, con SU orden (el de la semilla y el id del panel) y SU ruta: el
 * sistema en sf2/sf3, libusb en usb. Es el cableado que el plan no puede equivocar.
 */
class SeededWindows(private val windows: ListeningWindows, val seed: Long) {
    suspend fun overSystem(r: SmokeReporter, panel: String, fixture: String): Boolean =
        windows.run(r, panel, Route.SYSTEM, WindowOrder.of(seed, panel), fixture)

    suspend fun overUsb(r: SmokeReporter, fixture: String): Boolean =
        windows.run(r, Panel.USB.id, Route.LIBUSB, WindowOrder.of(seed, Panel.USB.id), fixture)
}

/**
 * AC-053.10 — el orden de las dos ventanas de un panel sale de la semilla que manda el script.
 *
 * La MISMA cuenta que `window_order` en `scripts/smoke-device.sh`: FNV-1a de 32 bits sobre
 * `"<semilla>:<panel>"` y el bit 0 de `h xor (h ushr 16)`. El juez recalcula el orden con la semilla
 * registrada y da FAIL si la app corrió otro; los dos lados fijan los mismos vectores en sus tests.
 */
object WindowOrder {
    fun of(seed: Long, panel: String): List<WindowKind> {
        var h = 2166136261u
        for (b in "$seed:$panel".encodeToByteArray()) {
            h = h xor (b.toUInt() and 0xFFu)
            h *= 16777619u
        }
        return if (((h shr 16) xor h) and 1u == 0u) {
            listOf(WindowKind.STIMULUS, WindowKind.CONTROL)
        } else {
            listOf(WindowKind.CONTROL, WindowKind.STIMULUS)
        }
    }
}
