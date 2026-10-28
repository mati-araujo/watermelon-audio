package com.watermellonstudios.audio.internal.bridge

import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertNull

/**
 * Hallazgo 3 (security-auditor de REQ-055) — el path de un volcado cruza a C con sus bytes UTF-8
 * EXACTOS, o no cruza. Es la guarda compartida de los dos puentes ([tunerDumpPathUtf8]).
 */
class TunerDumpPathTest {

    /**
     * Un `\u0000` en cualquier lugar NO cruza.
     *
     * Bug que atrapa: C corta en el NUL y el volcado graba en el prefijo (`a.wav\u0000.txt` -> `a.wav`),
     * un archivo distinto del que el resultado nombra. Mutante que mata: no buscar el NUL (o buscarlo
     * sólo al final).
     */
    @Test
    fun `hallazgo 3 - un path con NUL no cruza`() {
        assertNull(tunerDumpPathUtf8("dir/a.wav\u0000.txt"))
        assertNull(tunerDumpPathUtf8("\u0000dir/a.wav"))
        assertNull(tunerDumpPathUtf8("dir/a.wav\u0000"))
    }

    /**
     * Un surrogate suelto NO cruza: no tiene UTF-8 y reemplazarlo cambia el nombre.
     *
     * Mutante que mata: `encodeToByteArray()` sin `throwOnInvalidSequence` (cruzaría `EF BF BD`).
     */
    @Test
    fun `hallazgo 3 - un surrogate suelto no cruza`() {
        assertNull(tunerDumpPathUtf8("dir/a\uD83C.wav"), "alto sin bajo")
        assertNull(tunerDumpPathUtf8("dir/a\uDFB8.wav"), "bajo sin alto")
        assertNull(tunerDumpPathUtf8(""), "vacío")
    }

    /**
     * Un carácter fuera del BMP cruza como UTF-8 REAL de 4 bytes, no como el UTF-8 modificado del
     * JNI (dos surrogates de 3 bytes cada uno, `ED A0 BC ED BE B8`), que no es un nombre válido.
     *
     * Mutante que mata: codificar cada `Char` por separado (CESU-8).
     */
    @Test
    fun `hallazgo 3 - fuera del BMP cruza como UTF-8 real`() {
        val bytes = tunerDumpPathUtf8("a🎸ñ.wav")   // "a🎸ñ.wav"
        val expected = byteArrayOf(
            0x61,
            0xF0.toByte(), 0x9F.toByte(), 0x8E.toByte(), 0xB8.toByte(),
            0xC3.toByte(), 0xB1.toByte(),
            0x2E, 0x77, 0x61, 0x76,
        )
        assertContentEquals(expected, bytes)
    }
}
