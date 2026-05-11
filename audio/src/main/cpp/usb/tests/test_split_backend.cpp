#include "../../backends/SplitBackend.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

using namespace watermelon_audio;

namespace {

class FakeBackend final : public IAudioBackend {
public:
    explicit FakeBackend(BackendEndpointCapabilities caps)
        : mCaps(caps) {}

    BackendResult start() override {
        if (!mCallback) {
            return BackendResult::ERROR_NOT_INITIALIZED;
        }
        mRunning = true;
        return BackendResult::OK;
    }

    void stop() override { mRunning = false; }
    void pause() override { mPaused = true; }
    void resume() override { mPaused = false; }
    void setCallback(IAudioCallback* callback) override { mCallback = callback; }
    void setSampleRate(int sampleRate) override { mInfo.sampleRate = sampleRate; }
    void setBufferSize(int framesPerBuffer) override { mInfo.framesPerBuffer = framesPerBuffer; }
    void setFullDuplexEnabled(bool enable) override { mInfo.isFullDuplex = enable; }
    StreamInfo getStreamInfo() const override { return mInfo; }
    bool isRunning() const override { return mRunning && !mPaused; }
    float getOutputLatencyMs() const override { return 5.0f; }
    float getInputLatencyMs() const override { return 7.0f; }
    BackendType getType() const override { return BackendType::NONE; }
    bool supportsFullDuplex() const override { return true; }
    BackendEndpointCapabilities getEndpointCapabilities() const override { return mCaps; }

    IAudioCallback::Result emit(float* outputData, const float* inputData, int32_t frames) {
        if (!mCallback) {
            return IAudioCallback::Result::STOP;
        }
        return mCallback->onAudioReady(outputData, inputData, frames);
    }

private:
    BackendEndpointCapabilities mCaps;
    IAudioCallback* mCallback = nullptr;
    StreamInfo mInfo;
    bool mRunning = false;
    bool mPaused = false;
};

class CapturingUserCallback final : public IAudioCallback {
public:
    Result onAudioReady(float* outputData, const float* inputData, int32_t numFrames) override {
        ++calls;
        if (outputData) {
            for (int i = 0; i < numFrames * 2; ++i) {
                outputData[i] = 0.25f;
            }
        }
        if (inputData) {
            lastInput.assign(inputData, inputData + numFrames * 2);
        }
        return Result::CONTINUE;
    }

    void onBackendError(BackendError error) override {
        lastError = error;
    }

    int calls = 0;
    BackendError lastError = BackendError::NONE;
    std::vector<float> lastInput;
};

BackendEndpointCapabilities inputCaps() {
    BackendEndpointCapabilities caps;
    caps.roles = BackendStreamRole::INPUT_SOURCE;
    caps.hasInputSourceContract = true;
    caps.callbackCarriesInput = true;
    caps.drivesUserCallback = false;
    return caps;
}

BackendEndpointCapabilities outputCaps() {
    BackendEndpointCapabilities caps;
    caps.roles = BackendStreamRole::OUTPUT_SINK;
    caps.hasInputSourceContract = false;
    caps.callbackCarriesInput = false;
    caps.drivesUserCallback = true;
    return caps;
}

TEST(SplitBackendTest, RejectsInputWithoutSourceContract) {
    BackendEndpointCapabilities badInput = inputCaps();
    badInput.hasInputSourceContract = false;

    FakeBackend input(badInput);
    FakeBackend output(outputCaps());
    SplitBackend split(input, output);
    CapturingUserCallback user;
    split.setCallback(&user);

    EXPECT_EQ(split.start(), BackendResult::ERROR_INVALID_CONFIG);
}

TEST(SplitBackendTest, StartsAndStopsBothInnerBackends) {
    FakeBackend input(inputCaps());
    FakeBackend output(outputCaps());
    SplitBackend split(input, output);
    CapturingUserCallback user;
    split.setCallback(&user);

    EXPECT_EQ(split.start(), BackendResult::OK);
    EXPECT_TRUE(input.isRunning());
    EXPECT_TRUE(output.isRunning());

    split.stop();
    EXPECT_FALSE(input.isRunning());
    EXPECT_FALSE(output.isRunning());
}

TEST(SplitBackendTest, InputFramesReachUserCallbackFromOutputDriverOnly) {
    FakeBackend input(inputCaps());
    FakeBackend output(outputCaps());
    SplitBackend split(input, output);
    split.setBufferSize(4);

    CapturingUserCallback user;
    split.setCallback(&user);
    ASSERT_EQ(split.start(), BackendResult::OK);

    std::vector<float> inputBlock = {
        1.0f, 1.1f,
        2.0f, 2.1f,
        3.0f, 3.1f,
        4.0f, 4.1f
    };
    std::vector<float> discardedOutput(8, 99.0f);
    std::vector<float> outputBlock(8, 0.0f);

    EXPECT_EQ(input.emit(discardedOutput.data(), inputBlock.data(), 4),
              IAudioCallback::Result::CONTINUE);
    EXPECT_EQ(user.calls, 0);
    EXPECT_TRUE(std::all_of(discardedOutput.begin(), discardedOutput.end(),
                            [](float v) { return v == 0.0f; }));

    EXPECT_EQ(output.emit(outputBlock.data(), nullptr, 4),
              IAudioCallback::Result::CONTINUE);
    EXPECT_EQ(user.calls, 1);
    ASSERT_EQ(user.lastInput.size(), inputBlock.size());
    EXPECT_TRUE(std::equal(inputBlock.begin(), inputBlock.end(), user.lastInput.begin()));
    EXPECT_TRUE(std::all_of(outputBlock.begin(), outputBlock.end(),
                            [](float v) { return v == 0.25f; }));

    split.stop();
}

TEST(SplitBackendTest, OutputUnderrunSuppliesSilenceAndIncrementsCounter) {
    FakeBackend input(inputCaps());
    FakeBackend output(outputCaps());
    SplitBackend split(input, output);
    split.setBufferSize(4);

    CapturingUserCallback user;
    split.setCallback(&user);
    ASSERT_EQ(split.start(), BackendResult::OK);

    std::vector<float> outputBlock(8, 0.0f);
    EXPECT_EQ(output.emit(outputBlock.data(), nullptr, 4),
              IAudioCallback::Result::CONTINUE);

    EXPECT_EQ(user.calls, 1);
    ASSERT_EQ(user.lastInput.size(), 8u);
    EXPECT_TRUE(std::all_of(user.lastInput.begin(), user.lastInput.end(),
                            [](float v) { return v == 0.0f; }));
    EXPECT_EQ(split.getBridgeUnderruns(), 1u);

    split.stop();
}

} // namespace
