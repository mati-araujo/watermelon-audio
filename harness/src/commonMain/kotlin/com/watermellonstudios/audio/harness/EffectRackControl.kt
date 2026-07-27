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
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.watermellonstudios.audio.api.AudioEngine
import com.watermellonstudios.audio.api.InternalWatermelonApi
import com.watermellonstudios.audio.domain.effect.EffectType
import com.watermellonstudios.audio.internal.bridge.getAudioBridge
import kotlinx.coroutines.launch

/**
 * Control 3 de 7 — rack de efectos y topología de routing.
 *
 * Drena el ítem **2** del smoke (agregar más de 6 efectos para ver el tope de gama
 * baja de WA-1.2) y **todo lo que se migró en WA-2.6 categoría `effects`**.
 *
 * ## Dos superficies en el mismo control, a propósito
 *
 * Los efectos van por `AudioEngine` — la API pública, la que consume un cliente. El
 * **routing mode** no está ahí y no va a estar: es una de las cuatro cosas que la
 * decisión de 2026-07-27 dejó del lado del diagnóstico. Así que este control usa las
 * dos superficies a la vez, y esa mezcla es justamente lo que valida la decisión:
 * si el rack necesitara el bridge para todo, el opt-in sería una excusa; si no lo
 * necesitara para nada, sobraría.
 *
 * ## El tope de efectos se muestra, no se asume
 *
 * `AudioEngineFactory.create()` recorta `maxEffects` a 6 en un dispositivo de gama
 * baja (WA-1.2), y eso no lo ve ningún test de host: ni `/proc/meminfo` ni
 * `/sys/devices/system/cpu/possible` existen en macOS. Por eso el botón de agregar
 * **muestra lo que devolvió `addEffect`** en vez de tragárselo: el `false` del tope
 * es información, no un error a esconder.
 */
@OptIn(InternalWatermelonApi::class)
@Composable
fun EffectRackControl(engine: AudioEngine, modifier: Modifier = Modifier) {
    val bridge = remember { getAudioBridge() }
    val scope = rememberCoroutineScope()

    var chain by remember { mutableStateOf<List<EffectType>>(emptyList()) }
    var bypassed by remember { mutableStateOf<Set<Int>>(emptySet()) }
    var lastAddResult by remember { mutableStateOf<String?>(null) }
    var routingMode by remember { mutableStateOf(bridge.getRoutingMode()) }
    var parallelMix by remember { mutableStateOf(0.5f) }
    var feedback by remember { mutableStateOf(0.3f) }
    var refresh by remember { mutableStateOf(0) }

    // La cadena se relee del motor, no se lleva en paralelo en Kotlin: llevar una
    // copia es como se empieza a mostrar algo distinto de lo que suena.
    LaunchedEffect(refresh) {
        val count = bridge.getEffectCount()
        chain = (0 until count).mapNotNull { bridge.getEffectType(it) }
        bypassed = (0 until count).filter { bridge.isEffectBypassed(it) }.toSet()
    }

    Card(modifier = modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text("Rack de efectos", style = MaterialTheme.typography.titleMedium)

            Text(
                text = if (chain.isEmpty()) {
                    "cadena vacía"
                } else {
                    chain.mapIndexed { i, t ->
                        if (i in bypassed) "(${t.displayName})" else t.displayName
                    }.joinToString(" → ")
                },
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace,
            )
            Text(
                "${chain.size} efectos · entre paréntesis = bypass",
                style = MaterialTheme.typography.bodySmall,
            )

            lastAddResult?.let {
                Text(it, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
            }

            // ---- Agregar: unos pocos tipos, suficientes para pasar el tope de 6 ----
            Row(
                modifier = Modifier.horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                RACK_TYPES.forEach { type ->
                    Button(onClick = {
                        val ok = engine.addEffect(type)
                        lastAddResult = "addEffect(${type.displayName}) = $ok" +
                            if (!ok) "  ← tope de la cadena" else ""
                        refresh++
                    }) {
                        Text(type.displayName, style = MaterialTheme.typography.labelSmall, maxLines = 1)
                    }
                }
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    enabled = chain.isNotEmpty(),
                    onClick = { engine.removeEffect(chain.lastIndex); refresh++ },
                ) { Text("quitar último") }

                Button(
                    enabled = chain.size >= 2,
                    onClick = {
                        // Reordenar el primero al final: barato de disparar y fácil de
                        // verificar contra la cadena que se muestra arriba.
                        engine.reorderEffects(0, chain.lastIndex)
                        refresh++
                    },
                ) { Text("mover 1º al final") }
            }

            // ---- Bypass por efecto y global ----
            if (chain.isNotEmpty()) {
                Row(
                    modifier = Modifier.horizontalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    chain.indices.forEach { i ->
                        Button(onClick = {
                            engine.setEffectBypass(i, i !in bypassed)
                            refresh++
                        }) {
                            Text("byp $i", style = MaterialTheme.typography.labelSmall, maxLines = 1)
                        }
                    }
                }

                // Param 0 del primer efecto. No se le pone nombre porque cambia según
                // el tipo (en FILTER es el cutoff en Hz, no un normalizado); lo que
                // este slider prueba es que el parámetro llega y vuelve.
                Column {
                    val current = engine.getEffectParameter(0, 0)
                    Text(
                        "param 0 del efecto 0: ${current.oneDecimal()}",
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace,
                    )
                    Slider(
                        value = current.coerceIn(0f, 20000f),
                        valueRange = 0f..20000f,
                        onValueChange = { engine.setEffectParameter(0, 0, it); refresh++ },
                    )
                }
            }

            Row(verticalAlignment = Alignment.CenterVertically) {
                var allBypassed by remember { mutableStateOf(false) }
                Switch(
                    checked = allBypassed,
                    onCheckedChange = {
                        allBypassed = it
                        engine.setEffectsBypass(it)
                        refresh++
                    },
                )
                Text(" bypass global", style = MaterialTheme.typography.bodySmall)
            }

            Button(onClick = { scope.launch { bridge.clearAllEffects(); refresh++ } }) {
                Text("limpiar cadena")
            }

            // ---- Routing: la parte que vive detrás del opt-in ----
            Text("Routing", style = MaterialTheme.typography.labelLarge)
            Text(
                "modo: ${ROUTING_NAMES.getOrElse(routingMode) { "?" }} ($routingMode)",
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace,
            )
            Row(
                modifier = Modifier.horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                ROUTING_NAMES.forEachIndexed { mode, name ->
                    Button(
                        enabled = mode != routingMode,
                        onClick = {
                            bridge.setRoutingMode(mode)
                            // Se relee en vez de asumir: el motor recorta 0..5 y esta
                            // es la única forma de ver si aceptó lo que se pidió.
                            routingMode = bridge.getRoutingMode()
                        },
                    ) {
                        Text(name, style = MaterialTheme.typography.labelSmall, maxLines = 1)
                    }
                }
            }

            Column {
                Text("mezcla paralelo: ${parallelMix.twoDecimals()}", style = MaterialTheme.typography.bodySmall)
                Slider(
                    value = parallelMix,
                    onValueChange = { parallelMix = it; bridge.setParallelMix(it) },
                )
            }
            Column {
                Text("feedback: ${feedback.twoDecimals()}", style = MaterialTheme.typography.bodySmall)
                Slider(
                    value = feedback,
                    onValueChange = { feedback = it; bridge.setFeedbackAmount(it) },
                )
            }
        }
    }
}

/**
 * Siete tipos alcanzan para pasarse del tope de 6 de gama baja, que es lo que hay
 * que poder provocar. Los 23 no entran en una fila y no agregan nada: el rack prueba
 * el mecanismo de la cadena, no el catálogo.
 */
private val RACK_TYPES = listOf(
    EffectType.FILTER,
    EffectType.REVERB,
    EffectType.DELAY,
    EffectType.DISTORTION,
    EffectType.COMPRESSOR,
    EffectType.CHORUS,
    EffectType.PHASER,
)

/** Índice = valor de `RoutingMode` en `EffectTypes.h`. El orden es el contrato. */
private val ROUTING_NAMES = listOf(
    "serial",
    "paralelo",
    "split 2x2",
    "serial→par",
    "par→serial",
    "feedback",
)
