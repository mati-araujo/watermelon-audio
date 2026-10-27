package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.InternalWatermelonApi
import kotlinx.cinterop.ExperimentalForeignApi
import platform.Foundation.NSTemporaryDirectory
import platform.posix.F_OK
import platform.posix.access
import platform.posix.remove
import kotlin.random.Random
import kotlin.test.AfterTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * Hallazgo 3 (security-auditor de REQ-055) — el path del volcado del afinador cruza la frontera de
 * iOS con su nombre EXACTO, o no cruza.
 *
 * cinterop convierte el `String` a `const char*` en UTF-8 (`cstr`): un `'\u0000'` lo corta ahí y un
 * surrogate suelto se vuelve U+FFFD, así que el archivo quedaría con otro nombre que el del
 * resultado. Sin audio (no se arranca CoreAudio, igual que el resto de esta suite) las tomas cierran
 * inválidas: lo que se mide es el NOMBRE.
 */
@OptIn(InternalWatermelonApi::class, ExperimentalForeignApi::class)
class IosTunerDiagnosticsTest {

    private val bridge = IosAudioBridge()
    private val suffix = Random.nextInt(0, Int.MAX_VALUE).toString(16)
    private val tmp = NSTemporaryDirectory()
    private val created = mutableListOf<String>()

    private fun exists(path: String): Boolean = access(path, F_OK) == 0

    @AfterTest
    fun cleanup() {
        bridge.stopTunerInputDump()
        created.forEach { remove(it) }
    }

    /**
     * Un path con `\u0000` se rechaza SIN llamar al nativo: `ok.wav\u0000.wav` cortado por C sería
     * `ok.wav`, un path válido que el start aceptaría (y grabaría ahí).
     * Mutante que mata: quitar la guarda del puente de iOS.
     */
    @Test
    fun hallazgo3_aPathWithNulIsRejectedAndCreatesNothing() {
        val prefix = tmp + "req055-nul-$suffix.wav"
        created += listOf("$prefix.part", prefix, tmp + "req055-nul-$suffix.invalid.wav")

        assertFalse(bridge.startTunerInputDump("$prefix\u0000.wav"), "un path con NUL arrancó una toma")
        assertNull(bridge.stopTunerInputDump(), "un start rechazado no deja toma")
        assertFalse(exists("$prefix.part"), "se grabó en el prefijo anterior al NUL")
        assertFalse(exists(tmp + "req055-nul-$suffix.invalid.wav"))
    }

    /** Un surrogate suelto se rechaza: no tiene UTF-8 y su reemplazo cambiaría el nombre. */
    @Test
    fun hallazgo3_aLoneSurrogateIsRejected() {
        val path = tmp + "req055-sur-$suffix-\uD83C.wav"
        assertFalse(bridge.startTunerInputDump(path))
        assertNull(bridge.stopTunerInputDump())
        assertFalse(exists(tmp + "req055-sur-$suffix-�.wav.part"), "cruzó con U+FFFD")
    }

    /**
     * Un emoji (fuera del BMP) queda en el disco con ESE nombre, y es el que el resultado nombra.
     * Mutante que mata: una conversión que no sea UTF-8 de 4 bytes.
     */
    @Test
    fun hallazgo3_anEmojiPathEndsOnDiskWithThatExactName() {
        val requested = tmp + "req055-🎸-$suffix.wav"   // 🎸
        val invalid = tmp + "req055-🎸-$suffix.invalid.wav"
        created += listOf(invalid, "$requested.part")

        assertTrue(bridge.startTunerInputDump(requested), "el path es válido: el start tiene que andar")
        val r = assertNotNull(bridge.stopTunerInputDump())
        assertEquals(invalid, r.path, "sin audio la toma cierra inválida")
        assertTrue(r.finalized)
        assertTrue(exists(r.path), "el resultado apunta a un archivo que no está en el disco")
        assertFalse(exists("$requested.part"))
    }
}
