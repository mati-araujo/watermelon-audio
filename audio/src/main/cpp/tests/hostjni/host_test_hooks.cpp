/**
 * host_test_hooks.cpp — PALANCAS DE TEST, sólo en el `.so` del arnés de host.
 *
 * REQ-045 S1 (AC-045.1, AC-045.3) necesita dos estímulos que ninguna superficie
 * de producción sabe producir:
 *
 *   1. **que el arranque falle.** `AudioEngine::start()` devuelve false cuando
 *      `BackendManager::start()` falla, y en el host el backend es el
 *      `FakeAudioBackend` que `test_platform_backends.cpp` le entrega al manager.
 *      Ese fake ya tiene la palanca (`setStartResult`) y el `.so` del arnés ya
 *      compila esa TU: lo único que faltaba era poder accionarla **desde la JVM**.
 *   2. **que el stream reporte canales y modo que no sean el default**, para que
 *      "leído del stream" se distinga de "inventado en Kotlin" (D10).
 *
 * ## Por qué acá y no un hook en producción
 *
 * Este archivo NO está en `audio/src/main/cpp/jni/`, así que:
 *
 *   * `scripts/check-jni-results.py`, `check-jni-signatures.py` y
 *     `check-doc-counts.py` no lo ven (los tres leen las fuentes de `jni/`): sus conteos y
 *     su guarda de completitud siguen midiendo la superficie de PRODUCCIÓN, que
 *     es lo que tienen que medir;
 *   * `check-jni-symbols.py` compara contra el `.so` de Android, que no lo lleva;
 *   * `JniExports.fromTree()` del arnés tampoco, así que estas entradas no entran
 *     al denominador de cobertura ni se pueden anotar con `JniHarness.exercise`
 *     (su `verify` rechaza un nombre que no exista como `JNIEXPORT` de
 *     `AudioNativeBridge`). Se llaman directo, y eso es correcto: un hook de test
 *     no es cobertura de la frontera.
 *
 * El `.so` de producción no cambia: este archivo sólo lo agrega
 * `tests/hostjni/CMakeLists.txt`.
 *
 * 🔴 Estas funciones devuelven `false` cuando no pudieron accionar nada, y los
 * tests lo AFIRMAN. Una palanca que no hizo nada y contesta en silencio deja el
 * test midiendo el camino de éxito con nombre de camino de fallo — que es la
 * misma clase de falso verde que REQ-045 vino a borrar.
 */

#include <jni.h>

#include <mutex>

#include "api/watermelon_audio.h"
#include "backends/BackendManager.h"
#include "core/tests/support/FakeAudioBackend.h"
#include "jni/jni_common.h"

namespace {

/**
 * El fake que el manager tiene ahora, o `nullptr`.
 *
 * `getInstance()` se llama acá para FORZAR la construcción del manager: el fake
 * nace en su constructor (`createSystemAudioBackend`), así que sin esta llamada
 * `lastCreatedSystemBackend()` podría contestar `nullptr` sólo porque nadie tocó
 * el singleton todavía — y el test leería "no pude" donde en realidad no había
 * pedido nada.
 */
wma_test::FakeAudioBackend* fake() {
    (void)watermelon_audio::BackendManager::getInstance();
    return wma_test::lastCreatedSystemBackend();
}

}  // namespace

extern "C" {

/**
 * A partir de acá, `start()` del backend falla con `ERROR_STREAM_FAILED` (o
 * vuelve a `OK` con `fails = false`).
 *
 * Asegura el motor primero, porque sin él no hay manager ni fake que accionar —
 * y el motor se crea sin abrir stream, así que esto no arranca audio.
 */
JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_HostTestHooks_nativeSetStartFails(
    JNIEnv* env, jobject thiz, jboolean fails) {
    (void)env;
    (void)thiz;
    if (!ensureEngine()) return JNI_FALSE;
    auto* backend = fake();
    if (!backend) return JNI_FALSE;
    backend->setStartResult(fails == JNI_TRUE
                                ? watermelon_audio::BackendResult::ERROR_STREAM_FAILED
                                : watermelon_audio::BackendResult::OK);
    return JNI_TRUE;
}

/**
 * Devuelve el proceso al estado "todavia no hay motor" (REQ-045 S2, AC-045.4).
 *
 * ## Por que hace falta una palanca
 *
 * La propiedad de AC-045.4 es *"una configuracion llamada ANTES de que exista el
 * motor llega igual"*, y el estado "no hay motor" existe **una sola vez por
 * JVM**: el motor nativo es un singleton de proceso. Con una JVM por clase
 * (`forkEvery = 1`) eso alcanza para UNA configuracion, y la spec pide el
 * conjunto entero — no una muestra. Sin esto habria que elegir entre afirmar la
 * propiedad para una sola y afirmarla por lectura del codigo, y lo segundo no es
 * afirmarla.
 *
 * Hace **exactamente** lo que hace `JNI_OnUnload` (jni_engine.cpp): toma
 * `engineMutex`, suelta el InputNode con las dos manijas, despublica los dos
 * punteros ANTES de destruir, y recien ahi destruye. El orden no es cosmetico y
 * esta explicado alla.
 *
 * Devuelve `false` si ya no habia motor: un reset que no reseteo nada dejaria al
 * test midiendo el camino "con motor" con nombre de camino "sin motor".
 */
JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_HostTestHooks_nativeResetEngine(
    JNIEnv* env, jobject thiz) {
    (void)env;
    (void)thiz;
    std::lock_guard<std::mutex> lock(g_jniState.engineMutex);
    if (!g_wmaEngine) return JNI_FALSE;
    releaseInputNode();
    WmaEngine* engine = g_wmaEngine;
    g_wmaEngine = nullptr;
    g_jniState.engine = nullptr;
    wma_engine_destroy(engine);
    return JNI_TRUE;
}

/** Lo que el "device" va a reportar como canales y modo de baja latencia (D10). */
JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_HostTestHooks_nativeSetNegotiatedStream(
    JNIEnv* env, jobject thiz, jint channelCount, jboolean lowLatency) {
    (void)env;
    (void)thiz;
    if (!ensureEngine()) return JNI_FALSE;
    auto* backend = fake();
    if (!backend) return JNI_FALSE;
    backend->setNegotiatedChannelCount(channelCount);
    backend->setNegotiatedLowLatency(lowLatency == JNI_TRUE);
    return JNI_TRUE;
}

}  // extern "C"
