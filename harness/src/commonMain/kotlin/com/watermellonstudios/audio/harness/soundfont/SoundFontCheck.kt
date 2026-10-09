package com.watermellonstudios.audio.harness.soundfont

import com.watermellonstudios.audio.domain.AudioBackendType
import com.watermellonstudios.audio.harness.smoke.SmokeReporter
import kotlinx.coroutines.delay
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * MINI-038 — lo que el panel SoundFont y el plan automático necesitan del motor, y NADA más.
 *
 * Existe para que [SoundFontCheck] se pueda probar en commonTest con un fake: `IAudioNativeBridge`
 * tiene cientos de métodos y un fake de todo eso no se escribe. La implementación de verdad es
 * [BridgeSoundFontPort].
 */
interface SoundFontPort {
    fun load(path: String): Boolean
    fun isLoaded(): Boolean
    fun unload()
    fun presetCount(): Int
    fun presetName(index: Int): String?
    fun bankProgram(index: Int): IntArray?
    fun setPreset(index: Int)

    /** Arranca el motor si no está andando. `false` si no arrancó. */
    suspend fun ensureEngineRunning(): Boolean
    fun engineType(): Int
    fun setEngineType(type: Int)
    fun noteOn(midiNote: Int, velocity: Float)
    fun noteOff()

    /** El pico absoluto de la salida final del motor (lo que recibe el device), en el último bloque capturado. */
    fun outputPeak(): Float

    /** La posición del transport en frames: avanza en cada bloque de audio renderizado. */
    fun playFrame(): Long

    /** REQ-053 S3: el backend que el motor REPORTA (no el que se pidió): la ruta por la que sale. */
    fun backend(): AudioBackendType
}

/**
 * Las verificaciones del SoundFont, cada una con su línea `HARNESS-SMOKE`. Las usan igual el botón
 * de la UI y el plan por adb: el veredicto de la pantalla y el del script son el mismo número.
 *
 * Nada acá dice "OK" porque una llamada no tiró:
 * - una carga vale si **cargó Y tiene presets** — un `true` con cero presets no es un SoundFont que
 *   se pueda tocar;
 * - una nota vale si **el control está en silencio, la nota sube el pico y los frames avanzan** —
 *   el pico solo no alcanza (el último bloque capturado puede ser viejo si el render se paró) y los
 *   frames solos tampoco (el render avanza en silencio).
 */
class SoundFontCheck(
    private val port: SoundFontPort,
    private val pause: suspend (Long) -> Unit = { delay(it) },
) {

    /** Carga por path. Emite `step=carga` y, si cargó, `step=preset` con el preset 0. */
    fun load(reporter: SmokeReporter, panel: String, path: String, label: String): Boolean {
        val loaded = port.load(path)
        val presets = if (loaded) port.presetCount() else 0
        val ok = reporter.report(
            panel, "carga", loaded && presets > 0,
            "archivo" to label, "cargado" to loaded, "presets" to presets,
        )
        if (!ok) return false
        val name = port.presetName(0)
        val bp = port.bankProgram(0)
        // El preset es parte de "cargó": un font cuyo preset 0 no tiene nombre ni bank/program no
        // es uno que la UI pueda mostrar ni el plan tocar, y la pantalla no puede decir "cargado"
        // mientras el juez dice FAIL.
        return reporter.report(
            panel, "preset", name != null && bp != null && bp.size == 2,
            "indice" to 0, "nombre" to name, "bank" to bp?.getOrNull(0), "program" to bp?.getOrNull(1),
        )
    }

    /**
     * AC-3: un archivo que NO es SoundFont tiene que dar el fallo, no un crash y no un "cargado".
     * El `ok` de esta línea es que la carga FALLÓ. `queda-cargado` es informativo: si antes había
     * otro font cargado, que siga cargado no es un defecto de este rechazo.
     */
    fun loadRejects(reporter: SmokeReporter, panel: String, path: String, label: String): Boolean {
        val loaded = port.load(path)
        val stillLoaded = port.isLoaded()
        if (loaded) port.unload()
        return reporter.report(
            panel, "no-soundfont", !loaded,
            "archivo" to label, "cargado" to loaded, "queda-cargado" to stillLoaded,
        )
    }

    /** Toca [midiNote] con el preset [presetIndex] y mide que suene. Emite `step=nota`. */
    suspend fun playNote(
        reporter: SmokeReporter,
        panel: String,
        presetIndex: Int = 0,
        midiNote: Int = NOTE,
    ): Boolean {
        if (!port.isLoaded()) return reporter.report(panel, "nota", false, "motivo" to "sin-soundfont")
        if (!port.ensureEngineRunning()) return reporter.report(panel, "nota", false, "motivo" to "motor-no-arranca")

        val previousType = port.engineType()
        try {
            port.setEngineType(ENGINE_SOUNDFONT)
            port.setPreset(presetIndex)
            pause(SETTLE_MS)
            val before = port.outputPeak()
            val frame0 = port.playFrame()
            port.noteOn(midiNote, VELOCITY)
            pause(HOLD_MS)
            val during = port.outputPeak()
            val frames = port.playFrame() - frame0

            val quietControl = before < MIN_PEAK
            val sounds = during >= MIN_PEAK
            val advances = frames > 0
            val motivo = when {
                !quietControl -> "control-no-silencioso"
                !advances -> "frames-quietos"
                !sounds -> "sin-senal"
                else -> null
            }
            return reporter.report(
                panel, "nota", motivo == null,
                "nota" to midiNote, "preset" to presetIndex,
                "pico-antes" to formatPeak(before), "pico-durante" to formatPeak(during), "frames" to frames,
                "motivo" to motivo,
            )
        } finally {
            port.noteOff()
            port.setEngineType(previousType)
        }
    }

    /** Descarga. Emite `step=descarga`: vale si después no queda nada cargado. */
    fun unload(reporter: SmokeReporter, panel: String): Boolean {
        port.unload()
        val still = port.isLoaded()
        return reporter.report(panel, "descarga", !still, "queda-cargado" to still)
    }

    /**
     * La secuencia de un fixture: carga, nota, [listen] y descarga (la descarga corre aunque la nota
     * falle). [listen] (REQ-053 S3, las ventanas de escucha) corre con el font CARGADO, y sólo si cargó.
     */
    suspend fun runFixture(
        reporter: SmokeReporter,
        panel: String,
        path: String,
        label: String,
        listen: (suspend () -> Boolean)? = null,
    ): Boolean {
        if (!load(reporter, panel, path, label)) {
            if (port.isLoaded()) unload(reporter, panel)
            return false
        }
        val note = playNote(reporter, panel)
        var unloaded = false
        val listened = try {
            listen?.invoke() ?: true
        } finally {
            unloaded = unload(reporter, panel)
        }
        return note && listened && unloaded
    }

    companion object {
        /** `EngineTypeId::SOUNDFONT` de `engines/SynthEngine.h`. Kotlin no tiene enum de engines. */
        const val ENGINE_SOUNDFONT = 6
        const val NOTE = 69
        const val VELOCITY = 0.8f

        /** −40 dBFS. El fixture a velocity 0,8 rinde del orden de −15 dB RMS en host. */
        const val MIN_PEAK = 0.01f
        const val SETTLE_MS = 200L
        const val HOLD_MS = 400L

        /** Pico sobre un buffer de muestras. */
        fun peak(samples: FloatArray, count: Int): Float {
            var p = 0f
            for (i in 0 until count.coerceAtMost(samples.size)) {
                val a = abs(samples[i])
                if (a > p) p = a
            }
            return p
        }
    }
}

/** Un pico con cuatro decimales, sin depender del locale (lo lee el script). */
internal fun formatPeak(peak: Float): String {
    if (peak.isNaN() || peak.isInfinite()) return peak.toString()
    // Redondeado y no truncado: 0,3f * 10000 da 2999,9998 en Float.
    val scaled = (peak * 10000f).roundToInt()
    val whole = scaled / 10000
    val frac = (scaled % 10000).toString().padStart(4, '0')
    return "$whole.$frac"
}
