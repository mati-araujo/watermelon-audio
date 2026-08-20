/**
 * REQ-006.1 — adoptar un sample rate nuevo sin correr contra el thread de audio.
 *
 * EL DEFECTO QUE ESTOS TESTS PERSIGUEN
 * ------------------------------------
 * `AudioEngine::start()` pre-configura los componentes con el rate que ESPERA
 * (`AudioEngine.cpp:501`) y, si el device coerce a otro, los re-configura con el
 * real — pero eso ocurre **despues de `manager.start()`** (`:568`), o sea con el
 * backend ya entregando callbacks. `configureComponentsWithSampleRate()` llama a
 * `SynthEngineDispatcher::prepare()`, que reasigna la `DelayLine` de
 * Karplus-Strong y hace `resize()` del buffer de Granular. El thread de audio
 * esta leyendo esas mismas estructuras.
 *
 * Medido el 2026-08-20 con la misma operacion sobre un render concurrente:
 * **23 carreras y abort** bajo TSan, con la pila
 * `SynthEngineDispatcher::prepare` -> `KarplusStrongEngine::prepare` ->
 * `DelayLine::DelayLine` contra `KarplusStrongEngine::process` ->
 * `DelayLine::read/write`.
 *
 * POR QUE HAY UNA COMPUERTA Y NO MAS ITERACIONES
 * ---------------------------------------------
 * La ventana es de microsegundos y esta al final de `start()`. Un test que
 * arranque el render "mas o menos cuando arranca el motor" la pega por azar, que
 * es la clase de test que da verde con el codigo roto (paso en este repo: tres
 * tests pasaron 15 corridas con el bug vivo). `FakeAudioBackend` tiene un freno
 * en `start()` justo para esto: se bloquea, se espera a que `start()` haya
 * ENTRADO —momento en el que el motor ya paso a `Running`, porque
 * `transitionToState()` corre antes que `manager.start()`— se larga el render, y
 * recien ahi se destraba. El re-configure de la coercion corre entonces con el
 * thread de audio garantizadamente adentro.
 */

#include "support/BackendPathFixture.h"
#include "../../engines/tests/PitchHarness.h"

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

using wma_test::BackendPathFixture;

namespace {

/// Karplus-Strong: el engine cuyo `prepare()` reasigna la DelayLine.
constexpr int kKarplusStrong = 1;

/// El rate que el motor PIDE. El fake devuelve otro, y esa diferencia es la que
/// dispara la rama de coercion de `start()`.
constexpr int kRequestedRate = 48000;
constexpr int kNegotiatedRate = 44100;

} // namespace

/**
 * AC-006.1 — WHILE el motor renderiza, WHEN se adopta un rate distinto del
 * preparado, no puede haber carrera sobre el estado de los engines.
 *
 * El veredicto de este test NO es su exit code: es lo que diga ThreadSanitizer.
 * Sin TSan corre igual y sirve de humo (no debe colgarse ni abortar), pero la
 * asercion que importa la hace el sanitizer del CI.
 */
TEST_F(BackendPathFixture, RateCoercionReconfiguresEnginesWithoutRacingTheAudioThread) {
    mBackend->setNegotiatedSampleRate(kNegotiatedRate);
    mEngine->setPreferredSampleRate(kRequestedRate);
    mEngine->setEngineType(kKarplusStrong);
    mEngine->setFrequencyAndAmplitude(440.0f, 0.8f);
    mEngine->setUseBackendManager(true);
    ASSERT_TRUE(mManager->selectBackend(watermelon_audio::BackendType::OBOE));

    // 1. Trabar el backend ADENTRO de start().
    mBackend->blockStart();

    std::atomic<bool> startReturned{false};
    std::thread starter([&] {
        mEngine->start(0);
        startReturned.store(true, std::memory_order_release);
    });

    // 2. Esperar a que start() haya entrado. En ese punto el motor ya esta en
    //    Running, asi que los callbacks hacen trabajo de verdad.
    mBackend->waitUntilStartEntered();

    // 3. Largar el render. Corre hasta que start() vuelva, o sea cubre entera la
    //    ventana del re-configure de la coercion.
    std::atomic<bool> stopRender{false};
    std::thread audio([&] {
        // 512 y no un bloque mas grande, y esto esta MEDIDO: agrandar el bloque
        // EMPEORA la deteccion. Con 4096 el mutante que saca la compuerta pasa
        // de 15 carreras a 5. Lo que expone esta clase no es cuanto dura cada
        // callback sino CUANTOS entran mientras el control re-prepara, y un
        // bloque mas grande son menos entradas en la misma ventana.
        constexpr int kBlock = 512;
        std::vector<float> buffer(kBlock * 2, 0.0f);
        while (!stopRender.load(std::memory_order_relaxed)) {
            mEngine->onAudioReady(buffer.data(), nullptr, kBlock);
        }
    });

    // 4. Destrabar: manager.start() vuelve, se lee el rate negociado (44100 !=
    //    48000) y se re-configura con el render adentro.
    mBackend->releaseStart();

    starter.join();
    // Se espera POR CONDICION a que start() haya vuelto antes de cortar el
    // render, para que el re-configure quede cubierto entero.
    wma_test::waitUntil([&] { return startReturned.load(std::memory_order_acquire); });
    stopRender.store(true, std::memory_order_relaxed);
    audio.join();

    EXPECT_EQ(mEngine->currentSampleRate(), kNegotiatedRate)
        << "la coercion no se adopto: el motor sigue creyendo que corre a otro rate";

    mEngine->stop();
}

/**
 * AC-006.2 — WHEN el device coerce el rate despues de `manager.start()`, la
 * fundamental de Karplus-Strong queda dentro de +-15 cents de lo pedido.
 *
 * HOY ESTE TEST PASA, y esta puesto igual: es un TRINQUETE. El arreglo de
 * AC-006.1 toca justamente el camino que hace que hoy pase, asi que sin esta
 * red un quiesce mal puesto —que no llegue a preparar, o que prepare con el rate
 * viejo— cerraria la carrera rompiendo la afinacion, y la suite no lo notaria.
 *
 * El +-15 no es una tolerancia elegida: separa las dos poblaciones MEDIDAS el
 * 2026-08-20. Bien preparado da +2,86 / +5,41 cents; con el rate stale,
 * -143,89 / -140,68. No hay nada en el medio.
 */
TEST_F(BackendPathFixture, CoercedRateStillPlaysTheStringInTune) {
    mBackend->setNegotiatedSampleRate(kNegotiatedRate);
    mEngine->setPreferredSampleRate(kRequestedRate);
    mEngine->setUseBackendManager(true);
    ASSERT_TRUE(mManager->selectBackend(watermelon_audio::BackendType::OBOE));
    ASSERT_TRUE(mEngine->start(0));
    ASSERT_EQ(mEngine->currentSampleRate(), kNegotiatedRate);

    mEngine->setEngineType(kKarplusStrong);

    for (float target : {220.0f, 440.0f}) {
        mEngine->setFrequencyAndAmplitude(target, 0.8f);

        constexpr int kFrames = 512;
        const int blocks = kNegotiatedRate / kFrames;  // ~1 s
        std::vector<float> mono;
        std::vector<float> buffer(kFrames * 2, 0.0f);
        for (int b = 0; b < blocks; ++b) {
            std::fill(buffer.begin(), buffer.end(), 0.0f);
            mEngine->onAudioReady(buffer.data(), nullptr, kFrames);
            for (int i = 0; i < kFrames; ++i) {
                mono.push_back(buffer[static_cast<size_t>(i) * 2]);
            }
        }

        const size_t from = static_cast<size_t>(0.1 * kNegotiatedRate);
        const size_t len = static_cast<size_t>(0.4 * kNegotiatedRate);
        ASSERT_GE(mono.size(), from + len);
        const double f = wma::pitch::fundamentalHz(mono, from, len, kNegotiatedRate, target);
        ASSERT_GT(f, 0.0) << "no se pudo medir la fundamental de " << target << " Hz";

        const double offCents = 1200.0 * std::log2(f / static_cast<double>(target));
        EXPECT_LT(std::abs(offCents), 15.0)
            << "con el device coercionado a " << kNegotiatedRate << " Hz, la nota de " << target
            << " Hz salio en " << f << " Hz (" << offCents << " cents).\n"
            << "  Cerca de -140 cents significa que los engines quedaron preparados al rate "
            << "que se PIDIO (" << kRequestedRate << ") y no al que el device dio.";
    }

    mEngine->stop();
}
