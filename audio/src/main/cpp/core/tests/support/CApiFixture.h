#pragma once

/**
 * CApiFixture.h — host test support.
 *
 * An engine built the way a real consumer builds one: through
 * wma_engine_create(), not by newing an AudioEngine. That matters because the C
 * API owns more than the engine — it also builds the BackendManager and
 * registers it as the global instance — so a test that constructs the pieces by
 * hand (BackendPathFixture) is testing a different assembly than the one iOS and
 * the JNI actually run.
 *
 * The fake backend still arrives the same way: BackendManager's constructor
 * reaches the substituted platform registration point. See
 * test_platform_backends.cpp.
 */

#include "FakeAudioBackend.h"

#include "api/watermelon_audio.h"
#include "api/watermelon_audio_internal.h"
#include "platform/Logger.h"

#include <vector>

#include <gtest/gtest.h>

namespace wma_test {

class CApiFixture : public ::testing::Test {
protected:
    void SetUp() override {
        // The engine logs generously outside the RT path; a no-op sink keeps
        // the ctest output readable without touching production defaults.
        wma::setLogCallback([](wma::LogLevel, const char*, const char*) {});

        resetLastCreatedSystemBackend();
        mWma = wma_engine_create();
        ASSERT_NE(mWma, nullptr);

        mBackend = lastCreatedSystemBackend();
        ASSERT_NE(mBackend, nullptr)
            << "the test platform registration point did not hand the manager a fake";
    }

    void TearDown() override {
        // Destroys the engine and clears the global manager pointer, in that
        // order. AudioEngine's destructor reclaims any stop-fade worker.
        wma_engine_destroy(mWma);
        mWma = nullptr;
        mBackend = nullptr;
        wma::setLogCallback(nullptr);
    }

    /// Bring the engine up over the fake backend with the given fade argument.
    void startAt(int negotiatedSampleRate, int fadeTimeMs) {
        mBackend->setNegotiatedSampleRate(negotiatedSampleRate);
        wma_set_use_backend_manager(mWma, true);
        // BackendType::OBOE — the fake registers itself as the system backend.
        ASSERT_TRUE(wma_select_backend(1));
        ASSERT_EQ(wma_engine_start(mWma, fadeTimeMs), WMA_OK);
    }

    /**
     * Render @p blocks callbacks of @p framesPerBlock frames through the engine.
     *
     * More than a way to advance a fade: this is the only way several
     * subsystems make progress at all. Voice allocation, for one, happens in
     * VoiceManager::processSourceEvents() on the audio thread — updateMultiTouch
     * only hands the touches to the trigger source, so the active-voice count
     * does not move until a block has been rendered.
     */
    void render(int blocks, int framesPerBlock) {
        std::vector<float> buffer(static_cast<size_t>(framesPerBlock) * 2, 0.0f);
        for (int i = 0; i < blocks; ++i) {
            mWma->engine->onAudioReady(buffer.data(), nullptr, framesPerBlock);
        }
    }

    WmaEngine* mWma = nullptr;
    FakeAudioBackend* mBackend = nullptr;
};

}  // namespace wma_test
