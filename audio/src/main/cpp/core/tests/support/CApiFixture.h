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

    WmaEngine* mWma = nullptr;
    FakeAudioBackend* mBackend = nullptr;
};

}  // namespace wma_test
