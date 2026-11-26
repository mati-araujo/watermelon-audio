package com.watermellonstudios.audio.internal.bridge

import org.junit.AfterClass
import org.junit.FixMethodOrder
import org.junit.runners.MethodSorters
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * REQ-050 S2 — **las dos entradas USB que cambió esta etapa, ejecutadas contra un `JNIEnv`
 * real** (AC-050.3, AC-050.4).
 *
 * - `nativeStartUsbStreamingWithMode` devolvía un `jboolean`; ahora devuelve la causa
 *   (`UsbStreamStartStatus`). Acá se afirma que el `jint` CRUZA con su valor, en los dos caminos
 *   que el host alcanza: sin motor y sin device. El camino "sin callback" necesita un backend
 *   libusb, que el host no tiene (`createUsbAudioBackend()` devuelve `nullptr`): la decisión la
 *   afirma `test_usb_stream_start.cpp` y el éxito lo cubre el smoke en el g42.
 * - `nativeInitializeUsbDevice` no aseguraba el motor, así que sin él el backend caía en el
 *   `BackendManager` de respaldo y se perdía (MINI-041 #2). En el host la inicialización igual
 *   falla (no hay libusb), pero que el MOTOR quede creado sí es observable, y es lo que decide
 *   en qué manager cae el backend.
 *
 * El orden importa (una JVM por clase, `forkEvery = 1`): `a` necesita el proceso sin motor.
 *
 * 🔴 Backend FALSO adentro: valida la frontera JNI/Kotlin, **no** audio en dispositivo.
 */
@FixMethodOrder(MethodSorters.NAME_ASCENDING)
class UsbContractJniTest {

    companion object {
        private const val OWNER = "UsbContractJniTest"

        /** Un fd >= 0 cualquiera: en el host no se usa, porque no hay backend libusb que lo abra. */
        private const val FAKE_FD = 3
        private const val USBFS_PATH = "/dev/bus/usb/001/011"
        private const val PLAYBACK_ONLY = 0

        private val COVERED = setOf(
            "nativeStartUsbStreamingWithMode",
            "nativeInitializeUsbDevice",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)
    }

    private fun <T> jni(name: String, call: (AudioNativeBridge) -> T): T = JniHarness.exercise(OWNER, name, call)

    private fun start(): Int = jni("nativeStartUsbStreamingWithMode") {
        it.startUsbStreamingWithModeStatus(48000, 2, 24, PLAYBACK_ONLY)
    }

    /**
     * AC-050.3. Sin motor, la causa que cruza es NO_ENGINE, y preguntar no crea el motor.
     *
     * Bug que atrapa: volver al `jboolean` (cruzaría 0/1, y 1 se leería NOT_INITIALIZED), o
     * nombrar el device cuando lo que falta es el motor.
     */
    @Test
    fun `a - AC-050_3 sin motor, arrancar el streaming USB dice NO_ENGINE`() {
        JniHarness.requireNativeLibrary()
        assertFalse(
            AudioNativeBridge.getInstance().isEngineInitialized(),
            "premisa: esta JVM ya tenía motor (¿sigue `forkEvery = 1`?)",
        )

        assertEquals(UsbStreamStartStatus.NO_ENGINE, start())
        assertFalse(AudioNativeBridge.getInstance().isEngineInitialized(), "preguntar por el arranque creó el motor")
    }

    /**
     * AC-050.4. Inicializar el device crea el motor antes de tocar el `BackendManager`.
     *
     * Bug que atrapa: sacar el `ensureEngine()`. El backend se registraría en el manager de
     * respaldo y `wma_engine_create` lo descartaría al instalar el suyo.
     */
    @Test
    fun `b - AC-050_4 inicializar el device USB crea el motor primero`() {
        JniHarness.requireNativeLibrary()
        assertFalse(AudioNativeBridge.getInstance().isEngineInitialized(), "premisa: el motor ya existía")

        val initialized = jni("nativeInitializeUsbDevice") { it.initializeUsbDevice(FAKE_FD, USBFS_PATH) }

        assertFalse(initialized, "el host no tiene libusb: inicializar no puede dar true")
        assertTrue(AudioNativeBridge.getInstance().isEngineInitialized(), "inicializar el device USB no creó el motor")
    }

    /**
     * AC-050.3, con motor y sin device: la causa es el device, no el motor.
     *
     * Es también el gemelo de `a`: un `start` que dijera NO_ENGINE siempre pasaría `a`.
     */
    @Test
    fun `c - AC-050_3 con motor y sin device, arrancar dice NOT_INITIALIZED`() {
        JniHarness.requireNativeLibrary()
        assertTrue(AudioNativeBridge.getInstance().isEngineInitialized(), "premisa: `b` tenía que dejar el motor creado")

        assertEquals(UsbStreamStartStatus.NOT_INITIALIZED, start())
        assertFalse(
            AudioNativeBridge.getInstance().startUsbStreamingWithMode(48000, 2, 24, PLAYBACK_ONLY),
            "el envoltorio Boolean dijo que arrancó",
        )
    }
}
