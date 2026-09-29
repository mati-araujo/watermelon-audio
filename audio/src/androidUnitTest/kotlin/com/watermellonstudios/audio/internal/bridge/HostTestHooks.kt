package com.watermellonstudios.audio.internal.bridge

import kotlin.test.fail

/**
 * Las palancas del `.so` del arnés — **código de test, no de producción** (REQ-045 S1).
 *
 * Su contraparte C++ es `audio/src/main/cpp/tests/hostjni/host_test_hooks.cpp`, que
 * SÓLO entra al `.so` de host. Ver su encabezado para por qué vive fuera de
 * `jni/`: los cuatro gates de la frontera (`check-jni-results`,
 * `check-jni-signatures`, `check-jni-symbols`, `check-doc-counts`) siguen midiendo
 * la superficie de producción, y estas entradas no entran al denominador de
 * cobertura del arnés.
 *
 * 🔴 **No se llaman por [JniHarness.exercise] y no se pueden.** Su `verify` rechaza
 * un nombre que no exista como `JNIEXPORT` de `AudioNativeBridge`, que es
 * exactamente la guarda que impide inflar la cobertura con un string. Un hook de
 * test no es cobertura de la frontera, así que se llama directo.
 *
 * 🔴 **"No pude" nunca es un pase.** Las dos devuelven `false` cuando no había
 * backend que accionar, y los envoltorios de acá lo convierten en un fallo que
 * nombra la causa. Sin eso, un test de "el arranque falla" mediría el camino de
 * ÉXITO con nombre de camino de fallo: el mismo falso verde que REQ-045 borra.
 */
internal object HostTestHooks {

    /** Hace que `BackendManager::start()` falle (o vuelva a andar). */
    fun setStartFails(fails: Boolean) {
        JniHarness.requireNativeLibrary()
        if (!nativeSetStartFails(fails)) {
            fail(
                "la palanca de arranque no pudo accionarse: el host no tiene el FakeAudioBackend " +
                    "que test_platform_backends.cpp le da al BackendManager. Sin ella este test " +
                    "estaría midiendo el camino de éxito con nombre de camino de fallo.",
            )
        }
    }

    /** Lo que el "device" va a reportar como canales y modo de baja latencia. */
    fun setNegotiatedStream(channelCount: Int, lowLatency: Boolean) {
        JniHarness.requireNativeLibrary()
        if (!nativeSetNegotiatedStream(channelCount, lowLatency)) {
            fail(
                "no pude fijar el stream negociado del fake ($channelCount canales, " +
                    "lowLatency=$lowLatency): sin eso, un valor que VIAJÓ no se distingue de uno " +
                    "que Kotlin inventó, porque los dos leerían el default.",
            )
        }
    }

    private external fun nativeSetStartFails(fails: Boolean): Boolean
    private external fun nativeSetNegotiatedStream(channelCount: Int, lowLatency: Boolean): Boolean
}
