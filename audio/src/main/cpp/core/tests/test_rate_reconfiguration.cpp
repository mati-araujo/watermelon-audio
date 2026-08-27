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

/// El rate con el que el motor PREPARA sus componentes antes de que el device
/// negocie. El fake devuelve otro, y esa diferencia es la que dispara la rama de
/// coercion de `start()`.
///
/// 🔴 Hasta MINI-007 este valor se plantaba con `setPreferredSampleRate()`. Ese
/// setter se borro —ningun consumidor podia alcanzarlo— y no hizo falta cambiar
/// nada mas: 48000 es justamente el rate con el que `start()` pre-configura
/// cuando todavia no hay device al que preguntarle, asi que el escenario de estos
/// dos AC quedo intacto. Si ese literal de `AudioEngine::start()` cambiara, este
/// tiene que acompañarlo o los dos tests dejan de producir coercion.
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

// ---------------------------------------------------------------------------
// REQ-006.2 — el hueco de onStreamConfigChanged
// ---------------------------------------------------------------------------

namespace {

/// Renderiza `seconds` de audio por el motor y devuelve el canal izquierdo.
/// `renderRate` es el rate al que se INTERPRETA lo rendido, que es lo que hace
/// observable si los engines quedaron preparados a otro.
std::vector<float> renderMono(AudioEngine& engine, int renderRate, double seconds) {
    constexpr int kFrames = 512;
    const int blocks = static_cast<int>(seconds * renderRate) / kFrames;
    std::vector<float> mono;
    std::vector<float> buffer(kFrames * 2, 0.0f);
    for (int b = 0; b < blocks; ++b) {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        engine.onAudioReady(buffer.data(), nullptr, kFrames);
        for (int i = 0; i < kFrames; ++i) {
            mono.push_back(buffer[static_cast<size_t>(i) * 2]);
        }
    }
    return mono;
}

} // namespace

/**
 * AC-006.3 — WHEN `onStreamConfigChanged` transporta un sample rate distinto del
 * que los engines tienen preparado, THE SYSTEM SHALL propagarselo.
 *
 * COMO SE HACE OBSERVABLE
 * -----------------------
 * El motor arranca a 44,1 kHz y el device pasa a 48. Desde ese momento el stream
 * ENTREGA bloques que se reproducen a 48 kHz, asi que la salida se mide a 48.
 * Si los engines quedaron preparados a 44,1, el lazo de Karplus tiene menos
 * muestras de las que corresponden para ese rate: da la vuelta mas rapido y la
 * nota sale ALTA, por el ratio 48000/44100 = +147 cents.
 *
 * Es el mismo mecanismo que `AStaleSampleRatePlaysTheStringFlatNotSharp` mide en
 * la otra direccion, y por eso el signo es el contrario: alla el rate preparado
 * era MAS ALTO que el real (lazo largo, nota baja); aca es mas BAJO.
 *
 * El +-15 cents es el mismo umbral de AC-006.2, y por la misma razon: separa las
 * dos poblaciones medidas, que no tienen nada en el medio.
 */
TEST_F(BackendPathFixture, AConfigChangeReachesTheSynthEngines) {
    startEngineAt(kNegotiatedRate);          // 44100
    mEngine->setEngineType(kKarplusStrong);

    // El device se va a 48 kHz — hot-plug de USB, cambio de ruteo.
    watermelon_audio::StreamInfo info{};
    info.sampleRate = kRequestedRate;        // 48000
    info.channelCount = 2;
    mEngine->onStreamConfigChanged(info);

    for (float target : {220.0f, 440.0f}) {
        mEngine->setFrequencyAndAmplitude(target, 0.8f);
        const std::vector<float> mono = renderMono(*mEngine, kRequestedRate, 1.0);

        const size_t from = static_cast<size_t>(0.1 * kRequestedRate);
        const size_t len = static_cast<size_t>(0.4 * kRequestedRate);
        ASSERT_GE(mono.size(), from + len);
        const double f = wma::pitch::fundamentalHz(mono, from, len, kRequestedRate, target);
        ASSERT_GT(f, 0.0) << "no se pudo medir la fundamental de " << target << " Hz";

        const double offCents = 1200.0 * std::log2(f / static_cast<double>(target));
        EXPECT_LT(std::abs(offCents), 15.0)
            << "el motor paso a " << kRequestedRate << " Hz y la nota de " << target
            << " Hz salio en " << f << " Hz (" << offCents << " cents).\n"
            << "  Cerca de +147 significa que onStreamConfigChanged no le llevo el rate nuevo a "
            << "los engines: siguen preparados a " << kNegotiatedRate << ".";
    }

    mEngine->stop();
}

/**
 * AC-006.1, en el segundo call site. El quiesce que S1 puso adentro de
 * `configureComponentsWithSampleRate()` deberia cubrir tambien este camino
 * "gratis" — pero eso hay que MEDIRLO, no suponerlo. El veredicto lo da TSan.
 */
TEST_F(BackendPathFixture, AConfigChangeReconfiguresWithoutRacingTheAudioThread) {
    startEngineAt(kNegotiatedRate);
    mEngine->setEngineType(kKarplusStrong);
    mEngine->setFrequencyAndAmplitude(440.0f, 0.8f);

    std::atomic<bool> stopRender{false};
    std::thread audio([&] {
        constexpr int kBlock = 512;   // ver la nota de tamaño de bloque mas arriba
        std::vector<float> buffer(kBlock * 2, 0.0f);
        while (!stopRender.load(std::memory_order_relaxed)) {
            mEngine->onAudioReady(buffer.data(), nullptr, kBlock);
        }
    });

    // Varios cambios seguidos, como un hot-plug repetido.
    for (int i = 0; i < 6; ++i) {
        watermelon_audio::StreamInfo info{};
        info.sampleRate = (i % 2 == 0) ? kRequestedRate : kNegotiatedRate;
        info.channelCount = 2;
        mEngine->onStreamConfigChanged(info);
    }

    stopRender.store(true, std::memory_order_relaxed);
    audio.join();
    mEngine->stop();
}
