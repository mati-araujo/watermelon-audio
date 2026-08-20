#pragma once

/**
 * BackendPathFixture.h — host test support.
 *
 * Puts an AudioEngine on the BackendManager audio path with a FakeAudioBackend
 * underneath it, which is the configuration all three fixed bugs live in (USB
 * today, CoreAudio on iOS tomorrow). The legacy direct-Oboe path is compiled
 * out off Android, so it is out of scope here.
 *
 * Ordering matters and is enforced by the fixture rather than left to each
 * test: the manager must outlive the engine (the engine's destructor calls
 * stop(), which goes through BackendManager::getInstance()), and the global
 * instance must still point at our manager while that happens. TearDown does
 * engine → global pointer → manager, in that order.
 */

#include "tests/support/TestWait.h"
#include "FakeAudioBackend.h"

#include "backends/BackendManager.h"
#include "core/AudioEngine.h"
#include "platform/Logger.h"

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace wma_test {

class BackendPathFixture : public ::testing::Test {
protected:
    void SetUp() override {
        // The engine logs generously outside the RT path; a no-op sink keeps
        // the ctest output readable without touching production defaults.
        wma::setLogCallback([](wma::LogLevel, const char*, const char*) {});

        resetLastCreatedSystemBackend();
        mManager = std::make_unique<watermelon_audio::BackendManager>();
        watermelon_audio::BackendManager::setGlobalInstance(mManager.get());

        mBackend = lastCreatedSystemBackend();
        ASSERT_NE(mBackend, nullptr)
            << "the test platform registration point did not hand the manager a fake";

        mEngine = std::make_unique<AudioEngine>();
    }

    void TearDown() override {
        mEngine.reset();
        watermelon_audio::BackendManager::setGlobalInstance(nullptr);
        mManager.reset();
        mBackend = nullptr;
        wma::setLogCallback(nullptr);
    }

    /**
     * Make the manager report a running stream at @p negotiatedSampleRate,
     * without starting the engine. Models "a stream exists" for the queries
     * that only read stream state.
     */
    void runBackendAt(int negotiatedSampleRate) {
        mBackend->setNegotiatedSampleRate(negotiatedSampleRate);
        ASSERT_TRUE(mManager->selectBackend(watermelon_audio::BackendType::OBOE));
        mManager->setCallback(mEngine.get());
        ASSERT_EQ(mManager->start(), watermelon_audio::BackendResult::OK);
        ASSERT_TRUE(mManager->isRunning());
    }

    /**
     * Full engine start over the backend path: the device settles on
     * @p negotiatedSampleRate regardless of what the engine asked for.
     * Leaves the engine Running, so onAudioReady() renders for real.
     */
    void startEngineAt(int negotiatedSampleRate, int fadeTimeMs = 0) {
        mBackend->setNegotiatedSampleRate(negotiatedSampleRate);
        mEngine->setUseBackendManager(true);
        ASSERT_TRUE(mManager->selectBackend(watermelon_audio::BackendType::OBOE));
        ASSERT_TRUE(mEngine->start(fadeTimeMs));
    }

    /// Render @p blocks callbacks of @p framesPerBlock frames through the engine.
    void render(int blocks, int framesPerBlock) {
        std::vector<float> buffer(static_cast<size_t>(framesPerBlock) * 2, 0.0f);
        for (int i = 0; i < blocks; ++i) {
            mEngine->onAudioReady(buffer.data(), nullptr, framesPerBlock);
        }
    }

    /**
     * Outlive stopWithFade()'s detached thread. **RED DE SEGURIDAD, NO
     * SINCRONIZACION** — no la uses para esperar antes de una asercion.
     *
     * stopWithFade() spawns a detached thread that sleeps fadeTimeMs + 50 and
     * then calls stop() on the engine. Tearing the engine down before it fires
     * is a use-after-free, so every test that starts one waits it out here
     * instead of racing it.
     *
     * 🔴 ESTA FUNCION TENIA DOS ROLES MEZCLADOS, Y ESO ESCONDIA UN DEFECTO.
     * Ademas de la red de seguridad, tres tests la usaban para esperar antes de
     * afirmar — y uno de ellos (`StopWithFadeEventuallyStopsTheEngine`) afirmaba
     * `getEngineState() == kStateStopped` justo despues. Eso es sincronizacion
     * por duracion: la clase entera de REQ-002.
     *
     * **Lo encontro el instrumento (`scripts/check-time-dependence.sh`), no yo.**
     * Yo habia clasificado este sitio como "ausencia, no convertible" y me equivoque:
     * una funcion con dos responsabilidades hacia ver como no convertible algo que
     * si lo era. Para esperar-y-afirmar esta `awaitEngineStopped()`.
     */
    void awaitDetachedStop(int fadeTimeMs) {
        // AUSENCIA, y de la peor especie: no hay nada que consultar porque el
        // thread esta DETACHED — no se lo puede joinear ni preguntarle si
        // termino. Lo unico disponible es darle la ventana en la que habria
        // corrido.
        //
        // 🔴 Si esta espera se queda corta, el sintoma NO es un test rojo: es un
        // use-after-free sobre el motor. Es la misma forma del defecto que
        // REQ-002.2 encontro del otro lado (el receptor que moria antes que el
        // motor), y la salida de fondo es la misma: que el objeto viva mas, no
        // que la espera sea mas larga. Eso pide tocar `stopWithFade()`, que esta
        // fuera del alcance de este REQ — queda anotado, no resuelto.
        //
        // Va por `sleepFixed` para que el instrumento la VEA.
        wma_test::sleepFixed(std::chrono::milliseconds(fadeTimeMs + 250));
    }

    /**
     * Espera a que el motor QUEDE PARADO, por condicion y con techo. Esta es la
     * que va antes de una asercion sobre el estado.
     *
     * Despues de la condicion sigue corriendo la red de seguridad: que el estado
     * ya sea `kStateStopped` significa que el thread detached llamo a `stop()`,
     * no que haya terminado de salir.
     */
    bool awaitEngineStopped(int fadeTimeMs) {
        // 0 == stopped. El fixture no ve la constante `kStateStopped` de los
        // archivos de test, y duplicar el numero aca con su nombre al lado es
        // preferible a exportar una constante de test desde el header.
        constexpr int kStopped = 0;
        const bool stopped = wma_test::waitUntil(
            [this] { return mEngine->getEngineState() == kStopped; });
        awaitDetachedStop(fadeTimeMs);
        return stopped;
    }

    std::unique_ptr<watermelon_audio::BackendManager> mManager;
    std::unique_ptr<AudioEngine> mEngine;
    FakeAudioBackend* mBackend = nullptr;
};

}  // namespace wma_test
