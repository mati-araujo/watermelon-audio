/**
 * SplitBackend.h
 *
 * Composes one backend as an input source with another as the output sink.
 */

#pragma once

#include "DriftResampler.h"
#include "IAudioBackend.h"
#include "../dsp/LockFreeRingBuffer.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace watermelon_audio {

class SplitBackend : public IAudioBackend {
public:
    SplitBackend(IAudioBackend& inputSource, IAudioBackend& outputSink);
    ~SplitBackend() override;

    BackendResult start() override;
    void stop() override;
    void pause() override;
    void resume() override;

    void setCallback(IAudioCallback* callback) override;
    void setSampleRate(int sampleRate) override;
    void setBufferSize(int framesPerBuffer) override;
    void setFullDuplexEnabled(bool enable) override;

    StreamInfo getStreamInfo() const override;
    bool isRunning() const override;
    float getOutputLatencyMs() const override;
    float getInputLatencyMs() const override;
    BackendType getType() const override { return BackendType::SPLIT; }
    bool supportsFullDuplex() const override { return true; }
    bool supportsPause() const override;
    BackendEndpointCapabilities getEndpointCapabilities() const override;

    uint64_t getBridgeUnderruns() const { return mBridgeUnderruns.load(std::memory_order_acquire); }
    uint64_t getBridgeOverruns() const { return mBridgeOverruns.load(std::memory_order_acquire); }

private:
    class InputCallback final : public IAudioCallback {
    public:
        explicit InputCallback(SplitBackend& parent) : mParent(parent) {}
        Result onAudioReady(float* outputData, const float* inputData, int32_t numFrames) override;
        void onBackendError(BackendError error) override;
        void onStreamConfigChanged(const StreamInfo& newInfo) override;
    private:
        SplitBackend& mParent;
    };

    class OutputCallback final : public IAudioCallback {
    public:
        explicit OutputCallback(SplitBackend& parent) : mParent(parent) {}
        Result onAudioReady(float* outputData, const float* inputData, int32_t numFrames) override;
        void onBackendError(BackendError error) override;
        void onStreamConfigChanged(const StreamInfo& newInfo) override;
    private:
        SplitBackend& mParent;
    };

    static constexpr int kChannelCount = 2;
    static constexpr int kDefaultSampleRate = 48000;
    static constexpr int kDefaultFramesPerBuffer = 256;
    static constexpr int kBridgeBufferMs = 40;

    IAudioBackend& mInputSource;
    IAudioBackend& mOutputSink;
    InputCallback mInputCallback;
    OutputCallback mOutputCallback;

    IAudioCallback* mUserCallback = nullptr;

    int mSampleRate = kDefaultSampleRate;
    int mFramesPerBuffer = kDefaultFramesPerBuffer;
    // Atomic: el setter no puede tomar mLifecycleMutex (ver el .cpp).
    std::atomic<bool> mFullDuplexEnabled{true};

    std::unique_ptr<LockFreeRingBuffer> mInputBridge;
    std::vector<float> mUserInputBuffer;
    std::vector<float> mResampledInputBuffer;
    DriftResampler mDriftResampler;

    std::atomic<bool> mRunning{false};
    std::atomic<uint64_t> mBridgeUnderruns{0};
    std::atomic<uint64_t> mBridgeOverruns{0};
    mutable std::mutex mLifecycleMutex;

    bool validateEndpointContracts() const;
    void prepareBuffers();
    void applyConfig();
    IAudioCallback::Result handleInput(float* outputData, const float* inputData, int32_t numFrames);
    IAudioCallback::Result handleOutput(float* outputData, int32_t numFrames);
    void forwardError(BackendError error);
};

} // namespace watermelon_audio
