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

    /** El `motivo` del último fixture que no se pudo materializar, para que la UI diga lo mismo que el log. */
    var lastFailure: String? = null
        private set

    /**
     * El path del fixture [name] listo para cargar, o `null` (ya reportado).
     *
     * Primero el MANIFIESTO que escribió el build ([FixtureManifest]): un fixture que no figura ahí
     * no está empaquetado aunque sus bytes hayan quedado en el paquete de un build anterior (D10).
     */
    @OptIn(ExperimentalResourceApi::class)
    suspend fun materialize(reporter: SmokeReporter, panel: String, name: String): String? {
        lastFailure = null
        val manifest = try {
            FixtureManifest.parse(Res.readBytes("files/${FixtureManifest.FILE}").decodeToString())
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            reporter.report(
                panel, "fixture", false,
                "archivo" to name, "motivo" to "sin-manifiesto".also { lastFailure = it }, "error" to e::class.simpleName,
            )
            return null
        }
        val entry = manifest[name]
            ?: run {
                reporter.report(panel, "fixture", false, "archivo" to name, "motivo" to "no-empaquetado".also { lastFailure = it })
                return null
            }
        val bytes = try {
            Res.readBytes("files/$name")
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            reporter.report(
                panel, "fixture", false,
                "archivo" to name, "motivo" to "no-empaquetado".also { lastFailure = it }, "error" to e::class.simpleName,
            )
            return null
        }
        if (bytes.size.toLong() != entry.size) {
            reporter.report(
                panel, "fixture", false,
                "archivo" to name, "motivo" to "fixture-viejo".also { lastFailure = it }, "bytes" to bytes.size, "manifiesto" to entry.size,
            )
            return null
        }
        return write(reporter, panel, name, bytes, entry.sha256)
    }

    /** Un archivo que NO es SoundFont, para el caso de rechazo de AC-3. */
    fun notASoundFont(reporter: SmokeReporter, panel: String): String? =
        write(reporter, panel, NOT_A_SOUNDFONT, NOT_A_SOUNDFONT_BYTES, sha256 = null)

    private fun write(
        reporter: SmokeReporter,
        panel: String,
        name: String,
        bytes: ByteArray,
        sha256: String?,
    ): String? {
        val path = try {
            writeFile(name, bytes)
        } catch (e: Exception) {
            reporter.report(
                panel, "fixture", false,
                "archivo" to name, "motivo" to "escritura".also { lastFailure = it }, "error" to (e.message ?: e::class.simpleName),
            )
            return null
        }
        reporter.report(panel, "fixture", true, "archivo" to name, "bytes" to bytes.size, "sha256" to sha256)
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

/**
 * MINI-038, D10 — el manifiesto que escribe la task `generateHarnessSoundFonts`: una línea por
 * fixture empaquetado, `<nombre> <bytes> <sha256>`. El sha es el mismo que imprime la receta en el
 * log del build, así que una línea `step=fixture` se puede cruzar con el build que la produjo.
 *
 * Límite declarado: la app compara el TAMAÑO, no el sha (commonMain no trae SHA-256). Un fixture viejo
 * del mismo tamaño exacto que el nuevo pasaría; el sha viaja en la línea para verlo a ojo.
 */
object FixtureManifest {
    const val FILE = "fixtures-manifest.txt"

    data class Entry(val size: Long, val sha256: String)

    /** Tira [IllegalArgumentException] ante una línea mal formada: un manifiesto roto no es "vacío". */
    fun parse(text: String): Map<String, Entry> =
        text.lines().filter { it.isNotBlank() }.associate { line ->
            val parts = line.trim().split(' ')
            require(parts.size == 3) { "linea de manifiesto invalida: '$line'" }
            val size = requireNotNull(parts[1].toLongOrNull()) { "tamano invalido: '$line'" }
            parts[0] to Entry(size, parts[2])
        }
}
