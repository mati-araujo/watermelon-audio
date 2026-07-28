package com.watermellonstudios.audio.harness

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.watermellonstudios.audio.api.AudioEngineFactory
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
 */
@Composable
fun HarnessApp() {
    val scope = rememberCoroutineScope()

    // El motor sobrevive a las recomposiciones y se libera con la pantalla. Sin
    // el release() cada rotacion en Android dejaria un motor vivo con su stream.
    val engine = remember { AudioEngineFactory.create() }
    DisposableEffect(Unit) {
        onDispose { engine.release() }
    }

    val state by engine.state.collectAsState()

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
            }
        }
    }
}
