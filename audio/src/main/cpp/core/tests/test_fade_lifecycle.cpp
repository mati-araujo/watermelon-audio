/**
 * test_fade_lifecycle.cpp
 *
 * stopWithFade / pauseWithFade / resumeWithFade on the BackendManager path.
 *
 * All three used to branch on `if (mStream)`, and mStream is permanently null
 * once the engine runs through BackendManager. stopWithFade therefore fell
 * through to a bare stop() — the audio was cut dead, an audible click — while
 * pause and resume snapped the volume instead of ramping it. Only the legacy
 * Oboe path ever got a fade.
 *
 * The fade ramp is advanced by the render callback (applyEffectsAndLooper
 * pulls one block off FadeController), so these tests start the engine over a
 * fake backend and drive onAudioReady() by hand. That makes the assertions
 * about the *shape* of the ramp, not just about a flag having been set —
 * including the one that proves the ramp length comes from the rate the device
 * negotiated rather than the rate the app asked for.
 */

#include "tests/support/TestWait.h"
#include "support/BackendPathFixture.h"

#include <algorithm>
#include <cmath>

#include <gtest/gtest.h>

namespace wma_test {
namespace {

using FadeLifecycleTest = BackendPathFixture;

constexpr int kBlockFrames = 256;
constexpr int kBlocks = 4;
constexpr int kRenderedFrames = kBlockFrames * kBlocks;

// Engine states, mirroring EngineState in AudioEngine.h.
constexpr int kStateStopped = 0;
constexpr int kStateRunning = 2;

/// Bring the engine to full volume so a following fade-out starts from 1.0.
void settleAtFullVolume(AudioEngine& engine) {
    // start() arms a 0 → 1 ramp with zero length; one block completes it.
    std::vector<float> buffer(kBlockFrames * 2, 0.0f);
    engine.onAudioReady(buffer.data(), nullptr, kBlockFrames);
}

// ===========================================================================
// stopWithFade
// ===========================================================================

TEST_F(FadeLifecycleTest, StopWithFadeRampsDownInsteadOfCuttingTheAudio) {
    constexpr int kFadeMs = 100;
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);
    ASSERT_FLOAT_EQ(mEngine->getCurrentFadeVolume(), 1.0f);

    mEngine->stopWithFade(kFadeMs);

    // Before the fix this path armed no ramp at all: it called stop() straight
    // away, so there was nothing to fade and the volume jumped to zero.
    EXPECT_TRUE(mEngine->getIsFading());
    EXPECT_FLOAT_EQ(mEngine->getTargetFadeVolume(), 0.0f);
    EXPECT_FLOAT_EQ(mEngine->getCurrentFadeVolume(), 1.0f);

    render(kBlocks, kBlockFrames);

    // 1024 of the 4800 frames the ramp spans at 48 kHz.
    const float expected = 1.0f - static_cast<float>(kRenderedFrames) / 4800.0f;
    EXPECT_NEAR(mEngine->getCurrentFadeVolume(), expected, 0.01f);

    awaitDetachedStop(kFadeMs);
}

TEST_F(FadeLifecycleTest, StopWithFadeSpansTheNegotiatedRateNotThe48000Floor) {
    constexpr int kFadeMs = 100;
    // The device settles at half the rate the engine assumes by default. A ramp
    // measured against 48000 would last twice as many frames as it should, so the
    // audio would still be clearly audible when the stream is torn down.
    //
    // MINI-007: la alternativa equivocada la plantaba `setPreferredSampleRate(48000)`.
    // Borrado el setter, el 48000 sigue siendo la alternativa equivocada —es el
    // piso de `currentSampleRate()`— asi que el test discrimina igual sin el.
    startEngineAt(24000);
    settleAtFullVolume(*mEngine);
    ASSERT_FLOAT_EQ(mEngine->getCurrentFadeVolume(), 1.0f);

    mEngine->stopWithFade(kFadeMs);
    render(kBlocks, kBlockFrames);

    const float expectedAtNegotiatedRate = 1.0f - static_cast<float>(kRenderedFrames) / 2400.0f;
    const float expectedAtPreferredRate = 1.0f - static_cast<float>(kRenderedFrames) / 4800.0f;
    EXPECT_NEAR(mEngine->getCurrentFadeVolume(), expectedAtNegotiatedRate, 0.01f);
    EXPECT_GT(std::abs(expectedAtNegotiatedRate - expectedAtPreferredRate), 0.1f)
        << "the two rates must give visibly different ramps for this test to mean anything";

    awaitDetachedStop(kFadeMs);
}

TEST_F(FadeLifecycleTest, StopWithFadeStopsImmediatelyWhenNoFadeTimeIsGiven) {
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);
    ASSERT_EQ(mEngine->getEngineState(), kStateRunning);

    mEngine->stopWithFade(0);

    // No ramp, no detached thread — a synchronous stop, which is what a caller
    // asking for a zero-length fade means.
    EXPECT_EQ(mEngine->getEngineState(), kStateStopped);
    EXPECT_FALSE(mEngine->getIsFading());
}

TEST_F(FadeLifecycleTest, StopWithFadeEventuallyStopsTheEngine) {
    constexpr int kFadeMs = 20;
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);
    ASSERT_EQ(mEngine->getEngineState(), kStateRunning);

    mEngine->stopWithFade(kFadeMs);
    ASSERT_EQ(mEngine->getEngineState(), kStateRunning) << "the stop must trail the fade";

    EXPECT_TRUE(awaitEngineStopped(kFadeMs))
        << "el stop diferido nunca llego a parar el motor";

    EXPECT_EQ(mEngine->getEngineState(), kStateStopped);
}

// ---------------------------------------------------------------------------
// REQ-050 S2 (D8): un stop durante el fade NO reinicia la rampa.
//
// MINI-041 #4, medido en el g42: un segundo stopWithFade() con el fade en curso
// hacía `cancel()` + `startFade(1.0 -> 0.0)`: el audio volvía a pleno volumen y el
// motor tardaba un fade entero más en parar.
// ---------------------------------------------------------------------------

// Fade de 1 s y bloques largos, a proposito: las aserciones tienen que terminar antes
// de que el worker dispare (fade + 50 ms de PARED). Con 100 ms quedaban 150 ms para dos
// renders y media docena de aserciones, y bajo TSan con ctest en paralelo eso no esta
// garantizado (review de S2; es la clase de REQ-002). Con 1 s el margen es de un orden
// de magnitud, y la pendiente se sigue distinguiendo.
constexpr int kLongFadeMs = 1000;                 // 48000 frames a 48 kHz
constexpr int kLongBlocks = 40;                   // 40 x 256 = 10240 frames por tramo
constexpr int kLongTramo = kBlockFrames * kLongBlocks;

TEST_F(FadeLifecycleTest, AC050_6_ASecondStopDuringTheFadeNeitherRaisesTheVolumeNorRestartsTheRamp) {
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);

    mEngine->stopWithFade(kLongFadeMs);
    render(kLongBlocks, kBlockFrames);
    const float midFade = mEngine->getCurrentFadeVolume();
    ASSERT_NEAR(midFade, 1.0f - static_cast<float>(kLongTramo) / 48000.0f, 0.01f);

    mEngine->stopWithFade(kLongFadeMs);

    // Bug que atrapa (a): reiniciar desde 1.0, que es el salto audible que se midio.
    EXPECT_FLOAT_EQ(mEngine->getCurrentFadeVolume(), midFade)
        << "el segundo stop volvio a subir el volumen";
    EXPECT_FLOAT_EQ(mEngine->getTargetFadeVolume(), 0.0f);

    // Bug que atrapa (b): re-armar una rampa NUEVA desde el volumen actual. No sube el
    // volumen, pero estira la parada un fade entero: la pendiente sale de la rampa
    // ORIGINAL (20480 de 48000 frames -> 0.573), no de una nueva que arranca en midFade
    // (midFade * (1 - 10240/48000) = 0.619).
    render(kLongBlocks, kBlockFrames);
    EXPECT_NEAR(mEngine->getCurrentFadeVolume(),
                1.0f - static_cast<float>(2 * kLongTramo) / 48000.0f, 0.01f)
        << "el segundo stop re-armo la rampa en vez de dejar seguir la que estaba";

    EXPECT_TRUE(awaitEngineStopped(kLongFadeMs)) << "el stop del primer fade nunca llego";
}

TEST_F(FadeLifecycleTest, AC050_6_ACutDuringTheFadeStopsRightAway) {
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);

    mEngine->stopWithFade(kLongFadeMs);
    render(kLongBlocks, kBlockFrames);
    ASSERT_EQ(mEngine->getEngineState(), kStateRunning);

    // Un corte (fade 0) no "reinicia" nada: pide parar YA, y nunca mas fuerte.
    // Bug que atrapa: tratar el corte como un stop mas durante el fade e ignorarlo.
    mEngine->stopWithFade(0);
    EXPECT_EQ(mEngine->getEngineState(), kStateStopped);
}

TEST_F(FadeLifecycleTest, AC050_6_AStopOnAStoppedEngineDoesNotStopTheNextStart) {
    constexpr int kFadeMs = 20;
    startEngineAt(48000);
    mEngine->stopWithFade(0);
    ASSERT_EQ(mEngine->getEngineState(), kStateStopped);

    // Un segundo stop con el motor ya parado (lo que hace un stop serializado detras de
    // otro, D11) dejaba un worker que paraba el motor que se arrancara despues.
    mEngine->stopWithFade(kFadeMs);
    // Sobre un motor parado no hay nada que rampear: no se arma fade ni worker. (start()
    // recoge un worker huerfano igual, asi que sin esta asercion el caso 1 de
    // stopWithFade quedaba sin test: lo mostro su mutante, que sobrevivia.)
    EXPECT_FALSE(mEngine->getIsFading()) << "un stop sobre un motor parado armo una rampa";
    ASSERT_TRUE(mEngine->start(0));

    // AUSENCIA: no hay condicion que esperar, solo la ventana en la que el worker
    // espurio habria disparado (fade + 50 ms + un chunk de 10 ms).
    wma_test::sleepFixed(std::chrono::milliseconds(kFadeMs + 250));
    EXPECT_EQ(mEngine->getEngineState(), kStateRunning)
        << "un stop sobre un motor parado dejo programada la parada del arranque siguiente";
    mEngine->stopWithFade(0);
}

// Review de S2, hallazgo 1: un stop() DIRECTO (wma_engine_stop con -1, el de stopEngine)
// durante el fade dejaba vivo el worker de stopWithFade con el flag de "parada en curso".
// El arranque siguiente lo paraba ese worker, y un stopWithFade sobre ese arranque no
// armaba rampa (lo creia "en curso"): corte seco al disparar el worker viejo.
TEST_F(FadeLifecycleTest, AC050_6_AStopFadeWorkerOrphanedByADirectStopDoesNotStopTheNextStart) {
    constexpr int kFadeMs = 100;
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);

    mEngine->stopWithFade(kFadeMs);
    mEngine->stop();
    ASSERT_EQ(mEngine->getEngineState(), kStateStopped);

    ASSERT_TRUE(mEngine->start(0));
    settleAtFullVolume(*mEngine);

    // El stop con fade sobre el arranque nuevo tiene que armar SU rampa.
    mEngine->stopWithFade(kLongFadeMs);
    EXPECT_TRUE(mEngine->getIsFading()) << "el stop creyo que habia una parada en curso";
    EXPECT_FLOAT_EQ(mEngine->getTargetFadeVolume(), 0.0f);

    // AUSENCIA: la ventana del worker huerfano (fade + 50 ms + chunk), muy por debajo
    // del worker nuevo (1 s + 50 ms).
    wma_test::sleepFixed(std::chrono::milliseconds(kFadeMs + 250));
    EXPECT_EQ(mEngine->getEngineState(), kStateRunning)
        << "el worker del primer stopWithFade paro el arranque siguiente";

    mEngine->stopWithFade(0);
}

// ===========================================================================
// pauseWithFade / resumeWithFade
// ===========================================================================

TEST_F(FadeLifecycleTest, PauseWithFadeRampsDownBeforeItPauses) {
    constexpr int kFadeMs = 100;
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);

    mEngine->pauseWithFade(kFadeMs);

    // The old backend branch set paused=true on the spot; the pause is supposed
    // to land only once the ramp has run.
    EXPECT_FALSE(mEngine->getIsPaused());
    EXPECT_TRUE(mEngine->getIsFading());
    EXPECT_FLOAT_EQ(mEngine->getTargetFadeVolume(), 0.0f);
    EXPECT_FLOAT_EQ(mEngine->getCurrentFadeVolume(), 1.0f);

    render(kBlocks, kBlockFrames);
    const float expected = 1.0f - static_cast<float>(kRenderedFrames) / 4800.0f;
    EXPECT_NEAR(mEngine->getCurrentFadeVolume(), expected, 0.01f);
}

TEST_F(FadeLifecycleTest, PauseWithFadeLandsOnPausedAfterTheRamp) {
    constexpr int kFadeMs = 30;
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);

    mEngine->pauseWithFade(kFadeMs);
    ASSERT_FALSE(mEngine->getIsPaused());

    // PRESENCIA: se espera a que la pausa SE ARME, no a que pase un rato. El
    // margen viejo (`kFadeMs + 400`) era para el scheduling, y un margen para el
    // scheduling es exactamente lo que se queda corto en un runner cargado.
    EXPECT_TRUE(wma_test::waitUntil([&]() -> bool { return mEngine->getIsPaused(); }))
        << "el fade de pausa no llego a armarse";
}

TEST_F(FadeLifecycleTest, PauseWithFadePausesImmediatelyWhenNoFadeTimeIsGiven) {
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);

    mEngine->pauseWithFade(0);

    EXPECT_TRUE(mEngine->getIsPaused());
    EXPECT_FALSE(mEngine->getIsFading());
}

TEST_F(FadeLifecycleTest, ResumeWithFadeRampsUpFromSilence) {
    constexpr int kFadeMs = 100;
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);
    mEngine->pauseWithFade(0);
    ASSERT_TRUE(mEngine->getIsPaused());

    mEngine->resumeWithFade(kFadeMs);

    // The old backend branch cleared the pause and jumped straight back to 1.0.
    EXPECT_FALSE(mEngine->getIsPaused());
    EXPECT_TRUE(mEngine->getIsFading());
    EXPECT_FLOAT_EQ(mEngine->getCurrentFadeVolume(), 0.0f);
    EXPECT_FLOAT_EQ(mEngine->getTargetFadeVolume(), 1.0f);

    render(kBlocks, kBlockFrames);
    const float expected = static_cast<float>(kRenderedFrames) / 4800.0f;
    EXPECT_NEAR(mEngine->getCurrentFadeVolume(), expected, 0.01f);
}

TEST_F(FadeLifecycleTest, ResumeWithFadeSpansTheNegotiatedRateNotThe48000Floor) {
    constexpr int kFadeMs = 100;
    startEngineAt(24000);
    settleAtFullVolume(*mEngine);
    mEngine->pauseWithFade(0);

    mEngine->resumeWithFade(kFadeMs);
    render(kBlocks, kBlockFrames);

    const float expectedAtNegotiatedRate = static_cast<float>(kRenderedFrames) / 2400.0f;
    EXPECT_NEAR(mEngine->getCurrentFadeVolume(), expectedAtNegotiatedRate, 0.01f);
}

TEST_F(FadeLifecycleTest, ResumeWithFadeRestoresFullVolumeWhenNoFadeTimeIsGiven) {
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);
    mEngine->pauseWithFade(0);
    ASSERT_TRUE(mEngine->getIsPaused());

    mEngine->resumeWithFade(0);

    // Zero-length resume must land on 1.0, not on 0.0 — a resume that leaves
    // the fade volume at zero is silence the user cannot get out of.
    EXPECT_FALSE(mEngine->getIsPaused());
    EXPECT_FALSE(mEngine->getIsFading());
    EXPECT_FLOAT_EQ(mEngine->getCurrentFadeVolume(), 1.0f);
    EXPECT_FLOAT_EQ(mEngine->getTargetFadeVolume(), 1.0f);
}

TEST_F(FadeLifecycleTest, ResumeWithFadeCancelsAPendingPause) {
    constexpr int kPauseFadeMs = 30;
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);

    mEngine->pauseWithFade(kPauseFadeMs);
    mEngine->resumeWithFade(kPauseFadeMs);

    // AUSENCIA, y por eso ESTA espera se queda. El timer de pausa o dispara o no
    // dispara; una no-ocurrencia no se puede esperar por condicion, solo se le
    // puede dar la ventana en la que HABRIA ocurrido. Que esa ventana alcanza lo
    // demuestra su test hermano —`PauseWithFadeEventuallyPauses`, que espera a que
    // el MISMO timer dispare— y por eso el numero no es arbitrario.
    //
    // Va por `sleepFixed` para que el instrumento de REQ-002 la vea en vez de que
    // quede escondida en un `sleep_for` crudo.
    wma_test::sleepFixed(std::chrono::milliseconds(kPauseFadeMs + 400));

    EXPECT_FALSE(mEngine->getIsPaused());
}

// ===========================================================================
// Fade output, not just fade state
// ===========================================================================

TEST_F(FadeLifecycleTest, PauseSilencesTheRenderedBlock) {
    startEngineAt(48000);
    settleAtFullVolume(*mEngine);
    render(4, kBlockFrames);

    // A paused engine renders silence wherever the ramp happened to stop:
    // applyEffectsAndLooper pins both ends of the gain ramp to zero.
    mEngine->pauseWithFade(0);
    ASSERT_TRUE(mEngine->getIsPaused());

    // Not bit-exact zero, and legitimately so — the output stage's DC blocker
    // is a filter with memory, so it decays from the pre-pause signal rather
    // than snapping. One block of settling puts the tail well below -60 dBFS.
    render(1, kBlockFrames);

    std::vector<float> buffer(kBlockFrames * 2, 1.0f);
    mEngine->onAudioReady(buffer.data(), nullptr, kBlockFrames);

    float peak = 0.0f;
    for (float sample : buffer) {
        peak = std::max(peak, std::abs(sample));
    }
    EXPECT_LT(peak, 1.0e-3f);
}

}  // namespace
}  // namespace wma_test
