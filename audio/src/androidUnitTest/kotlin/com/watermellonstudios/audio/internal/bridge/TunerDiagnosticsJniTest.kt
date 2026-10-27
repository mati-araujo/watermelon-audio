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
 * firmas NO ven y acá sí —que los bytes UTF-8 del path (un `jbyteArray`) lleguen enteros al
 * disco, que los 7 valores salgan en el orden que Kotlin espera y el path que informa el nativo cruce intacto, que "sin toma" cruce como `null` y no como un array
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
        HostTestHooks.forceTunerDumpMailboxTimeouts(0)
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
     * Se fuerza con un DIRECTORIO que aparece, durante la toma, donde iría el `.invalid.wav`: el
     * renombre (que nunca reemplaza) falla. Con el nombre ocupado ANTES el start ya rechaza. Sin el
     * séptimo valor Kotlin calcularía `toma.invalid.wav`, que existe... como directorio vacío.
     * Mata: ignorar `finalized` al armar el path; el JNI que copie 6 valores en vez de 7.
     */
    @Test
    fun `una toma que no se pudo nombrar cruza sin finalizar y apunta al part`() {
        ensureEngine()
        val path = pathOf("toma.wav")

        assertTrue(start(path))
        assertTrue(File(pathOf("toma.invalid.wav")).mkdir(), "premisa: un directorio ocupa el nombre final")
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
     * Un start rechazado porque ya hay una toma NO pisa el path de la viva: el stop informa el de la
     * que sí está grabando.
     *
     * Mata: un puente que recuerda el último pedido sin mirar si el nativo lo aceptó (el resultado
     * apuntaría a `otra.invalid.wav`, un archivo que no existe).
     */
    @Test
    fun `un start rechazado por toma viva no pisa el path de la viva`() {
        ensureEngine()

        assertTrue(start(pathOf("viva.wav")))
        assertFalse(start(pathOf("otra.wav")), "ya hay una toma: el nativo rechaza")
        val r = assertNotNull(stop())

        assertEquals(pathOf("viva.invalid.wav"), r.path)
        assertTrue(File(r.path).isFile)
    }

    /**
     * Un start fallido sin toma no contamina el path de la toma siguiente, ni sobrevive a un stop
     * que devuelve null.
     *
     * Mata: un path "pendiente" que gana sobre el de un start posterior que sí arrancó, o que se
     * cuela en el resultado de una toma ajena.
     */
    @Test
    fun `un start fallido sin toma no contamina el path de la toma siguiente`() {
        ensureEngine()

        assertFalse(start(pathOf("no/existe/a.wav")))
        assertFalse(start(pathOf("no/existe/b.wav")))
        assertNull(stop())

        assertTrue(start(pathOf("buena.wav")))
        assertFalse(start(pathOf("no/existe/c.wav")), "rechazado por toma viva (y de paso sin directorio)")
        val r = assertNotNull(stop())

        assertEquals(pathOf("buena.invalid.wav"), r.path)
    }

    /**
     * BugA (REQ-055 S0) — un start que vence el techo del buzón en el armado Y en el desarme deja la
     * toma VIVA; un start posterior, rechazado, no puede cambiar el path que informa el stop.
     *
     * Antes, los dos starts devolvían false y el puente recordaba el ÚLTIMO como "pendiente": el stop
     * nombraba `segunda.invalid.wav`, un archivo que no existe, mientras `primera.invalid.wav` quedaba
     * sin reportar. El techo se fuerza con [HostTestHooks.forceTunerDumpMailboxTimeouts].
     * Mutante que mata: un puente que arma el path con el último pedido en vez del que informa el nativo.
     */
    @Test
    fun `BugA - un start rechazado con una toma viva de un start fallido no cambia el path del stop`() {
        ensureEngine()
        val first = pathOf("primera.wav")
        try {
            HostTestHooks.forceTunerDumpMailboxTimeouts(2)
            assertFalse(start(first), "armado y desarme vencieron el techo: el start dice false")
            assertEquals(0, HostTestHooks.pendingTunerDumpMailboxTimeouts(), "premisa: la palanca disparó dos veces")
        } finally {
            HostTestHooks.forceTunerDumpMailboxTimeouts(0)
        }
        assertTrue(File("$first.part").isFile, "premisa: la toma quedó viva con su temporal")

        assertFalse(start(pathOf("segunda.wav")), "ya hay una toma viva: el nativo rechaza")
        val r = assertNotNull(stop(), "había una toma viva: el stop la cierra")

        assertEquals(pathOf("primera.invalid.wav"), r.path, "el stop informó otro archivo que el de la toma")
        assertTrue(File(r.path).isFile)
        assertEquals(listOf("primera.invalid.wav"), dir.list().orEmpty().sorted(), "archivos de más en el disco")
    }

    /**
     * BugB (REQ-055 S0) — un start cuyo armado vence el techo (y cuyo desarme sí se confirma) devuelve
     * false y no deja NADA en disco; el mismo nombre se puede volver a usar.
     *
     * Antes quedaba `x.invalid.wav`: nadie lo reportaba (el start dijo false) y, con el chequeo de
     * no-pisar, ese nombre ya no se podía usar. Mutante que mata: cerrar la toma en vez de abortarla.
     */
    @Test
    fun `BugB - un start que vence el techo no deja archivos y el nombre se puede reusar`() {
        ensureEngine()
        val path = pathOf("techo.wav")
        try {
            HostTestHooks.forceTunerDumpMailboxTimeouts(1)
            assertFalse(start(path), "el armado venció el techo: el start dice false")
            assertEquals(0, HostTestHooks.pendingTunerDumpMailboxTimeouts(), "premisa: la palanca disparó")
        } finally {
            HostTestHooks.forceTunerDumpMailboxTimeouts(0)
        }
        assertEquals(emptyList(), dir.list().orEmpty().toList(), "un start fallido dejó archivos de su toma")
        assertNull(stop(), "el desarme se confirmó: no queda toma")

        assertTrue(start(path), "el nombre de un start fallido quedó bloqueado")
        assertEquals(pathOf("techo.invalid.wav"), assertNotNull(stop()).path)
    }

    /** La premisa de los tests de nombres: sin ella no se distingue un bug del puente de un locale ASCII. */
    private fun requireUtf8FileNames() {
        val jnu = System.getProperty("sun.jnu.encoding").orEmpty()
        assertTrue(
            jnu.equals("UTF-8", ignoreCase = true) || jnu.equals("UTF8", ignoreCase = true),
            "premisa rota: la JVM codifica los nombres de archivo en '$jnu', no en UTF-8; este test " +
                "no puede distinguir un bug del puente de un locale ASCII. Corré con LANG=C.UTF-8.",
        )
    }

    /** Los nombres del directorio, en NFC (macOS puede devolverlos descompuestos). */
    private fun namesInDir(): List<String> =
        dir.list().orEmpty().map { Normalizer.normalize(it, Normalizer.Form.NFC) }

    /**
     * Un path con un carácter no ASCII del BMP llega INTACTO al disco.
     *
     * Un puente que trate el path como bytes de 8 bits, o que corte por `jchar`, crea un archivo con
     * otro nombre y el consumidor no lo encuentra.
     */
    @Test
    fun `un path con un caracter no ASCII llega intacto al disco`() {
        requireUtf8FileNames()
        ensureEngine()
        val requested = pathOf("toma-ñ.wav")

        assertTrue(start(requested))
        val r = assertNotNull(stop())

        assertEquals(TunerInputDumpResult.invalidPathFor(requested), r.path)
        assertTrue(File(r.path).isFile, "el archivo no esta en el path que Kotlin espera: el nombre no llegó intacto")
        val names = namesInDir()
        assertTrue("toma-ñ.invalid.wav" in names, "archivos en el directorio: $names")
        assertEquals(1, names.size, "sobró algún archivo (un .part, o un nombre mal codificado): $names")
    }

    /**
     * Hallazgo 3 — un carácter FUERA DEL BMP (un emoji) queda en el disco con ESE nombre exacto, y
     * `TunerInputDumpResult.path` es ese archivo.
     *
     * Es lo que el `jstring` no podía: `GetStringUTFChars` da UTF-8 *modificado*, que parte 🎸 en dos
     * surrogates de 3 bytes (`ED A0 BC ED BE B8`); el archivo queda con otro nombre (o el FS lo
     * rechaza) y el resultado apunta a uno que no existe. Mutante que mata: volver al `jstring`.
     */
    @Test
    fun `hallazgo 3 - un path con un emoji queda en disco con ese nombre exacto`() {
        requireUtf8FileNames()
        ensureEngine()
        val requested = pathOf("toma-\uD83C\uDFB8.wav")   // "toma-🎸.wav"

        assertTrue(start(requested), "el path es válido: el start tiene que andar")
        val r = assertNotNull(stop())

        assertEquals(pathOf("toma-\uD83C\uDFB8.invalid.wav"), r.path)
        assertTrue(File(r.path).isFile, "el resultado apunta a un archivo que no está en el disco")
        assertEquals(listOf("toma-\uD83C\uDFB8.invalid.wav"), namesInDir(), "el nombre en disco no es el pedido")
        // Los bytes del nombre en disco son UTF-8 real (4 bytes para el emoji), no CESU-8.
        val onDisk = namesInDir().single().toByteArray(Charsets.UTF_8)
        assertEquals(
            listOf(0xF0, 0x9F, 0x8E, 0xB8),
            onDisk.drop(5).take(4).map { it.toInt() and 0xFF },
            "el emoji no quedó como UTF-8 de 4 bytes",
        )
    }

    /**
     * Hallazgo 3 — un path con `\u0000` se rechaza y no crea NADA: ni el prefijo, ni un `.part`.
     *
     * El path elegido haría daño si cruzara: `ok.wav\u0000.wav` truncado por C es `ok.wav`, un path
     * válido que el start aceptaría. Mutante que mata: el JNI que arma el string con `c_str()` del
     * array (o sin buscar el 0) y sin la guarda de Kotlin.
     */
    @Test
    fun `hallazgo 3 - un path con NUL se rechaza y no crea nada`() {
        ensureEngine()
        assertFalse(start(pathOf("ok.wav\u0000.wav")), "un path con NUL no puede arrancar una toma")
        assertNull(stop(), "un start rechazado no deja toma")
        assertEquals(emptyList(), namesInDir(), "el rechazo creó algo en el directorio")
    }

    /**
     * Hallazgo 3 — un surrogate suelto se rechaza en el puente y no crea nada: no tiene UTF-8, y
     * codificarlo con reemplazo (U+FFFD) crearía un archivo con otro nombre que el del resultado.
     * Mutante que mata: el puente que codifica con `encodeToByteArray()` sin la guarda compartida.
     */
    @Test
    fun `hallazgo 3 - un surrogate suelto se rechaza y no crea nada`() {
        ensureEngine()
        assertFalse(start(pathOf("toma-\uD83C.wav")), "un surrogate suelto no tiene nombre en disco")
        assertNull(stop())
        assertEquals(emptyList(), namesInDir(), "el rechazo creó algo en el directorio")
    }

    /**
     * Hallazgo 3 — el JNI mismo rechaza un byte 0 adentro del array, aunque Kotlin no lo filtrara.
     *
     * Es la segunda línea: la `external fun` es privada pero el `.so` la expone a cualquiera que la
     * llame. Se invoca por reflexión con los bytes de `ok.wav\0.wav`, que un `c_str()` truncaría a
     * `ok.wav` (válido). Mutante que mata: quitar el `find('\0')` del JNI.
     */
    @Test
    fun `hallazgo 3 - el JNI rechaza un byte 0 dentro del array`() {
        ensureEngine()
        val bytes = (pathOf("ok.wav") + "\u0000.wav").toByteArray(Charsets.UTF_8)
        val native = AudioNativeBridge::class.java
            .getDeclaredMethod("nativeTunerDiagInputDumpStart", ByteArray::class.java)
            .apply { isAccessible = true }
        val started = jni("nativeTunerDiagInputDumpStart") { native.invoke(it, bytes) as Boolean }
        if (started) AudioNativeBridge.getInstance().stopTunerInputDump()
        assertFalse(started, "el JNI aceptó un path con un 0 adentro (y grabó en el prefijo)")
        assertEquals(emptyList(), namesInDir(), "el rechazo creó algo en el directorio")
    }
}
