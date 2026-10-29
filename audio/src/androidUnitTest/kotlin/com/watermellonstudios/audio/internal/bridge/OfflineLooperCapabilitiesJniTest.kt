package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.ISongBridge
import com.watermellonstudios.audio.domain.error.NativeBridgeException
import org.junit.AfterClass
import java.io.File
import java.nio.file.Files
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertTrue

/**
 * REQ-055 S12b — **las capacidades del looper por handle offline** cruzando de verdad:
 * `nativeOfflineLooperSetCapabilities` contra un `JNIEnv` real y la C API detrás.
 *
 * Sin esto una instancia offline nace con 48 MiB de clips y 8 pistas aunque el vivo del
 * consumidor use 192 MiB. El motor de la cuenta (el presupuesto medido por carga) ya lo mide
 * `test_c_api_song.cpp`; acá se ve lo que sólo se ve cruzando: el `jlong` del presupuesto, el
 * rechazo de rangos ANTES de aplicar nada y el handle muerto.
 *
 * 🔴 Backend FALSO adentro: valida la frontera JNI/Kotlin, no audio en un device. Ver [JniHarness].
 */
class OfflineLooperCapabilitiesJniTest {

    companion object {
        private const val OWNER = "OfflineLooperCapabilitiesJniTest"

        private val COVERED = setOf(
            "nativeOfflineLooperSetCapabilities",
            "nativeOfflineCreate",
            "nativeOfflineDestroy",
            "nativeOfflineStart",
            "nativeOfflineSongLoadClip",
            "nativeOfflineSongSetSchedule",
            "nativeOfflineSongGetPlayFrame",
            "nativeOfflineMixSetTrack",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)

        private const val RATE = 48_000
        private const val BLOCK = 480
        private const val FILE_FRAMES = 4_800

        /** 12 MiB por clip (`frames * 2 * 4`): cuatro llenan los 48 MiB del default. */
        private const val CLIP_FRAMES = 1_572_864L
        private const val MIB = 1024L * 1024L
        private const val DEFAULT_BUDGET_CLIPS = 4

        private const val ERROR_NOT_INITIALIZED = -1
        private const val ERROR_OUT_OF_RANGE = -4

        private val dir: File by lazy { Files.createTempDirectory("s12b-caps").toFile().apply { deleteOnExit() } }

        /** Un archivo chico: el motor lo estira a `CLIP_FRAMES`, que es lo que cuenta el presupuesto. */
        private val clipPath: String by lazy {
            MinimalWav.writeTo(File(dir, "clip.wav").apply { deleteOnExit() }, FILE_FRAMES,
                listOf(MinimalWav.Region(0, FILE_FRAMES)))
        }
    }

    private fun <R> jni(name: String, call: (AudioNativeBridge) -> R): R = JniHarness.exercise(OWNER, name, call)

    private fun newRenderer(): OfflineRenderer {
        JniHarness.requireNativeLibrary()
        val r = OfflineRenderer.create().getOrThrow()
        JniCoverage.record(OWNER, "nativeOfflineCreate")
        assertTrue(r.start(RATE, BLOCK).isSuccess)
        JniCoverage.record(OWNER, "nativeOfflineStart")
        return r
    }

    private fun Result<*>.failure(): NativeBridgeException =
        exceptionOrNull() as? NativeBridgeException ?: error("se esperaba un NativeBridgeException, vino $this")

    private fun OfflineRenderer.load(): Result<Int> =
        songLoadClip(clipPath, RATE, 0, 0, CLIP_FRAMES).also { JniCoverage.record(OWNER, "nativeOfflineSongLoadClip") }

    private fun OfflineRenderer.track(track: Int): Result<Unit> =
        mixSetTrack(track, 1f, 0f, 0f).also { JniCoverage.record(OWNER, "nativeOfflineMixSetTrack") }

    private fun OfflineRenderer.fillDefaultBudget() {
        repeat(DEFAULT_BUDGET_CLIPS) { assertTrue(load().isSuccess, "clip $it") }
    }

    private fun caps(r: OfflineRenderer, bytes: Long, tracks: Int): Result<Unit> =
        r.setLooperCapabilities(bytes, tracks).also { JniCoverage.record(OWNER, "nativeOfflineLooperSetCapabilities") }

    // Bug plausible: el setter no llega al looper de LA INSTANCIA (se aplica al motor vivo, o se
    // trunca el jlong a int): el export de un proyecto grande sigue cayendo en 48 MiB. Con el default
    // el quinto clip de 12 MiB no entra; con 60 MiB entra el quinto y no el sexto; con 192 MiB, sí.
    @Test
    fun `AC-055_12b el presupuesto subido a 192 MiB deja cargar lo que con 48 MiB daba MemoryBudgetExceeded`() {
        val r = newRenderer()
        try {
            r.fillDefaultBudget()
            assertIs<NativeBridgeException.MemoryBudgetExceeded>(r.load().failure(), "con 48 MiB")
            // 60 MiB exactos: entra el quinto y el sexto ya no (el tope se aplica al byte, no se "desbloquea")
            assertTrue(caps(r, 60 * MIB, 0).isSuccess)
            assertTrue(r.load().isSuccess, "quinto clip con 60 MiB")
            assertIs<NativeBridgeException.MemoryBudgetExceeded>(r.load().failure(), "sexto clip con 60 MiB")
            assertTrue(caps(r, 192 * MIB, 0).isSuccess)
            assertTrue(r.load().isSuccess, "sexto clip con 192 MiB")
        } finally {
            r.close()
            JniCoverage.record(OWNER, "nativeOfflineDestroy")
        }
    }

    // Bug plausible: las pistas no cruzan o el 0 se toma por valor: la pista 8 sigue fuera de rango
    // con el default (8 pistas) y entra con 12; un 0 en pistas no las baja ni toca el presupuesto.
    @Test
    fun `AC-055_12b las pistas activas suben con el setter y el cero deja cada valor como estaba`() {
        val r = newRenderer()
        try {
            assertIs<NativeBridgeException.ParameterOutOfRange>(r.track(8).failure(), "default: 8 pistas")
            assertTrue(caps(r, 0, 12).isSuccess)
            assertTrue(r.track(8).isSuccess)
            assertTrue(r.track(11).isSuccess)
            assertIs<NativeBridgeException.ParameterOutOfRange>(r.track(12).failure())
            assertTrue(caps(r, 0, 0).isSuccess, "todo cero es un no-op")
            assertTrue(r.track(11).isSuccess, "el cero no bajó las pistas")
            r.fillDefaultBudget()
            assertIs<NativeBridgeException.MemoryBudgetExceeded>(r.load().failure(), "las pistas no tocaron el presupuesto")
        } finally {
            r.close()
            JniCoverage.record(OWNER, "nativeOfflineDestroy")
        }
    }

    // Bug plausible: un valor fuera de rango se recorta o se aplica a medias (el presupuesto pasa y
    // las pistas no). Cada uno rechaza TIPADO y no cambia NADA del otro.
    @Test
    fun `AC-055_12b un valor fuera de rango se rechaza tipado sin aplicar ninguno de los dos`() {
        val r = newRenderer()
        try {
            val sixtyFourGib = 64L * 1024 * MIB
            val bad = listOf(
                -1L to 0, Long.MIN_VALUE to 0, sixtyFourGib + 1 to 0, Long.MAX_VALUE to 0,
                192 * MIB to -1, 192 * MIB to ISongBridge.MAX_TRACKS + 1, 192 * MIB to Int.MAX_VALUE, 192 * MIB to Int.MIN_VALUE,
            )
            for ((bytes, tracks) in bad) {
                assertIs<NativeBridgeException.ParameterOutOfRange>(caps(r, bytes, tracks).failure(), "$bytes / $tracks")
            }
            // los dos del par válido no se aplicaron: el 5.o clip sigue sin entrar y la pista 8 sigue afuera
            r.fillDefaultBudget()
            assertIs<NativeBridgeException.MemoryBudgetExceeded>(r.load().failure(), "el presupuesto se aplicó a medias")
            assertIs<NativeBridgeException.ParameterOutOfRange>(r.track(8).failure(), "las pistas se aplicaron a medias")
            // y los bordes válidos sí pasan
            assertTrue(caps(r, 64L * 1024 * MIB, ISongBridge.MAX_TRACKS).isSuccess)
            assertTrue(r.track(15).isSuccess)
            assertTrue(r.load().isSuccess)
        } finally {
            r.close()
            JniCoverage.record(OWNER, "nativeOfflineDestroy")
        }
    }

    // Bug plausible: [start] reinicia el looper a los defaults (el export reconfigura el rate y pierde
    // el presupuesto) o bajar el presupuesto con clips adentro los suelta. Se documenta: sobrevive al
    // start, y bajarlo sólo frena las cargas siguientes.
    @Test
    fun `AC-055_12b el setter sobrevive al start y bajarlo con clips cargados no los suelta`() {
        val r = newRenderer()
        try {
            assertTrue(caps(r, 192 * MIB, 12).isSuccess)
            val a = r.load().getOrThrow()
            // mismo rate: el start no invalida los clips, y la agenda de `a` es válida antes de bajar
            assertTrue(r.start(RATE, BLOCK).isSuccess)
            JniCoverage.record(OWNER, "nativeOfflineStart")
            repeat(DEFAULT_BUDGET_CLIPS + 1) { assertTrue(r.load().isSuccess, "tras el start, clip $it") }
            assertTrue(r.track(11).isSuccess, "las pistas sobrevivieron al start")

            assertTrue(caps(r, 1 * MIB, 0).isSuccess)
            assertIs<NativeBridgeException.MemoryBudgetExceeded>(r.load().failure(), "bajarlo frena la carga siguiente")
            val p0 = r.songGetPlayFrame()
            JniCoverage.record(OWNER, "nativeOfflineSongGetPlayFrame")
            val row = IntArray(ISongBridge.MAX_TRACKS) { ISongBridge.SILENCE }.also { it[0] = a }
            // `a` sigue cargado tras bajar el presupuesto a 1 MiB: si se hubiera soltado, la agenda fallaría
            val scheduled = r.songSetSchedule(CLIP_FRAMES, longArrayOf(p0 + 9_600), row, p0 + 96_000)
            JniCoverage.record(OWNER, "nativeOfflineSongSetSchedule")
            assertTrue(scheduled.isSuccess, "el clip cargado antes de bajar el presupuesto sigue agendable")
        } finally {
            r.close()
            JniCoverage.record(OWNER, "nativeOfflineDestroy")
        }
    }

    // Bug plausible: el recorte del looper mira sólo sus tomas y no la agenda de la canción: bajar las
    // pistas por debajo de una pista agendada la apaga en silencio (`Success` y la pista 10 calla).
    @Test
    fun `AC-055_12b bajar las pistas por debajo de la agenda de la cancion se rechaza aun con la agenda apagada`() {
        val r = newRenderer()
        try {
            assertTrue(caps(r, 192 * MIB, 12).isSuccess)
            val a = r.load().getOrThrow()
            val p0 = r.songGetPlayFrame()
            JniCoverage.record(OWNER, "nativeOfflineSongGetPlayFrame")
            val row = IntArray(ISongBridge.MAX_TRACKS) { ISongBridge.SILENCE }.also { it[10] = a }
            assertTrue(r.songSetSchedule(CLIP_FRAMES, longArrayOf(p0 + 9_600), row, p0 + 96_000).isSuccess)
            JniCoverage.record(OWNER, "nativeOfflineSongSetSchedule")

            assertIs<NativeBridgeException.ParameterOutOfRange>(caps(r, 0, 8).failure(), "pista 10 agendada")
            assertIs<NativeBridgeException.ParameterOutOfRange>(caps(r, 0, 10).failure(), "la pista 10 necesita 11")
            assertTrue(r.track(10).isSuccess, "el rechazo no tocó las pistas")
            // rechazar el par no aplica el presupuesto: el quinto clip de 12 MiB sigue entrando con 192 MiB
            assertIs<NativeBridgeException.ParameterOutOfRange>(caps(r, 1 * MIB, 8).failure())
            assertTrue(r.load().isSuccess, "el presupuesto no se aplicó a medias")

            assertTrue(caps(r, 0, 11).isSuccess, "justo la pista más alta + 1")
            assertTrue(caps(r, 0, 0).isSuccess, "el cero no baja nada")
            // apagar la agenda NO habilita bajar: el RT la adopta en el bloque siguiente y la pista 10 aún tiene cola
            assertTrue(r.songSetSchedule(1, LongArray(0), IntArray(0), 0).isSuccess)
            assertIs<NativeBridgeException.ParameterOutOfRange>(caps(r, 0, 8).failure(), "agenda apagada, cola viva")
            assertTrue(r.track(10).isSuccess)
            // sólo el start vuelve a permitirlo (resetSongForRender)
            assertTrue(r.start(RATE, BLOCK).isSuccess)
            JniCoverage.record(OWNER, "nativeOfflineStart")
            assertTrue(caps(r, 0, 8).isSuccess, "tras el start no hay pista que apagar")
            assertIs<NativeBridgeException.ParameterOutOfRange>(r.track(10).failure())
        } finally {
            r.close()
            JniCoverage.record(OWNER, "nativeOfflineDestroy")
        }
    }

    private fun OfflineRenderer.schedule(a: Int, track: Int, p0: Long): Result<Unit> {
        val row = IntArray(ISongBridge.MAX_TRACKS) { ISongBridge.SILENCE }.also { it[track] = a }
        return songSetSchedule(CLIP_FRAMES, longArrayOf(p0 + 9_600), row, p0 + 96_000)
            .also { JniCoverage.record(OWNER, "nativeOfflineSongSetSchedule") }
    }

    // Bug plausible (el probe del reviewer): el contador baja con la agenda vacía y el set(0,8) corta la
    // cola de fundido de la pista 10 (clic, maxDiff 0,43) devolviendo Success. El render tras el intento
    // debe ser idéntico al de un control que no lo intentó.
    @Test
    fun `AC-055_12b con la pista 10 sonando y la agenda vacia bajar a 8 se rechaza y el render no cambia`() {
        fun run(tryLower: Boolean): FloatArray {
            val r = newRenderer()
            try {
                assertTrue(caps(r, 0, 12).isSuccess)
                val a = r.songLoadClip(clipPath, RATE, 0, 0, FILE_FRAMES.toLong()).getOrThrow()
                JniCoverage.record(OWNER, "nativeOfflineSongLoadClip")
                assertTrue(r.track(10).isSuccess)
                val p0 = r.songGetPlayFrame()
                JniCoverage.record(OWNER, "nativeOfflineSongGetPlayFrame")
                val row = IntArray(ISongBridge.MAX_TRACKS) { ISongBridge.SILENCE }.also { it[10] = a }
                assertTrue(r.songSetSchedule(FILE_FRAMES.toLong(), longArrayOf(p0), row, p0 + 48_000).isSuccess)
                JniCoverage.record(OWNER, "nativeOfflineSongSetSchedule")
                val buf = FloatArray(BLOCK * 2)
                repeat(10) { assertTrue(r.render(buf, BLOCK).isSuccess) }
                assertTrue(r.songSetSchedule(1, LongArray(0), IntArray(0), 0).isSuccess)
                if (tryLower) assertIs<NativeBridgeException.ParameterOutOfRange>(caps(r, 0, 8).failure())
                assertTrue(r.render(buf, BLOCK).isSuccess)
                return buf.copyOf()
            } finally {
                r.close()
                JniCoverage.record(OWNER, "nativeOfflineDestroy")
            }
        }
        val control = run(tryLower = false)
        assertTrue(control.any { it != 0f }, "el control tiene que tener la cola sonando")
        assertTrue(control.contentEquals(run(tryLower = true)), "el intento rechazado no cambia el render")
    }

    // Bug plausible: el contador se actualiza aunque la agenda falle. Buena en la pista 3 (necesita 4);
    // la fallida pone un clip inexistente en la pista 10 y no puede subirlo a 11.
    @Test
    fun `AC-055_12b una agenda fallida no cambia el contador de pistas`() {
        val r = newRenderer()
        try {
            assertTrue(caps(r, 0, 12).isSuccess)
            val a = r.load().getOrThrow()
            val p0 = r.songGetPlayFrame()
            JniCoverage.record(OWNER, "nativeOfflineSongGetPlayFrame")
            assertTrue(r.schedule(a, 3, p0).isSuccess)
            assertTrue(r.schedule(a + 1_000, 10, p0).isFailure, "clip inexistente")
            assertIs<NativeBridgeException.ParameterOutOfRange>(caps(r, 0, 3).failure(), "la pista 3 necesita 4")
            assertTrue(caps(r, 0, 8).isSuccess, "la agenda fallida no subió el contador a 11")
        } finally {
            r.close()
            JniCoverage.record(OWNER, "nativeOfflineDestroy")
        }
    }

    // Bug plausible: el start no vuelve el contador a 0 y la instancia queda atada para siempre a la
    // agenda más alta que tuvo.
    @Test
    fun `AC-055_12b el start vuelve el contador de pistas a cero`() {
        val r = newRenderer()
        try {
            assertTrue(caps(r, 0, 12).isSuccess)
            val a = r.load().getOrThrow()
            assertTrue(r.schedule(a, 10, r.songGetPlayFrame()).isSuccess)
            JniCoverage.record(OWNER, "nativeOfflineSongGetPlayFrame")
            assertIs<NativeBridgeException.ParameterOutOfRange>(caps(r, 0, 8).failure())
            assertTrue(r.start(RATE, BLOCK).isSuccess)
            JniCoverage.record(OWNER, "nativeOfflineStart")
            assertTrue(caps(r, 0, 8).isSuccess)
        } finally {
            r.close()
            JniCoverage.record(OWNER, "nativeOfflineDestroy")
        }
    }

    // Bug plausible: un handle cerrado cruza igual, o uno reciclado opera sobre otra instancia.
    @Test
    fun `AC-055_12b con el handle cerrado el setter falla tipado y el id muerto no toca memoria`() {
        val r = newRenderer()
        val stale = r.nativeHandle
        r.close()
        JniCoverage.record(OWNER, "nativeOfflineDestroy")
        assertIs<NativeBridgeException.EngineNotInitialized>(r.setLooperCapabilities(192 * MIB, 12).failure())
        for (h in listOf(stale, 0L, Long.MAX_VALUE)) {
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineLooperSetCapabilities") { it.offlineLooperSetCapabilities(h, 192 * MIB, 12) }, "$h")
            // el handle muerto gana al rango: nunca se mira el valor de una instancia que no existe
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineLooperSetCapabilities") { it.offlineLooperSetCapabilities(h, -1, 99) }, "$h fuera de rango")
        }
        // una instancia nueva no hereda lo pedido a la muerta
        val fresh = newRenderer()
        try {
            fresh.fillDefaultBudget()
            assertIs<NativeBridgeException.MemoryBudgetExceeded>(fresh.load().failure())
            assertEquals(ERROR_OUT_OF_RANGE, jni("nativeOfflineLooperSetCapabilities") { it.offlineLooperSetCapabilities(fresh.nativeHandle, -1, 0) })
        } finally {
            fresh.close()
            JniCoverage.record(OWNER, "nativeOfflineDestroy")
        }
    }
}
