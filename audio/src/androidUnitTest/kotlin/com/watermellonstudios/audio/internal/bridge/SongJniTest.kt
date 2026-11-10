package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.ISongBridge
import com.watermellonstudios.audio.domain.error.NativeBridgeException
import org.junit.AfterClass
import java.io.File
import java.lang.reflect.InvocationTargetException
import java.nio.file.Files
import kotlin.math.abs
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertTrue

/**
 * REQ-055 S11 (AC-055.12, AC-055.13) — **la canción programada cruzando de verdad**: las
 * `nativeSong*` del motor vivo y las `nativeOfflineSong*` por handle, contra un `JNIEnv` real.
 *
 * Lo que sólo se ve cruzando (el motor de la canción ya lo mide `test_c_api_song.cpp`):
 *
 * - el `IntArray` de salida del id del clip y los `LongArray`/`IntArray` de la agenda
 *   (largo, copia fuera del pin, `null`);
 * - el `jlong` de los frames absolutos (una agenda que cruza 2^32);
 * - el handle: un cerrado no cruza, uno destruido a mano da `NOT_INITIALIZED`, y el vivo y
 *   el handle jamás comparten clips ni agenda.
 *
 * 🔴 Backend FALSO adentro: en el host el transporte VIVO no avanza (lo mueve el callback),
 * así que la canción SUENA sólo por el render offline; el vivo se afirma a nivel de control
 * (cargas, agenda aceptada o rechazada, estado). La canción sonando en un callback real es
 * del smoke de desktop. Ver el KDoc de [JniHarness].
 */
class SongJniTest {

    companion object {
        private const val OWNER = "SongJniTest"

        private val COVERED = setOf(
            "nativeSongLoadClip",
            "nativeSongUnloadClip",
            "nativeSongSetSchedule",
            "nativeSongGetSection",
            "nativeSongGetLateAdoptions",
            "nativeTransportGetPlayFrame",
            "nativeOfflineSongLoadClip",
            "nativeOfflineSongUnloadClip",
            "nativeOfflineSongSetSchedule",
            "nativeOfflineSongGetSection",
            "nativeOfflineSongGetLateAdoptions",
            "nativeOfflineSongGetPlayFrame",
            "nativeOfflineCreate",
            "nativeOfflineDestroy",
            "nativeOfflineStart",
            "nativeOfflineRender",
            "nativeOfflineStop",
            "nativeOfflineSetFrequencyAndAmplitude",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)

        private const val RATE = 48_000
        private const val LOOP = 4_800L          // 100 ms: un loop sin costura de la onda del fixture
        private const val BLOCK = 480
        private const val OUTPUT_DELAY = 240     // el lookahead del limitador de salida
        private const val FADE = 240             // F = max(64, 0,005 * 48000)
        private const val T = ISongBridge.MAX_TRACKS
        private const val S = ISongBridge.SILENCE

        private const val ERROR_NOT_INITIALIZED = -1
        private const val ERROR_OUT_OF_RANGE = -4
        private const val ERROR_INVALID_OPERATION = -9

        private val dir: File by lazy { Files.createTempDirectory("s11-song").toFile().apply { deleteOnExit() } }

        /** Un WAV float32 estéreo de [LOOP] frames con contenido en TODOS: un loop sin costura. */
        private fun clipFile(name: String): String =
            MinimalWav.writeTo(File(dir, "$name.wav").apply { deleteOnExit() }, LOOP.toInt(),
                listOf(MinimalWav.Region(0, LOOP.toInt())))
    }

    private fun <R> live(name: String, call: (AudioNativeBridge) -> R): R = JniHarness.exercise(OWNER, name, call)

    private fun newRenderer(): OfflineRenderer {
        JniHarness.requireNativeLibrary()
        val r = OfflineRenderer.create().getOrThrow()
        JniCoverage.record(OWNER, "nativeOfflineCreate")
        return r
    }

    private fun OfflineRenderer.started(): OfflineRenderer {
        assertTrue(start(RATE, BLOCK).isSuccess)
        JniCoverage.record(OWNER, "nativeOfflineStart")
        // Sin instrumento: lo único que se oye es lo que ponga la canción.
        assertTrue(setFrequencyAndAmplitude(440f, 0f).isSuccess)
        JniCoverage.record(OWNER, "nativeOfflineSetFrequencyAndAmplitude")
        return this
    }

    private fun row(vararg perTrack: Pair<Int, Int>): IntArray =
        IntArray(T) { S }.also { r -> perTrack.forEach { (track, clip) -> r[track] = clip } }

    private fun Result<*>.failureCode(): NativeBridgeException =
        exceptionOrNull() as? NativeBridgeException ?: error("se esperaba un NativeBridgeException, vino $this")

    private fun FloatArray.peak(fromFrame: Int, toFrame: Int): Float {
        var p = 0f
        for (i in fromFrame * 2 until toFrame * 2) p = maxOf(p, abs(this[i]))
        return p
    }

    /** Cruza con `null` donde Kotlin no deja: las `external fun` no llevan chequeo de nulos. */
    private fun crossRaw(name: String, types: Array<Class<*>>, vararg args: Any?): Any? {
        JniHarness.requireNativeLibrary()
        val m = AudioNativeBridge::class.java.getDeclaredMethod(name, *types).apply { isAccessible = true }
        try {
            return m.invoke(AudioNativeBridge.getInstance(), *args)
        } catch (e: InvocationTargetException) {
            throw AssertionError("$name lanzó una excepción al cruzar con un argumento nulo", e.targetException)
        }
    }

    private val scheduleTypes = arrayOf<Class<*>>(Long::class.java, LongArray::class.java, IntArray::class.java, Long::class.java)
    private val offlineScheduleTypes = arrayOf<Class<*>>(Long::class.java) + scheduleTypes
    private val loadTypes = arrayOf<Class<*>>(String::class.java, Int::class.java, Long::class.java, Long::class.java,
        Long::class.java, IntArray::class.java)
    private val offlineLoadTypes = arrayOf<Class<*>>(Long::class.java) + loadTypes

    // ------------------------------------------------------------------------------------
    // Offline: la canción suena (AC-055.12)
    // ------------------------------------------------------------------------------------

    // Bug plausible: el JNI por handle no llega a la agenda (clip, starts o la fila de clips mal
    // leídos del array) y la sección con clip sale muda, o la de silencio suena porque el fondo
    // cae a las tomas del looper. Un get_section que devuelve el índice de la fila equivocada
    // también cae acá.
    @Test
    fun `AC-055_12 el render offline por JNI suena en la seccion con clip y calla en la de silencio`() {
        newRenderer().use { r ->
            r.started()
            val clip = r.songLoadClip(clipFile("a"), RATE, 0, 0, LOOP).getOrThrow()
            JniCoverage.record(OWNER, "nativeOfflineSongLoadClip")
            val p0 = r.songGetPlayFrame()
            JniCoverage.record(OWNER, "nativeOfflineSongGetPlayFrame")
            val starts = longArrayOf(p0 + 2 * LOOP, p0 + 4 * LOOP)
            val clips = row(0 to clip) + row()
            val set = r.songSetSchedule(LOOP, starts, clips, p0 + 6 * LOOP)
            JniCoverage.record(OWNER, "nativeOfflineSongSetSchedule")
            assertTrue(set.isSuccess, "set_schedule falló: ${set.exceptionOrNull()}")

            val total = (8 * LOOP).toInt()
            val out = FloatArray(total * 2)
            val block = FloatArray(BLOCK * 2)
            val sectionAfter = HashMap<Int, Int>()
            for (b in 0 until total / BLOCK) {
                assertTrue(r.render(block, BLOCK).isSuccess)
                JniCoverage.record(OWNER, "nativeOfflineRender")
                block.copyInto(out, b * BLOCK * 2)
                sectionAfter[(b + 1) * BLOCK] = r.songGetSection()
                JniCoverage.record(OWNER, "nativeOfflineSongGetSection")
            }

            val l = LOOP.toInt()
            assertTrue(out.peak(0, 2 * l - 2 * FADE) < 0.001f, "sonó antes del primer start")
            assertTrue(out.peak(2 * l + 2 * FADE, 4 * l - 100) > 0.2f, "la sección con clip salió muda")
            assertTrue(out.peak(4 * l + 3 * FADE, 6 * l) < 0.001f, "la sección de silencio suena (cayó a las tomas?)")
            assertTrue(out.peak(6 * l + 3 * FADE, 8 * l) < 0.001f, "sonó después del endFrame")

            assertEquals(-1, sectionAfter[l], "antes del primer start")
            assertEquals(0, sectionAfter[3 * l], "sección con clip: índice 0 de la agenda")
            assertEquals(1, sectionAfter[5 * l], "sección de silencio: índice 1 de la agenda")
            assertEquals(-1, sectionAfter[7 * l], "pasado el endFrame")
            assertEquals(0L, r.songGetLateAdoptions())
            JniCoverage.record(OWNER, "nativeOfflineSongGetLateAdoptions")
            assertTrue(r.stop().isSuccess)
            JniCoverage.record(OWNER, "nativeOfflineStop")
        }
    }

    // Bug plausible: un clip referenciado por la agenda se libera mientras suena (use-after-free
    // en el callback), o el error de unload no viaja. Apagar la canción lo vuelve a permitir.
    @Test
    fun `AC-055_12 unload de un clip que la agenda usa da INVALID_OPERATION y apagar la agenda lo permite`() {
        newRenderer().use { r ->
            r.started()
            val clip = r.songLoadClip(clipFile("busy"), RATE, 0, 0, LOOP).getOrThrow()
            JniCoverage.record(OWNER, "nativeOfflineSongLoadClip")
            val p0 = r.songGetPlayFrame()
            assertTrue(r.songSetSchedule(LOOP, longArrayOf(p0 + LOOP), row(0 to clip), p0 + 3 * LOOP).isSuccess)
            JniCoverage.record(OWNER, "nativeOfflineSongSetSchedule")

            val busy = r.songUnloadClip(clip)
            JniCoverage.record(OWNER, "nativeOfflineSongUnloadClip")
            assertIs<NativeBridgeException.InvalidOperation>(busy.failureCode())

            assertTrue(r.songSetSchedule(LOOP, LongArray(0), IntArray(0), 0).isSuccess, "starts vacío debe apagar")
            assertTrue(r.songUnloadClip(clip).isSuccess, "con la agenda apagada el clip se puede sacar")
            assertIs<NativeBridgeException.InvalidOperation>(
                r.songUnloadClip(clip).failureCode(), "sacar dos veces el mismo clip",
            )
        }
    }

    // Bug plausible: un largo que no cierra (clips != starts*16) lee de más (sobrelectura del
    // pin) o publica media agenda. Se rechaza SIN cruzar a la C API y la agenda vigente no cambia.
    @Test
    fun `AC-055_12 arrays de largos distintos se rechazan y no tocan la agenda publicada`() {
        newRenderer().use { r ->
            r.started()
            val clip = r.songLoadClip(clipFile("len"), RATE, 0, 0, LOOP).getOrThrow()
            val p0 = r.songGetPlayFrame()
            assertTrue(r.songSetSchedule(LOOP, longArrayOf(p0 + LOOP), row(0 to clip), p0 + 3 * LOOP).isSuccess)

            val short = r.songSetSchedule(LOOP, longArrayOf(p0 + LOOP, p0 + 2 * LOOP), row(0 to clip), p0 + 4 * LOOP)
            assertIs<NativeBridgeException.ParameterOutOfRange>(short.failureCode())
            val long = r.songSetSchedule(LOOP, longArrayOf(p0 + LOOP), row(0 to clip) + row(), p0 + 3 * LOOP)
            assertIs<NativeBridgeException.ParameterOutOfRange>(long.failureCode())
            val tooMany = r.songSetSchedule(
                LOOP, LongArray(ISongBridge.MAX_SECTIONS + 1) { p0 + (it + 1) * LOOP },
                IntArray((ISongBridge.MAX_SECTIONS + 1) * T) { S }, p0 + 2_000 * LOOP,
            )
            assertIs<NativeBridgeException.ParameterOutOfRange>(tooMany.failureCode())

            assertIs<NativeBridgeException.InvalidOperation>(
                r.songUnloadClip(clip).failureCode(), "la agenda original sigue publicada",
            )
        }
    }

    // Bug plausible: un argumento nulo deja una excepción JNI pendiente o desreferencia null
    // (crash de la JVM). Tiene que volver un código de error, sin excepción, y poder seguir usando
    // el handle. Se cruza por reflexión porque Kotlin no deja pasar null a un LongArray.
    @Test
    fun `AC-055_12 argumentos nulos devuelven error sin excepcion pendiente - handle y vivo`() {
        newRenderer().use { r ->
            r.started()
            val h = r.nativeHandle
            val outId = IntArray(1)
            val file = clipFile("nulls")

            assertEquals(ERROR_OUT_OF_RANGE, crossRaw("nativeOfflineSongSetSchedule", offlineScheduleTypes, h, LOOP, null, IntArray(T), 0L))
            assertEquals(ERROR_OUT_OF_RANGE, crossRaw("nativeOfflineSongSetSchedule", offlineScheduleTypes, h, LOOP, LongArray(1), null, 0L))
            assertEquals(ERROR_OUT_OF_RANGE, crossRaw("nativeOfflineSongLoadClip", offlineLoadTypes, h, null, RATE, 0L, 0L, LOOP, outId))
            assertEquals(ERROR_OUT_OF_RANGE, crossRaw("nativeOfflineSongLoadClip", offlineLoadTypes, h, file, RATE, 0L, 0L, LOOP, null))
            assertEquals(ERROR_OUT_OF_RANGE, crossRaw("nativeOfflineSongLoadClip", offlineLoadTypes, h, file, RATE, 0L, 0L, LOOP, IntArray(0)))
            JniCoverage.record(OWNER, "nativeOfflineSongSetSchedule")
            JniCoverage.record(OWNER, "nativeOfflineSongLoadClip")

            assertEquals(ERROR_OUT_OF_RANGE, crossRaw("nativeSongSetSchedule", scheduleTypes, LOOP, null, IntArray(T), 0L))
            assertEquals(ERROR_OUT_OF_RANGE, crossRaw("nativeSongSetSchedule", scheduleTypes, LOOP, LongArray(1), null, 0L))
            assertEquals(ERROR_OUT_OF_RANGE, crossRaw("nativeSongLoadClip", loadTypes, null, RATE, 0L, 0L, LOOP, outId))
            assertEquals(ERROR_OUT_OF_RANGE, crossRaw("nativeSongLoadClip", loadTypes, file, RATE, 0L, 0L, LOOP, null))
            assertEquals(ERROR_OUT_OF_RANGE, crossRaw("nativeSongLoadClip", loadTypes, file, RATE, 0L, 0L, LOOP, IntArray(0)))
            JniCoverage.record(OWNER, "nativeSongSetSchedule")
            JniCoverage.record(OWNER, "nativeSongLoadClip")

            assertTrue(r.songLoadClip(file, RATE, 0, 0, LOOP).isSuccess, "el handle quedó inservible tras los nulos")
        }
    }

    // Bug plausible: el id del clip no llega a Kotlin (SetIntArrayRegion con índice o largo
    // equivocado), o un id viejo se reusa. Los dos clips tienen que dar ids distintos, y una ruta
    // inexistente falla como IO sin escribir un id.
    @Test
    fun `AC-055_12 el id del clip vuelve por el IntArray y una ruta inexistente da IO`() {
        newRenderer().use { r ->
            r.started()
            val a = r.songLoadClip(clipFile("id_a"), RATE, 0, 0, LOOP).getOrThrow()
            val b = r.songLoadClip(clipFile("id_b"), RATE, 0, 0, LOOP).getOrThrow()
            JniCoverage.record(OWNER, "nativeOfflineSongLoadClip")
            assertTrue(a in 0 until ISongBridge.MAX_CLIPS && b in 0 until ISongBridge.MAX_CLIPS && a != b, "ids $a y $b")
            assertIs<NativeBridgeException.IoError>(
                r.songLoadClip(File(dir, "no-existe.wav").path, RATE, 0, 0, LOOP).failureCode(),
            )
        }
    }

    // ------------------------------------------------------------------------------------
    // Handle cerrado / desconocido
    // ------------------------------------------------------------------------------------

    // Bug plausible: tras close() una operación cruza con el handle viejo (el registro lo trata
    // como desconocido, pero el contrato es que ni cruza), o una lectura devuelve basura.
    @Test
    fun `AC-055_12 con el handle cerrado fallan con EngineNotInitialized sin cruzar y las lecturas dan el default`() {
        val r = newRenderer().started()
        val clip = r.songLoadClip(clipFile("closed"), RATE, 0, 0, LOOP).getOrThrow()
        JniCoverage.record(OWNER, "nativeOfflineSongLoadClip")
        r.close()
        JniCoverage.record(OWNER, "nativeOfflineDestroy")

        assertIs<NativeBridgeException.EngineNotInitialized>(r.songLoadClip(clipFile("closed2"), RATE, 0, 0, LOOP).failureCode())
        assertIs<NativeBridgeException.EngineNotInitialized>(r.songUnloadClip(clip).failureCode())
        assertIs<NativeBridgeException.EngineNotInitialized>(
            r.songSetSchedule(LOOP, LongArray(0), IntArray(0), 0).failureCode(),
        )
        assertEquals(-1, r.songGetSection())
        assertEquals(0L, r.songGetLateAdoptions())
        assertEquals(-1L, r.songGetPlayFrame())
    }

    // Bug plausible: un handle destruido a mano (o inventado) opera sobre memoria muerta o sobre
    // otra instancia. El registro tiene que contestar NOT_INITIALIZED a cada una de las seis.
    @Test
    fun `AC-055_12 un handle destruido o desconocido contesta NOT_INITIALIZED en las seis`() {
        val bridge = AudioNativeBridge.getInstance()
        val r = newRenderer()
        val dead = r.nativeHandle
        r.close()
        JniCoverage.record(OWNER, "nativeOfflineDestroy")
        for (h in longArrayOf(dead, 0L, 987_654_321L)) {
            assertEquals(ERROR_NOT_INITIALIZED, bridge.offlineSongLoadClip(h, clipFile("dead"), RATE, 0, 0, LOOP, IntArray(1)))
            assertEquals(ERROR_NOT_INITIALIZED, bridge.offlineSongUnloadClip(h, 0))
            assertEquals(ERROR_NOT_INITIALIZED, bridge.offlineSongSetSchedule(h, LOOP, LongArray(0), IntArray(0), 0))
            assertEquals(-1, bridge.offlineSongGetSection(h))
            assertEquals(0L, bridge.offlineSongGetLateAdoptions(h))
            assertEquals(-1L, bridge.offlineSongGetPlayFrame(h))
        }
        JniCoverage.record(OWNER, "nativeOfflineSongLoadClip")
        JniCoverage.record(OWNER, "nativeOfflineSongUnloadClip")
        JniCoverage.record(OWNER, "nativeOfflineSongSetSchedule")
        JniCoverage.record(OWNER, "nativeOfflineSongGetSection")
        JniCoverage.record(OWNER, "nativeOfflineSongGetLateAdoptions")
        JniCoverage.record(OWNER, "nativeOfflineSongGetPlayFrame")
    }

    // ------------------------------------------------------------------------------------
    // Vivo
    // ------------------------------------------------------------------------------------

    // Bug plausible: el id no vuelve, una agenda con frames sobre 2^32 se estrecha a 32 bits (los
    // starts dejarían de ser crecientes y se rechazaría), la unload de un clip en uso no avisa, o
    // apagar la agenda no libera. La lectura de sección pre-callback da -1.
    @Test
    fun `AC-055_12 vivo - carga, agenda que cruza 2^32, unload ocupado y apagado`() {
        val bridge = AudioNativeBridge.getInstance()
        val clip = live("nativeSongLoadClip") { it.songLoadClip(clipFile("live"), RATE, 0, 0, LOOP) }.getOrThrow()
        assertTrue(clip in 0 until ISongBridge.MAX_CLIPS)

        val base = (1L shl 32) - 2 * LOOP
        val starts = longArrayOf(base, base + 4 * LOOP)
        val set = live("nativeSongSetSchedule") {
            it.songSetSchedule(LOOP, starts, row(0 to clip) + row(0 to clip), base + 8 * LOOP)
        }
        assertTrue(set.isSuccess, "una agenda sobre 2^32 se rechazó (estrechamiento a 32 bits?): ${set.exceptionOrNull()}")

        assertEquals(-1, live("nativeSongGetSection") { it.songGetSection() }, "sin callback no hay sección")
        assertEquals(0L, live("nativeSongGetLateAdoptions") { it.songGetLateAdoptions() })
        assertTrue(live("nativeTransportGetPlayFrame") { it.songGetPlayFrame() } >= 0L)
        assertIs<NativeBridgeException.InvalidOperation>(
            live("nativeSongUnloadClip") { it.songUnloadClip(clip) }.failureCode(),
        )

        assertTrue(bridge.songSetSchedule(LOOP, LongArray(0), IntArray(0), 0).isSuccess)
        assertTrue(bridge.songUnloadClip(clip).isSuccess)
        assertIs<NativeBridgeException.ParameterOutOfRange>(
            bridge.songSetSchedule(LOOP, longArrayOf(1L), IntArray(3), 0).failureCode(),
        )
    }

    // Bug plausible: una nativeOfflineSong* lee g_wmaEngine (o una nativeSong* el registro) y
    // el vivo y el handle comparten clips o agenda. El id 0 existe en el vivo y NO en el handle.
    @Test
    fun `AC-055_13 el vivo y el handle no comparten clips ni agenda`() {
        val bridge = AudioNativeBridge.getInstance()
        val liveClip = live("nativeSongLoadClip") { it.songLoadClip(clipFile("indep"), RATE, 0, 0, LOOP) }.getOrThrow()
        newRenderer().use { r ->
            r.started()
            assertIs<NativeBridgeException.InvalidOperation>(
                r.songUnloadClip(liveClip).failureCode(), "el handle vio un clip del vivo",
            )
            val p0 = r.songGetPlayFrame()
            assertIs<NativeBridgeException.InvalidOperation>(
                r.songSetSchedule(LOOP, longArrayOf(p0 + LOOP), row(0 to liveClip), p0 + 3 * LOOP).failureCode(),
                "el handle aceptó una agenda con un clip que sólo existe en el vivo",
            )

            val ownClip = r.songLoadClip(clipFile("indep_h"), RATE, 0, 0, LOOP).getOrThrow()
            assertTrue(r.songSetSchedule(LOOP, longArrayOf(p0 + LOOP), row(0 to ownClip), p0 + 3 * LOOP).isSuccess)
            assertTrue(bridge.songUnloadClip(liveClip).isSuccess, "la agenda del handle retuvo un clip del vivo")
            assertIs<NativeBridgeException.InvalidOperation>(
                r.songUnloadClip(ownClip).failureCode(), "la agenda del handle sigue publicada",
            )
        }
    }
}
