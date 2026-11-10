package com.watermellonstudios.studiospike

import java.io.File
import java.util.Locale
import javax.swing.SwingUtilities
import kotlin.math.log10
import kotlin.math.sin
import kotlin.random.Random

/**
 * Medición automatizada de 3.5 (REQ-055 S3). Corre DENTRO de la app empaquetada (mismo
 * hardened runtime, mismo dylib) y escribe un reporte de texto con números crudos.
 *
 * Método, en tres fases:
 *
 * 1. **7/7 engines suenan desde la JVM.** Piso: RMS de salida con el synth a volumen 0
 *    durante 1,5 s. Por engine: se selecciona por el bridge, se toca (pad simulado, mismo
 *    código que el pointer handler) 3 s barriendo X/Y (SOUNDFONT: notas), y se registra el
 *    RMS de salida máximo (`getOutputRmsLevel`, post master). Suena = máx ≥ piso + 20 dB y
 *    ≥ -70 dBFS. El master está BAJO (`--volume`, default 0,05): corre en los parlantes.
 * 2. **Toque→sonido, cadena software.** N toques simulados en el EDT de AWT (el thread donde
 *    Compose Desktop despacha el pointer input), cada uno con el `press` del pad. El nativo
 *    estampa el reloj mach al final del press; el primer render block posterior registra su
 *    entrada y el `mHostTime` de su AudioTimeStamp. De ahí:
 *      toque→render  = entrada al bloque − toque
 *      toque→buffer  = mHostTime − toque
 *      toque→DAC     = toque→buffer + (latencia de dispositivo + de stream) que reporta el HAL
 *      toque→DAC+so  = lo anterior + safety offset (cota pesimista)
 *    SIN MEDIR: HID → ventana → evento AWT → Compose (antes del handler), el DAC y el aire.
 * 3. **Xruns, 10 min a 256 frames.** Se ponen a cero los contadores y se toca sin parar
 *    (toques de 80–400 ms, cambio de engine cada 30 s). Xrun = overload del HAL
 *    (`kAudioDeviceProcessorOverload`) o discontinuidad del reloj de muestras del render
 *    block. Se informan además los bloques cuyo render excedió su presupuesto.
 */
class Measure(
    private val engine: SpikeEngine,
    private val out: File,
    private val xrunMinutes: Double,
    private val touches: Int,
    private val status: (String) -> Unit,
) {
    private val report = StringBuilder()
    private fun line(s: String = "") {
        report.appendLine(s)
        println(s)
        status(s)
    }

    private fun onEdt(block: () -> Unit) = SwingUtilities.invokeAndWait(block)
    private fun sleep(ms: Long) = Thread.sleep(ms)  // estímulo: la duración ES el experimento
    private fun db(x: Float) = if (x > 0f) 20.0 * log10(x.toDouble()) else -200.0
    private fun f(x: Double, d: Int = 2) = String.format(Locale.ROOT, "%.${d}f", x)

    /** Devuelve true si las tres fases dan dentro del criterio de S3. */
    fun run(): Boolean {
        line("# REQ-055 S3 — medicion de la espiga (${java.time.OffsetDateTime.now()})")
        line("nativos: ${engine.nativeDir}")
        line("java: ${System.getProperty("java.vendor")} ${System.getProperty("java.version")}")
        val info = DeviceInfo.read()
        if (info == null) {
            line("FALLA: sin datos del dispositivo (el backend macOS no abrió)")
            return finish(false)
        }
        line(
            "dispositivo: id=${info.deviceId} tasa nominal=${f(info.nominalRate, 0)} Hz stream=${f(info.rate, 0)} Hz buffer pedido=${info.bufferRequested} " +
                "efectivo=${info.bufferActual} rango=[${info.bufferMin}..${info.bufferMax}] " +
                "setStatus=${info.bufferSetStatus} listenerStatus=${info.listenerStatus}",
        )
        line(
            "latencias HAL (frames): dispositivo=${info.deviceLatency} safety=${info.safetyOffset} " +
                "stream=${info.streamLatency} buffer=${info.bufferActual} -> total ${f(info.totalOutputMs)} ms; " +
                "presentationLatency AVAudioEngine=${f(info.presentationLatencySec * 1000)} ms",
        )
        engine.bridge.getStreamInfo()?.let { (sr, buf, lat) ->
            line("stream del motor: sr=$sr buffer=$buf outputLatencyMs=${f(lat.toDouble())}")
        }
        val okEngines = phaseEngines()
        val okLatency = phaseLatency(info)
        val okXruns = phaseXruns(info)
        line()
        line("VEREDICTO: engines=${if (okEngines) "OK" else "FALLA"} toque->sonido=${if (okLatency) "OK" else "FALLA"} xruns=${if (okXruns) "OK" else "FALLA"}")
        return finish(okEngines && okLatency && okXruns)
    }

    private fun finish(ok: Boolean): Boolean {
        out.parentFile?.mkdirs()
        out.writeText(report.toString())
        return ok
    }

    private fun phaseEngines(): Boolean {
        line()
        line("## 1. 7/7 engines (RMS de salida, dBFS, post master)")
        onEdt { engine.release() }
        sleep(400)
        var floor = 0f
        repeat(75) { floor = maxOf(floor, engine.outputRms()); sleep(20) }
        val floorDb = db(floor)
        line("piso (synth a 0, 1,5 s): ${f(floorDb, 1)} dBFS")
        var all = true
        for (type in ENGINES.indices) {
            if (type == SOUNDFONT && !engine.soundFontLoaded) {
                line("${ENGINES[type]}: SIN SOUNDFONT CARGADO -> FALLA")
                all = false
                continue
            }
            onEdt { engine.selectEngine(type) }
            sleep(300)
            // Que suene NO alcanza: tiene que ser el engine pedido (review de S3).
            val actualType = engine.bridge.getEngineType()
            if (actualType != type) {
                line("${ENGINES[type]}: el motor quedo en ${ENGINES.getOrElse(actualType) { "$actualType" }} -> FALLA")
                all = false
                continue
            }
            var peak = 0f
            val levels = mutableListOf<Double>()
            val t0 = System.nanoTime()
            var pressed = false
            var i = 0
            while (System.nanoTime() - t0 < 3_000_000_000L) {
                val t = (System.nanoTime() - t0) / 1e9
                val x = (0.5 + 0.4 * sin(t * 1.7)).toFloat()
                val y = (0.5 + 0.4 * sin(t * 1.1 + 1.0)).toFloat()
                // SOUNDFONT: nota nueva cada 0,5 s; el resto, un toque sostenido barriendo.
                if (!pressed || (type == SOUNDFONT && i % 25 == 0)) {
                    onEdt { if (type == SOUNDFONT) engine.release(); engine.press(x, y) }
                    pressed = true
                } else {
                    onEdt { engine.drag(x, y) }
                }
                val rms = engine.outputRms()
                peak = maxOf(peak, rms)
                levels += db(rms)
                i++
                sleep(20)
            }
            onEdt { engine.release() }
            val peakDb = db(peak)
            // Sostenido, no un transitorio: al menos la mitad de las lecturas de los 3 s
            // por encima de -70 dBFS y de piso + 20 dB (con piso en silencio digital, el
            // +20 dB solo no exige nada).
            val threshold = maxOf(-70.0, floorDb + 20.0)
            val above = levels.count { it >= threshold }.toDouble() / levels.size.coerceAtLeast(1)
            val ok = above >= 0.5
            all = all && ok
            line("${ENGINES[type]} (getEngineType=$actualType): max RMS ${f(peakDb, 1)} dBFS (piso + ${f(peakDb - floorDb, 1)} dB), " +
                "${f(above * 100, 0)} % de ${levels.size} lecturas >= ${f(threshold, 1)} dBFS -> ${if (ok) "SUENA" else "FALLA"}")
            sleep(300)
        }
        return all
    }

    private fun phaseLatency(info: DeviceInfo): Boolean {
        line()
        line("## 2. toque->sonido (cadena software, $touches toques en el EDT)")
        onEdt { engine.selectEngine(0) }
        if (!resetApplied()) {
            line("FALLA: el RT no aplico el reset en 2 s (no hay render)")
            return false
        }
        val rnd = Random(55)
        repeat(touches) {
            val x = rnd.nextFloat(); val y = rnd.nextFloat()
            onEdt { engine.press(x, y) }
            sleep(30L + rnd.nextLong(50))
            onEdt { engine.release() }
            sleep(20L + rnd.nextLong(40))
        }
        sleep(100)
        val samples = readTouchSamples().orEmpty()
        val marks = RenderStats.read()?.touchMarks ?: -1
        line("toques: $touches pedidos, $marks estampados, ${samples.size} registrados por el render")
        val valid = samples.filter { it.bufferHostNs > 0 }
        line("con mHostTime valido: ${valid.size}")
        // Un toque pisado antes de llegar a un bloque es, por construccion, de los lentos:
        // si falta alguno, el percentil no vale.
        val complete = marks == touches.toLong() && samples.size == touches && valid.size == samples.size
        if (!complete) line("FALLA: faltan toques (pisados o sin mHostTime); los percentiles no valen")
        if (valid.isEmpty()) return false
        fun ms(xs: List<Long>) = xs.map { it / 1e6 }.sorted()
        val toRender = ms(samples.map { it.renderEntryNs - it.touchNs })
        val toBuffer = ms(valid.map { it.bufferHostNs - it.touchNs })
        val toDac = toBuffer.map { it + info.hostToDacMs }
        val toDacSo = toDac.map { it + info.safetyOffset / info.rate * 1000.0 }
        fun row(name: String, v: List<Double>) = line(
            "$name: p50=${f(pct(v, 50.0))} p95=${f(pct(v, 95.0))} p99=${f(pct(v, 99.0))} max=${f(v.last())} min=${f(v.first())} ms",
        )
        row("toque->render", toRender)
        row("toque->buffer (mHostTime)", toBuffer)
        row("toque->DAC (HAL dev+stream ${f(info.hostToDacMs)} ms)", toDac)
        row("toque->DAC + safety offset", toDacSo)
        line("frames por bloque en los toques: ${samples.map { it.frames }.distinct().sorted()}")
        // Semantica de mHostTime: si es el instante en que el bloque entra al dispositivo,
        // mHostTime - entrada al render ~= buffer + safety offset.
        val ahead = ms(valid.map { it.bufferHostNs - it.renderEntryNs })
        line("mHostTime - entrada al render: p50=${f(pct(ahead, 50.0))} ms; buffer + safety = " +
            "${f((info.bufferActual + info.safetyOffset) / info.rate * 1000.0)} ms")
        line("SIN MEDIR: HID -> evento de ventana -> AWT -> Compose (antes del handler); la rampa de " +
            "mSynthVolume dentro del primer bloque (el inicio audible puede caer hasta un bloque despues " +
            "de mHostTime); DAC -> aire")
        return complete && pct(toDacSo, 99.0) <= 20.0
    }

    private fun phaseXruns(info: DeviceInfo): Boolean {
        line()
        line("## 3. xruns, ${f(xrunMinutes, 1)} min tocando (buffer efectivo ${info.bufferActual} frames)")
        if (!resetApplied()) {
            line("FALLA: el RT no aplico el reset en 2 s (no hay render)")
            return false
        }
        val rnd = Random(3)
        val end = System.nanoTime() + (xrunMinutes * 60e9).toLong()
        val t0 = System.nanoTime()
        var lastEngine = -1
        var nextReport = t0 + 60_000_000_000L
        while (System.nanoTime() < end) {
            val elapsed = (System.nanoTime() - t0) / 1e9
            val type = ((elapsed / 30.0).toInt()) % ENGINES.size
            val target = if (type == SOUNDFONT && !engine.soundFontLoaded) 0 else type
            if (target != lastEngine) {
                onEdt { engine.selectEngine(target) }
                lastEngine = target
            }
            val x = rnd.nextFloat(); val y = rnd.nextFloat()
            onEdt { engine.press(x, y) }
            val hold = 80L + rnd.nextLong(320)
            var held = 0L
            while (held < hold) {
                sleep(16); held += 16
                val dx = (x + held / 2000f).coerceIn(0f, 1f)
                onEdt { engine.drag(dx, y) }
            }
            onEdt { engine.release() }
            sleep(20L + rnd.nextLong(80))
            if (System.nanoTime() >= nextReport) {
                RenderStats.read()?.let { line("  t=${f(elapsed, 0)} s ${fmt(it)}") }
                nextReport += 60_000_000_000L
            }
        }
        onEdt { engine.release() }
        val s = RenderStats.read() ?: return false
        val seconds = (System.nanoTime() - t0) / 1e9
        val expected = seconds * info.rate / info.bufferActual.coerceAtLeast(1)
        line("final: ${fmt(s)}")
        line("bloques esperados a ${info.bufferActual} frames en ${f(seconds, 0)} s: ~${f(expected, 0)}; vistos ${s.blocks}")
        val xruns = s.halOverloads + s.discontinuities
        line("XRUNS = overloads HAL (${s.halOverloads}) + discontinuidades (${s.discontinuities}) = $xruns")
        // Cero xruns sólo vale si el instrumento podía verlos (review de S3): el stream
        // corrió todo el tiempo, con reloj de muestras, con listener, y a 256 frames.
        val checks = listOf(
            "bloques vistos dentro de ±1 % de los esperados" to (kotlin.math.abs(s.blocks - expected) <= 0.01 * expected),
            "todos los bloques con mSampleTime valido" to (s.sampleTimeInvalid == 0L),
            "listener de overload registrado" to (info.listenerStatus == 0),
            "buffer efectivo = 256 y todos los bloques de 256" to
                (info.bufferActual == 256 && s.minFrames == 256L && s.maxFrames == 256L),
        )
        checks.forEach { (name, ok) -> line("  ${if (ok) "ok" else "FALLA"}: $name") }
        return xruns == 0L && checks.all { it.second }
    }

    /** Pide el reset y espera a que el RT lo aplique (techo 2 s): polling con deadline. */
    private fun resetApplied(): Boolean {
        SpikeNative.reset()
        val deadline = System.nanoTime() + 2_000_000_000L
        while (System.nanoTime() < deadline) {
            if (readTouchSamples() != null) return true
            sleep(10)
        }
        return false
    }

    private fun fmt(s: RenderStats) =
        "bloques=${s.blocks} overloadsHAL=${s.halOverloads} discontinuidades=${s.discontinuities} " +
            "sinSampleTime=${s.sampleTimeInvalid} sobrePresupuesto=${s.overBudget} " +
            "render medio=${f(s.meanRenderUs, 1)} us max=${f(s.maxRenderNs / 1e3, 1)} us " +
            "frames=[${s.minFrames}..${s.maxFrames}]"

    private fun pct(sorted: List<Double>, p: Double): Double {
        if (sorted.isEmpty()) return Double.NaN
        val idx = ((p / 100.0) * (sorted.size - 1)).toInt().coerceIn(0, sorted.size - 1)
        return sorted[idx]
    }
}
