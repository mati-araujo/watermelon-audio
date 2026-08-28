/**
 * test_current_sample_rate.cpp
 *
 * AudioEngine::currentSampleRate() — the resolution order every call site now
 * shares: running stream → offline render rate → 48000, never <= 0.
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
 */

#include "support/BackendPathFixture.h"

#include <gtest/gtest.h>

namespace wma_test {
namespace {

using CurrentSampleRateTest = BackendPathFixture;

// Un render offline chico: lo unico que importa de estos parametros es que
// `startOffline()` los acepte, porque es el unico escritor del rung del medio.
constexpr int kOfflineBlockFrames = 512;

TEST_F(CurrentSampleRateTest, FallsBackTo48000WhenNothingIsConfigured) {
    // Fresh engine: no stream running, no offline render. The documented floor
    // is 48000 — the value the old code would have reported as 0.
    EXPECT_EQ(mEngine->currentSampleRate(), 48000);
}

TEST_F(CurrentSampleRateTest, UsesTheOfflineRenderRateWhenNoStreamIsRunning) {
    // El rung del medio, por su unico escritor de produccion. Sin backend al que
    // preguntarle, esto es lo unico que sabe a que rate corre el motor: un render
    // offline a 44,1 kHz tiene que reportar 44,1, no el piso.
    ASSERT_TRUE(mEngine->startOffline(44100, kOfflineBlockFrames));

    EXPECT_EQ(mEngine->currentSampleRate(), 44100);
}

TEST_F(CurrentSampleRateTest, PrefersNegotiatedBackendRateOverTheOfflineRate) {
    // The scenario that desynchronised SoundFont playback: something prepared
    // at one rate while the device settles on another ends up detuned.
    //
    // El rung del medio va en un valor DISTINTO del piso a proposito: con 48000
    // ahi, este test no distinguiria "gano el stream" de "cayo al piso".
    ASSERT_TRUE(mEngine->startOffline(96000, kOfflineBlockFrames));
    runBackendAt(44100);

    EXPECT_EQ(mEngine->currentSampleRate(), 44100);
}

TEST_F(CurrentSampleRateTest, IgnoresBackendRateUntilTheBackendIsActuallyRunning) {
    // Selected but never started: getStreamInfo() would happily report the
    // backend's default, so the running check is what keeps a stale rate out.
    mBackend->setNegotiatedSampleRate(96000);
    ASSERT_TRUE(mManager->selectBackend(watermelon_audio::BackendType::OBOE));
    ASSERT_FALSE(mManager->isRunning());

    EXPECT_EQ(mEngine->currentSampleRate(), 48000);
    EXPECT_NE(mEngine->currentSampleRate(), 96000);
}

TEST_F(CurrentSampleRateTest, ReturnsToTheFloorAfterTheBackendStops) {
    runBackendAt(96000);
    ASSERT_EQ(mEngine->currentSampleRate(), 96000);

    mManager->stop();

    // Sin stream y sin render offline queda el piso — y sobre todo NO queda
    // pegado el ultimo rate negociado, que es lo que este test vigila.
    EXPECT_EQ(mEngine->currentSampleRate(), 48000);
}

TEST_F(CurrentSampleRateTest, FollowsTheBackendAcrossARenegotiation) {
    runBackendAt(44100);
    ASSERT_EQ(mEngine->currentSampleRate(), 44100);

    // A hot-plugged device can come back at a different rate without the
    // engine restarting; the answer must track the backend, not a snapshot.
    mBackend->setNegotiatedSampleRate(96000);

    EXPECT_EQ(mEngine->currentSampleRate(), 96000);
}

TEST_F(CurrentSampleRateTest, FallsBackWhenARunningBackendReportsANonPositiveRate) {
    // A backend can be running and still have nothing sensible to report
    // (mid-reconfiguration, or a descriptor that never yielded a rate). El rung
    // del medio va cargado para que la caida sea observable y no se confunda con
    // el piso.
    ASSERT_TRUE(mEngine->startOffline(44100, kOfflineBlockFrames));
    runBackendAt(0);

    EXPECT_EQ(mEngine->currentSampleRate(), 44100);
}

TEST_F(CurrentSampleRateTest, NeverReturnsANonPositiveRate) {
    // Every combination of junk inputs still yields something usable, because
    // callers divide by this value and convert milliseconds with it.
    //
    // 🔴 El eje de basura del rung del medio DESAPARECIO con MINI-007, y no es
    // un recorte de cobertura: `startOffline()` RECHAZA un rate <= 0 (lo afirma
    // el bloque de abajo), asi que ya no existe un camino que meta basura ahi.
    // El `> 0` que guarda ese rung se queda igual, como defensa en profundidad.
    const int junkNegotiatedRates[] = {0, -1, -44100};

    for (int junk : {0, -1, -48000}) {
        EXPECT_FALSE(mEngine->startOffline(junk, kOfflineBlockFrames))
            << "startOffline deberia rechazar el rate " << junk;
        EXPECT_GT(mEngine->currentSampleRate(), 0)
            << "tras rechazar " << junk;
    }

    runBackendAt(48000);
    for (int negotiated : junkNegotiatedRates) {
        mBackend->setNegotiatedSampleRate(negotiated);
        EXPECT_GT(mEngine->currentSampleRate(), 0)
            << "negotiated=" << negotiated;
    }
}

TEST_F(CurrentSampleRateTest, ResolvesOnTheLegacyPathToo) {
    // With BackendManager disabled there is no stream at all off Android, so
    // this exercises the same fallback chain with the backend branch skipped.
    // It is the shape the legacy Oboe path degrades to before a stream opens.
    mEngine->setUseBackendManager(false);
    ASSERT_FALSE(mEngine->isUsingBackendManager());

    EXPECT_EQ(mEngine->currentSampleRate(), 48000);

    ASSERT_TRUE(mEngine->startOffline(88200, kOfflineBlockFrames));
    EXPECT_EQ(mEngine->currentSampleRate(), 88200);
}

TEST_F(CurrentSampleRateTest, IgnoresARunningBackendWhenTheBackendPathIsDisabled) {
    // getStreamInfo() consults the manager only when the engine is on the
    // backend path; a running backend must not leak into the legacy answer.
    runBackendAt(96000);
    mEngine->setUseBackendManager(false);

    EXPECT_EQ(mEngine->currentSampleRate(), 48000);
    EXPECT_NE(mEngine->currentSampleRate(), 96000);
}

}  // namespace
}  // namespace wma_test
