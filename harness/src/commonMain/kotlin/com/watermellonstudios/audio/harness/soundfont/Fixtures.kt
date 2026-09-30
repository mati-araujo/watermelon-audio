package com.watermellonstudios.audio.harness.soundfont

import com.watermellonstudios.audio.harness.resources.Res
import com.watermellonstudios.audio.harness.smoke.SmokeReporter
import kotlinx.coroutines.CancellationException
import org.jetbrains.compose.resources.ExperimentalResourceApi

/**
 * MINI-038 — los fixtures `.sf2`/`.sf3` que el build GENERA (`scripts/gen-harness-soundfonts.py`)
 * y empaqueta como recursos de Compose (`files/`): assets en Android, bundle en iOS.
 *
 * El motor los carga por PATH, así que acá se leen del paquete y se escriben a un archivo de la
 * app con el `writeFile` del slot de plataforma. Cada paso emite `step=fixture`: si el fixture no
 * está empaquetado, eso es un FAIL con su motivo, no un panel que no hace nada.
 */
class Fixtures(private val writeFile: (name: String, bytes: ByteArray) -> String) {

    /** El path del fixture [name] listo para cargar, o `null` (ya reportado). */
    @OptIn(ExperimentalResourceApi::class)
    suspend fun materialize(reporter: SmokeReporter, panel: String, name: String): String? {
        val bytes = try {
            Res.readBytes("files/$name")
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            reporter.report(
                panel, "fixture", false,
                "archivo" to name, "motivo" to "no-empaquetado", "error" to e::class.simpleName,
            )
            return null
        }
        return write(reporter, panel, name, bytes)
    }

    /** Un archivo que NO es SoundFont, para el caso de rechazo de AC-3. */
    fun notASoundFont(reporter: SmokeReporter, panel: String): String? =
        write(reporter, panel, NOT_A_SOUNDFONT, NOT_A_SOUNDFONT_BYTES)

    private fun write(reporter: SmokeReporter, panel: String, name: String, bytes: ByteArray): String? {
        val path = try {
            writeFile(name, bytes)
        } catch (e: Exception) {
            reporter.report(
                panel, "fixture", false,
                "archivo" to name, "motivo" to "escritura", "error" to (e.message ?: e::class.simpleName),
            )
            return null
        }
        reporter.report(panel, "fixture", true, "archivo" to name, "bytes" to bytes.size)
        return path
    }

    companion object {
        const val SF2 = "wma-fixture.sf2"
        const val SF3 = "wma-fixture.sf3"
        const val NOT_A_SOUNDFONT = "no-soundfont.sf2"

        /** Empieza como un RIFF para que el rechazo no dependa del primer byte. */
        val NOT_A_SOUNDFONT_BYTES: ByteArray =
            "RIFF\u0010\u0000\u0000\u0000WAVEesto no es un SoundFont".encodeToByteArray()
    }
}
