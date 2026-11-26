package com.watermellonstudios.audio.internal.bridge

import org.junit.AfterClass
import kotlin.test.Test
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * REQ-050 S3 (AC-050.8, D17) — **el centinela de "volver a la selección automática", del lado
 * C++ de la frontera, ejecutado contra un `JNIEnv` real**.
 *
 * El runner USB renegocia cada fila con `selectAltsetting`/`selectClockSource`, y esa selección
 * es pegajosa en el backend. Para devolverle al consumidor la elección automática, las dos
 * JNIEXPORT que ya existían aceptan un centinela: (-1, -1, -1) y reloj 0. Kotlin lo valida antes
 * de cruzar (`UsbConnectionContractTest`); acá se afirma que el nativo lo valida OTRA VEZ por su
 * cuenta, porque `AudioNativeBridge` es público para el resto del módulo y nada obliga a pasar por
 * el manager.
 *
 * En el host no hay backend libusb, y eso hace OBSERVABLE la clasificación: limpiar sin backend
 * es éxito (no hay selección manual que olvidar: el estado ya es el automático), y cualquier otro
 * pedido es `false` — el rechazo, antes de buscar el backend, y la selección válida, porque no hay
 * backend que la reciba. Un clasificador que tomara cualquier negativo por centinela daría `true`
 * en los rechazos.
 *
 * 🔴 Backend FALSO adentro: valida la frontera JNI/Kotlin, **no** la selección en un device.
 */
class UsbSelectionJniTest {

    companion object {
        private const val OWNER = "UsbSelectionJniTest"

        private val COVERED = setOf(
            "nativeSelectUsbAltsetting",
            "nativeSelectUsbClockSource",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)
    }

    private fun alt(i: Int, a: Int, f: Int): Boolean =
        JniHarness.exercise(OWNER, "nativeSelectUsbAltsetting") { it.selectUsbAltsetting(i, a, f) }

    private fun clock(id: Int): Boolean =
        JniHarness.exercise(OWNER, "nativeSelectUsbClockSource") { it.selectUsbClockSource(id) }

    /** D17. El centinela exacto limpia, y sin backend eso es un éxito. */
    @Test
    fun `D17 el centinela exacto vuelve a automatico`() {
        JniHarness.requireNativeLibrary()
        assertTrue(alt(-1, -1, -1), "(-1,-1,-1) no se tomó como 'volver a automático'")
        assertTrue(clock(0), "reloj 0 no se tomó como 'volver a automático'")
    }

    /**
     * D17. Cualquier otro valor fuera de rango se sigue rechazando.
     *
     * Bug que atrapa: un clasificador que tome cualquier negativo (o cualquier reloj <= 0) por
     * centinela; daría `true` acá.
     */
    @Test
    fun `D17 un negativo distinto del centinela se rechaza`() {
        JniHarness.requireNativeLibrary()
        assertFalse(alt(-1, -1, 0), "(-1,-1,0) se tomó como centinela")
        assertFalse(alt(0, -1, -1), "(0,-1,-1) se tomó como centinela")
        assertFalse(alt(-2, -2, -2), "(-2,-2,-2) se tomó como centinela")
        assertFalse(clock(-1), "el reloj -1 se tomó como centinela")
        assertFalse(clock(256), "el reloj 256 (no entra en un byte) se aceptó")
    }
}
