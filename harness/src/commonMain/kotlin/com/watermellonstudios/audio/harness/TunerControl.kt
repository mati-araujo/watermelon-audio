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
import com.watermellonstudios.audio.api.ITuner
import com.watermellonstudios.audio.api.TunerFactory
import com.watermellonstudios.audio.api.TunerReading
import com.watermellonstudios.audio.domain.tuning.Tuning
import com.watermellonstudios.audio.domain.tuning.TuningConfiguration
import kotlinx.coroutines.delay

/**
 * Control 9 — el afinador. **Y, sobre todo, el control de AC-010.1.**
 *
 * ## Este archivo es una aserción de compilación, no sólo una pantalla
 *
 * REQ-010 existe porque el afinador no tenía puerta pública: el único camino era
 * `getAudioBridge()`, detrás de `@InternalWatermelonApi` —nivel `ERROR`, y con su propio
 * KDoc diciendo que lo de atrás *"puede cambiar o desaparecer en cualquier versión, incluida
 * una de patch"*.
 *
 * 🔴 **Por eso este archivo NO tiene `@OptIn`, y eso es el requisito — no una omisión.** Si
 * alguien se lo agrega, o si [TunerFactory] deja de bastar y hay que volver al bridge, la
 * puerta dejó de ser puerta y el REQ cruzó su criterio de muerte. La aserción es que
 * `:harness` **compila**: es un módulo distinto de `:audio`, así que ve exactamente lo que ve
 * un consumidor de afuera.
 *
 * > **Por qué el control no puede vivir en `:audio`.** `internal` en Kotlin es de *módulo*:
 * > `audio/src/commonTest` ve `TunerImpl` y `getAudioBridge()` sin optar-in por ser el mismo
 * > módulo. Un test ahí compilaría con la factory, sin la factory y con la factory rota — el
 * > test-teatro perfecto, verde por construcción. La única forma de afirmar "puerta" es
 * > cruzando la frontera de módulo, que es lo que hace este archivo.
 *
 * Y ejercita los **siete** miembros de [ITuner], no sólo `create()`: una puerta que devuelve
 * algo que después no se puede usar sin optar-in tampoco es una puerta.
 *
 * ## Por qué muestra los cents y no un "afinado / desafinado"
 *
 * Mismo criterio que el medidor de [InputMonitorControl]: un booleano se satisface con
 * silencio. Lo que prueba que el afinador mide es **un número que se mueve cuando tocás la
 * cuerda**, y los tres estados hay que poder distinguirlos:
 *
 * - **sin lectura** (`null`): el motor todavía no publicó nada. No es "afinado" ni cero.
 * - **midiendo, sin enganche**: hay snapshot y los cents son `null`.
 * - **midiendo, con enganche**: los cents se mueven, y `isConverged` dice si el motor lo
 *   declara convergido.
 *
 * Colapsar el primero con el segundo es el modo de falla caro: una aguja clavada en 0,0 que se
 * lee como "afinado exacto".
 *
 * ## Lo que este control NO puede contestar
 *
 * Si el motor mide **bien**. `isConverged` sale de `snapshot.state`, y REQ-009 de este repo
 * dejó medido que el motor publica `CONVERGIDO` con 1,75 cents de error. Este control muestra
 * lo que el motor dice; que lo que dice sea cierto es otro requisito.
 */
@Composable
fun TunerControl(modifier: Modifier = Modifier) {
    // La configuración es obligatoria: no hay default honesto para "contra qué afinar".
    // Ver el KDoc de TunerFactory.
    val tuner: ITuner = remember {
        TunerFactory.create(TuningConfiguration(Tuning.GUITAR_STANDARD))
    }

    // El afinador toma el device de captura. Se suelta con la pantalla, igual que el nodo
    // de entrada: sin esto, cada rotación en Android lo deja tomado.
    DisposableEffect(Unit) {
        onDispose { tuner.stop() }
    }

    var running by remember { mutableStateOf(tuner.isRunning) }
    var selected by remember { mutableStateOf(tuner.selectedString) }
    var lastStartFailed by remember { mutableStateOf(false) }
    var reading by remember { mutableStateOf<TunerReading?>(null) }

    // Un solo cruce de frontera por tick, y sólo mientras corre: preguntar con el análisis
    // parado gasta cruces para leer lo mismo.
    LaunchedEffect(running) {
        if (!running) return@LaunchedEffect
        while (true) {
            reading = tuner.reading()
            running = tuner.isRunning
            delay(POLL_MS)
        }
    }

    Card(modifier = modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text("Afinador", style = MaterialTheme.typography.titleMedium)

            Text(
                text = when (val r = reading) {
                    // Tres estados y no dos. Ver el KDoc.
                    null -> if (running) "sin lectura (el motor no publicó nada)" else "detenido"
                    else -> when (val cents = r.cents) {
                        null -> "midiendo · sin enganche"
                        else -> "${cents.oneDecimal()} cents" +
                            (r.target?.let { " · objetivo ${it.note.name}" } ?: " · sin objetivo") +
                            (if (r.isConverged) " · convergido" else "")
                    }
                },
                fontFamily = FontFamily.Monospace,
                style = MaterialTheme.typography.bodySmall,
            )

            // `targets` en orden de CUERDA, no de altura: para guitarra la cuerda 1 es el mi
            // agudo. Ver el invariante de Instrument.kt — no reordenar.
            Row(
                modifier = Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                tuner.targets.forEach { target ->
                    Button(
                        onClick = {
                            // Asignar dispara el empuje del objetivo al motor — pero sólo si
                            // cambió. Ver TunerImpl: empujarlo de más reinicia la integración.
                            tuner.selectedString = target.stringIndex
                            selected = tuner.selectedString
                        },
                    ) {
                        Text(
                            text = "${target.stringIndex}·${target.note.name}" +
                                (if (selected == target.stringIndex) " ✓" else ""),
                        )
                    }
                }
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = {
                        if (running) {
                            tuner.stop()
                            running = false
                            reading = null
                        } else {
                            // El valor devuelto se MUESTRA en vez de tragarse: es la
                            // diferencia entre "no arrancó" (sin permiso de micrófono) y
                            // "arrancó y no entra nada".
                            val ok = tuner.start()
                            lastStartFailed = !ok
                            running = tuner.isRunning
                        }
                    },
                ) {
                    Text(if (running) "Parar" else "Arrancar")
                }

                Button(
                    onClick = {
                        // Cambiar la configuración recalcula `targets` y re-empuja el
                        // objetivo. Es el otro camino de AC-010.2.
                        tuner.configuration = TuningConfiguration(Tuning.GUITAR_DROP_D)
                        selected = tuner.selectedString
                    },
                ) {
                    Text("Drop D")
                }
            }

            if (lastStartFailed) {
                Text(
                    text = "start() devolvió false — sin motor o sin entrada de audio " +
                        "(¿permiso de micrófono?)",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }
    }
}

private const val POLL_MS = 80L
