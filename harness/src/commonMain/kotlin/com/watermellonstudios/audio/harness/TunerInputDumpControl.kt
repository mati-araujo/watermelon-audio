package com.watermellonstudios.audio.harness

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.selection.SelectionContainer
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
import com.watermellonstudios.audio.api.ITunerDiagnostics
import com.watermellonstudios.audio.api.InternalWatermelonApi
import com.watermellonstudios.audio.api.TunerInputDumpResult
import com.watermellonstudios.audio.harness.smoke.SmokeSink
import com.watermellonstudios.audio.internal.bridge.getAudioBridge
import kotlinx.coroutines.delay
import kotlin.time.Clock
import kotlin.time.TimeMark
import kotlin.time.TimeSource

/**
 * Control 9b — grabar lo que el afinador VE (REQ-055 S0 · 0.1), para medirlo sobre material real.
 *
 * ## Por qué es un archivo aparte de [TunerControl]
 *
 * [TunerControl] es la aserción de compilación de REQ-010: no puede tener `@OptIn`. Este control
 * llega al puente (`getAudioBridge() as? ITunerDiagnostics`), que es superficie interna, así que
 * lleva el opt-in acá y deja el otro intacto.
 *
 * ## Qué hace
 *
 * Graba a [dumpDir] un WAV float32 mono con la señal que entra al análisis (no la de una grabadora
 * aparte). Al parar muestra el veredicto —VÁLIDA / INVÁLIDA, con el porqué—, la ruta FINAL del archivo
 * y el `adb pull` para bajarlo, en texto seleccionable. Además emite una línea `HARNESS-DUMP` al sink
 * de la plataforma (logcat / stdout) para verla sin mirar la pantalla.
 *
 * **No arranca el afinador**: sin afinador corriendo no entra audio, y el panel lo dice.
 * Si la pantalla se va con una toma en curso, la toma se cierra: nunca queda un `.part` colgado.
 *
 * @param dumpDir dónde grabar; `null` si la plataforma no dio uno (el panel lo dice).
 * @param sink dónde va la línea `HARNESS-DUMP`.
 */
@OptIn(InternalWatermelonApi::class)
@Composable
fun TunerInputDumpControl(dumpDir: String?, sink: SmokeSink, modifier: Modifier = Modifier) {
    val bridge = remember { getAudioBridge() }
    val diagnostics = remember { bridge as? ITunerDiagnostics }

    var recordingPath by remember { mutableStateOf<String?>(null) }
    var startedAt by remember { mutableStateOf<TimeMark?>(null) }
    var elapsedSeconds by remember { mutableStateOf(0L) }
    var tunerRunning by remember { mutableStateOf(false) }
    var result by remember { mutableStateOf<TunerInputDumpResult?>(null) }
    var message by remember { mutableStateOf<String?>(null) }

    // Cierra la toma y publica el veredicto: pantalla y línea de log cuentan lo mismo.
    fun finishTake() {
        val closed = diagnostics?.stopTunerInputDump()
        recordingPath = null
        startedAt = null
        if (closed == null) {
            result = null
            message = DUMP_STOP_WITHOUT_TAKE_TEXT
        } else {
            result = closed
            message = null
            sink.emit(tunerInputDumpLogLine(closed))
        }
    }

    // Un cruce barato cada medio segundo: el reloj de la toma y si el afinador sigue corriendo.
    LaunchedEffect(Unit) {
        while (true) {
            tunerRunning = bridge.isTunerRunning()
            elapsedSeconds = startedAt?.elapsedNow()?.inWholeSeconds ?: 0L
            delay(STATUS_POLL_MS)
        }
    }

    DisposableEffect(Unit) {
        onDispose { if (recordingPath != null) finishTake() }
    }

    Card(modifier = modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text("Grabar entrada del afinador", style = MaterialTheme.typography.titleMedium)

            when {
                diagnostics == null -> Text(DUMP_UNAVAILABLE_TEXT, style = MaterialTheme.typography.bodySmall)
                dumpDir == null -> Text(DUMP_NO_DIR_TEXT, style = MaterialTheme.typography.bodySmall)
                else -> {
                    Button(
                        onClick = {
                            if (recordingPath != null) {
                                finishTake()
                            } else {
                                val path = tunerInputDumpPath(
                                    dumpDir,
                                    tunerInputDumpFileName(Clock.System.now().toString()),
                                )
                                result = null
                                if (diagnostics.startTunerInputDump(path)) {
                                    recordingPath = path
                                    startedAt = TimeSource.Monotonic.markNow()
                                    elapsedSeconds = 0L
                                    message = null
                                } else {
                                    message = tunerInputDumpStartFailedText(path)
                                }
                            }
                        },
                    ) {
                        Text(if (recordingPath != null) "Detener grabación" else "Grabar entrada del afinador")
                    }

                    // Seleccionable: la ruta y el `adb pull` se copian de la pantalla.
                    SelectionContainer {
                        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Text(
                                text = tunerInputDumpStatusText(recordingPath, elapsedSeconds, tunerRunning),
                                fontFamily = FontFamily.Monospace,
                                style = MaterialTheme.typography.bodySmall,
                            )
                            message?.let {
                                Text(it, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall)
                            }
                            result?.let {
                                Text(
                                    text = tunerInputDumpResultText(it),
                                    fontFamily = FontFamily.Monospace,
                                    style = MaterialTheme.typography.bodySmall,
                                )
                            }
                        }
                    }
                }
            }
        }
    }
}

private const val STATUS_POLL_MS = 500L
