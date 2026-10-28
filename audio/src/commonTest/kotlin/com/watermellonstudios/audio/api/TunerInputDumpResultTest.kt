package com.watermellonstudios.audio.api

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * REQ-055 S0 · 0.1 — el resultado del volcado a WAV de la entrada del afinador, del lado Kotlin.
 *
 * AC-055.1 — el corpus se captura por el camino del afinador. Esta clase prueba el último eslabón
 * antes de la app: cómo los siete `Long` y el path que devuelve la frontera nativa se vuelven un
 * [TunerInputDumpResult]. Es lógica pura, sin motor ni disco, y es donde se rompería en silencio:
 * un índice corrido hace que "perdidas" se lea como "frames escritos", y la app mostraría una toma
 * sana con el contador equivocado.
 *
 * Los siete valores del test llevan números DISTINTOS entre sí (y ninguno es 0 ni 1), para que un
 * orden de campos equivocado cambie algo observable: con ceros y unos, dos índices permutados dan
 * el mismo objeto.
 */
@OptIn(InternalWatermelonApi::class)
class TunerInputDumpResultTest {

    /** framesWritten, framesLost, discontinuities, sampleRate, inputSource, valid, finalized. */
    private fun values(valid: Long = 1L, finalized: Long = 1L) =
        longArrayOf(132_300L, 7L, 3L, 44_100L, 2L, valid, finalized)

    /**
     * Una ausencia o un largo que no es 7 es `null`, no un resultado de ceros.
     *
     * Bug que atrapa: tolerar un array de otro largo (un cliente nativo viejo o uno nuevo con un
     * campo de más) y leer basura o un `ArrayIndexOutOfBounds` en la app. Mutante que mata: `size < 7`
     * en vez de `size != 7`; devolver un resultado vacío para `null`.
     */
    @Test
    fun `null o un largo distinto de siete no es un resultado`() {
        assertNull(TunerInputDumpResult.fromNative(null, "a/b.wav"), "null cruza como ausencia")
        assertNull(TunerInputDumpResult.fromNative(longArrayOf(), "a/b.wav"), "vacío")
        assertNull(TunerInputDumpResult.fromNative(longArrayOf(1, 2, 3, 4, 5, 6), "a/b.wav"), "seis")
        assertNull(TunerInputDumpResult.fromNative(longArrayOf(1, 2, 3, 4, 5, 6, 7, 8), "a/b.wav"), "ocho")
        assertEquals(7, TunerInputDumpResult.VALUE_COUNT)
    }

    /**
     * El ORDEN de los siete campos: framesWritten, framesLost, discontinuities, sampleRate,
     * inputSource, valid, finalized. Es el mismo que `wma_tuner_diag_input_dump_stop` escribe.
     * Mutante que mata: cualquier permutación de índices.
     */
    @Test
    fun `los siete valores se leen en el orden del contrato`() {
        val r = assertNotNull(TunerInputDumpResult.fromNative(values(), "a/b.wav"))
        assertEquals(132_300L, r.framesWritten)
        assertEquals(7L, r.framesLost)
        assertEquals(3L, r.discontinuities)
        assertEquals(44_100, r.sampleRate)
        assertEquals(2, r.inputSource)
        assertTrue(r.valid)
        assertTrue(r.finalized)
    }

    /**
     * BugA (REQ-055 S0) — el path del resultado es EL QUE INFORMA EL NATIVO, tal cual, sea cual sea
     * la combinación de `valid` y `finalized`: Kotlin no lo reconstruye con un path pedido.
     *
     * Bug que atrapa: un `fromNative` que vuelve a derivar el nombre (con el último path pedido, que
     * tras un start rechazado no es el de la toma que corrió). Mutante que mata: recalcular el path
     * con `invalidPathFor`/`.part` en vez de usar el del nativo.
     */
    @Test
    fun `BugA - el path es el que informa el nativo sin reconstruirlo`() {
        // Un path que NINGUNA regla de nombres produce a partir de otro: si sale igual, viajó.
        val native = "dir/la-que-corrio.invalid.wav"
        for (valid in longArrayOf(0L, 1L)) {
            for (finalized in longArrayOf(0L, 1L)) {
                val r = assertNotNull(TunerInputDumpResult.fromNative(values(valid, finalized), native))
                assertEquals(native, r.path, "valid=$valid finalized=$finalized")
            }
        }
    }

    /** Sin path nativo no hay resultado: una toma cerrada siempre deja un archivo. */
    @Test
    fun `BugA - sin path nativo no hay resultado`() {
        assertNull(TunerInputDumpResult.fromNative(values(), null))
        assertNull(TunerInputDumpResult.fromNative(values(), ""))
    }

    /**
     * `valid` es el índice 5: 1 = sana, 0 = no, y no depende del path.
     * Mutante que mata: `valid` leído de otro índice o invertido.
     */
    @Test
    fun `valid sale del indice 5`() {
        assertTrue(assertNotNull(TunerInputDumpResult.fromNative(values(valid = 1L), "dir/toma.wav")).valid)
        val bad = assertNotNull(TunerInputDumpResult.fromNative(values(valid = 0L), "dir/toma.invalid.wav"))
        assertFalse(bad.valid)
        // Y el resto de los campos no depende de `valid`.
        assertEquals(132_300L, bad.framesWritten)
        assertEquals(7L, bad.framesLost)
    }

    /**
     * `finalized = 0` (no se pudo cerrar o renombrar): la toma no es válida aunque el índice `valid`
     * dijera 1. Mutante que mata: ignorar `values[6]`.
     */
    @Test
    fun `una toma sin finalizar nunca es valida`() {
        for (valid in longArrayOf(0L, 1L)) {
            val r = assertNotNull(TunerInputDumpResult.fromNative(values(valid = valid, finalized = 0L), "dir/toma.wav.part"))
            assertFalse(r.finalized)
            assertFalse(r.valid, "sin archivo nombrado no hay toma sana (valid nativo = $valid)")
        }
    }

    /**
     * `invalidPathFor` aplica LA MISMA regla que `invalidDumpPathFor` de C++ (el archivo lo nombra
     * el nativo y Kotlin tiene que predecir el nombre, sin leer el disco).
     *
     * Casos de borde que delatan una regla distinta: sin extensión, un `.wav` en un DIRECTORIO, doble
     * extensión, varios puntos. Mutante que mata: reemplazar el primer `.`; usar `substringBeforeLast`
     * sobre el path entero.
     */
    @Test
    fun `invalidPathFor sigue la regla del nativo`() {
        assertEquals("a/b.invalid.wav", TunerInputDumpResult.invalidPathFor("a/b.wav"))
        assertEquals("x.invalid.wav", TunerInputDumpResult.invalidPathFor("x.wav"))
        assertEquals("a/b.invalid.wav", TunerInputDumpResult.invalidPathFor("a/b"), "sin .wav se agrega el sufijo entero")
        assertEquals(
            "dir.wav/take.invalid.wav",
            TunerInputDumpResult.invalidPathFor("dir.wav/take"),
            "el .wav de un directorio no es la extensión",
        )
        assertEquals("x.wav.invalid.wav", TunerInputDumpResult.invalidPathFor("x.wav.wav"))
        assertEquals("a.b.c.invalid.wav", TunerInputDumpResult.invalidPathFor("a.b.c.wav"))
    }

    /**
     * `seconds` = framesWritten / sampleRate, en Double, y 0 cuando el rate es 0.
     *
     * Bugs que atrapa: la división entera (66 150 / 44 100 daría 1 y no 1,5), la conversión de un
     * `Long` grande a `Int` (más de 2^31 frames desborda: a 44,1 kHz son 13,5 horas), y un 0/0 que
     * devuelve NaN (una toma que nunca recibió audio no puede mostrar "NaN s" en la app).
     */
    @Test
    fun `seconds divide en Double sin desbordar y es cero sin rate`() {
        fun secondsOf(frames: Long, rate: Long): Double =
            assertNotNull(TunerInputDumpResult.fromNative(longArrayOf(frames, 0, 0, rate, 0, 1, 1), "x.wav")).seconds

        assertEquals(1.0, secondsOf(44_100, 44_100))
        assertEquals(1.5, secondsOf(66_150, 44_100), "división entera: 66150/44100 no es 1")
        assertEquals(3.0, secondsOf(96_000, 32_000))
        assertEquals(100_000.0, secondsOf(4_410_000_000L, 44_100), "más de 2^31 frames no puede desbordar un Int")

        assertEquals(0.0, secondsOf(0, 0), "0 frames y rate 0: cero, no NaN")
        assertEquals(0.0, secondsOf(123_456, 0), "sin rate no hay duración: cero, no infinito")
    }

    /**
     * Una toma de cero frames (la del host, o una sin audio) es un resultado completo y NO válido.
     * Los campos cruzan tal cual: el cero es un dato, no una ausencia.
     */
    @Test
    fun `una toma vacia es un resultado y no un null`() {
        val r = assertNotNull(TunerInputDumpResult.fromNative(longArrayOf(0, 0, 0, 0, 0, 0, 1), "t.invalid.wav"))
        assertEquals(0L, r.framesWritten)
        assertFalse(r.valid)
        assertEquals("t.invalid.wav", r.path)
        assertEquals(0.0, r.seconds)
    }
}
