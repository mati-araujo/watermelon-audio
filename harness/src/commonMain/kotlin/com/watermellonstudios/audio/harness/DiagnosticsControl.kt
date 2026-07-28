package com.watermellonstudios.audio.harness

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.watermellonstudios.audio.api.InternalWatermelonApi
import com.watermellonstudios.audio.api.currentDeviceCapabilities
import com.watermellonstudios.audio.domain.usb.AudioBackendType
import com.watermellonstudios.audio.internal.bridge.getAudioBridge

/**
 * Control 7 de 7 — panel de diagnóstico.
 *
 * Drena tres ítems del smoke: **2** (device caps / gama), **6** (recommended
 * buffer size) y sobre todo **10**, que es el interesante.
 *
 * ## El ítem 10: `selectBackend` devuelve `true` aunque no consiga el backend
 *
 * Pedir LIBUSB sin USB presente cae al backend de sistema y **reporta éxito**.
 * Sólo `getCurrentBackendType()` lo delata. Por eso este control no muestra lo
 * que devolvió `selectBackend` ni lo que el usuario pidió: muestra **las dos
 * cosas juntas**, el pedido y el tipo que el motor reporta después. Un botón que
 * dijera "OK" sería exactamente el reporte que ya sabemos que miente.
 *
 * ## Por qué esto vive detrás de `@OptIn`
 *
 * Backend, routing y captura de logs son **superficie de diagnóstico**: el harness
 * es tooling, no un consumidor del motor. La decisión de 2026-07-27 fue no
 * ensanchar `AudioEngine` para esto y darle al harness la puerta del puente detrás
 * de [InternalWatermelonApi]. Este archivo es el primer usuario real de esa puerta,
 * y por eso el `@OptIn` está acá arriba y no escondido en una función.
 */
@OptIn(InternalWatermelonApi::class)
@Composable
fun DiagnosticsControl(modifier: Modifier = Modifier) {
    val bridge = remember { getAudioBridge() }
    val caps = remember { currentDeviceCapabilities() }

    var requestedBackend by remember { mutableStateOf<AudioBackendType?>(null) }
    var selectReturned by remember { mutableStateOf<Boolean?>(null) }
    var actualBackend by remember { mutableStateOf(AudioBackendType.fromId(bridge.getCurrentBackendType())) }

    var logsEnabled by remember { mutableStateOf(false) }
    var logLines by remember { mutableStateOf<List<String>>(emptyList()) }
    var dropped by remember { mutableStateOf(0) }

    // La captura es global al proceso: dejarla prendida al salir de la pantalla
    // haría que el anillo siga llenándose para nadie.
    DisposableEffect(Unit) {
        onDispose { bridge.setLogCaptureEnabled(false) }
    }

    Card(modifier = modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text("Diagnóstico", style = MaterialTheme.typography.titleMedium)

            // ---- Device caps (ítem 2 del smoke) ----
            Text(
                text = "${caps.platform} · API ${caps.apiLevel} · ${caps.totalRamMb} MB · " +
                    "${caps.cpuCoreCount} cores",
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace,
            )
            Text(
                text = "low latency: ${caps.supportsLowLatencyAudio} · " +
                    "gama baja: ${caps.isLowEndDevice}",
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace,
            )

            // ---- Backend: el pedido y la realidad, uno al lado del otro ----
            Text("Backend", style = MaterialTheme.typography.labelLarge)
            Text(
                text = buildString {
                    append("reporta: ${actualBackend.displayName}")
                    val req = requestedBackend
                    if (req != null) {
                        append("   ·   pedido: ${req.displayName}")
                        append("   ·   selectBackend(): $selectReturned")
                    }
                },
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace,
            )
            // La discrepancia se dice en pantalla, no se deja para el que lea el log.
            requestedBackend?.let { req ->
                if (selectReturned == true && req != actualBackend) {
                    Text(
                        "selectBackend() devolvió true y el motor quedó en " +
                            "${actualBackend.displayName} — ítem 10 del smoke, reproducido.",
                        color = MaterialTheme.colorScheme.error,
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
            }
            Row(
                modifier = Modifier.horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                listOf(AudioBackendType.OBOE, AudioBackendType.LIBUSB).forEach { candidate ->
                    Button(onClick = {
                        requestedBackend = candidate
                        selectReturned = bridge.selectBackend(candidate.id)
                        actualBackend = AudioBackendType.fromId(bridge.getCurrentBackendType())
                    }) {
                        Text(candidate.displayName, style = MaterialTheme.typography.labelSmall, maxLines = 1)
                    }
                }
            }
            Text(
                "USB disponible: ${bridge.isUsbBackendAvailable()}",
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace,
            )

            // ---- Vista de logs nativos ----
            Text("Logs nativos", style = MaterialTheme.typography.labelLarge)
            Row(verticalAlignment = Alignment.CenterVertically) {
                Switch(
                    checked = logsEnabled,
                    onCheckedChange = {
                        logsEnabled = it
                        bridge.setLogCaptureEnabled(it)
                        if (!it) logLines = emptyList()
                    },
                )
                Text(" capturar", style = MaterialTheme.typography.bodySmall)
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    enabled = logsEnabled,
                    onClick = {
                        // El drain es destructivo, así que lo que llega se acumula acá:
                        // volver a apretar no puede volver a traer lo mismo.
                        logLines = (logLines + bridge.drainCapturedLogs()).takeLast(MAX_VISIBLE_LINES)
                        dropped = bridge.getLogCaptureDropped()
                    },
                ) { Text("vaciar") }

                Button(
                    enabled = logLines.isNotEmpty(),
                    onClick = { logLines = emptyList() },
                ) { Text("limpiar vista") }
            }

            // `dropped` es acumulado y NO se resetea al vaciar: si crece, la vista
            // está mostrando menos de lo que pasó y hay que decirlo.
            if (dropped > 0) {
                Text(
                    "$dropped líneas descartadas por desborde del anillo",
                    color = MaterialTheme.colorScheme.error,
                    style = MaterialTheme.typography.bodySmall,
                )
            }

            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 48.dp, max = 220.dp)
                    .clip(RoundedCornerShape(4.dp))
                    .background(HarnessTokens.InsetSurface)
                    .padding(6.dp)
                    .verticalScroll(rememberScrollState()),
            ) {
                if (logLines.isEmpty()) {
                    Text(
                        if (logsEnabled) "sin líneas — apretá \"vaciar\"" else "captura apagada",
                        color = HarnessTokens.LogMeta,
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace,
                    )
                } else {
                    logLines.forEach { line ->
                        Text(
                            line,
                            color = HarnessTokens.LogText,
                            style = MaterialTheme.typography.bodySmall,
                            fontFamily = FontFamily.Monospace,
                        )
                    }
                }
            }
        }
    }
}

/**
 * El anillo nativo guarda 4000 líneas; mostrarlas todas en una `Column` de Compose
 * arma 4000 composables y traba la UI. Este tope es de la vista, no de la captura:
 * el contador de descartes sigue contando lo que el **anillo** perdió, que es otra
 * cosa y no hay que confundirla.
 */
private const val MAX_VISIBLE_LINES = 300
