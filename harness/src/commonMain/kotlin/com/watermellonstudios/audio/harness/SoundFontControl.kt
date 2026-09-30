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
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.watermellonstudios.audio.harness.smoke.SmokeReporter
import com.watermellonstudios.audio.harness.soundfont.Fixtures
import com.watermellonstudios.audio.harness.soundfont.SoundFontCheck
import com.watermellonstudios.audio.harness.soundfont.SoundFontPort
import kotlinx.coroutines.launch

/**
 * Control 10 — SoundFont (MINI-038).
 *
 * Carga los fixtures `.sf2`/`.sf3` que el build genera, lista los presets, toca una nota y
 * descarga. Cada botón corre la MISMA verificación que el plan por adb ([SoundFontCheck]) y emite
 * la misma línea `HARNESS-SMOKE` (`run=ui`): lo que dice la pantalla es lo que dice el log.
 *
 * El `.sf3` es el camino de stb_vorbis (REQ-048): sin este control no se ejercitaba en ningún
 * device. En Android, [extras] suma el selector de archivos del sistema, que carga por fd.
 */
@Composable
fun SoundFontControl(
    port: SoundFontPort,
    check: SoundFontCheck,
    fixtures: Fixtures,
    reporter: SmokeReporter,
    extras: (@Composable (SoundFontCheck, SmokeReporter) -> Unit)?,
    modifier: Modifier = Modifier,
) {
    val scope = rememberCoroutineScope()
    var busy by remember { mutableStateOf(false) }
    var status by remember { mutableStateOf("sin SoundFont") }
    var presets by remember { mutableStateOf<List<String>>(emptyList()) }
    var preset by remember { mutableStateOf(0) }
    // El panel de las líneas de "tocar" y "descargar" es el del último fixture cargado.
    var panel by remember { mutableStateOf("sf2") }

    fun refresh() {
        if (!port.isLoaded()) {
            presets = emptyList()
            preset = 0
            return
        }
        val count = port.presetCount()
        presets = (0 until count).map { i ->
            val bp = port.bankProgram(i)
            "$i: ${port.presetName(i) ?: "?"}" + (bp?.let { " (${it[0]}:${it[1]})" } ?: "")
        }
        preset = preset.coerceIn(0, (count - 1).coerceAtLeast(0))
    }

    fun loadFixture(name: String) {
        busy = true
        scope.launch {
            try {
                panel = if (name == Fixtures.SF3) "sf3" else "sf2"
                val path = fixtures.materialize(reporter, panel, name)
                status = when {
                    path == null && name == Fixtures.SF3 && fixtures.lastFailure == "no-empaquetado" -> SF3_NOT_PACKAGED
                    path == null -> "$name: fixture no disponible (${fixtures.lastFailure ?: "ver log"})"
                    check.load(reporter, panel, path, name) -> "$name: cargado, ${port.presetCount()} presets"
                    else -> "$name: NO cargó (presets = ${port.presetCount()})"
                }
                refresh()
            } finally {
                busy = false
            }
        }
    }

    Card(modifier = modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text("SoundFont", style = MaterialTheme.typography.titleMedium)
            Text(status, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)

            Row(
                modifier = Modifier.horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Button(enabled = !busy, onClick = { loadFixture(Fixtures.SF2) }) { Text(".sf2") }
                Button(enabled = !busy, onClick = { loadFixture(Fixtures.SF3) }) { Text(".sf3") }
                Button(enabled = !busy, onClick = {
                    val path = fixtures.notASoundFont(reporter, "sf2")
                    status = if (path == null) {
                        "no se pudo escribir el archivo trucho (ver log)"
                    } else if (check.loadRejects(reporter, "sf2", path, Fixtures.NOT_A_SOUNDFONT)) {
                        "no-SoundFont: rechazado, como corresponde"
                    } else {
                        "no-SoundFont: ¡CARGÓ! (AC-3 roto)"
                    }
                    refresh()
                }) { Text("no-SoundFont") }
                Button(enabled = !busy, onClick = {
                    status = if (check.unload(reporter, panel)) "descargado" else "descargar FALLÓ: sigue cargado"
                    refresh()
                }) { Text("descargar") }
            }

            if (presets.isNotEmpty()) {
                Text(
                    "preset ${presets.getOrElse(preset) { "?" }}  ·  ${presets.size} en total",
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    Button(enabled = preset > 0, onClick = { preset-- }) { Text("◀") }
                    Button(enabled = preset < presets.size - 1, onClick = { preset++ }) { Text("▶") }
                    Button(enabled = !busy, onClick = {
                        busy = true
                        scope.launch {
                            try {
                                val ok = check.playNote(reporter, panel, presetIndex = preset)
                                status = if (ok) "nota: sonó (ver pico en el log)" else "nota: NO sonó (ver motivo en el log)"
                            } finally {
                                busy = false
                            }
                        }
                    }) { Text("tocar A4") }
                }
            }

            extras?.invoke(check, reporter)
        }
    }
}

/**
 * D10: el build empaqueta el .sf3 sólo si tuvo encoder Vorbis. Sin él, la pantalla lo dice con estas
 * palabras, y la línea `panel=sf3 step=fixture ok=false motivo=no-empaquetado` lo dice al script.
 */
const val SF3_NOT_PACKAGED = "fixture .sf3 no empaquetado: el build no tenía encoder Vorbis"
