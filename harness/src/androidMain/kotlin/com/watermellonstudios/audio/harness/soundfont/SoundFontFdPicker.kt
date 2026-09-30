package com.watermellonstudios.audio.harness.soundfont

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import com.watermellonstudios.audio.harness.smoke.SmokeReporter
import com.watermellonstudios.audio.api.InternalWatermelonApi
import com.watermellonstudios.audio.internal.bridge.AudioNativeBridge
import com.watermellonstudios.audio.internal.bridge.getAudioBridge
import kotlinx.coroutines.launch

/**
 * MINI-038 — el selector de archivos del sistema (`ACTION_OPEN_DOCUMENT`) que carga un SoundFont
 * por FD con `AudioNativeBridge.loadSoundFontFromFd`, el camino de NoisyPad (asset pack de Play).
 *
 * El fd es del LLAMADOR (KDoc de `loadSoundFontFromFd`): se cierra con `use {}` en todo camino,
 * incluido el de una excepción. Emite `panel=sf-fd step=carga` y, si cargó, toca la nota con el
 * mismo [SoundFontCheck] que los fixtures.
 */
@OptIn(InternalWatermelonApi::class)
@Composable
fun SoundFontFdPicker(check: SoundFontCheck, reporter: SmokeReporter) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var status by remember { mutableStateOf("elegí un .sf2/.sf3 del sistema") }

    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri == null) {
            status = "cancelado"
            return@rememberLauncherForActivityResult
        }
        // `loadSoundFontFromFd` es Android-only a propósito (KDoc de ISoundFontBridge): el puente
        // común no lo tiene, así que acá se baja al de Android.
        val bridge = getAudioBridge() as AudioNativeBridge
        val loaded = try {
            context.contentResolver.openFileDescriptor(uri, "r")?.use { pfd ->
                val size = pfd.statSize
                val ok = size > 0 && bridge.loadSoundFontFromFd(pfd.fd, 0L, size)
                reporter.report(
                    PANEL, "carga", ok && bridge.getSoundFontPresetCount() > 0,
                    "bytes" to size, "cargado" to ok, "presets" to bridge.getSoundFontPresetCount(),
                    "motivo" to if (size <= 0) "tamano-desconocido" else null,
                )
            } ?: reporter.report(PANEL, "carga", false, "motivo" to "sin-fd")
        } catch (e: Exception) {
            reporter.report(PANEL, "carga", false, "motivo" to "excepcion", "error" to (e.message ?: e::class.simpleName))
        }
        status = if (loaded) "cargado por fd, ${bridge.getSoundFontPresetCount()} presets" else "NO cargó (ver log)"
        if (loaded) {
            scope.launch {
                status += if (check.playNote(reporter, PANEL)) " · nota: sonó" else " · nota: NO sonó"
            }
        }
    }

    Text(status, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
    Button(onClick = { launcher.launch(arrayOf("*/*")) }) { Text("elegir archivo (fd)") }
}

private const val PANEL = "sf-fd"
