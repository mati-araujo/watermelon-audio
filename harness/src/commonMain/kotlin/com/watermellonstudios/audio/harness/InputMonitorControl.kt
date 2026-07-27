package com.watermellonstudios.audio.harness

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.watermellonstudios.audio.api.AudioInput
import com.watermellonstudios.audio.api.AudioInputFactory
import com.watermellonstudios.audio.domain.input.InputMetering
import com.watermellonstudios.audio.domain.input.InputSource

/**
 * Control 1 de 7 — **el que justifica el proyecto entero**.
 *
 * Es lo único que puede contestar si el camino de entrada de iOS captura audio
 * de verdad. Ese camino (`CoreAudioBackend`, etapas 1 y 2 del input path) se
 * escribió a ciegas y nada lo había ejercitado: la suite de host sustituye
 * `InputNode` por un stub sin comportamiento, y `CinteropSmokeTest`
 * deliberadamente no arranca el motor.
 *
 * ## Por qué el medidor es de nivel y no un booleano
 *
 * "El stream arrancó" y "está entrando señal" son dos afirmaciones distintas, y
 * la primera se cumple perfectamente con silencio. Un indicador de encendido
 * habría dado verde durante todo el desarrollo del input path sin probar nada.
 * Lo que prueba algo es una barra que **se mueve cuando hablás**.
 *
 * ## Los tres estados del medidor, y por qué son tres
 *
 * - **sin medición** (`null`): no hay nodo de entrada. Se dibuja distinto del
 *   silencio a propósito — ver [InputMetering].
 * - **midiendo, en silencio**: la barra existe y está en el piso.
 * - **midiendo, con señal**: la barra se mueve.
 *
 * Colapsar los dos primeros es el modo de falla más caro que tiene este harness:
 * un medidor plano y convincente que manda a buscar el bug al lugar equivocado.
 *
 * ## El caso que más importa probar
 *
 * **El permiso denegado.** Es el único disparador del `@try` de
 * `CoreAudioBackend` —`AVAudioEngine.inputNode` *lanza* cuando no hay entrada
 * usable, no devuelve error— y es el ítem 3 del smoke. Por eso [start] muestra
 * su `false` en pantalla en vez de tragárselo: así se ve la diferencia entre
 * "no arrancó" y "arrancó y no entra nada".
 */
@Composable
fun InputMonitorControl(modifier: Modifier = Modifier) {
    val input = remember { AudioInputFactory.create() }

    // El nodo de entrada se suelta con la pantalla. Sin esto, cada rotación en
    // Android deja el device de captura tomado.
    DisposableEffect(Unit) {
        onDispose {
            input.stop()
            input.release()
        }
    }

    var running by remember { mutableStateOf(input.isRunning) }
    var lastStartFailed by remember { mutableStateOf(false) }
    var monitoring by remember { mutableStateOf(input.monitoringEnabled) }
    var monitorVolume by remember { mutableStateOf(input.monitoringVolume) }
    var gainDb by remember { mutableStateOf(input.gainDb) }
    var source by remember { mutableStateOf(input.source) }
    var metering by remember { mutableStateOf<InputMetering?>(null) }

    // Un solo cruce de frontera por tick, vía el snapshot. Sólo mientras corre:
    // preguntar con el stream cerrado gasta cruces para leer null.
    LaunchedEffect(running) {
        if (!running) {
            metering = null
            return@LaunchedEffect
        }
        input.meteringFlow().collect { metering = it }
    }

    Card(modifier = modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text("Entrada", style = MaterialTheme.typography.titleMedium)

            MeterBar(metering)

            Text(
                text = when (val m = metering) {
                    null -> if (running) "sin medición (no hay nodo de entrada)" else "detenido"
                    else -> "L ${m.levelDbLeft.oneDecimal()} dB · " +
                        "R ${m.levelDbRight.oneDecimal()} dB · " +
                        "${m.latencyMs.oneDecimal()} ms" +
                        (if (m.isNoiseGateOpen) " · gate abierto" else "")
                },
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace,
            )

            if (lastStartFailed) {
                Text(
                    "start() devolvió false — permiso denegado o sin device de entrada",
                    color = MaterialTheme.colorScheme.error,
                    style = MaterialTheme.typography.bodySmall,
                )
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    enabled = !running,
                    onClick = {
                        val ok = input.start()
                        lastStartFailed = !ok
                        running = input.isRunning
                    },
                ) { Text("capturar") }

                Button(
                    enabled = running,
                    onClick = {
                        input.stop()
                        running = input.isRunning
                        lastStartFailed = false
                    },
                ) { Text("detener") }
            }

            Row(verticalAlignment = Alignment.CenterVertically) {
                Switch(
                    checked = monitoring,
                    onCheckedChange = {
                        input.monitoringEnabled = it
                        monitoring = input.monitoringEnabled
                    },
                )
                // El aviso no es decorativo: sin auriculares, el monitoreo
                // realimenta el micrófono y el chillido tapa cualquier medición.
                Text(" monitorear (usar auriculares)", style = MaterialTheme.typography.bodySmall)
            }

            LabeledSlider(
                label = "volumen monitor",
                value = monitorVolume,
                valueRange = 0f..1f,
                display = monitorVolume.twoDecimals(),
            ) {
                input.monitoringVolume = it
                monitorVolume = input.monitoringVolume
            }

            LabeledSlider(
                label = "ganancia",
                value = gainDb,
                valueRange = -24f..24f,
                display = "${gainDb.oneDecimal()} dB",
            ) {
                input.gainDb = it
                gainDb = input.gainDb
            }

            // Scroll horizontal: las cuatro fuentes no entran en el ancho de un
            // telefono y sin esto Compose parte "Bluetooth" en vertical, una
            // letra por linea. Es fea, pero ilegible no.
            Row(
                modifier = Modifier.horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                InputSource.entries.forEach { candidate ->
                    Button(
                        enabled = candidate != source,
                        onClick = {
                            input.source = candidate
                            source = input.source
                        },
                    ) {
                        Text(
                            candidate.displayName,
                            style = MaterialTheme.typography.labelSmall,
                            maxLines = 1,
                        )
                    }
                }
            }
        }
    }
}

/**
 * La barra.
 *
 * El escalado es **por dB y no lineal** a propósito: en lineal, todo lo que uno
 * mide hablándole a un teléfono vive apretado contra el cero y la barra no se
 * mueve — que es indistinguible de "no captura", justo lo que hay que
 * distinguir.
 */
@Composable
private fun MeterBar(metering: InputMetering?) {
    val floorDb = -60f
    val fraction = metering?.let {
        val db = if (it.levelDbLeft > it.levelDbRight) it.levelDbLeft else it.levelDbRight
        ((db - floorDb) / -floorDb).coerceIn(0f, 1f)
    } ?: 0f

    Box(
        modifier = Modifier
            .fillMaxWidth()
            .height(24.dp)
            .clip(RoundedCornerShape(4.dp))
            // Sin medición el fondo es distinto: la barra vacía y el "no sé"
            // no pueden verse igual.
            .background(
                if (metering == null) Color(0xFF3A3A3A) else Color(0xFF1E1E1E)
            ),
    ) {
        if (metering != null) {
            Box(
                modifier = Modifier
                    .fillMaxWidth(fraction)
                    .height(24.dp)
                    .background(if (metering.isClipping) Color(0xFFE53935) else Color(0xFF43A047)),
            )
        }
    }
}

@Composable
private fun LabeledSlider(
    label: String,
    value: Float,
    valueRange: ClosedFloatingPointRange<Float>,
    display: String,
    onChange: (Float) -> Unit,
) {
    Column {
        Text("$label: $display", style = MaterialTheme.typography.bodySmall)
        Slider(value = value, valueRange = valueRange, onValueChange = onChange)
    }
}

/** Formateo mínimo — Kotlin común no tiene `String.format`. */
private fun Float.oneDecimal(): String {
    if (isNaN()) return "NaN"
    if (this < -1000f) return "-inf"
    val scaled = (this * 10f).toInt()
    return "${scaled / 10}.${(if (scaled < 0) -scaled else scaled) % 10}"
}

private fun Float.twoDecimals(): String {
    val scaled = (this * 100f).toInt()
    val whole = scaled / 100
    val frac = (if (scaled < 0) -scaled else scaled) % 100
    return "$whole.${if (frac < 10) "0$frac" else "$frac"}"
}
