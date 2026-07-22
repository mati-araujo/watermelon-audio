#pragma once

/**
 * FakeAudioBackend.h — TEST DOUBLE, host test build only.
 *
 * A controllable IAudioBackend so the core suite can drive the BackendManager
 * path (USB today, CoreAudio on iOS tomorrow) without a device. It is handed to
 * BackendManager through the platform registration point — see
 * test_platform_backends.cpp for why that works and what it replaces.
 *
 * The one behaviour worth calling out: the *requested* sample rate (what the
 * engine pushes down via setSampleRate) and the *negotiated* sample rate (what
 * getStreamInfo reports) are separate knobs. Real devices coerce — a 48000
 * request can come back as 44100 — and telling those two numbers apart is
 * exactly what AudioEngine::currentSampleRate() has to get right.
 */

#include "backends/IAudioBackend.h"

#include <atomic>

namespace wma_test {

class FakeAudioBackend : public watermelon_audio::IAudioBackend {
public:
    // ---- IAudioBackend ----------------------------------------------------

    watermelon_audio::BackendResult start() override {
        if (mStartResult != watermelon_audio::BackendResult::OK) {
            return mStartResult;
        }
        mRunning.store(true, std::memory_order_release);
        return watermelon_audio::BackendResult::OK;
    }

    void stop() override { mRunning.store(false, std::memory_order_release); }
    void pause() override { mPaused = true; }
    void resume() override { mPaused = false; }

    void setCallback(watermelon_audio::IAudioCallback* callback) override {
        mCallback = callback;
    }

    // Recorded, deliberately NOT applied to the reported stream info: a real
    // device is free to ignore the request, and the tests rely on that gap.
    void setSampleRate(int sampleRate) override { mRequestedSampleRate = sampleRate; }

    void setBufferSize(int framesPerBuffer) override {
        mInfo.framesPerBuffer = framesPerBuffer;
    }

    void setFullDuplexEnabled(bool enable) override { mInfo.isFullDuplex = enable; }

    watermelon_audio::StreamInfo getStreamInfo() const override { return mInfo; }

    bool isRunning() const override { return mRunning.load(std::memory_order_acquire); }

    float getOutputLatencyMs() const override { return mInfo.outputLatencyMs; }
    float getInputLatencyMs() const override { return mInfo.inputLatencyMs; }

    watermelon_audio::BackendType getType() const override {
        return watermelon_audio::BackendType::OBOE;
    }

    bool supportsFullDuplex() const override { return true; }

    // ---- Test knobs -------------------------------------------------------

    /// The rate the "device" settled on, reported through getStreamInfo().
    void setNegotiatedSampleRate(int sampleRate) { mInfo.sampleRate = sampleRate; }

    /// The rate the engine asked for, as recorded by setSampleRate().
    int requestedSampleRate() const { return mRequestedSampleRate; }

    /// Make start() fail, so the manager never reports isRunning().
    void setStartResult(watermelon_audio::BackendResult result) { mStartResult = result; }

    watermelon_audio::IAudioCallback* callback() const { return mCallback; }
    bool isPaused() const { return mPaused; }

private:
    watermelon_audio::StreamInfo mInfo{};
    watermelon_audio::IAudioCallback* mCallback = nullptr;
    watermelon_audio::BackendResult mStartResult = watermelon_audio::BackendResult::OK;
    std::atomic<bool> mRunning{false};
    bool mPaused = false;
    int mRequestedSampleRate = 0;
};

/**
 * The FakeAudioBackend created by the most recent BackendManager construction,
 * or nullptr if none has been built yet. Owned by that manager — valid only
 * while it lives.
 */
FakeAudioBackend* lastCreatedSystemBackend();

/// Forget the pointer above. Call before constructing a manager you intend to
/// query, so a stale one can never be mistaken for the fresh one.
void resetLastCreatedSystemBackend();

}  // namespace wma_test
