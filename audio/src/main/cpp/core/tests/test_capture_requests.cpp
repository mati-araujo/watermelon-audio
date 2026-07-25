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

namespace wma_test {
namespace {

using Requester = watermelon_audio::BackendManager::CaptureRequester;

class CaptureRequestTest : public BackendPathFixture {
protected:
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
    ASSERT_TRUE(mManager->requestCapture(Requester::INPUT_NODE, true, true));

    mManager->setFullDuplexEnabled(false);  // the MODE requester withdrawing

    EXPECT_TRUE(mManager->isCaptureLive());
    EXPECT_TRUE(mBackend->fullDuplexRequested());
}

TEST_F(CaptureRequestTest, StoppingInputDoesNotKillCaptureTheModeStillNeeds) {
    // The mirror image: INPUT_FX is on, the app stops its own input, and the
    // mode's need for capture has to survive.
    runWithoutCapture();
    mManager->setFullDuplexEnabled(true);
    ASSERT_TRUE(mManager->requestCapture(Requester::INPUT_NODE, true, true));
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

    EXPECT_TRUE(mManager->requestCapture(Requester::INPUT_NODE, true, true));

    EXPECT_EQ(mBackend->startCount(), startsBefore + 1);
    EXPECT_TRUE(mManager->isCaptureLive());
}

TEST_F(CaptureRequestTest, WithdrawingCaptureNeverReopensTheStream) {
    // Turning capture off is not worth a gap: the backend simply stops handing
    // the frames over.
    runWithoutCapture();
    ASSERT_TRUE(mManager->requestCapture(Requester::INPUT_NODE, true, true));
    const int startsBefore = mBackend->startCount();

    mManager->requestCapture(Requester::INPUT_NODE, false, true);

    EXPECT_EQ(mBackend->startCount(), startsBefore);
}

TEST_F(CaptureRequestTest, RequestingCaptureTwiceReopensOnlyOnce) {
    runWithoutCapture();
    ASSERT_TRUE(mManager->requestCapture(Requester::INPUT_NODE, true, true));
    const int startsAfterFirst = mBackend->startCount();

    EXPECT_TRUE(mManager->requestCapture(Requester::INPUT_NODE, true, true));

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

    EXPECT_FALSE(mManager->requestCapture(Requester::INPUT_NODE, true, true));

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

    EXPECT_FALSE(mManager->requestCapture(Requester::INPUT_NODE, true, true));

    EXPECT_FALSE(mBackend->fullDuplexRequested())
        << "the request that could not be honored must be dropped, or the next "
           "reopen would fail the same way";
}

}  // namespace
}  // namespace wma_test
