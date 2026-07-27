package com.watermellonstudios.audio.harness

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
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
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.watermellonstudios.audio.api.InternalWatermelonApi
import com.watermellonstudios.audio.internal.bridge.getAudioBridge
import kotlinx.coroutines.delay

/**
 * Control 6 de 7 — metrónomo y reloj musical.
 *
 * ## El ítem 5 del smoke es el motivo de que este control exista
 *
 * El fix del off-by-one del metrónomo (WA-2.6, categoría `metronome`) **es el único
 * cambio de toda esa serie que altera algo que ya sonaba bien** — o casi bien: el
 * click iba ~4 ms adelantado. Eso no lo contesta ningún test: hay que escucharlo.
 * Lo que este control aporta es poder dispararlo a voluntad, con un BPM elegido, y
 * escuchar un count-in contra algo.
 *
 * ## Los beats restantes, que es lo que hace visible el scheduler
 *
 * `transportGetRemainingBeats()` se lee en un loop mientras corre. No es decoración:
 * es la única forma de ver desde afuera que el scheduler **descuenta** — es decir,
 * que los clicks los emite el thread de audio desde el render callback y no un timer
 * de UI. Si el número se queda quieto o salta, ahí está el bug.
 *
 * **Y hay una trampa de contrato pinchada acá:** en modo continuo ese valor **no es
 * una cuenta**, es el centinela de armado del scheduler (1). Mostrarlo como "quedan
 * 1 beats" sería inventar. Por eso el modo continuo dice `∞` y nunca lee el número.
 */
@OptIn(InternalWatermelonApi::class)
@Composable
fun MetronomeControl(modifier: Modifier = Modifier) {
    val bridge = remember { getAudioBridge() }

    var bpm by remember { mutableStateOf(bridge.getBpm().takeIf { it > 0f } ?: 120f) }
    var beatsPerBar by remember { mutableStateOf(bridge.transportGetBeatsPerBar()) }
    var running by remember { mutableStateOf(bridge.transportIsMetronomeRunning()) }
    var continuous by remember { mutableStateOf(bridge.transportIsMetronomeContinuous()) }
    var remaining by remember { mutableStateOf(0) }

    // Dejar el click sonando al salir de la pantalla sería exactamente el tipo de
    // fuga que este harness existe para no tener.
    DisposableEffect(Unit) {
        onDispose { bridge.transportStopMetronome() }
    }

    // Poll mientras corre. 50 ms es bastante más rápido que un beat a cualquier BPM
    // razonable, así que la cuenta se ve bajar de a uno y no de a saltos.
    LaunchedEffect(running) {
        while (running) {
            running = bridge.transportIsMetronomeRunning()
            continuous = bridge.transportIsMetronomeContinuous()
            remaining = if (continuous) -1 else bridge.transportGetRemainingBeats()
            delay(50)
        }
        remaining = 0
    }

    Card(modifier = modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text("Metrónomo", style = MaterialTheme.typography.titleMedium)

            Text(
                text = "corriendo: $running · " +
                    "restantes: ${if (continuous) "∞ (continuo)" else remaining}",
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace,
            )

            // frames/beat y frames/bar salen del mismo reloj que el looper usa para
            // cuantizar. Verlos acá es lo que permite cruzarlos con el control 5.
            Text(
                text = "frames/beat: ${bridge.transportFramesPerBeat()} · " +
                    "frames/bar: ${bridge.transportFramesPerBar(1)}",
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace,
            )

            Column {
                Text("BPM: ${bpm.oneDecimal()}", style = MaterialTheme.typography.bodySmall)
                Slider(
                    value = bpm,
                    valueRange = 40f..240f,
                    onValueChange = {
                        bpm = it
                        // setBpm y no un setter de transporte: el mismo valor le llega
                        // al Transport y a los efectos sincronizados al tempo. Ver
                        // IAudioNativeBridge, sección TRANSPORT.
                        bridge.setBpm(it)
                    },
                )
            }

            Column {
                Text("beats por compás: $beatsPerBar", style = MaterialTheme.typography.bodySmall)
                Slider(
                    value = beatsPerBar.toFloat(),
                    valueRange = 1f..16f,
                    steps = 14,
                    onValueChange = {
                        bridge.transportSetBeatsPerBar(it.toInt())
                        beatsPerBar = bridge.transportGetBeatsPerBar()
                    },
                )
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = {
                    bridge.transportStartMetronome(beatsPerBar, firstIsDownbeat = true)
                    running = true
                }) { Text("count-in (1 compás)") }

                Button(onClick = {
                    bridge.transportStartMetronomeContinuous(everyBeatPattern = true)
                    running = true
                }) { Text("continuo") }
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    enabled = running,
                    onClick = {
                        bridge.transportStopMetronome()
                        running = bridge.transportIsMetronomeRunning()
                    },
                ) { Text("parar") }

                // `beats <= 0` detiene en vez de arrancar — está documentado en la C
                // API y en la interfaz, así que conviene tener con qué ejercitarlo.
                Button(onClick = {
                    bridge.transportStartMetronome(0)
                    running = bridge.transportIsMetronomeRunning()
                }) { Text("start(0) = parar") }
            }
        }
    }
}
