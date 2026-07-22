/**
 * test_current_sample_rate.cpp
 *
 * AudioEngine::currentSampleRate() — the resolution order every call site now
 * shares: running stream → preferred rate → 48000, never <= 0.
 *
 * What made this worth a suite: the call sites it replaced all read
 * `mStream ? mStream->getSampleRate() : 0`, and on the BackendManager path
 * mStream is permanently null. Each site then patched the resulting 0 its own
 * way, or did not patch it at all. The tests below pin the single answer.
 */

#include "support/BackendPathFixture.h"

#include <gtest/gtest.h>

namespace wma_test {
namespace {

using CurrentSampleRateTest = BackendPathFixture;

TEST_F(CurrentSampleRateTest, FallsBackTo48000WhenNothingIsConfigured) {
    // Fresh engine: no stream running, no preferred rate. The documented floor
    // is 48000 — the value the old code would have reported as 0.
    EXPECT_EQ(mEngine->currentSampleRate(), 48000);
}

TEST_F(CurrentSampleRateTest, UsesPreferredRateWhenNoStreamIsRunning) {
    mEngine->setPreferredSampleRate(44100);

    EXPECT_EQ(mEngine->currentSampleRate(), 44100);
}

TEST_F(CurrentSampleRateTest, PrefersNegotiatedBackendRateOverPreferredRate) {
    // The scenario that desynchronised SoundFont playback: the app asks for
    // 48000, the device settles on 44100, and anything prepared at the
    // preferred rate ends up detuned.
    mEngine->setPreferredSampleRate(48000);
    runBackendAt(44100);

    EXPECT_EQ(mEngine->currentSampleRate(), 44100);
}

TEST_F(CurrentSampleRateTest, IgnoresBackendRateUntilTheBackendIsActuallyRunning) {
    // Selected but never started: getStreamInfo() would happily report the
    // backend's default, so the running check is what keeps a stale rate out.
    mBackend->setNegotiatedSampleRate(96000);
    ASSERT_TRUE(mManager->selectBackend(watermelon_audio::BackendType::OBOE));
    ASSERT_FALSE(mManager->isRunning());

    mEngine->setPreferredSampleRate(44100);

    EXPECT_EQ(mEngine->currentSampleRate(), 44100);
}

TEST_F(CurrentSampleRateTest, ReturnsToThePreferredRateAfterTheBackendStops) {
    mEngine->setPreferredSampleRate(44100);
    runBackendAt(96000);
    ASSERT_EQ(mEngine->currentSampleRate(), 96000);

    mManager->stop();

    EXPECT_EQ(mEngine->currentSampleRate(), 44100);
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
    // (mid-reconfiguration, or a descriptor that never yielded a rate).
    mEngine->setPreferredSampleRate(44100);
    runBackendAt(0);

    EXPECT_EQ(mEngine->currentSampleRate(), 44100);
}

TEST_F(CurrentSampleRateTest, NeverReturnsANonPositiveRate) {
    // Every combination of junk inputs still yields something usable, because
    // callers divide by this value and convert milliseconds with it.
    const int junkPreferredRates[] = {0, -1, -48000};
    const int junkNegotiatedRates[] = {0, -1, -44100};

    for (int preferred : junkPreferredRates) {
        mEngine->setPreferredSampleRate(preferred);
        EXPECT_GT(mEngine->currentSampleRate(), 0)
            << "no stream, preferred=" << preferred;
    }

    runBackendAt(48000);
    for (int negotiated : junkNegotiatedRates) {
        mBackend->setNegotiatedSampleRate(negotiated);
        for (int preferred : junkPreferredRates) {
            mEngine->setPreferredSampleRate(preferred);
            EXPECT_GT(mEngine->currentSampleRate(), 0)
                << "negotiated=" << negotiated << ", preferred=" << preferred;
        }
    }
}

TEST_F(CurrentSampleRateTest, ResolvesOnTheLegacyPathToo) {
    // With BackendManager disabled there is no stream at all off Android, so
    // this exercises the same fallback chain with the backend branch skipped.
    // It is the shape the legacy Oboe path degrades to before a stream opens.
    mEngine->setUseBackendManager(false);
    ASSERT_FALSE(mEngine->isUsingBackendManager());

    EXPECT_EQ(mEngine->currentSampleRate(), 48000);

    mEngine->setPreferredSampleRate(88200);
    EXPECT_EQ(mEngine->currentSampleRate(), 88200);
}

TEST_F(CurrentSampleRateTest, IgnoresARunningBackendWhenTheBackendPathIsDisabled) {
    // getStreamInfo() consults the manager only when the engine is on the
    // backend path; a running backend must not leak into the legacy answer.
    runBackendAt(96000);
    mEngine->setUseBackendManager(false);
    mEngine->setPreferredSampleRate(44100);

    EXPECT_EQ(mEngine->currentSampleRate(), 44100);
}

}  // namespace
}  // namespace wma_test
