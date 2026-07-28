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
import com.watermellonstudios.audio.domain.mode.AudioMode
import com.watermellonstudios.audio.internal.bridge.getAudioBridge
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/**
 * Control 8 — sistema de modos. **Existe por el call site 26 de WA-1.4.**
 *
 * ## Qué cierra
 *
 * La migración a `BridgeConcurrency` (WA-1.4) tocó 26 call sites. El smoke en el AVD
 * ejercitó 25: LIFECYCLE, EFFECTS e INPUT tienen todos control en el harness. El que
 * faltaba es **`Category.MODE`, cuyo único call site es `setAudioMode`** — y el harness
 * no lo exponía por ningún lado, así que era literalmente inalcanzable desde la UI.
 *
 * ## Por qué el riesgo concreto es un cuelgue, no una excepción
 *
 * `BridgeConcurrency` serializa con un `Mutex` de corrutinas, que **no es reentrante**:
 * si un camino reentrara su propia categoría, no falla — **se cuelga**. Un ANR, no un
 * stack trace. Por eso los tres botones que importan no son los tres modos:
 *
 * - **modo inválido (7)** ejercita el camino de *falla dentro del `guarded`*, que es
 *   donde un `return` mal puesto se llevaría el lock a la tumba. Que el botón siguiente
 *   responda es la mitad de la prueba.
 * - **ráfaga** dispara los tres modos a la vez sobre el mismo mutex. Es la única forma
 *   de ver contención de verdad; un botón a la vez no la produce nunca.
 *
 * ## Y se lee el modo de vuelta, no el retorno de la llamada
 *
 * `NativeModeStateWriter` en Android loguea `MODE DID NOT CHANGE!` justamente porque un
 * `Result.success` no prueba que el motor haya cambiado de modo. Acá el `modo actual`
 * sale de `getAudioMode()`, o sea del motor, y es el que hay que mirar.
 *
 * ## Lo que este control NO contesta
 *
 * Si el modo *suena* distinto. INPUT_FX y MIX enrutan la entrada, y ni el AVD tiene
 * salida de audio real ni hay device para escuchar. Lo que sí valida —el contrato del
 * puente y la disciplina de concurrencia— no dependía de escuchar nada.
 */
@OptIn(InternalWatermelonApi::class)
@Composable
fun ModeControl(modifier: Modifier = Modifier) {
    val bridge = remember { getAudioBridge() }
    val scope = rememberCoroutineScope()

    var current by remember { mutableStateOf(-1) }
    var transitioning by remember { mutableStateOf(false) }
    var lastCall by remember { mutableStateOf<String?>(null) }
    var lastBurst by remember { mutableStateOf<String?>(null) }

    // El polling es lo que delata un cuelgue: si el mutex de MODE quedara tomado, la
    // corrutina del botón no vuelve, pero esta línea sigue latiendo. Un panel
    // congelado y un motor colgado se distinguen así.
    LaunchedEffect(Unit) {
        while (true) {
            current = bridge.getAudioMode()
            transitioning = bridge.isInModeTransition()
            delay(100)
        }
    }

    Card(modifier = modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text("Modo (WA-1.4 · call site 26)", style = MaterialTheme.typography.titleMedium)

            Mono(
                "modo actual: $current (${AudioMode.fromId(current).displayName})" +
                    "  ·  en transición: $transitioning"
            )

            lastCall?.let { Mono(it) }
            lastBurst?.let { Mono(it) }

            Row(
                modifier = Modifier.horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                AudioMode.entries.forEach { mode ->
                    Button(onClick = {
                        scope.launch {
                            val result = bridge.setAudioMode(mode.id)
                            lastCall = "setAudioMode(${mode.id}) = ${result.render()}" +
                                "  → getAudioMode() = ${bridge.getAudioMode()}"
                        }
                    }) {
                        Text(mode.displayName, style = MaterialTheme.typography.labelSmall, maxLines = 1)
                    }
                }
            }

            Button(onClick = {
                scope.launch {
                    val before = bridge.getAudioMode()
                    val result = bridge.setAudioMode(INVALID_MODE)
                    val after = bridge.getAudioMode()
                    lastCall = "setAudioMode($INVALID_MODE) = ${result.render()}" +
                        "  → modo $before → $after" +
                        if (result.isFailure && before == after) "  ← rechazado sin tocar el motor" else ""
                }
            }) { Text("modo inválido ($INVALID_MODE)") }

            Button(onClick = {
                scope.launch {
                    // Los tres a la vez sobre el mismo mutex de categoría. Si volvieran
                    // los tres, la serialización funciona; si esto no vuelve nunca, hay
                    // reentrancia y el número de arriba lo va a seguir contando.
                    lastBurst = "ráfaga: en vuelo…"
                    val results = AudioMode.entries
                        .map { mode -> async { mode.id to bridge.setAudioMode(mode.id) } }
                        .awaitAll()
                    val ok = results.count { it.second.isSuccess }
                    lastBurst = "ráfaga: ${results.size} en paralelo, $ok ok" +
                        "  → getAudioMode() = ${bridge.getAudioMode()}"
                }
            }) { Text("ráfaga (${AudioMode.entries.size} modos en paralelo)") }
        }
    }
}

/** `Result<Unit>` en una línea, sin perder el motivo de la falla. */
private fun Result<Unit>.render(): String =
    fold(onSuccess = { "ok" }, onFailure = { "FALLA ${it::class.simpleName}: ${it.message}" })

@Composable
private fun Mono(text: String) = Text(
    text,
    style = MaterialTheme.typography.bodySmall,
    fontFamily = FontFamily.Monospace,
)

/**
 * Fuera del rango 0..2 que valida el puente. No es un número mágico cualquiera: tiene
 * que caer del lado rechazado en las dos plataformas, y `AudioMode` tiene tres entradas.
 */
private const val INVALID_MODE = 7
