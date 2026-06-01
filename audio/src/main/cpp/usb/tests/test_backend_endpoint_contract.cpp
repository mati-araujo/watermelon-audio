#include "../../backends/IAudioBackend.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

using namespace watermelon_audio;

namespace {

class FakeFullDuplexBackend : public IAudioBackend {
public:
    BackendResult start() override { return BackendResult::OK; }
    void stop() override {}
    void pause() override {}
    void resume() override {}
    void setCallback(IAudioCallback* callback) override { mCallback = callback; }
    void setSampleRate(int sampleRate) override { mInfo.sampleRate = sampleRate; }
    void setBufferSize(int framesPerBuffer) override { mInfo.framesPerBuffer = framesPerBuffer; }
    void setFullDuplexEnabled(bool enable) override { mInfo.isFullDuplex = enable; }
    StreamInfo getStreamInfo() const override { return mInfo; }
    bool isRunning() const override { return false; }
    float getOutputLatencyMs() const override { return 0.0f; }
    float getInputLatencyMs() const override { return 0.0f; }
    BackendType getType() const override { return BackendType::NONE; }
    bool supportsFullDuplex() const override { return true; }

private:
    IAudioCallback* mCallback = nullptr;
    StreamInfo mInfo;
};

class FakeInputSource : public IAudioInputSource {
public:
    int32_t readInput(float* outputData, int32_t maxFrames) override {
        if (!outputData || maxFrames <= 0) {
            return 0;
        }

        for (int32_t frame = 0; frame < maxFrames; ++frame) {
            outputData[frame * 2] = static_cast<float>(100 + frame);
            outputData[frame * 2 + 1] = static_cast<float>(200 + frame);
        }
        return maxFrames;
    }

    StreamInfo getInputStreamInfo() const override {
        StreamInfo info;
        info.sampleRate = 48000;
        info.channelCount = 2;
        info.framesPerBuffer = 4;
        info.isFullDuplex = true;
        return info;
    }
};

class FakeOutputSink : public IAudioOutputSink {
public:
    int32_t writeOutput(const float* inputData, int32_t frames) override {
        if (!inputData || frames <= 0) {
            return 0;
        }

        const int32_t samples = frames * 2;
        mLastWrite.assign(inputData, inputData + samples);
        return frames;
    }

    StreamInfo getOutputStreamInfo() const override {
        StreamInfo info;
        info.sampleRate = 48000;
        info.channelCount = 2;
        info.framesPerBuffer = 4;
        return info;
    }

    const std::vector<float>& lastWrite() const {
        return mLastWrite;
    }

private:
    std::vector<float> mLastWrite;
};

TEST(BackendEndpointContractTest, RoleFlagsComposeAndQueryExplicitly) {
    const auto roles = BackendStreamRole::INPUT_SOURCE | BackendStreamRole::OUTPUT_SINK;

    EXPECT_TRUE(hasBackendRole(roles, BackendStreamRole::INPUT_SOURCE));
    EXPECT_TRUE(hasBackendRole(roles, BackendStreamRole::OUTPUT_SINK));
    EXPECT_TRUE(hasBackendRole(roles, BackendStreamRole::FULL_DUPLEX));
    EXPECT_FALSE(hasBackendRole(BackendStreamRole::OUTPUT_SINK, BackendStreamRole::INPUT_SOURCE));
}

TEST(BackendEndpointContractTest, FullDuplexSupportDoesNotImplyInputSourceContract) {
    FakeFullDuplexBackend backend;
    const auto caps = backend.getEndpointCapabilities();

    EXPECT_TRUE(hasBackendRole(caps.roles, BackendStreamRole::FULL_DUPLEX));
    EXPECT_FALSE(caps.hasInputSourceContract);
    EXPECT_FALSE(caps.callbackCarriesInput);
    EXPECT_TRUE(caps.drivesUserCallback);
}

TEST(BackendEndpointContractTest, InputSourceAndOutputSinkMoveFramesThroughExplicitContract) {
    FakeInputSource source;
    FakeOutputSink sink;

    std::vector<float> bridge(8, 0.0f);
    const int32_t framesRead = source.readInput(bridge.data(), 4);
    const int32_t framesWritten = sink.writeOutput(bridge.data(), framesRead);

    ASSERT_EQ(framesRead, 4);
    ASSERT_EQ(framesWritten, 4);
    ASSERT_EQ(sink.lastWrite().size(), bridge.size());
    EXPECT_TRUE(std::equal(bridge.begin(), bridge.end(), sink.lastWrite().begin()));
}

TEST(BackendEndpointContractTest, ExplicitInputSourceCapabilityCanBeRequiredBySplitGate) {
    BackendEndpointCapabilities inputCaps;
    inputCaps.roles = BackendStreamRole::INPUT_SOURCE;
    inputCaps.hasInputSourceContract = true;
    inputCaps.callbackCarriesInput = true;
    inputCaps.drivesUserCallback = false;

    BackendEndpointCapabilities outputCaps;
    outputCaps.roles = BackendStreamRole::OUTPUT_SINK;
    outputCaps.hasInputSourceContract = false;
    outputCaps.callbackCarriesInput = false;
    outputCaps.drivesUserCallback = true;

    const bool canBuildSplit =
        hasBackendRole(inputCaps.roles, BackendStreamRole::INPUT_SOURCE) &&
        inputCaps.hasInputSourceContract &&
        hasBackendRole(outputCaps.roles, BackendStreamRole::OUTPUT_SINK) &&
        outputCaps.drivesUserCallback;

    EXPECT_TRUE(canBuildSplit);
}

} // namespace
