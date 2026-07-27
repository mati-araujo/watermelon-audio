package com.watermellonstudios.audio.harness

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.watermellonstudios.audio.api.InternalWatermelonApi
import com.watermellonstudios.audio.internal.bridge.getAudioBridge
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Control 5 de 7 — tira de looper.
 *
 * Drena cuatro ítems del smoke (5, 7, 8, 9), y tres de ellos son **valores de
 * retorno que antes nadie miraba**. Por eso esta tira muestra números y no estados:
 *
 * | Ítem | Qué se ve acá |
 * |---|---|
 * | 7 | `armAtNextBar()` devolviendo **-1** cuando el arm no prende, en vez de un trigger frame para una grabación que nunca arranca |
 * | 8 | `prepareTrackBars()` devolviendo **-1** con un `bars` que desborda int32, en vez de alocar una pista con el largo envuelto |
 * | 9 | `exportMix()` devolviendo **false** en vez de dejar escapar una excepción que abortaba el proceso |
 * | 5 | el reloj compartido con el metrónomo — `framesPerBar` es lo que cuantiza el largo del loop |
 *
 * ## El botón de "bars absurdo" no es una broma
 *
 * Es la única forma de disparar el camino del ítem 8 desde la UI, y ese camino sólo
 * se ve con conteos de barras absurdos. Un harness que sólo pruebe valores razonables
 * no lo toca nunca.
 *
 * ## Lo que esta tira NO hace
 *
 * No reproduce ni exporta audio de verdad **en este simulador**, porque para eso hace
 * falta que el motor esté corriendo con un stream — y la captura de entrada, que es
 * lo que llenaría una pista, está bloqueada por el cuelgue de `playAndRecord` del
 * simulador (§10). Lo que sí ejercita, y no dependía de nada de eso, es **el contrato
 * de los retornos**, que es donde estaban los tres bugs.
 */
@OptIn(InternalWatermelonApi::class)
@Composable
fun LooperStripControl(modifier: Modifier = Modifier) {
    val bridge = remember { getAudioBridge() }
    val scope = rememberCoroutineScope()

    var track by remember { mutableStateOf(0) }
    var bars by remember { mutableStateOf(2) }
    var lastPrepare by remember { mutableStateOf<String?>(null) }
    var lastArm by remember { mutableStateOf<String?>(null) }
    var lastExport by remember { mutableStateOf<String?>(null) }
    var recording by remember { mutableStateOf(false) }
    var playing by remember { mutableStateOf(false) }
    var trackActive by remember { mutableStateOf(false) }

    DisposableEffect(Unit) {
        onDispose {
            bridge.looperStopRecording()
            bridge.looperStopAll()
        }
    }

    LaunchedEffect(track) {
        while (true) {
            recording = bridge.looperIsRecording()
            playing = bridge.looperIsPlaying()
            trackActive = bridge.looperIsTrackActive(track)
            delay(100)
        }
    }

    Card(modifier = modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text("Looper", style = MaterialTheme.typography.titleMedium)

            Text(
                "grabando: $recording · sonando: $playing · pista $track activa: $trackActive",
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace,
            )

            // Los tres retornos, cada uno con su línea. Son el control.
            lastPrepare?.let { Mono(it) }
            lastArm?.let { Mono(it) }
            lastExport?.let { Mono(it) }

            Row(
                modifier = Modifier.horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                (0 until 4).forEach { i ->
                    Button(enabled = i != track, onClick = { track = i }) {
                        Text("pista $i", style = MaterialTheme.typography.labelSmall, maxLines = 1)
                    }
                }
            }

            Column {
                Text("compases: $bars", style = MaterialTheme.typography.bodySmall)
                Slider(
                    value = bars.toFloat(),
                    valueRange = 1f..16f,
                    steps = 14,
                    onValueChange = { bars = it.toInt() },
                )
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = {
                    val frames = bridge.looperPrepareTrackBars(track, bars, SAMPLE_RATE)
                    lastPrepare = "prepareTrackBars($track, $bars) = $frames" +
                        if (frames < 0) "  ← rechazado (ítem 8)" else " frames"
                }) { Text("preparar") }

                // Ítem 8: el único disparador del camino de desborde.
                Button(onClick = {
                    val frames = bridge.looperPrepareTrackBars(track, ABSURD_BARS, SAMPLE_RATE)
                    lastPrepare = "prepareTrackBars($track, $ABSURD_BARS) = $frames" +
                        if (frames < 0) "  ← rechazado, como debe (ítem 8)"
                        else "  ← ACEPTÓ un largo envuelto"
                }) { Text("bars absurdo") }
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                // Ítem 7: el valor devuelto es el control, no el hecho de armar.
                Button(onClick = {
                    val trigger = bridge.looperArmAtNextBar(track)
                    lastArm = "armAtNextBar($track) = $trigger" +
                        if (trigger < 0) "  ← no se armó nada (ítem 7)"
                        else "  ← frame de disparo"
                }) { Text("armar al compás") }

                Button(onClick = {
                    bridge.looperStartRecording(track)
                    recording = bridge.looperIsRecording()
                }) { Text("grabar") }
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = {
                    bridge.looperStopRecording()
                    recording = bridge.looperIsRecording()
                }) { Text("parar grabación") }

                Button(onClick = {
                    bridge.looperStopAll()
                    playing = bridge.looperIsPlaying()
                }) { Text("parar todo") }

                Button(onClick = {
                    bridge.looperClearAll()
                    trackActive = bridge.looperIsTrackActive(track)
                }) { Text("limpiar") }
            }

            // Ítem 9. El export es sincrónico y bloquea: va a IO, no al main thread.
            // Y la ruta a propósito NO es escribible, que es el caso que antes
            // abortaba el proceso en vez de devolver false.
            Button(onClick = {
                scope.launch {
                    val ok = withContext(Dispatchers.Default) {
                        bridge.looperExportMix(UNWRITABLE_PATH)
                    }
                    lastExport = "exportMix(ruta no escribible) = $ok" +
                        if (!ok) "  ← devolvió false sin tirar (ítem 9)" else ""
                }
            }) { Text("export a ruta imposible") }
        }
    }
}

@Composable
private fun Mono(text: String) = Text(
    text,
    style = MaterialTheme.typography.bodySmall,
    fontFamily = FontFamily.Monospace,
)

/**
 * Suficiente para desbordar int32 al pasar compases a frames: a 48 kHz y 4/4 un
 * compás son 96000 frames a 120 BPM, así que ~22400 compases ya pasan los 2^31.
 * Este valor deja el margen bien arriba para que no dependa del BPM del momento.
 */
private const val ABSURD_BARS = 2_000_000

/** El motor negocia 48 kHz en las dos plataformas; el harness no adivina otra cosa. */
private const val SAMPLE_RATE = 48000

/**
 * Directorio raíz: no escribible en el sandbox de iOS ni en Android. Es el caso del
 * ítem 9 — antes, un export imposible dejaba escapar la excepción y abortaba.
 */
private const val UNWRITABLE_PATH = "/watermelon-export-imposible.wav"
