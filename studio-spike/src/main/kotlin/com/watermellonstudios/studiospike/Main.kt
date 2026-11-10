package com.watermellonstudios.studiospike

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.BasicText
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.PointerEventType
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Window
import androidx.compose.ui.window.application
import kotlinx.coroutines.delay
import java.io.File
import kotlin.system.exitProcess

/**
 * Espiga de S3. Sin argumentos: pad x/y + selector de engine + panel de diagnóstico.
 *
 *   --measure <reporte.txt>   corre la medición de 3.5 (Measure.kt) y sale (0 = criterio OK)
 *   --minutes <n>             duración de la fase de xruns (default 10)
 *   --touches <n>             toques de la fase de latencia (default 300)
 *   --volume <0..1>           master (default 0,05: bajo, corre en los parlantes)
 *   --soundfont <ruta.sf2|sf3> para el engine SOUNDFONT
 *   --native-dir <dir>        dylibs fuera del bundle (desarrollo); default: recursos de la app
 */
fun main(args: Array<String>) {
    val opts = parseArgs(args)
    val nativeDir = SpikeEngine.resolveNativeDir(opts["native-dir"])
    val loaded = if (nativeDir == null) {
        Result.failure(IllegalStateException("sin compose.application.resources.dir ni --native-dir"))
    } else {
        SpikeEngine.load(nativeDir)
    }
    val engine = loaded.getOrElse { e ->
        val msg = "CARGA FALLIDA: ${e::class.java.simpleName}: ${e.message}"
        System.err.println(msg)
        opts["measure"]?.let { File(it).apply { parentFile?.mkdirs() }.writeText(msg + "\n") }
        if (opts.containsKey("measure")) exitProcess(3)
        application { Window(onCloseRequest = ::exitApplication, title = "Studio spike") { Label(msg) } }
        return
    }
    val volume = opts["volume"]?.toFloatOrNull() ?: 0.05f
    opts["soundfont"]?.let { engine.loadSoundFont(it) }
    val started = engine.start(volume)
    if (started.isFailure) {
        val msg = "startEngine FALLO: ${started.exceptionOrNull()}"
        System.err.println(msg)
        opts["measure"]?.let { File(it).writeText(msg + "\n") }
        exitProcess(4)
    }

    val measureOut = opts["measure"]
    if (measureOut != null) {
        runMeasure(engine, File(measureOut), opts)
        return
    }
    application {
        Window(onCloseRequest = { engine.stop(); exitApplication() }, title = "NoisyPad Studio — espiga S3") {
            SpikeScreen(engine)
        }
    }
}

private fun runMeasure(engine: SpikeEngine, out: File, opts: Map<String, String>) {
    var status by mutableStateOf("midiendo…")
    val worker = Thread {
        val ok = runCatching {
            Measure(
                engine, out,
                xrunMinutes = opts["minutes"]?.toDoubleOrNull() ?: 10.0,
                touches = opts["touches"]?.toIntOrNull() ?: 300,
            ) { status = it }.run()
        }.getOrElse { e ->
            out.appendText("EXCEPCION: $e\n")
            false
        }
        engine.stop()
        exitProcess(if (ok) 0 else 1)
    }
    application {
        Window(onCloseRequest = { exitProcess(2) }, title = "Studio spike — midiendo") {
            LaunchedEffect(Unit) { worker.start() }
            Label(status)
        }
    }
}

private fun parseArgs(args: Array<String>): Map<String, String> {
    val m = mutableMapOf<String, String>()
    var i = 0
    while (i < args.size) {
        val a = args[i]
        if (a.startsWith("--")) {
            val v = args.getOrNull(i + 1)?.takeUnless { it.startsWith("--") }
            m[a.removePrefix("--")] = v ?: "true"
            i += if (v != null) 2 else 1
        } else {
            i++
        }
    }
    return m
}

@Composable
private fun Label(text: String) = BasicText(text, style = TextStyle(fontSize = 13.sp, color = Color.White),
    modifier = Modifier.background(Color(0xFF202020)).padding(8.dp))

@Composable
private fun SpikeScreen(engine: SpikeEngine) {
    var selected by remember { mutableStateOf(engine.engineType) }
    var cursor by remember { mutableStateOf<Offset?>(null) }
    var diag by remember { mutableStateOf("") }
    LaunchedEffect(Unit) {
        while (true) {
            diag = diagnostics(engine)
            delay(500)
        }
    }
    Column(Modifier.fillMaxSize().background(Color(0xFF141414)).padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            ENGINES.forEachIndexed { i, name ->
                val on = i == selected
                BasicText(name, style = TextStyle(fontSize = 12.sp, color = if (on) Color.Black else Color.White),
                    modifier = Modifier.background(if (on) Color(0xFF7CFC8A) else Color(0xFF333333))
                        .clickable { engine.selectEngine(i); selected = i }.padding(8.dp))
            }
        }
        Box(Modifier.fillMaxWidth().height(380.dp).border(1.dp, Color(0xFF555555))
            .pointerInput(Unit) {
                awaitPointerEventScope {
                    while (true) {
                        val ev = awaitPointerEvent()
                        val p = ev.changes.firstOrNull() ?: continue
                        val x = (p.position.x / size.width).coerceIn(0f, 1f)
                        val y = (p.position.y / size.height).coerceIn(0f, 1f)
                        when (ev.type) {
                            PointerEventType.Press -> { engine.press(x, y); cursor = p.position }
                            PointerEventType.Move -> if (p.pressed) { engine.drag(x, y); cursor = p.position }
                            PointerEventType.Release -> { engine.release(); cursor = null }
                            else -> Unit
                        }
                    }
                }
            }) {
            Canvas(Modifier.fillMaxSize()) {
                cursor?.let { drawCircle(Color(0xFF7CFC8A), radius = 18f, center = it) }
            }
        }
        BasicText(diag, style = TextStyle(fontSize = 12.sp, color = Color(0xFFCCCCCC)))
    }
}

private fun diagnostics(engine: SpikeEngine): String {
    val d = DeviceInfo.read()
    val s = RenderStats.read()
    val t = readTouchSamples().orEmpty().filter { it.bufferHostNs > 0 }
    val lat = if (t.isEmpty() || d == null) "sin toques" else {
        val v = t.map { (it.bufferHostNs - it.touchNs) / 1e6 + d.hostToDacMs }.sorted()
        "toque->DAC (software) p50=%.2f max=%.2f ms (n=%d)".format(v[v.size / 2], v.last(), v.size)
    }
    return buildString {
        appendLine("salida RMS: %.1f dBFS".format(20 * kotlin.math.log10(engine.outputRms().coerceAtLeast(1e-10f))))
        d?.let {
            appendLine("HAL: %.0f Hz, buffer %d (pedido %d), latencia declarada %.2f ms".format(
                it.rate, it.bufferActual, it.bufferRequested, it.totalOutputMs))
        }
        s?.let {
            appendLine("render: %d bloques, overloads HAL %d, discontinuidades %d, sobre presupuesto %d, medio %.1f us, max %.1f us".format(
                it.blocks, it.halOverloads, it.discontinuities, it.overBudget, it.meanRenderUs, it.maxRenderNs / 1e3))
        }
        append(lat)
    }
}
