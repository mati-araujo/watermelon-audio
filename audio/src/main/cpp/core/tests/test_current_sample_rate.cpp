/**
 * test_current_sample_rate.cpp
 *
 * AudioEngine::currentSampleRate() — the resolution order every call site now
 * shares: el rate del stream que el motor abrio → offline render rate → 48000,
 * never <= 0.
 *
 * What made this worth a suite: the call sites it replaced all read
 * `mStream ? mStream->getSampleRate() : 0`, and on the BackendManager path
 * mStream is permanently null. Each site then patched the resulting 0 its own
 * way, or did not patch it at all. The tests below pin the single answer.
 *
 * 🔴 MINI-007 CAMBIO EL RUNG DEL MEDIO, no la cadena. Antes era "el rate
 * preferido", que se establecia con `setPreferredSampleRate()` — un setter que
 * NINGUNA superficie publica alcanzaba (cero `wma_*`, cero `JNIEXPORT`), asi que
 * en un device ese rung valia siempre 0. Los tests lo usaban igual, y por eso la
 * suite estaba verde sobre un escalon que produccion no podia pisar.
 *
 * Ahora el rung del medio es el rate del render offline, y su UNICO escritor es
 * `startOffline()` — que SI es produccion (el puerto de REQ-015). Donde antes se
 * plantaba un rate preferido, ahora se planta por ese camino o se afirma contra
 * el piso de 48000; lo que ya no se puede escribir es el estado que produccion
 * no podia alcanzar.
 *
 * 🔴 MINI-033 CAMBIO EL RUNG DE ARRIBA, y con el la FORMA de estos tests.
 * ---------------------------------------------------------------------
 * Antes el rung de arriba era *preguntarle al backend en vivo*: `isRunning()` +
 * `BackendManager::getStreamInfo()`, o sea **dos mutex anidados en el hilo RT de
 * captura** (`captureMonitoringBlock` llama a esta funcion en cada bloque).
 * Ahora el rung de arriba es un atomic del motor (`mStreamSampleRate`) que se
 * escribe donde el motor YA se entera del rate: `start()` en sus dos caminos,
 * `onStreamConfigChanged()`, y `0` en `stop()` / `rollbackFailedStart()`.
 *
 * Consecuencia para los tests: **el estimulo es el motor, no el manager**. Un
 * `mManager->start()` a espaldas del motor ya NO mueve la respuesta, y eso no es
 * una perdida de cobertura — es el costo declarado del diseño (un cambio de rate
 * INTERNO del backend que nadie notifica deja el valor viejo, exactamente igual
 * que le pasa al DSP). El estimulo de un cambio en caliente es
 * `onStreamConfigChanged()`, que es el unico hook que produccion tiene para eso.
 */

#include "support/BackendPathFixture.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "../../nodes/InputNode.h"

// El gancho de WD-1.3: retiene al thread de SALIDA adentro del callback, lo que
// deja `mActiveCallbacks` en 1 y hace que el quiesce de re-configuracion se
// agote. Es el mismo mecanismo que usa `test_rate_cableado.cpp`.
extern std::atomic<bool> gInputNodeHoldInCallback;
extern std::atomic<bool> gInputNodeIsInCallback;

namespace wma_test {
namespace {

using CurrentSampleRateTest = BackendPathFixture;

// Un render offline chico: lo unico que importa de estos parametros es que
// `startOffline()` los acepte, porque es el unico escritor del rung del medio.
constexpr int kOfflineBlockFrames = 512;

/// Deja el rung del medio cargado SIN dejar el motor en modo offline, para que
/// despues se le pueda arrancar un stream encima y observar la precedencia.
/// `stop()` apaga `mOfflineMode` y no toca `mOfflineSampleRate`, que es
/// justamente el estado que hace falta.
void plantOfflineRungAt(AudioEngine& engine, int rate) {
    ASSERT_TRUE(engine.startOffline(rate, kOfflineBlockFrames));
    engine.stop();
}

watermelon_audio::StreamInfo streamInfoAt(int rate) {
    watermelon_audio::StreamInfo info{};
    info.sampleRate = rate;
    info.channelCount = 2;
    return info;
}

TEST_F(CurrentSampleRateTest, FallsBackTo48000WhenNothingIsConfigured) {
    // Fresh engine: no stream running, no offline render. The documented floor
    // is 48000 — the value the old code would have reported as 0.
    EXPECT_EQ(mEngine->currentSampleRate(), 48000);
}

TEST_F(CurrentSampleRateTest, UsesTheOfflineRenderRateWhenNoStreamIsRunning) {
    // El rung del medio, por su unico escritor de produccion. Sin stream abierto,
    // esto es lo unico que sabe a que rate corre el motor: un render offline a
    // 44,1 kHz tiene que reportar 44,1, no el piso.
    ASSERT_TRUE(mEngine->startOffline(44100, kOfflineBlockFrames));

    EXPECT_EQ(mEngine->currentSampleRate(), 44100);
}

/**
 * AC-M033.1 — el rate del stream, despues de `start()`.
 *
 * El escenario que desincronizaba la reproduccion de SoundFonts: algo preparado a
 * un rate mientras el device se asienta en otro suena desafinado.
 *
 * El rung del medio va cargado en un valor DISTINTO a proposito: con 48000 ahi,
 * este test no distinguiria "gano el stream" de "cayo al piso".
 */
TEST_F(CurrentSampleRateTest, PrefersTheStartedStreamRateOverTheOfflineRate) {
    plantOfflineRungAt(*mEngine, 96000);
    startEngineAt(44100);

    EXPECT_EQ(mEngine->currentSampleRate(), 44100)
        << "el rate del stream que start() negocio tiene que ganarle al rung offline";

    mEngine->stop();
}

/**
 * AC-M033.1 — y vuelve al fallback despues de `stop()`.
 *
 * Es el mutante (c) del MINI: no limpiar el atomic en `stop()` deja pegado el
 * ultimo rate negociado sobre un motor sin stream.
 */
TEST_F(CurrentSampleRateTest, ReturnsToTheFloorAfterTheEngineStops) {
    startEngineAt(96000);
    ASSERT_EQ(mEngine->currentSampleRate(), 96000);

    mEngine->stop();

    // Sin stream y sin render offline queda el piso — y sobre todo NO queda
    // pegado el ultimo rate negociado, que es lo que este test vigila.
    EXPECT_EQ(mEngine->currentSampleRate(), 48000);
}

/**
 * AC-M033.1 — un `start()` que no prospera no deja rate publicado.
 *
 * 🔴 ES UN TRINQUETE, y conviene decir de que: HOY el atomic todavia vale 0
 * cuando `rollbackFailedStart()` corre, porque se escribe DESPUES de que
 * `manager.start()` volvio OK. O sea que el limpiado de ese camino es defensa en
 * profundidad y este test no lo mata solo.
 *
 * Lo que SI mata es mover el store hacia arriba — publicar el rate *pedido*
 * antes de que el backend conteste, que es la forma natural de "simplificar" esto
 * mas adelante. Con el store adelantado y sin el limpiado, un motor que quedo en
 * Stopped seguiria contestando el rate de un stream que nunca corrio.
 */
TEST_F(CurrentSampleRateTest, AFailedStartLeavesNoStreamRateBehind) {
    mBackend->setNegotiatedSampleRate(44100);
    mBackend->setStartResult(watermelon_audio::BackendResult::ERROR_DEVICE_NOT_FOUND);
    mEngine->setUseBackendManager(true);
    ASSERT_TRUE(mManager->selectBackend(watermelon_audio::BackendType::OBOE));

    ASSERT_FALSE(mEngine->start(0)) << "premisa: el backend tenia que rechazar el start";

    EXPECT_EQ(mEngine->currentSampleRate(), 48000);
}

/**
 * AC-M033.2 — `onStreamConfigChanged` mueve el rate en caliente.
 *
 * Es el UNICO hook de cambio de rate sin reiniciar el motor (REQ-006.2). Antes
 * de MINI-033 esto lo cubria "cambiarle el rate al fake y volver a preguntar",
 * que dejaba de valer en cuanto la respuesta salio de un atomic: ese estimulo
 * modelaba un cambio que NADIE notifica, que es el costo declarado del diseño.
 */
TEST_F(CurrentSampleRateTest, FollowsAHotStreamConfigChange) {
    startEngineAt(44100);
    ASSERT_EQ(mEngine->currentSampleRate(), 44100);

    mEngine->onStreamConfigChanged(streamInfoAt(96000));

    EXPECT_EQ(mEngine->currentSampleRate(), 96000);

    mEngine->stop();
}

/**
 * AC-M033.2 — un `start()` cuyo quiesce falla publica IGUAL el rate del stream.
 *
 * El control del punto 2 del diseño (R-MOT-1): si el drenaje no se confirma,
 * `configureComponentsWithSampleRate()` **no re-prepara** y el DSP queda al rate
 * viejo — pero lo que el stream corre es `actualRate`, y eso es lo que el flag de
 * mismatch y el largo de los fades necesitan saber. Publicar el rate y
 * re-preparar son dos pasos distintos, y el segundo puede no ocurrir.
 *
 * Se fuerza con dos mecanismos que ya existen: el freno de `FakeAudioBackend`
 * adentro de `start()` (para meter el render en la ventana exacta) y el gancho de
 * WD-1.3 (para que ese render quede retenido adentro del callback, dejando
 * `mActiveCallbacks` en 1 mientras el quiesce agota su techo de 250 ms).
 */
TEST_F(CurrentSampleRateTest, PublishesTheStreamRateEvenWhenTheQuiesceFails) {
    constexpr int kNegotiated = 44100;
    constexpr int kBlockFrames = 256;
    static_assert(kNegotiated != AudioEngine::kPreNegotiationSampleRate,
                  "sin coercion no se entra a la rama del re-configure que este test cubre");

    auto node = std::make_shared<InputNode>();
    node->prepare(48000, kBlockFrames);
    mEngine->setInputNode(node);

    mBackend->setNegotiatedSampleRate(kNegotiated);
    mEngine->setUseBackendManager(true);
    ASSERT_TRUE(mManager->selectBackend(watermelon_audio::BackendType::OBOE));

    gInputNodeHoldInCallback.store(true, std::memory_order_release);
    std::atomic<bool> keepRendering{true};
    std::thread audio;

    // Suelta el gancho pase lo que pase: un thread retenido cuelga el binario
    // entero, incluso si una asercion aborta el cuerpo del test.
    struct Release {
        std::atomic<bool>& keepRendering;
        std::thread& audio;
        AudioEngine& engine;
        ~Release() {
            gInputNodeHoldInCallback.store(false, std::memory_order_release);
            keepRendering.store(false, std::memory_order_release);
            if (audio.joinable()) audio.join();
            engine.stop();
            engine.setInputNode(nullptr);
        }
    } release{keepRendering, audio, *mEngine};

    // 1. Trabar el backend ADENTRO de start(): en ese punto el motor ya paso a
    //    Running (`transitionToState` corre antes que `manager.start()`), asi que
    //    los callbacks hacen trabajo de verdad y el gancho los puede retener.
    mBackend->blockStart();
    std::atomic<bool> startReturned{false};
    std::atomic<bool> startOk{false};
    std::thread starter([&] {
        startOk.store(mEngine->start(0), std::memory_order_release);
        startReturned.store(true, std::memory_order_release);
    });
    mBackend->waitUntilStartEntered();

    // 2. Largar el render, que queda retenido adentro del callback.
    audio = std::thread([&] {
        std::vector<float> in(static_cast<size_t>(kBlockFrames) * 2, 0.25f);
        std::vector<float> out(static_cast<size_t>(kBlockFrames) * 2, 0.0f);
        while (keepRendering.load(std::memory_order_acquire)) {
            mEngine->onAudioReady(out.data(), in.data(), kBlockFrames);
        }
    });
    ASSERT_TRUE(wma_test::waitUntil(
        [] { return gInputNodeIsInCallback.load(std::memory_order_acquire); }))
        << "premisa: el thread de salida nunca quedo retenido adentro del callback, "
           "asi que el quiesce del re-configure NO iba a fallar";

    // 3. Destrabar: `manager.start()` vuelve, se lee 44100 != 48000 y se entra al
    //    re-configure — cuyo quiesce se va a agotar con el callback adentro.
    mBackend->releaseStart();
    starter.join();
    ASSERT_TRUE(wma_test::waitUntil(
        [&] { return startReturned.load(std::memory_order_acquire); }));
    ASSERT_TRUE(startOk.load(std::memory_order_acquire))
        << "un quiesce fallido no puede hacer fallar el start: solo deja el DSP al rate viejo";

    EXPECT_EQ(mEngine->currentSampleRate(), kNegotiated)
        << "el rate del STREAM se publica aunque el re-configure no haya podido correr";
}

TEST_F(CurrentSampleRateTest, DoesNotConsultABackendTheEngineNeverStarted) {
    // Un manager corriendo a espaldas del motor. Antes esto ERA el rung de
    // arriba; ahora la respuesta sale del atomic del motor, que nadie escribio.
    // El test sigue puesto porque afirma justamente eso: `currentSampleRate()` no
    // le pregunta al backend (que es lo que le costaba dos mutex en el hilo RT).
    runBackendAt(96000);

    EXPECT_EQ(mEngine->currentSampleRate(), 48000);
    EXPECT_NE(mEngine->currentSampleRate(), 96000);
}

TEST_F(CurrentSampleRateTest, FallsBackWhenTheNegotiatedRateIsNonPositive) {
    // A backend can start and still have nothing sensible to report
    // (mid-reconfiguration, or a descriptor that never yielded a rate). El rung
    // del medio va cargado para que la caida sea observable y no se confunda con
    // el piso.
    plantOfflineRungAt(*mEngine, 44100);
    startEngineAt(0);

    EXPECT_EQ(mEngine->currentSampleRate(), 44100);

    mEngine->stop();
}

TEST_F(CurrentSampleRateTest, NeverReturnsANonPositiveRate) {
    // Every combination of junk inputs still yields something usable, because
    // callers divide by this value and convert milliseconds with it.
    //
    // 🔴 El eje de basura del rung del medio DESAPARECIO con MINI-007, y no es
    // un recorte de cobertura: `startOffline()` RECHAZA un rate <= 0 (lo afirma
    // el bloque de abajo), asi que ya no existe un camino que meta basura ahi.
    // El `> 0` que guarda ese rung se queda igual, como defensa en profundidad.
    for (int junk : {0, -1, -48000}) {
        EXPECT_FALSE(mEngine->startOffline(junk, kOfflineBlockFrames))
            << "startOffline deberia rechazar el rate " << junk;
        EXPECT_GT(mEngine->currentSampleRate(), 0)
            << "tras rechazar " << junk;
    }

    // El eje de basura del rung DE ARRIBA entra por donde produccion lo entrega:
    // el hook de cambio de config. Un `sampleRate` no positivo ahi no puede
    // pisar lo que ya se sabia ni hundir la respuesta.
    startEngineAt(48000);
    for (int junk : {0, -1, -44100}) {
        mEngine->onStreamConfigChanged(streamInfoAt(junk));
        EXPECT_EQ(mEngine->currentSampleRate(), 48000)
            << "onStreamConfigChanged(" << junk << ") piso un rate que era valido";
    }

    mEngine->stop();
}

TEST_F(CurrentSampleRateTest, ResolvesOnTheLegacyPathToo) {
    // With BackendManager disabled there is no stream at all off Android, so
    // this exercises the same fallback chain with nothing publishing the top
    // rung. It is the shape the legacy Oboe path degrades to before a stream
    // opens.
    mEngine->setUseBackendManager(false);
    ASSERT_FALSE(mEngine->isUsingBackendManager());

    EXPECT_EQ(mEngine->currentSampleRate(), 48000);

    ASSERT_TRUE(mEngine->startOffline(88200, kOfflineBlockFrames));
    EXPECT_EQ(mEngine->currentSampleRate(), 88200);
}

TEST_F(CurrentSampleRateTest, IgnoresARunningBackendWhenTheBackendPathIsDisabled) {
    // Un backend corriendo no se puede colar en la respuesta de un motor que no
    // esta en ese camino. `setUseBackendManager()` es no-op con el motor
    // corriendo, asi que el orden importa: la bandera se baja con el motor
    // parado, que es el unico momento en que produccion la toca.
    runBackendAt(96000);
    mEngine->setUseBackendManager(false);
    ASSERT_FALSE(mEngine->isUsingBackendManager());

    EXPECT_EQ(mEngine->currentSampleRate(), 48000);
    EXPECT_NE(mEngine->currentSampleRate(), 96000);
}

}  // namespace
}  // namespace wma_test
