/**
 * REQ-055 S2 — el estado ESTATICO que dos instancias del motor compartian.
 *
 * Con un solo motor por proceso, un singleton o una tabla perezosa no se notan.
 * Con dos (el vivo y la instancia de render offline de Studio) cada uno es un
 * canal entre instancias. El inventario entero, con su clasificacion (seguro /
 * arreglado / documentado), esta en docs/dev/render-offline.md; aca van los que
 * se ARREGLARON, cada uno con el test que se pone rojo si vuelve:
 *
 *  - `BackendManager::g_globalInstance`: `wma_engine_create()` lo pisaba siempre y
 *    `wma_engine_destroy()` lo dejaba en nulo, y el `AudioEngine` iba siempre al
 *    global. Una segunda instancia le robaba el manager al vivo.
 *  - `g_cLogCallback` de la API C: puntero plano leido desde cualquier hilo.
 *
 * Los de LFO (semilla no reproducible) y de stb_vorbis (tabla CRC estatica) se
 * prueban en `dsp/tests/test_lfo.cpp` y en el render de dos instancias de
 * `test_offline_handle.cpp`. El registro de `getNumParams` (EffectChain.cpp) se
 * arreglo sin test: su unico camino, `savePreset`/`loadPreset`, no tiene llamador
 * de produccion, y un test que lo ejerza solo fabricaria una sonda nueva.
 */

#include "support/FakeAudioBackend.h"
#include "tests/support/TestWait.h"

#include "api/watermelon_audio.h"
#include "api/watermelon_audio_internal.h"
#include "backends/BackendManager.h"
#include "platform/Logger.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace {

using watermelon_audio::BackendManager;

class OfflineStaticState : public ::testing::Test {
protected:
    void SetUp() override {
        wma::setLogCallback([](wma::LogLevel, const char*, const char*) {});
    }
    void TearDown() override { wma::setLogCallback(nullptr); }
};

}  // namespace

TEST_F(OfflineStaticState, ASecondEngineDoesNotStealTheGlobalBackendManager) {
    // Bug que atrapa: el JNI entero (USB, backend, split) pasa por getInstance().
    // Si crear la instancia offline la registra, el vivo queda manejado por el
    // manager de OTRO motor; si destruirla la limpia, cae al default estatico.
    WmaEngine* live = wma_engine_create();
    ASSERT_NE(live, nullptr);
    BackendManager* liveManager = live->backendManager.get();
    ASSERT_EQ(&BackendManager::getInstance(), liveManager);

    WmaEngine* offline = wma_engine_create();
    ASSERT_NE(offline, nullptr);
    EXPECT_EQ(&BackendManager::getInstance(), liveManager)
        << "crear una segunda instancia cambio el manager global";

    wma_engine_destroy(offline);
    EXPECT_EQ(&BackendManager::getInstance(), liveManager)
        << "destruir la segunda instancia des-registro el manager del vivo";

    wma_engine_destroy(live);
}

TEST_F(OfflineStaticState, AnOfflineInstanceCreatedFirstGivesTheGlobalBackToTheLive) {
    // Bug que atrapa (review de S2): con "el primero que se registra gana", una
    // instancia offline creada ANTES que el vivo se quedaba con el global, y las
    // funciones sin handle (wma_select_backend, wma_usb_*) configuraban la offline.
    WmaEngine* offline = wma_engine_create();
    ASSERT_NE(offline, nullptr);
    ASSERT_EQ(&BackendManager::getInstance(), offline->backendManager.get())
        << "precondicion: siendo la primera, la offline se registro";
    ASSERT_EQ(wma_offline_start(offline, 48000, 256), WMA_OK);
    EXPECT_NE(&BackendManager::getInstance(), offline->backendManager.get())
        << "arrancar un render offline no le devolvio el global";

    WmaEngine* live = wma_engine_create();
    ASSERT_NE(live, nullptr);
    EXPECT_EQ(&BackendManager::getInstance(), live->backendManager.get())
        << "el vivo creado despues no quedo como global";

    wma_offline_stop(offline);
    wma_engine_destroy(offline);
    EXPECT_EQ(&BackendManager::getInstance(), live->backendManager.get());
    wma_engine_destroy(live);
}

TEST_F(OfflineStaticState, AnOfflineEngineDoesNotReadTheLiveStreamThroughTheGlobal) {
    // Bug que atrapa: el AudioEngine de la instancia offline preguntaba al manager
    // GLOBAL (el del vivo) por el stream, asi que con el vivo corriendo a 44,1 kHz
    // la instancia offline a 48 kHz "veia" un stream de 44,1 que no es suyo — y de
    // ahi sale el rate de control con que se cargan los SoundFonts.
    wma_test::resetLastCreatedSystemBackend();
    WmaEngine* live = wma_engine_create();
    ASSERT_NE(live, nullptr);
    auto* fake = wma_test::lastCreatedSystemBackend();
    ASSERT_NE(fake, nullptr);
    fake->setNegotiatedSampleRate(44100);
    wma_set_use_backend_manager(live, true);
    ASSERT_TRUE(wma_select_backend(1));
    ASSERT_EQ(wma_engine_start(live, 0), WMA_OK);

    WmaEngine* offline = wma_engine_create();
    ASSERT_NE(offline, nullptr);
    ASSERT_EQ(wma_offline_start(offline, 48000, 256), WMA_OK);

    EXPECT_EQ(offline->engine->controlSampleRate(), 48000)
        << "la instancia offline leyo el rate del stream del vivo";
    int32_t rate = 0, buffer = 0;
    double latency = 0.0;
    EXPECT_FALSE(offline->engine->queryStreamInfo(rate, buffer, latency))
        << "la instancia offline reporta un stream (el del vivo, a " << rate << " Hz)";
    EXPECT_EQ(live->engine->controlSampleRate(), 44100) << "el vivo perdio su propio rate";

    wma_offline_stop(offline);
    wma_engine_destroy(offline);
    wma_engine_stop(live, 0);
    wma_engine_destroy(live);
}

namespace {
std::atomic<int> gCLogCalls{0};
void countingCLog(WmaLogLevel, const char*, const char*) { gCLogCalls.fetch_add(1); }
}  // namespace

TEST_F(OfflineStaticState, TheCLogCallbackCanBeSwappedWhileAnotherInstanceLogs) {
    // `g_cLogCallback` es de proceso: lo comparten todas las instancias. Un hilo
    // que lo cambia mientras otra instancia loguea era una carrera (TSan).
    std::atomic<bool> stop{false};
    std::thread logger([&] {
        while (!stop.load()) {
            WmaEngine* e = wma_engine_create();   // loguea al construir
            wma_engine_destroy(e);
        }
    });
    for (int i = 0; i < 200; ++i) {
        wma_set_log_callback(countingCLog);
        wma_set_log_callback(nullptr);
    }
    wma_set_log_callback(countingCLog);
    const int before = gCLogCalls.load();
    EXPECT_TRUE(wma_test::waitUntil([&] { return gCLogCalls.load() > before; }))
        << "el callback C instalado nunca recibio una linea";
    stop.store(true);
    logger.join();
    wma_set_log_callback(nullptr);
}
