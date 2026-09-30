package com.watermellonstudios.audio.harness.usb

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
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.watermellonstudios.audio.domain.usb.UsbConnectionState
import com.watermellonstudios.audio.domain.usb.UsbTestResult
import com.watermellonstudios.audio.domain.usb.UsbTestStatus
import com.watermellonstudios.audio.domain.usb.UsbTransferStats
import com.watermellonstudios.audio.harness.smoke.SmokeReporter
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/**
 * Control 11 — USB (MINI-038, androidMain). Entra a `HarnessApp` por el slot de plataforma.
 *
 * Es la única forma de que libusb corra en un device desde este repo: el backend USB necesita el
 * fd de `UsbManager.openDevice()`, y hasta acá el harness nunca lo pedía — el selector de backend
 * de Diagnóstico caía a Oboe con "USB backend not available".
 *
 * Cada botón llama al mismo [UsbHarness] que el plan por adb, así que emite la misma línea
 * `HARNESS-SMOKE panel=usb`, y la pantalla muestra el estado y el error TIPADO que devolvió la
 * librería, no un "OK".
 */
@Composable
fun UsbPanel(
    usb: UsbHarness,
    reporter: SmokeReporter,
    prepareForUsb: suspend () -> Boolean,
    modifier: Modifier = Modifier,
) {
    val scope = rememberCoroutineScope()
    val devices by usb.manager.connectedDevices.collectAsState()
    val state by usb.manager.connectionState.collectAsState()
    var selected by remember { mutableStateOf(0) }
    var busy by remember { mutableStateOf(false) }
    var last by remember { mutableStateOf("—") }
    var stats by remember { mutableStateOf<UsbTransferStats?>(null) }
    var results by remember { mutableStateOf<List<UsbTestResult>>(emptyList()) }

    // Stats en vivo mientras hay streaming. Un poll de UI: la librería no expone un Flow de stats.
    LaunchedEffect(state) {
        while (state == UsbConnectionState.STREAMING) {
            stats = usb.manager.getTransferStats()
            delay(250)
        }
    }

    fun act(label: String, block: suspend () -> Boolean) {
        busy = true
        scope.launch {
            try {
                last = "$label: " + if (block()) "ok" else "FALLÓ (ver la línea en el log)"
            } finally {
                busy = false
            }
        }
    }

    // Con un dispositivo desenchufado el índice puede quedar afuera: se vuelve al primero.
    val current = if (selected < devices.size) selected else 0
    val device = devices.getOrNull(current)

    Card(modifier = modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("USB (libusb)", style = MaterialTheme.typography.titleMedium)
            Text(
                "estado: $state · fd ${usb.manager.getFileDescriptor()} · UAC ${usb.manager.getUacVersion()}",
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace,
            )
            Text("último: $last", style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)

            if (devices.isEmpty()) {
                Text("sin dispositivos de audio USB", style = MaterialTheme.typography.bodySmall)
            }
            devices.forEachIndexed { i, d ->
                Text(
                    (if (i == current) "▶ " else "  ") + UsbHarness.describe(d),
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                )
            }

            Row(
                modifier = Modifier.horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Button(enabled = !busy, onClick = { act("listar") { usb.listDevices(reporter).isNotEmpty() } }) {
                    Text("listar")
                }
                Button(enabled = !busy && devices.size > 1, onClick = { selected = (current + 1) % devices.size }) {
                    Text("siguiente")
                }
                Button(enabled = !busy && device != null, onClick = {
                    act("conectar") {
                        // Misma invariante que el plan: el motor no se pelea con USB por el device.
                        prepareForUsb() && usb.connect(reporter, device!!, humanTimeoutMs = UI_PERMISSION_TIMEOUT_MS)
                    }
                }) { Text("permiso + conectar") }
                Button(enabled = !busy, onClick = { act("desconectar") { usb.disconnect(reporter) } }) {
                    Text("desconectar")
                }
            }
            Row(
                modifier = Modifier.horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Button(enabled = !busy, onClick = { act("streaming") { prepareForUsb() && usb.startStreaming(reporter) } }) {
                    Text("start stream")
                }
                Button(enabled = !busy, onClick = { act("stop") { usb.stopStreaming(reporter) } }) {
                    Text("stop stream")
                }
                Button(enabled = !busy && device != null, onClick = {
                    results = emptyList()
                    act("suite") { usb.runSuite(reporter, device!!) { results = results + it } }
                }) { Text("suite") }
            }

            stats?.let { s ->
                Text(
                    "paquetes ${s.packetsCompleted}/${s.packetsSubmitted} · err ${s.packetsErrors} · " +
                        "under ${s.underruns} · over ${s.overruns} · ${s.currentSampleRateHz} Hz · ${s.latencyDisplay}",
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                )
            }
            results.forEachIndexed { i, res ->
                val failed = res.status != UsbTestStatus.PASSED
                Text(
                    "suite ${i + 1}: ${res.testType.displayName} @${res.config.sampleRate} → ${res.status}" +
                        (res.errorMessage?.let { " — $it" } ?: ""),
                    color = if (failed) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface,
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                )
            }
        }
    }
}

/** Desde la UI el humano está delante: si no contesta en dos minutos, se reporta y se suelta. */
private const val UI_PERMISSION_TIMEOUT_MS = 120_000L
