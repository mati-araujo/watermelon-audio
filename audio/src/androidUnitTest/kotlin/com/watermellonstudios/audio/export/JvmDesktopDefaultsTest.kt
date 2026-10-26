package com.watermellonstudios.audio.export

import com.watermellonstudios.audio.callback.StderrAudioLogger
import kotlinx.coroutines.runBlocking
import java.io.ByteArrayOutputStream
import java.io.PrintStream
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue

/**
 * Los defaults de desktop que viven en jvmShared (REQ-055 S1). Corren acá porque el
 * `desktopTest` no lo corre ningún gate; los `actual` de desktop sólo los enlazan.
 */
class JvmDesktopDefaultsTest {

    // Bug que atrapa: un encoder de desktop que "termina" sin AAC —un Result vacío o un
    // success=true— y el export comprimido le dice a la UI que salió bien.
    @Test
    fun `el encoder sin AAC falla explicito con excepcion, no con un resultado`() {
        val e = assertFailsWith<UnsupportedOperationException> {
            runBlocking { UnsupportedAacEncoder.wavToM4a("in.wav", "out.m4a", Mp4AacTranscoder.Bitrate.HIGH, null) }
        }
        assertEquals(UnsupportedAacEncoder.MESSAGE, e.message)
    }

    // Bug que atrapa: niveles cruzados, un sufijo agregado con params vacío (el mismo
    // contrato que LogcatAudioLogger) o el throwable perdido.
    @Test
    fun `el logger de stderr escribe L-TAG-mensaje y la traza del throwable`() {
        val buf = ByteArrayOutputStream()
        val ps = PrintStream(buf, true, "UTF-8")
        val logger = StderrAudioLogger { ps }

        logger.debug("T", "d")
        logger.info("T", "i")
        logger.warn("T", "w", mapOf("k" to 1))
        logger.error("T", "e", IllegalStateException("boom"))

        val lines = buf.toString("UTF-8").lines()
        assertEquals("D/T: d", lines[0])
        assertEquals("I/T: i", lines[1])
        assertEquals("W/T: w {k=1}", lines[2])
        assertEquals("E/T: e", lines[3])
        assertTrue(lines[4].contains("IllegalStateException: boom"), lines[4])
    }
}
