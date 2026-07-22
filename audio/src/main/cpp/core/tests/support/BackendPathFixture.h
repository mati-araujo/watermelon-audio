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
     * Outlive stopWithFade()'s detached thread.
     *
     * stopWithFade() spawns a detached thread that sleeps fadeTimeMs + 50 and
     * then calls stop() on the engine. Tearing the engine down before it fires
     * is a use-after-free, so every test that starts one waits it out here
     * instead of racing it.
     */
    void awaitDetachedStop(int fadeTimeMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(fadeTimeMs + 250));
    }

    std::unique_ptr<watermelon_audio::BackendManager> mManager;
    std::unique_ptr<AudioEngine> mEngine;
    FakeAudioBackend* mBackend = nullptr;
};

}  // namespace wma_test
