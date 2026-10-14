package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.InternalWatermelonApi
import com.watermellonstudios.audio.api.TunerInputDumpResult
import org.junit.After
import org.junit.AfterClass
import org.junit.Before
import org.junit.FixMethodOrder
import org.junit.runners.MethodSorters
import java.io.File
import java.nio.file.Files
import java.text.Normalizer
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * REQ-055 S0 · 0.1 — **el volcado a WAV de la entrada del afinador, cruzando el JNI.**
 *
 * AC-055.1 — el corpus se captura por el camino del afinador; este volcado es su instrumento y el
 * JNI es por donde lo maneja la app. Esta clase ejerce `nativeTunerDiagInputDumpStart` y
 * `nativeTunerDiagInputDumpStop` contra un `JNIEnv` real: lo que el gate de símbolos y el de
 * firmas NO ven y acá sí —que el `jstring` llegue entero al disco, que el `jlongArray` salga con
 * 7 valores y con el orden que Kotlin espera, que "sin toma" cruce como `null` y no como un array
 * de ceros— se mide con valores que cruzaron.
 *
 * ## Qué NO prueba, y el hueco declarado
 *
 * En el host **no entra audio** (el arnés no puede empujar capturas por la frontera), así que
 * todas las tomas de acá son de CERO frames: el camino con datos —el WAV con contenido, los
 * contadores no nulos— lo cubre `core/tests/test_c_api_tuner_dump.cpp`, que sí maneja el backend.
 * Una toma de cero frames igual discrimina lo que importa de la frontera: que cierra como
 * INVÁLIDA (el path pedido NO existe; el `.invalid` sí), que el header escrito es el de 106 bytes y
 * que el nombre viaja intacto.
 *
 * Los dos `startTunerSync`/`stopTunerSync` de acá se llaman DIRECTO y no por [JniHarness.exercise]
 * a propósito: son sólo la forma de tener motor (`nativeStartTuner` es el que lo crea), y anotarlos
 * rompería el trinquete bidireccional de [COVERED].
 *
 * 🔴 Vale lo mismo que en [JniHarness]: backend falso, **no** es verificación en dispositivo.
 */
@OptIn(InternalWatermelonApi::class)
@FixMethodOrder(MethodSorters.NAME_ASCENDING)
class TunerDiagnosticsJniTest {

    companion object {
        private const val OWNER = "TunerDiagnosticsJniTest"

        /** Trinquete bidireccional: ver `JniCoverage.ratchet`. Su diff es la revisión. */
        private val COVERED = setOf("nativeTunerDiagInputDumpStart", "nativeTunerDiagInputDumpStop")

        /** Largo del header fijo del volcado (RIFF/WAVE + fmt + fact + wmad + data). */
        private const val HEADER_BYTES = 106L

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)
    }

    private lateinit var dir: File

    private fun <T> jni(name: String, call: (AudioNativeBridge) -> T): T =
        JniHarness.exercise(OWNER, name, call)

    private fun start(path: String): Boolean =
        jni("nativeTunerDiagInputDumpStart") { it.startTunerInputDump(path) }

    private fun stop(): TunerInputDumpResult? =
        jni("nativeTunerDiagInputDumpStop") { it.stopTunerInputDump() }

    /** Motor vivo: `nativeStartTuner` es el único que lo crea. Directo, sin anotar (ver el KDoc). */
    private fun ensureEngine() {
        assertTrue(AudioNativeBridge.getInstance().startTunerSync(), "no se pudo crear el motor del host")
    }

    private fun pathOf(name: String): String = File(dir, name).path

    private fun ascii(bytes: ByteArray, offset: Int, length: Int): String =
        String(bytes, offset, length, Charsets.US_ASCII)

    @Before
    fun setUp() {
        JniHarness.requireNativeLibrary()
        dir = Files.createTempDirectory("req055-dump").toFile()
    }

    @After
    fun tearDown() {
        // Una toma que haya quedado viva (un test que falló a mitad) no puede contaminar al siguiente
        // ni dejar el afinador corriendo. Directo: no es cobertura, es limpieza.
        val bridge = AudioNativeBridge.getInstance()
        bridge.stopTunerInputDump()
        bridge.stopTunerSync()
        dir.deleteRecursively()
    }

    /**
     * **`null` y no un array de ceros**, con y sin motor.
     *
     * Es la regla que MINI-003 dejó escrita para el snapshot: un array de ceros es un dato plausible
     * y un fallo que lo devuelve se lee como una medición. Acá "sin toma" tiene que cruzar como
     * `null`. Va PRIMERA (nombre `a`) para ver el estado sin motor, que existe una vez por JVM.
     * Mutante que mata: mapear el `false` de la C API a un array vacío o de ceros.
     */
    @Test
    fun `a - stop sin toma devuelve null, con motor y sin motor`() {
        assertNull(stop(), "sin motor ni toma el resultado es null, no un array de ceros")

        ensureEngine()
        assertNull(stop(), "con motor pero sin toma activa sigue siendo null")
    }

    /**
     * El camino entero de una toma sin audio, cruzando el JNI de punta a punta.
     *
     * Mata: el `jlongArray` con otro largo u otro orden de campos; un resultado "valid" sobre cero
     * frames (un WAV vacío con nombre de sano); el rename que no ocurre (queda el `.part`) o que
     * deja el nombre pedido; el header que no es el de 106 bytes; un segundo start que no se rechaza
     * (truncaría el temporal de la toma viva) o que deja la toma a medias.
     */
    @Test
    fun `una toma sin audio cierra invalida con su archivo y su header en el disco`() {
        ensureEngine()
        val path = pathOf("toma.wav")

        assertTrue(start(path), "con motor y un directorio escribible el start tiene que andar")
        assertFalse(start(pathOf("otra.wav")), "ya hay una toma activa: el segundo start se rechaza")
        assertFalse(File(pathOf("otra.wav.part")).exists(), "el start rechazado abrió un archivo")

        val r = assertNotNull(stop(), "había una toma: el stop tiene que devolver su resultado")

        // Los siete valores, en su orden. En el host no entra audio: todo cero, y eso es INVÁLIDO.
        assertEquals(0L, r.framesWritten)
        assertEquals(0L, r.framesLost)
        assertEquals(0L, r.discontinuities)
        assertEquals(0, r.sampleRate, "0 = nunca llegó audio")
        assertEquals(0, r.inputSource, "la fuente por defecto es el micrófono")
        assertFalse(r.valid, "una toma de cero frames no puede declararse válida")
        assertTrue(r.finalized, "el archivo quedó cerrado y nombrado (como inválido)")
        assertEquals(0.0, r.seconds)

        // El path final es el del invalido, y es el que existe en el disco.
        val invalid = TunerInputDumpResult.invalidPathFor(path)
        assertEquals(invalid, r.path)
        assertEquals(pathOf("toma.invalid.wav"), r.path)
        assertTrue(File(invalid).isFile, "el archivo invalido no esta en el disco")
        assertFalse(File(path).exists(), "el nombre PEDIDO no puede existir: es el de una toma sana")
        assertFalse(File("$path.part").exists(), "quedó el temporal: el cierre no hizo el rename")

        // El header: 106 bytes (sin datos), con las etiquetas en su lugar y el wmad al día.
        val bytes = File(invalid).readBytes()
        assertEquals(HEADER_BYTES, bytes.size.toLong(), "sin frames el archivo es sólo el header")
        assertEquals("RIFF", ascii(bytes, 0, 4))
        assertEquals("WAVE", ascii(bytes, 8, 4))
        assertEquals("fmt ", ascii(bytes, 12, 4))
        assertEquals("fact", ascii(bytes, 38, 4))
        assertEquals("wmad", ascii(bytes, 50, 4))
        assertEquals("data", ascii(bytes, 98, 4))
        assertEquals(1, bytes[58].toInt(), "wmad.version")
        assertEquals(0, bytes[94].toInt(), "wmad.valid")

        assertNull(stop(), "la toma ya se cerró: un segundo stop no tiene nada que devolver")
    }

    /**
     * Una toma que no se pudo NOMBRAR cruza como `finalized = false` y apunta al `.part`.
     *
     * Se fuerza con un DIRECTORIO donde iría el `.invalid.wav`: el `rename` del `.part` falla. Sin el
     * séptimo valor Kotlin calcularía `toma.invalid.wav`, que existe... como directorio vacío.
     * Mata: ignorar `finalized` al armar el path; el JNI que copie 6 valores en vez de 7.
     */
    @Test
    fun `una toma que no se pudo nombrar cruza sin finalizar y apunta al part`() {
        ensureEngine()
        val path = pathOf("toma.wav")
        assertTrue(File(pathOf("toma.invalid.wav")).mkdir(), "premisa: un directorio ocupa el nombre final")

        assertTrue(start(path))
        val r = assertNotNull(stop())

        assertFalse(r.finalized)
        assertFalse(r.valid)
        assertEquals("$path.part", r.path)
        assertTrue(File(r.path).isFile, "el archivo real es el .part")
        assertTrue(File(pathOf("toma.invalid.wav")).isDirectory, "el nombre final sigue siendo el directorio")
    }

    /**
     * Cerrar una toma deja la puerta abierta para la siguiente (y el JNI no guarda estado de más).
     *
     * Mata: un start que queda en "toma activa" tras el stop (el segundo start devolvería false
     * para siempre y la app no podría volver a grabar hasta reiniciar).
     */
    @Test
    fun `tras cerrar una toma se puede empezar otra con su propio archivo`() {
        ensureEngine()

        assertTrue(start(pathOf("uno.wav")))
        val first = assertNotNull(stop())
        assertEquals(pathOf("uno.invalid.wav"), first.path)

        assertTrue(start(pathOf("dos.wav")), "tras cerrar se puede volver a grabar")
        val second = assertNotNull(stop())
        assertEquals(pathOf("dos.invalid.wav"), second.path)

        assertTrue(File(first.path).isFile, "la segunda toma tocó el archivo de la primera")
        assertTrue(File(second.path).isFile)
    }

    /**
     * Un start que no puede abrir el archivo devuelve false Y NO deja una toma activa.
     *
     * Mata: un start que devuelve false pero armó el volcado (el stop devolvería un resultado de
     * una toma que el consumidor sabe que no empezó, y el siguiente start fallaría).
     */
    @Test
    fun `un start que no puede abrir devuelve false y no deja toma`() {
        ensureEngine()

        assertFalse(start(pathOf("no/existe/toma.wav")), "el directorio no existe: el sink no abre")
        assertNull(stop(), "un start fallido no deja una toma que cerrar")

        assertTrue(start(pathOf("despues.wav")), "tras un start fallido uno bueno tiene que andar")
        assertNotNull(stop())
    }

    /**
     * Un path con un carácter no ASCII llega INTACTO al disco.
     *
     * El `jstring` cruza por `GetStringUTFChars` (UTF-8 modificado, idéntico a UTF-8 para el BMP): un
     * puente que lo trate como bytes de 8 bits, o que corte por `jchar`, crea un archivo con otro
     * nombre y el consumidor no lo encuentra. Se compara en NFC porque macOS puede devolver el nombre
     * descompuesto. La premisa —que la JVM codifique nombres de archivo en UTF-8— falla ruidosa: sin
     * ella el test no distinguiría un bug del puente de una JVM con locale ASCII.
     */
    @Test
    fun `un path con un caracter no ASCII llega intacto al disco`() {
        val jnu = System.getProperty("sun.jnu.encoding").orEmpty()
        assertTrue(
            jnu.equals("UTF-8", ignoreCase = true) || jnu.equals("UTF8", ignoreCase = true),
            "premisa rota: la JVM codifica los nombres de archivo en '$jnu', no en UTF-8; este test " +
                "no puede distinguir un bug del puente de un locale ASCII. Corré con LANG=C.UTF-8.",
        )
        ensureEngine()
        val requested = pathOf("toma-ñ.wav")

        assertTrue(start(requested))
        val r = assertNotNull(stop())

        assertEquals(TunerInputDumpResult.invalidPathFor(requested), r.path)
        assertTrue(File(r.path).isFile, "el archivo no esta en el path que Kotlin espera: el nombre no llegó intacto")
        val names = dir.list().orEmpty().map { Normalizer.normalize(it, Normalizer.Form.NFC) }
        assertTrue("toma-ñ.invalid.wav" in names, "archivos en el directorio: $names")
        assertEquals(1, names.size, "sobró algún archivo (un .part, o un nombre mal codificado): $names")
    }
}
