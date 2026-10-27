package com.watermellonstudios.audio.internal.bridge

import java.io.File
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/**
 * Hallazgo 2 (security-auditor de REQ-055) — las overrides de [com.watermellonstudios.audio.api.ITunerDiagnostics]
 * en los DOS puentes llevan `@InternalWatermelonApi`.
 *
 * El `@OptIn` de la clase (que exige nombrar el supertipo marcado) NO propaga el requisito: sin el
 * marcador en la override, `AudioNativeBridge.getInstance().startTunerInputDump(p)` compila sin
 * opt-in (medido: Notas de REQ-055 S0). El marcador es `BINARY`, invisible por reflexión, y el repo
 * no tiene un fixture de compilación negativa: por eso se afirma sobre la FUENTE. Lee los dos
 * archivos (el de iOS no compila en la JVM, pero su texto sí se lee).
 *
 * Mutante que mata: quitar `@InternalWatermelonApi` de cualquiera de las cuatro overrides.
 */
class TunerDiagnosticsOptInTest {

    private val sources = listOf(
        "src/jvmSharedMain/kotlin/com/watermellonstudios/audio/internal/bridge/AudioNativeBridge.kt",
        "src/iosMain/kotlin/com/watermellonstudios/audio/internal/bridge/IosAudioBridge.kt",
    )

    /** El módulo `:audio`, buscado hacia arriba desde el directorio de trabajo del test. */
    private fun moduleFile(relative: String): File {
        var dir: File? = File(System.getProperty("user.dir")).absoluteFile
        while (dir != null) {
            File(dir, relative).takeIf { it.isFile }?.let { return it }
            File(dir, "audio/$relative").takeIf { it.isFile }?.let { return it }
            dir = dir.parentFile
        }
        error("no encontré $relative desde ${System.getProperty("user.dir")}")
    }

    @Test
    fun `hallazgo 2 - las overrides del diagnostico propagan el opt-in en los dos puentes`() {
        val override = Regex("""^\s*override fun (startTunerInputDump|stopTunerInputDump)\b""")
        for (relative in sources) {
            val lines = moduleFile(relative).readLines()
            val found = lines.indices.filter { override.containsMatchIn(lines[it]) }
            assertEquals(2, found.size, "$relative: se esperaban las dos overrides, hay ${found.size}")
            for (i in found) {
                // Las anotaciones van en las líneas inmediatamente anteriores (sin KDoc en el medio).
                val annotations = generateSequence(i - 1) { it - 1 }
                    .takeWhile { it >= 0 && lines[it].trim().startsWith("@") }
                    .map { lines[it].trim() }
                    .toList()
                assertTrue(
                    "@InternalWatermelonApi" in annotations,
                    "$relative:${i + 1} `${lines[i].trim()}` no lleva @InternalWatermelonApi: " +
                        "sin el marcador la llamada sin opt-in compila (hallazgo 2)",
                )
            }
        }
    }
}
