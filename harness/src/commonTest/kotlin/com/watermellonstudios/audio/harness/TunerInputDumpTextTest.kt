package com.watermellonstudios.audio.harness

import com.watermellonstudios.audio.api.InternalWatermelonApi
import com.watermellonstudios.audio.api.TunerInputDumpResult
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/** REQ-055 S0 · 0.1 — los textos del control de grabación de la entrada del afinador. */
@OptIn(InternalWatermelonApi::class)
class TunerInputDumpTextTest {

    private fun result(
        written: Long = 132_300L,
        lost: Long = 0L,
        discontinuities: Long = 0L,
        rate: Int = 44_100,
        source: Int = 2,
        valid: Boolean = true,
        finalized: Boolean = true,
        path: String = "/sdcard/a.wav",
    ) = TunerInputDumpResult(written, lost, discontinuities, rate, source, valid, finalized, path)

    /** Bug que atrapa: un nombre sin fecha (las tomas se pisan) o con la fecha mal ordenada. */
    @Test
    fun theFileNameCarriesTheDateAndTimeSortably() {
        assertEquals(
            "afinador-entrada-20261014-150405.wav",
            tunerInputDumpFileName("2026-10-14T15:04:05.123456Z"),
        )
        assertEquals("afinador-entrada-20261014-150405.wav", tunerInputDumpFileName("2026-10-14T15:04:05Z"))
    }

    /** Bug que atrapa: inventar una fecha cuando el reloj no dio una forma conocida. */
    @Test
    fun aMalformedInstantSaysSoInsteadOfInventingADate() {
        assertEquals("afinador-entrada-sin-hora.wav", tunerInputDumpFileName("ayer"))
    }

    /** Bug que atrapa: una doble barra en el path (o ninguna) según cómo venga el directorio. */
    @Test
    fun theDirectoryIsJoinedWithASingleSlash() {
        assertEquals("/d/x.wav", tunerInputDumpPath("/d", "x.wav"))
        assertEquals("/d/x.wav", tunerInputDumpPath("/d/", "x.wav"))
    }

    /** Bug que atrapa: una toma sana que se presenta como mala o al revés, o sin el comando para bajarla. */
    @Test
    fun aValidTakeSaysSoAndOffersThePullOfTheFinalPath() {
        val text = tunerInputDumpResultText(result())
        assertTrue(text.startsWith("VÁLIDA · 3.0 s · 44100 Hz · USB"), text)
        assertTrue("frames 132300 · perdidos 0 · discontinuidades 0" in text, text)
        assertTrue(text.endsWith("/sdcard/a.wav\nadb pull /sdcard/a.wav"), text)
        assertFalse("INVÁLIDA" in text)
        assertFalse("NO sirve" in text)
    }

    /** Bug que atrapa: una toma con huecos presentada sin el porqué, o con el path pedido y no el final. */
    @Test
    fun anInvalidTakeNamesEveryReasonAndTheFinalInvalidPath() {
        val text = tunerInputDumpResultText(
            result(lost = 7, discontinuities = 3, valid = false, path = "/sdcard/a.invalid.wav"),
        )
        assertTrue(text.startsWith("INVÁLIDA"), text)
        assertTrue("se perdieron 7 frames" in text, text)
        assertTrue("hubo 3 discontinuidades" in text, text)
        assertTrue("esta toma NO sirve para medir" in text, text)
        assertTrue(text.endsWith("adb pull /sdcard/a.invalid.wav"), text)
    }

    /** Bug que atrapa: una toma vacía (sin afinador) que se presenta sin decir por qué. */
    @Test
    fun anEmptyTakeSaysNoAudioEnteredAndTheRateIsUnknown() {
        val text = tunerInputDumpResultText(result(written = 0, rate = 0, valid = false, path = "/s/a.invalid.wav"))
        assertTrue("no entró audio" in text, text)
        assertTrue("rate desconocido" in text, text)
        assertTrue("0.0 s" in text, text)
    }

    /** Bug que atrapa: una inválida sin contadores que no dice nada (error del disco). */
    @Test
    fun anInvalidTakeWithCleanCountersBlamesTheFile() {
        val text = tunerInputDumpResultText(result(valid = false))
        assertTrue("falló la escritura o el cierre del archivo" in text, text)
    }

    /**
     * Bug que atrapa: una toma que no se pudo cerrar presentada con `adb pull` de un archivo que no
     * existe (o como "INVÁLIDA" con el nombre `.invalid` que nunca se creó), y el .part sin mencionar.
     */
    @Test
    fun aTakeThatCouldNotBeFinalizedPointsAtThePartAndOffersNoPull() {
        val text = tunerInputDumpResultText(
            result(valid = false, finalized = false, path = "/sdcard/a.wav.part"),
        )
        assertTrue(text.startsWith("SIN CERRAR"), text)
        assertTrue("NO SE PUDO CERRAR NI RENOMBRAR" in text, text)
        assertTrue("/sdcard/a.wav.part" in text, text)
        assertFalse("adb pull" in text, text)
        assertFalse("falló la escritura o el cierre" in text, text)
        assertTrue("finalized=false" in tunerInputDumpLogLine(result(valid = false, finalized = false, path = "/sdcard/a.wav.part")))
    }

    /** Bug que atrapa: la línea de log sin la ruta final o sin el veredicto (es lo que se ve con adb logcat). */
    @Test
    fun theLogLineCarriesPathVerdictSecondsAndRate() {
        assertEquals(
            "HARNESS-DUMP path=/sdcard/a.wav valid=true seconds=3.0 rate=44100 source=2 frames=132300 " +
                "lost=0 discontinuities=0 finalized=true",
            tunerInputDumpLogLine(result()),
        )
    }

    /** Bug que atrapa: no avisar que sin afinador la toma sale vacía. */
    @Test
    fun theStatusWarnsWhenTheTunerIsNotRunning() {
        assertEquals("sin grabar", tunerInputDumpStatusText(null, 0, tunerRunning = true))
        assertTrue("NO ESTÁ CORRIENDO" in tunerInputDumpStatusText(null, 0, tunerRunning = false))
        val recording = tunerInputDumpStatusText("/s/a.wav", 12, tunerRunning = true)
        assertEquals("grabando · 12s\n/s/a.wav.part", recording)
        assertTrue("NO ESTÁ CORRIENDO" in tunerInputDumpStatusText("/s/a.wav", 12, tunerRunning = false))
    }
}
