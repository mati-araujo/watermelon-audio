package com.watermellonstudios.audio.harness

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.material3.Card
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.watermellonstudios.audio.api.AudioEngineFactory
import com.watermellonstudios.audio.api.AudioInputFactory
import com.watermellonstudios.audio.api.InternalWatermelonApi
import com.watermellonstudios.audio.harness.smoke.HarnessSmoke
import com.watermellonstudios.audio.harness.smoke.ListeningWindows
import com.watermellonstudios.audio.harness.smoke.SmokePlanRunner
import com.watermellonstudios.audio.harness.smoke.SmokeReporter
import com.watermellonstudios.audio.harness.smoke.SmokeSink
import com.watermellonstudios.audio.harness.smoke.stopEngineForUsb
import com.watermellonstudios.audio.harness.soundfont.BridgeSoundFontPort
import com.watermellonstudios.audio.harness.soundfont.Fixtures
import com.watermellonstudios.audio.harness.soundfont.SoundFontCheck
import com.watermellonstudios.audio.internal.bridge.getAudioBridge
import kotlinx.coroutines.launch

/**
 * WA-5.5 — la raiz del harness, compartida por Android e iOS.
 *
 * Vive entera en commonMain a proposito: `AudioEngineFactory.create()` no pide
 * Context ni nada de plataforma, asi que la superficie que el harness ejercita
 * es exactamente la que consume un cliente KMP. Un shell por plataforma
 * (MainActivity / MainViewController) es todo lo que hay afuera.
 *
 * Esta version es el esqueleto: transporte y lectura de estado, que es lo minimo
 * que prueba que la cadena entera esta viva —Compose -> commonMain -> bridge ->
 * C API -> C++— en las dos plataformas. Los otros seis controles de la propuesta
 * (pad XY, rack de efectos, MONITOR DE ENTRADA, looper, metronomo, diagnostico)
 * van encima de este mismo andamio.
 *
 * La UI es fea y va a seguir siendo fea hasta que exista el design system. Es un
 * requisito de la etapa, no una concesion: si el harness espera al design
 * system, la pregunta de si el input path de iOS captura se sigue sin contestar
 * mientras tanto.
 *
 * ## MINI-038: el slot de plataforma y el smoke por adb
 *
 * [platform] trae lo que sólo existe en una plataforma (USB, el selector por fd, a dónde van las
 * líneas `HARNESS-SMOKE`) sin expect/actual: ver [HarnessPlatform]. Si el shell pasó un
 * [SmokeRequest], la corrida automática arranca sola al componer; los botones de los paneles
 * emiten las mismas líneas con `run=ui`.
 */
@OptIn(InternalWatermelonApi::class)
@Composable
fun HarnessApp(platform: HarnessPlatform) {
    val scope = rememberCoroutineScope()

    // El motor sobrevive a las recomposiciones y se libera con la pantalla. Sin
    // el release() cada rotacion en Android dejaria un motor vivo con su stream.
    val engine = remember { AudioEngineFactory.create() }
    DisposableEffect(Unit) {
        onDispose { engine.release() }
    }

    val state by engine.state.collectAsState()

    // Las líneas HARNESS-SMOKE van al sink de la plataforma Y a la vista de abajo: la pantalla y
    // el log no pueden contar dos historias distintas. La pantalla sólo CALLA lo que tiene que ser
    // ciego para el oyente (D7): qué ventana fue estímulo y cuál control, y la semilla.
    val smokeLines = remember { mutableStateListOf<String>() }
    val sink = remember {
        SmokeSink { line ->
            platform.smokeSink.emit(line)
            smokeLines.add(HarnessSmoke.forScreen(line))
            while (smokeLines.size > MAX_SMOKE_LINES) smokeLines.removeAt(0)
        }
    }
    val uiReporter = remember { SmokeReporter(sink, run = "ui") }
    val bridge = remember { getAudioBridge() }
    val sfPort = remember { BridgeSoundFontPort(bridge, engine) }
    val sfCheck = remember { SoundFontCheck(sfPort) }
    val fixtures = remember { Fixtures(platform.writeFile) }

    platform.smokeRequest?.let { request ->
        LaunchedEffect(request) {
            SmokePlanRunner(
                engine = engine,
                input = AudioInputFactory.create(),
                playFrame = { bridge.transportGetPlayFrame() },
                engineState = { bridge.getEngineState() },
                soundFont = sfCheck,
                fixtures = fixtures,
                usb = platform.usbSmoke,
                windows = ListeningWindows(sfPort),
            ).run(request.plan, SmokeReporter(sink, run = request.run), request.problem, request.seed)
        }
    }

    MaterialTheme {
        Surface(modifier = Modifier.fillMaxSize()) {
            Column(
                modifier = Modifier
                    .padding(16.dp)
                    .verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text("Watermelon Audio — harness", style = MaterialTheme.typography.titleLarge)

                Text("lifecycle: ${state.lifecycle}")
                Text("paused: ${state.isPaused}   ·   fading: ${state.isFading}")
                Text("osc: ${state.oscillator}   ·   ${state.frequency} Hz")
                Text("stream: ${state.streamInfo ?: "—"}")
                Text("error: ${state.error ?: "—"}")

                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = { scope.launch { engine.start() } }) { Text("start") }
                    Button(onClick = { scope.launch { engine.stop() } }) { Text("stop") }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = { scope.launch { engine.pause() } }) { Text("pause") }
                    Button(onClick = { scope.launch { engine.resume() } }) { Text("resume") }
                }

                // Control 1 de 7 — el que justifica el proyecto. Va primero
                // porque es la unica pregunta abierta que no puede contestar
                // ningun test: si el input path de iOS captura de verdad.
                InputMonitorControl()

                // Control 2 de 7 — el unico camino de tiempo real del programa.
                XYPadControl(engine)

                // Control 3 de 7 — rack de efectos. Mezcla API publica (efectos)
                // con superficie de diagnostico (routing), que es exactamente lo
                // que valida la decision del opt-in.
                EffectRackControl(engine)

                // Control 5 de 7 — tira de looper. Muestra los VALORES DEVUELTOS
                // (arm, prepare, export), que es donde estaban los tres bugs de
                // WA-2.6 que un boton de "listo" no habria visto nunca.
                LooperStripControl()

                // Control 6 de 7 — metronomo. Existe sobre todo por el item 5 del
                // smoke: el off-by-one del click es el unico cambio de WA-2.6 que
                // altera algo que ya sonaba bien, y eso hay que escucharlo.
                MetronomeControl()

                // Control 7 de 7 — diagnostico. Primer usuario real de
                // @InternalWatermelonApi y de la captura de logs.
                DiagnosticsControl()

                // Control 8 — modos. No estaba en la propuesta original de los 7;
                // lo pidio el smoke: `Category.MODE` de WA-1.4 tiene UN solo call
                // site (`setAudioMode`) y era el unico de los 26 que ninguna
                // pantalla podia alcanzar. Ya encontro una divergencia iOS/Android.
                ModeControl()

                // Control 9 — afinador. Ademas de probar el afinador en el
                // dispositivo, es EL CONTROL DE AC-010.1: no tiene @OptIn, asi
                // que su compilacion afirma que `TunerFactory` alcanza sin tocar
                // la superficie interna. Ver el KDoc de TunerControl.
                TunerControl()

                // Control 10 — SoundFont (MINI-038): los fixtures .sf2/.sf3 generados, y en
                // Android el selector del sistema por fd.
                SoundFontControl(sfPort, sfCheck, fixtures, uiReporter, platform.soundFontExtras)

                // Control 11 — USB (MINI-038). Sólo Android lo tiene; en iOS el lugar lo dice.
                val usbPanel = platform.usbPanel
                if (usbPanel != null) {
                    usbPanel(uiReporter) { stopEngineForUsb(engine, { bridge.getEngineState() }, uiReporter) }
                } else {
                    Card(modifier = Modifier.fillMaxWidth()) {
                        Text(
                            USB_NOT_APPLICABLE,
                            modifier = Modifier.padding(12.dp),
                            style = MaterialTheme.typography.bodyMedium,
                        )
                    }
                }

                // Las últimas líneas HARNESS-SMOKE, tal cual las ve el script.
                SmokeLogView(smokeLines)
            }
        }
    }
}

/** El texto del lugar del panel USB cuando la plataforma no lo tiene. AC-5 lo busca tal cual. */
const val USB_NOT_APPLICABLE = "USB no aplica en iOS"

private const val MAX_SMOKE_LINES = 60

@Composable
private fun SmokeLogView(lines: List<String>) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text("HARNESS-SMOKE", style = MaterialTheme.typography.titleMedium)
            if (lines.isEmpty()) {
                Text("sin líneas todavía", style = MaterialTheme.typography.bodySmall)
            }
            lines.asReversed().forEach { line ->
                Text(
                    line.removePrefix("HARNESS-SMOKE "),
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                )
            }
        }
    }
}
