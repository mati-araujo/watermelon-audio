/**
 * CoreAudioBackend.h
 *
 * Apple (iOS / macOS) audio backend built on AVAudioEngine + AVAudioSourceNode.
 * This is the CoreAudio counterpart to OboeBackend: AAudio is to Android what
 * AVAudioEngine is to Apple.
 *
 * Scope: output (WA-2.4) plus full-duplex capture. Capture rides the SAME
 * AVAudioEngine as the output — an AVAudioSinkNode hanging off `engine.inputNode`
 * — rather than a second engine. One engine means one clock domain, so there is
 * no input/output drift to resample away, and one buffer less of latency.
 *
 * The captured audio is handed to the engine through the `inputData` argument of
 * IAudioCallback::onAudioReady, which is the same path the USB backend already
 * uses. AudioEngine routes it to direct INPUT_FX (guitar FX) or to
 * InputNode::feedExternalInput() (vocoder / MIX) with no platform-specific code.
 *
 * This header is deliberately free of any Objective-C: it is included from
 * PlatformBackends.cpp (compiled as C++), so every Apple/ObjC type is hidden
 * behind a PIMPL (`Impl`) defined in the .mm. Only the .mm is Objective-C++.
 *
 * Thread Safety:
 * - Configuration methods: call from a control thread before start().
 * - start/stop/pause/resume: serialized by mStreamMutex.
 * - The render and capture blocks: run on CoreAudio real-time threads (two
 *   different ones). They touch only atomics, the pre-allocated scratch buffers
 *   and an SPSC ring — never the PIMPL, never ObjC, never a lock or allocation.
 */

#pragma once

#include "IAudioBackend.h"
#include "CaptureGapMailbox.h"
#include "../dsp/LockFreeRingBuffer.h"

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
    float getInputLatencyMs() const override;
    BackendType getType() const override { return BackendType::COREAUDIO; }
    bool supportsFullDuplex() const override { return true; }
    bool supportsPause() const override { return true; }

    /**
     * True when a capture stream is actually live.
     *
     * Distinct from supportsFullDuplex(): capture can be requested and still not
     * happen — no mic permission, a simulator with no input device, or a session
     * the host app configured for playback only. Callers that need to know
     * whether input is really flowing must ask this, not the capability.
     */
    bool isCaptureActive() const { return mCaptureActive.load(std::memory_order_acquire); }

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

    // -------------------------------------------------------------------------
    // Capture (full duplex).
    // -------------------------------------------------------------------------

    // Requested by setFullDuplexEnabled(). Like OboeBackend, a change only takes
    // effect on the next start().
    //
    // **Atomic y NO bajo mStreamMutex, a proposito.** start() retiene mStreamMutex
    // toda la apertura, asi que un setter que lo pidiera bloquearia al que llama
    // durante una reapertura entera — que es justo lo que se saco de
    // BackendManager. El flag es un bool: un atomic alcanza y no bloquea a nadie.
    std::atomic<bool> mFullDuplexRequested{false};

    // A capture stream is open and the sink block is running.
    std::atomic<bool> mCaptureActive{false};

    // Gates *delivery* of captured audio to the callback, so a mode change can
    // turn input off and on again live without restarting the engine. Capture
    // itself keeps running — tearing down the sink node at runtime would mean
    // reconfiguring the audio session, which glitches output.
    std::atomic<bool> mDeliverInput{false};

    // Latched once the ring holds a full block. Without it the first blocks would
    // alternate between "input available" and "not available", and AudioEngine
    // reads a null inputData as "not in INPUT_FX mode" — the mode would flap.
    std::atomic<bool> mCapturePrimed{false};

    // Capture thread -> render thread. SPSC: the sink block is the only writer,
    // the render block the only reader. Sized in start() to one second.
    LockFreeRingBuffer mInputRing;

    /// REQ-009 S3 (3.4b). El cruce entre el callback de ENTRADA —que es el que
    /// detecta el overrun de `mInputRing`— y el de SALIDA, que es el unico que
    /// puede posicionar la costura porque es el que escribe el ring del
    /// afinador. Ver `CaptureGapMailbox`.
    wma::backends::CaptureGapMailbox mCaptureGap;

    // Interleaved stereo scratch, one per RT thread so they never share memory.
    // Both sized in start() to mMaxFrames * 2 and never resized while running.
    std::vector<float> mCaptureScratch;  // sink block only
    std::vector<float> mInputScratch;    // render block only

    std::atomic<float> mInputLatencyMs{0.0f};

    // Teardown synchronization — stop() blocks until in-flight callbacks drain,
    // exactly like OboeBackend.
    std::mutex mStreamMutex;
    std::condition_variable mStopCondition;

    // Negotiated stream values, published after start().
    // Lo publica la apertura del stream y lo vacía stop(); getStreamInfo() sólo
    // lo copia. El mutex es lo que cumple el contrato de IAudioBackend: seguro
    // contra un start()/stop() concurrente. Ver los tres sitios en el .mm.
    mutable std::mutex mStreamInfoMutex;
    mutable StreamInfo mCachedStreamInfo;
    mutable std::atomic<bool> mStreamInfoValid{false};
    std::atomic<float> mOutputLatencyMs{0.0f};

    // Internal helpers (defined in the .mm).
    BackendResult openEngineLocked();
    void closeEngineLocked();
};

} // namespace watermelon_audio
