/**
 * test_capture_requests.cpp
 *
 * BackendManager::requestCapture() — who is allowed to turn the microphone on,
 * who is allowed to turn it off, and who is allowed to reopen the stream.
 *
 * Why this suite exists: two independent callers ask for captured input — the
 * mode system (INPUT_FX needs it) and an explicit wma_input_start(). Before
 * this, both wrote ONE bool. Last writer won, so leaving INPUT_FX would kill a
 * capture the app had started on purpose. This repo has already been bitten
 * twice by exactly that shape (the duplicated InputNode, the duplicated mode
 * state), so the OR of the two requesters is pinned here rather than trusted.
 *
 * The second thing pinned here is the reopen asymmetry. Every backend decides
 * on capture when it opens a stream (OboeBackend.cpp:63; CoreAudio attaches its
 * sink node at open), so a running stream cannot grow a capture path without
 * being reopened — which is audible. A mode change must never do that; an
 * explicit input-start may.
 */

#include "support/BackendPathFixture.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace wma_test {
namespace {

using Requester = watermelon_audio::BackendManager::CaptureRequester;
using Outcome   = watermelon_audio::BackendManager::CaptureOutcome;

class CaptureRequestTest : public BackendPathFixture {
protected:
    /**
     * Pedir captura y esperar a que el reopen termine.
     *
     * El reopen dejó de correr en el thread del llamador (bloquearlo era un
     * freeze del main thread en iOS), así que un test que afirme sobre el
     * resultado tiene que esperarlo. Lo que NO se esconde acá es el valor
     * devuelto: los tests de más abajo afirman que es PENDING y no LIVE.
     */
    Outcome requestCaptureAndSettle(Requester who, bool want, bool allowRestart) {
        const Outcome outcome = mManager->requestCapture(who, want, allowRestart);
        mManager->waitForCaptureRequest();
        return outcome;
    }

    /// A running stream with no capture path, which is where every case starts.
    void runWithoutCapture() {
        ASSERT_TRUE(mManager->selectBackend(watermelon_audio::BackendType::OBOE));
        mManager->setCallback(mEngine.get());
        ASSERT_EQ(mManager->start(), watermelon_audio::BackendResult::OK);
        ASSERT_TRUE(mManager->isRunning());
        ASSERT_FALSE(mManager->isCaptureLive());
    }
};

// --- The two requesters must not overwrite each other ----------------------

TEST_F(CaptureRequestTest, LeavingInputFxDoesNotKillAnExplicitlyStartedCapture) {
    // The regression this suite was written for. The app starts the microphone
    // on purpose, the mode system independently turns INPUT_FX off, and with a
    // single shared bool the capture died with it.
    runWithoutCapture();
    ASSERT_EQ(requestCaptureAndSettle(Requester::INPUT_NODE, true, true), Outcome::PENDING);
    ASSERT_TRUE(mManager->isCaptureLive());

    mManager->setFullDuplexEnabled(false);  // the MODE requester withdrawing

    EXPECT_TRUE(mManager->isCaptureLive());
    EXPECT_TRUE(mBackend->fullDuplexRequested());
}

TEST_F(CaptureRequestTest, StoppingInputDoesNotKillCaptureTheModeStillNeeds) {
    // The mirror image: INPUT_FX is on, the app stops its own input, and the
    // mode's need for capture has to survive.
    runWithoutCapture();
    mManager->setFullDuplexEnabled(true);
    requestCaptureAndSettle(Requester::INPUT_NODE, true, true);
    ASSERT_TRUE(mManager->isCaptureLive());

    mManager->requestCapture(Requester::INPUT_NODE, false, false);

    EXPECT_TRUE(mBackend->fullDuplexRequested())
        << "the mode still wants capture; withdrawing the other requester must "
           "not clear the request";
}

TEST_F(CaptureRequestTest, CaptureRequestClearsOnlyWhenBothRequestersAreDone) {
    runWithoutCapture();
    mManager->setFullDuplexEnabled(true);
    mManager->requestCapture(Requester::INPUT_NODE, true, false);
    ASSERT_TRUE(mBackend->fullDuplexRequested());

    mManager->setFullDuplexEnabled(false);
    ASSERT_TRUE(mBackend->fullDuplexRequested());

    mManager->requestCapture(Requester::INPUT_NODE, false, false);

    EXPECT_FALSE(mBackend->fullDuplexRequested());
}

// --- Who may reopen a running stream ---------------------------------------

TEST_F(CaptureRequestTest, AModeChangeNeverReopensARunningStream) {
    // A mode change must not punch an audible gap into playback. The request is
    // recorded for the next start(); the stream keeps running untouched.
    runWithoutCapture();
    const int startsBefore = mBackend->startCount();

    mManager->setFullDuplexEnabled(true);

    EXPECT_EQ(mBackend->startCount(), startsBefore) << "the stream was reopened";
    EXPECT_FALSE(mManager->isCaptureLive());
    EXPECT_TRUE(mBackend->fullDuplexRequested())
        << "the request must survive for the next start()";
}

TEST_F(CaptureRequestTest, AnExplicitInputStartReopensAndCapturesForReal) {
    runWithoutCapture();
    const int startsBefore = mBackend->startCount();

    EXPECT_EQ(requestCaptureAndSettle(Requester::INPUT_NODE, true, true), Outcome::PENDING);

    EXPECT_EQ(mBackend->startCount(), startsBefore + 1);
    EXPECT_TRUE(mManager->isCaptureLive());
}

TEST_F(CaptureRequestTest, WithdrawingCaptureNeverReopensTheStream) {
    // Turning capture off is not worth a gap: the backend simply stops handing
    // the frames over.
    runWithoutCapture();
    requestCaptureAndSettle(Requester::INPUT_NODE, true, true);
    const int startsBefore = mBackend->startCount();

    mManager->requestCapture(Requester::INPUT_NODE, false, true);

    EXPECT_EQ(mBackend->startCount(), startsBefore);
}

TEST_F(CaptureRequestTest, RequestingCaptureTwiceReopensOnlyOnce) {
    runWithoutCapture();
    requestCaptureAndSettle(Requester::INPUT_NODE, true, true);
    const int startsAfterFirst = mBackend->startCount();

    EXPECT_EQ(requestCaptureAndSettle(Requester::INPUT_NODE, true, true), Outcome::LIVE);

    EXPECT_EQ(mBackend->startCount(), startsAfterFirst)
        << "capture was already live; there was nothing to reopen for";
}

// --- Requested is not the same as granted ----------------------------------

TEST_F(CaptureRequestTest, ADeniedMicrophoneIsReportedAsFalseNotAsSuccess) {
    // The user denied microphone access. The reopen happens, the stream comes
    // back, and capture is still not live — which the caller has to be told,
    // because "requested" and "granted" are different facts.
    runWithoutCapture();
    mBackend->setCaptureAvailable(false);

    EXPECT_EQ(requestCaptureAndSettle(Requester::INPUT_NODE, true, true), Outcome::PENDING);

    EXPECT_FALSE(mManager->isCaptureLive());
    EXPECT_TRUE(mManager->isRunning()) << "output must survive a denied microphone";
}

TEST_F(CaptureRequestTest, CaptureIsNotLiveBeforeTheStreamRuns) {
    ASSERT_TRUE(mManager->selectBackend(watermelon_audio::BackendType::OBOE));
    mManager->setFullDuplexEnabled(true);

    EXPECT_FALSE(mManager->isCaptureLive())
        << "a request on a stopped stream is not a live capture";
}

// --- Failure leaves the user with audio, not silence -----------------------

TEST_F(CaptureRequestTest, AFailedReopenFallsBackToStreamingWithoutCapture) {
    // The worst case: the reopen with capture fails. Giving up would leave the
    // app silent, so the manager drops the capture request and reopens plain.
    runWithoutCapture();
    mBackend->setStartResult(watermelon_audio::BackendResult::ERROR_STREAM_FAILED);

    requestCaptureAndSettle(Requester::INPUT_NODE, true, true);

    EXPECT_FALSE(mBackend->fullDuplexRequested())
        << "the request that could not be honored must be dropped, or the next "
           "reopen would fail the same way";
}

// --- El reopen no corre en el thread del llamador ---------------------------
//
// Estos cuatro existen porque el reopen sincrónico congelaba el main thread: en
// iOS `[AVAudioSession setActive:]` se colgaba adentro de la HAL de CoreAudio y
// la app quedaba muerta con el dedo todavía en el botón. Que "no bloquea" no se
// puede afirmar contra un start() instantáneo, así que el fake trae un freno.

TEST_F(CaptureRequestTest, RequestingCaptureReturnsBeforeTheReopenFinishes) {
    runWithoutCapture();
    mBackend->blockStart();

    const Outcome outcome = mManager->requestCapture(Requester::INPUT_NODE, true, true);

    // La afirmación que importa: volvimos, y el reopen todavía no pasó por
    // start(). Con el reopen sincrónico esta línea no se alcanzaba nunca.
    EXPECT_EQ(outcome, Outcome::PENDING)
        << "un reopen agendado no puede reportarse como LIVE ni como NOT_LIVE";
    EXPECT_TRUE(mManager->isCaptureRequestPending());
    EXPECT_FALSE(mManager->isCaptureLive()) << "todavía no hay captura, y hay que decirlo";

    mBackend->waitUntilStartEntered();
    mBackend->releaseStart();
    mManager->waitForCaptureRequest();

    EXPECT_FALSE(mManager->isCaptureRequestPending());
    EXPECT_TRUE(mManager->isCaptureLive());
}

TEST_F(CaptureRequestTest, StateReadsDoNotBlockWhileTheStreamIsBeingReopened) {
    // **El test que de verdad pincha el requisito.** Mover el reopen a un thread
    // propio no alcanza por sí solo: `BackendManager::start()` toma mMutex y
    // adentro llama al start() del backend, así que el worker lo retiene durante
    // TODA la reapertura. Sin los espejos sin lock, estas tres lecturas —que son
    // las que la UI pollea en cada frame— se cuelgan en el mutex y el main thread
    // queda congelado igual que antes, sólo que en otra llamada.
    //
    // Si los espejos desaparecen, este test no falla: se **cuelga**, y lo agarra
    // el timeout de ctest.
    runWithoutCapture();
    mBackend->blockStart();

    mManager->requestCapture(Requester::INPUT_NODE, true, true);
    mBackend->waitUntilStartEntered();  // el worker está adentro de start(), con mMutex

    EXPECT_TRUE(mManager->isCaptureRequestPending());
    EXPECT_FALSE(mManager->isCaptureLive()) << "todavía no hay captura";
    EXPECT_FALSE(mManager->isRunning()) << "el stream está cerrado a mitad del reopen";
    (void)mManager->getStreamInfo();  // no puede colgarse

    mBackend->releaseStart();
    mManager->waitForCaptureRequest();

    EXPECT_TRUE(mManager->isCaptureLive());
    EXPECT_TRUE(mManager->isRunning());
}

TEST_F(CaptureRequestTest, RequestingCaptureAgainMidReopenDoesNotBlockTheCaller) {
    // **La primera versión de este test deadlockeaba**, y por eso existe.
    // `BackendManager::start()` retenía `mMutex` toda la reapertura, así que un
    // segundo `wma_input_start()` / `wma_input_stop()` desde el main thread se
    // quedaba esperando el reopen entero. Mover el reopen a un worker no lo
    // arreglaba: sólo cambiaba de lugar el freeze.
    //
    // Ahora la reapertura corre bajo `mOpMutex` y anotar el pedido sólo necesita
    // `mMutex`, que es corto por construcción. Si alguien vuelve a poner una
    // llamada lenta bajo `mMutex`, este test **se cuelga**.
    runWithoutCapture();
    mBackend->blockStart();

    mManager->requestCapture(Requester::INPUT_NODE, true, true);
    mBackend->waitUntilStartEntered();  // el worker está adentro de start()

    // Las dos direcciones, que son las dos que la UI puede disparar con un tap.
    EXPECT_EQ(mManager->requestCapture(Requester::INPUT_NODE, true, true), Outcome::PENDING);
    mManager->requestCapture(Requester::INPUT_NODE, false, false);
    mManager->setFullDuplexEnabled(true);

    mBackend->releaseStart();
    mManager->waitForCaptureRequest();

    SUCCEED() << "ninguna de las tres llamadas se quedó esperando la reapertura";
}

TEST_F(CaptureRequestTest, ARequestThatLandsMidReopenGetsItsOwnPass) {
    // Sin la generación, un pedido que llega mientras el worker ya pasó el punto
    // donde start() lee el flag se pierde en silencio: el worker termina, ve su
    // trabajo hecho, y nadie vuelve a intentar.
    //
    // Se mide en **pasadas** y no en "quedó viva", y por eso el micrófono está
    // denegado todo el test: con la captura consiguiéndose en la primera pasada,
    // el atajo de "ya está cumplido" corta la segunda —correctamente— y el
    // mutante de la generación sobrevive sin que se note. Con el permiso
    // denegado no hay atajo posible y la única razón para una segunda pasada es
    // el pedido nuevo.
    runWithoutCapture();
    mBackend->setCaptureAvailable(false);
    mBackend->blockStart();
    const int startsBefore = mBackend->startCount();

    mManager->requestCapture(Requester::INPUT_NODE, true, true);
    mBackend->waitUntilStartEntered();  // el worker está adentro de start()

    // Acá está lo determinista: el pedido llega con el reopen en vuelo.
    ASSERT_EQ(mManager->requestCapture(Requester::INPUT_NODE, true, true), Outcome::PENDING);

    mBackend->releaseStart();
    mManager->waitForCaptureRequest();

    EXPECT_EQ(mBackend->startCount(), startsBefore + 2)
        << "el pedido que llegó a mitad del reopen no se llevó su propia pasada";
}

TEST_F(CaptureRequestTest, ASecondRequestAlreadySatisfiedDoesNotCostAnExtraGap) {
    // El doble tap sobre "capturar" con el micrófono disponible: el segundo
    // pedido llega en vuelo y sube la generación, pero la pasada que ya estaba
    // corriendo lo deja cumplido. Otra reapertura sería un corte audible gratis.
    runWithoutCapture();
    mBackend->blockStart();
    const int startsBefore = mBackend->startCount();

    mManager->requestCapture(Requester::INPUT_NODE, true, true);
    mBackend->waitUntilStartEntered();
    mManager->requestCapture(Requester::INPUT_NODE, true, true);

    mBackend->releaseStart();
    mManager->waitForCaptureRequest();

    EXPECT_TRUE(mManager->isCaptureLive());
    EXPECT_EQ(mBackend->startCount(), startsBefore + 1)
        << "se reabrió de nuevo con la captura ya viva: un corte audible por nada";
}

TEST_F(CaptureRequestTest, ADeniedMicrophoneIsNotRetriedInALoop) {
    // El tope de pasadas existe para pedidos nuevos, no para insistir contra un
    // permiso. Un micrófono denegado no sube la generación, así que se reabre una
    // sola vez: cada reintento sería otro corte audible por nada.
    runWithoutCapture();
    mBackend->setCaptureAvailable(false);
    const int startsBefore = mBackend->startCount();

    requestCaptureAndSettle(Requester::INPUT_NODE, true, true);

    EXPECT_EQ(mBackend->startCount(), startsBefore + 1)
        << "se reintentó contra un micrófono denegado";
    EXPECT_FALSE(mManager->isCaptureLive());
}

TEST_F(CaptureRequestTest, DestroyingTheManagerMidReopenDoesNotLeaveAThreadBehind) {
    // Destruir el manager con un reopen en curso no puede colgarse ni crashear.
    //
    // > [!NOTE]
    // > **Lo que este test NO distingue, y por qué se deja igual.** Mutar el
    // > `join()` del destructor a `detach()` **no lo hace fallar**, ni siquiera
    // > bajo TSan: después del detach, `stop()` toma `mMutex` y el worker lo
    // > tiene mientras está adentro de `start()`, así que el destructor termina
    // > esperando lo mismo por otro camino.
    // >
    // > Eso es justamente lo que hace peligroso al detach: **la corrección
    // > quedaría apoyada en que `stop()` incidentalmente tome el mismo mutex**,
    // > no en una garantía. Queda una ventana real —el worker todavía usa
    // > `mReopenMutex` y `mReopenDone` después de su última toma de `mMutex`—
    // > que es de unas pocas instrucciones y no se reproduce a pedido. El join
    // > la cierra por construcción; el test cubre el resto.
    auto manager = std::make_unique<watermelon_audio::BackendManager>();
    auto* backend = lastCreatedSystemBackend();
    ASSERT_NE(backend, nullptr);

    ASSERT_TRUE(manager->selectBackend(watermelon_audio::BackendType::OBOE));
    manager->setCallback(mEngine.get());
    ASSERT_EQ(manager->start(), watermelon_audio::BackendResult::OK);

    backend->blockStart();
    manager->requestCapture(Requester::INPUT_NODE, true, true);
    backend->waitUntilStartEntered();

    // El destrabe llega DESPUÉS de que empiece la destrucción, y ahí está la
    // gracia: si el destructor joinea, no puede volver hasta que esto corra; si
    // detacha, vuelve enseguida y `released` todavía es false.
    //
    // Soltar el freno antes de destruir —que es lo que hacía la primera versión
    // de este test— deja al worker ya terminado y `joinable()` en false, así que
    // el mutante del detach sobrevivía sin que se notara.
    std::atomic<bool> released{false};
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        released.store(true, std::memory_order_release);
        backend->releaseStart();
    });

    manager.reset();

    EXPECT_TRUE(released.load(std::memory_order_acquire))
        << "el destructor volvió con el worker todavía adentro de start(): "
           "no joineó, y ese thread sigue usando un manager liberado";

    releaser.join();
}

}  // namespace
}  // namespace wma_test
