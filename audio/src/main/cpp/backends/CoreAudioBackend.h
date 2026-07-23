/**
 * CoreAudioBackend.h
 *
 * Apple (iOS / macOS) audio backend built on AVAudioEngine + AVAudioSourceNode.
 * This is the CoreAudio counterpart to OboeBackend: AAudio is to Android what
 * AVAudioEngine is to Apple.
 *
 * Scope (WA-2.4, decision D2 iteration 1): OUTPUT ONLY. No capture, no
 * full-duplex — that path is InputNode, still Android-only. supportsFullDuplex()
 * therefore returns false and setFullDuplexEnabled() is a no-op.
 *
 * This header is deliberately free of any Objective-C: it is included from
 * PlatformBackends.cpp (compiled as C++), so every Apple/ObjC type is hidden
 * behind a PIMPL (`Impl`) defined in the .mm. Only the .mm is Objective-C++.
 *
 * Thread Safety:
 * - Configuration methods: call from a control thread before start().
 * - start/stop/pause/resume: serialized by mStreamMutex.
 * - The render block: runs on the CoreAudio real-time thread. It touches only
 *   atomics and the pre-allocated scratch buffer — never the PIMPL, never ObjC,
 *   never a lock or allocation.
 */

#pragma once

#include "IAudioBackend.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

namespace watermelon_audio {

/**
 * CoreAudioBackend
 *
 * IAudioBackend implementation using AVAudioEngine. An AVAudioSourceNode pulls
 * interleaved stereo float from the engine's DSP callback and hands it to the
 * mainMixerNode -> outputNode chain.
 */
class CoreAudioBackend : public IAudioBackend {
public:
    CoreAudioBackend();
    ~CoreAudioBackend() override;

    // Stream ownership is exclusive.
    CoreAudioBackend(const CoreAudioBackend&) = delete;
    CoreAudioBackend& operator=(const CoreAudioBackend&) = delete;
    CoreAudioBackend(CoreAudioBackend&&) = delete;
    CoreAudioBackend& operator=(CoreAudioBackend&&) = delete;

    // =========================================================================
    // IAudioBackend Implementation
    // =========================================================================

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
    float getInputLatencyMs() const override { return 0.0f; }  // output-only
    BackendType getType() const override { return BackendType::COREAUDIO; }
    bool supportsFullDuplex() const override { return false; }
    bool supportsPause() const override { return true; }

private:
    // Opaque holder for the Objective-C engine objects (AVAudioEngine,
    // AVAudioSourceNode, AVAudioFormat). Defined in the .mm so this header stays
    // pure C++. Managed by ARC there.
    struct Impl;
    std::unique_ptr<Impl> mImpl;

    // -------------------------------------------------------------------------
    // Requested configuration (applied on the next start()).
    // -------------------------------------------------------------------------
    int mRequestedSampleRate = 0;   // 0 = let the session/hardware decide
    int mRequestedBufferSize = 0;   // 0 = let the session/hardware decide

    // -------------------------------------------------------------------------
    // State touched from the render block. Raw atomics only — the block captures
    // `this` and dereferences these directly; nothing here allocates or locks.
    // -------------------------------------------------------------------------
    std::atomic<IAudioCallback*> mCallback{nullptr};
    std::atomic<bool> mIsRunning{false};
    std::atomic<bool> mIsPaused{false};
    std::atomic<int>  mActiveCallbacks{0};
    std::atomic<bool> mStopping{false};

    // Pre-allocated interleaved scratch for the deinterleaved fallback path.
    // Sized in start() to mMaxFrames * 2 and never resized while running, so the
    // render block can read .data() lock-free.
    std::vector<float> mScratchInterleaved;
    int mMaxFrames = 0;

    // Teardown synchronization — stop() blocks until in-flight callbacks drain,
    // exactly like OboeBackend.
    std::mutex mStreamMutex;
    std::condition_variable mStopCondition;

    // Negotiated stream values, published after start().
    mutable StreamInfo mCachedStreamInfo;
    mutable std::atomic<bool> mStreamInfoValid{false};
    std::atomic<float> mOutputLatencyMs{0.0f};

    // Internal helpers (defined in the .mm).
    BackendResult openEngineLocked();
    void closeEngineLocked();
};

} // namespace watermelon_audio
