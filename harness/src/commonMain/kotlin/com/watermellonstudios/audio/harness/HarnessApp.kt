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
            }
        }
    }
}
