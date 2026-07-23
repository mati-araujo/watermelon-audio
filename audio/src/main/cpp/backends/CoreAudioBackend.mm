/**
 * CoreAudioBackend.mm
 *
 * Objective-C++ implementation of the Apple audio backend (WA-2.4).
 *
 * Signal path:
 *
 *     engine DSP callback (interleaved stereo float)
 *        -> AVAudioSourceNode render block  [RT thread]
 *        -> mainMixerNode
 *        -> outputNode  (hardware)
 *
 * The render block is a thin trampoline into the C++ mixer: it captures a raw
 * `this` pointer and touches only atomics and one pre-allocated scratch buffer.
 * No allocation, no lock, no ObjC messaging, no logging on the RT path.
 */

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>

#include "CoreAudioBackend.h"
#include "../platform/Logger.h"

#include <algorithm>
#include <cstring>

#define LOG_TAG "CoreAudioBackend"
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) wma::logMessage(wma::LogLevel::WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)

namespace watermelon_audio {

// =============================================================================
// PIMPL — holds the Objective-C engine objects. Under ARC these members are
// strong references; resetting mImpl (or nil-ing them in closeEngineLocked)
// releases the engine.
// =============================================================================
struct CoreAudioBackend::Impl {
    AVAudioEngine*     engine     = nil;
    AVAudioSourceNode* sourceNode = nil;
    AVAudioFormat*     format     = nil;
};

// =============================================================================
// Constructor / Destructor
// =============================================================================

CoreAudioBackend::CoreAudioBackend()
    : mImpl(std::make_unique<Impl>()) {
    LOGI("CoreAudioBackend created");
}

CoreAudioBackend::~CoreAudioBackend() {
    if (mIsRunning.load()) {
        stop();
    }
    LOGI("CoreAudioBackend destroyed");
}

// =============================================================================
// Configuration
// =============================================================================

void CoreAudioBackend::setCallback(IAudioCallback* callback) {
    mCallback.store(callback, std::memory_order_release);
}

void CoreAudioBackend::setSampleRate(int sampleRate) {
    mRequestedSampleRate = sampleRate;
}

void CoreAudioBackend::setBufferSize(int framesPerBuffer) {
    mRequestedBufferSize = framesPerBuffer;
}

void CoreAudioBackend::setFullDuplexEnabled(bool /*enable*/) {
    // Output-only backend (WA-2.4). Input capture is InputNode's job and remains
    // Android-only for now, so this is intentionally a no-op.
}

// =============================================================================
// Lifecycle
// =============================================================================

BackendResult CoreAudioBackend::start() {
    std::lock_guard<std::mutex> lock(mStreamMutex);

    if (mIsRunning.load()) {
        LOGW("Already running");
        return BackendResult::ERROR_ALREADY_RUNNING;
    }
    if (mCallback.load(std::memory_order_acquire) == nullptr) {
        LOGE("No callback set");
        return BackendResult::ERROR_NOT_INITIALIZED;
    }

    BackendResult result = openEngineLocked();
    if (result != BackendResult::OK) {
        closeEngineLocked();
        return result;
    }

    mStopping.store(false);
    mIsPaused.store(false);
    mIsRunning.store(true);
    mStreamInfoValid.store(false);

    LOGI("CoreAudioBackend started");
    return BackendResult::OK;
}

void CoreAudioBackend::stop() {
    std::unique_lock<std::mutex> lock(mStreamMutex);

    if (!mIsRunning.load()) {
        return;
    }

    LOGI("Stopping CoreAudioBackend...");

    // Signal the render block to stop producing and to wake us when it drains,
    // then stop the engine so no new callbacks are scheduled.
    mStopping.store(true);

    if (mImpl->engine != nil) {
        [mImpl->engine stop];
    }

    // Wait for any in-flight render callback to finish before tearing down.
    const auto timeout = std::chrono::milliseconds(1000);
    const bool finished = mStopCondition.wait_for(lock, timeout, [this] {
        return mActiveCallbacks.load() == 0;
    });
    if (!finished) {
        LOGW("Timeout waiting for render callbacks to drain");
    }

    closeEngineLocked();

    mIsRunning.store(false);
    mIsPaused.store(false);
    mStreamInfoValid.store(false);

    LOGI("CoreAudioBackend stopped");
}

void CoreAudioBackend::pause() {
    std::lock_guard<std::mutex> lock(mStreamMutex);

    if (!mIsRunning.load() || mIsPaused.load()) {
        return;
    }

    // Flip the flag first so any callback already scheduled emits silence, then
    // pause the engine (keeps the graph resident for a fast resume()).
    mIsPaused.store(true);
    if (mImpl->engine != nil) {
        [mImpl->engine pause];
    }
    LOGI("CoreAudioBackend paused");
}

void CoreAudioBackend::resume() {
    std::lock_guard<std::mutex> lock(mStreamMutex);

    if (!mIsRunning.load() || !mIsPaused.load()) {
        return;
    }

    if (mImpl->engine != nil) {
        NSError* err = nil;
        if (![mImpl->engine startAndReturnError:&err]) {
            LOGE("Failed to resume engine: %s",
                 err ? err.localizedDescription.UTF8String : "unknown");
            return;
        }
    }
    mIsPaused.store(false);
    LOGI("CoreAudioBackend resumed");
}

// =============================================================================
// State Queries
// =============================================================================

bool CoreAudioBackend::isRunning() const {
    return mIsRunning.load() && !mIsPaused.load();
}

float CoreAudioBackend::getOutputLatencyMs() const {
    return mOutputLatencyMs.load(std::memory_order_acquire);
}

StreamInfo CoreAudioBackend::getStreamInfo() const {
    if (mStreamInfoValid.load()) {
        return mCachedStreamInfo;
    }
    // Before start() (or after stop()) report the requested configuration.
    StreamInfo info;
    info.sampleRate      = mRequestedSampleRate > 0 ? mRequestedSampleRate : 48000;
    info.channelCount    = 2;
    info.framesPerBuffer = mRequestedBufferSize > 0 ? mRequestedBufferSize : 256;
    info.format          = AudioFormat::FLOAT_32;
    info.outputLatencyMs = 0.0f;
    info.isFullDuplex    = false;
    info.backendType     = BackendType::COREAUDIO;
    info.deviceName      = "Core Audio";
    return info;
}

// =============================================================================
// Engine setup / teardown (called with mStreamMutex held)
// =============================================================================

BackendResult CoreAudioBackend::openEngineLocked() {
    // ---- 1. Negotiate the session (iOS only; macOS has no AVAudioSession) ----
    double negotiatedSampleRate = mRequestedSampleRate > 0 ? mRequestedSampleRate : 48000;
    double ioBufferDuration     = 0.0;   // seconds
    float  sessionOutputLatency = 0.0f;  // seconds

#if TARGET_OS_IOS
    {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError* err = nil;

        // Output-only for now: playback category. (playAndRecord would be the
        // switch when input lands.)
        if (![session setCategory:AVAudioSessionCategoryPlayback error:&err]) {
            LOGW("setCategory failed: %s",
                 err ? err.localizedDescription.UTF8String : "unknown");
            err = nil;
        }

        if (mRequestedSampleRate > 0) {
            [session setPreferredSampleRate:(double)mRequestedSampleRate error:&err];
            err = nil;
        }
        if (mRequestedBufferSize > 0 && negotiatedSampleRate > 0) {
            const double dur = (double)mRequestedBufferSize / negotiatedSampleRate;
            [session setPreferredIOBufferDuration:dur error:&err];
            err = nil;
        }

        if (![session setActive:YES error:&err]) {
            LOGE("Failed to activate audio session: %s",
                 err ? err.localizedDescription.UTF8String : "unknown");
            return BackendResult::ERROR_STREAM_FAILED;
        }

        // Read back the values the system actually negotiated — these can differ
        // from what we asked for.
        negotiatedSampleRate = session.sampleRate;
        ioBufferDuration     = session.IOBufferDuration;
        sessionOutputLatency = (float)session.outputLatency;
    }
#endif // TARGET_OS_IOS

    if (negotiatedSampleRate <= 0) {
        negotiatedSampleRate = 48000;
    }

    // Derive the frames-per-buffer actually in effect.
    int negotiatedFrames = mRequestedBufferSize > 0 ? mRequestedBufferSize : 256;
    if (ioBufferDuration > 0.0) {
        negotiatedFrames = (int)std::lround(ioBufferDuration * negotiatedSampleRate);
    }
    if (negotiatedFrames <= 0) {
        negotiatedFrames = 256;
    }

    // ---- 2. Pre-allocate the deinterleave scratch (never touched at RT) ----
    // Size generously so the render block's frameCount can never exceed it; the
    // block also clamps defensively.
    mMaxFrames = std::max({mRequestedBufferSize, negotiatedFrames, 4096});
    mScratchInterleaved.assign(static_cast<size_t>(mMaxFrames) * 2, 0.0f);

    // ---- 3. Build the interleaved stereo float format ----
    AVAudioFormat* format =
        [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                         sampleRate:negotiatedSampleRate
                                           channels:2
                                        interleaved:YES];
    if (format == nil) {
        LOGE("Failed to build AVAudioFormat");
        return BackendResult::ERROR_INVALID_CONFIG;
    }
    mImpl->format = format;

    // ---- 4. The render block: a raw trampoline into the C++ mixer ----
    CoreAudioBackend* backend = this;  // raw capture, no ObjC retain

    AVAudioSourceNodeRenderBlock renderBlock =
        ^OSStatus(BOOL* isSilence,
                  const AudioTimeStamp* /*timestamp*/,
                  AVAudioFrameCount frameCount,
                  AudioBufferList* outputData) {

        backend->mActiveCallbacks.fetch_add(1, std::memory_order_acquire);

        const UInt32 numBuffers = outputData->mNumberBuffers;
        IAudioCallback* cb = backend->mCallback.load(std::memory_order_acquire);

        // Paused or no callback: emit silence.
        if (cb == nullptr || backend->mIsPaused.load(std::memory_order_acquire)) {
            for (UInt32 b = 0; b < numBuffers; ++b) {
                std::memset(outputData->mBuffers[b].mData, 0,
                            outputData->mBuffers[b].mDataByteSize);
            }
            *isSilence = YES;
            if (backend->mActiveCallbacks.fetch_sub(1, std::memory_order_release) == 1 &&
                backend->mStopping.load(std::memory_order_acquire)) {
                backend->mStopCondition.notify_all();
            }
            return noErr;
        }

        int frames = static_cast<int>(frameCount);
        if (frames > backend->mMaxFrames) {
            frames = backend->mMaxFrames;  // defensive clamp; should never trip
        }

        if (numBuffers == 1) {
            // Interleaved: the engine honored our format — hand the buffer
            // straight to the mixer, zero conversion.
            float* out = static_cast<float*>(outputData->mBuffers[0].mData);
            cb->onAudioReady(out, /*inputData=*/nullptr, frames);
        } else {
            // Deinterleaved fallback: mix into the pre-allocated interleaved
            // scratch, then split into the L/R planar buffers.
            float* scratch = backend->mScratchInterleaved.data();
            cb->onAudioReady(scratch, /*inputData=*/nullptr, frames);

            float* left  = static_cast<float*>(outputData->mBuffers[0].mData);
            float* right = static_cast<float*>(outputData->mBuffers[1].mData);
            for (int i = 0; i < frames; ++i) {
                left[i]  = scratch[2 * i];
                right[i] = scratch[2 * i + 1];
            }
        }

        *isSilence = NO;

        if (backend->mActiveCallbacks.fetch_sub(1, std::memory_order_release) == 1 &&
            backend->mStopping.load(std::memory_order_acquire)) {
            backend->mStopCondition.notify_all();
        }
        return noErr;
    };

    // ---- 5. Wire up the engine graph ----
    AVAudioEngine* engine = [[AVAudioEngine alloc] init];
    AVAudioSourceNode* sourceNode =
        [[AVAudioSourceNode alloc] initWithFormat:format renderBlock:renderBlock];

    mImpl->engine     = engine;
    mImpl->sourceNode = sourceNode;

    [engine attachNode:sourceNode];
    // Touching mainMixerNode instantiates it and lazily connects it to
    // outputNode. Connect the source into it with our interleaved format.
    AVAudioMixerNode* mixer = engine.mainMixerNode;
    [engine connect:sourceNode to:mixer format:format];

    // ---- 6. Start ----
    [engine prepare];
    NSError* startErr = nil;
    if (![engine startAndReturnError:&startErr]) {
        LOGE("Failed to start AVAudioEngine: %s",
             startErr ? startErr.localizedDescription.UTF8String : "unknown");
        return BackendResult::ERROR_STREAM_FAILED;
    }

    // ---- 7. Publish the negotiated stream info ----
    const float ioBufferMs = (ioBufferDuration > 0.0)
        ? (float)(ioBufferDuration * 1000.0)
        : ((float)negotiatedFrames / (float)negotiatedSampleRate) * 1000.0f;
    const float outLatencyMs = sessionOutputLatency * 1000.0f + ioBufferMs;
    mOutputLatencyMs.store(outLatencyMs, std::memory_order_release);

    mCachedStreamInfo.sampleRate      = (int)std::lround(negotiatedSampleRate);
    mCachedStreamInfo.channelCount    = 2;
    mCachedStreamInfo.framesPerBuffer = negotiatedFrames;
    mCachedStreamInfo.format          = AudioFormat::FLOAT_32;
    mCachedStreamInfo.outputLatencyMs = outLatencyMs;
    mCachedStreamInfo.inputLatencyMs  = 0.0f;
    mCachedStreamInfo.isFullDuplex    = false;
    mCachedStreamInfo.backendType     = BackendType::COREAUDIO;
    mCachedStreamInfo.deviceName      = "Core Audio";
    mStreamInfoValid.store(true);

    LOGI("=== CoreAudio OUTPUT STREAM OPENED ===");
    LOGI("  Sample rate:      %d Hz", mCachedStreamInfo.sampleRate);
    LOGI("  Frames/buffer:    %d", negotiatedFrames);
    LOGI("  Output latency:   %.2f ms", outLatencyMs);
    LOGI("======================================");

    return BackendResult::OK;
}

void CoreAudioBackend::closeEngineLocked() {
    if (mImpl->engine != nil) {
        [mImpl->engine stop];
    }
    // Dropping the strong refs releases the engine, source node and its render
    // block (ARC). Done off the RT path.
    mImpl->sourceNode = nil;
    mImpl->engine     = nil;
    mImpl->format     = nil;

#if TARGET_OS_IOS
    NSError* err = nil;
    [[AVAudioSession sharedInstance] setActive:NO
                                   withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                                         error:&err];
#endif
}

} // namespace watermelon_audio
