/**
 * CoreAudioBackend.mm
 *
 * Objective-C++ implementation of the Apple audio backend (WA-2.4).
 *
 * Signal path (output):
 *
 *     engine DSP callback (interleaved stereo float)
 *        -> AVAudioSourceNode render block  [RT thread]
 *        -> mainMixerNode
 *        -> outputNode  (hardware)
 *
 * Signal path (capture, full duplex):
 *
 *     inputNode (hardware)
 *        -> AVAudioSinkNode receiver block  [capture RT thread]
 *        -> mInputRing (SPSC, lock-free)
 *        -> AVAudioSourceNode render block  [output RT thread]
 *        -> onAudioReady(output, input, frames)
 *
 * Both directions ride the SAME AVAudioEngine, so they share one clock domain
 * and the ring never has to absorb drift — only the phase offset between the two
 * callbacks, which is one buffer.
 *
 * The blocks are thin trampolines into C++: they capture a raw `this` pointer
 * and touch only atomics, pre-allocated scratch and the ring. No allocation, no
 * lock, no ObjC messaging, no logging on the RT path.
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
    AVAudioSinkNode*   sinkNode   = nil;
    AVAudioFormat*     format     = nil;
};

// =============================================================================
// Constructor / Destructor
// =============================================================================

CoreAudioBackend::CoreAudioBackend()
    : mImpl(std::make_unique<Impl>())
    // Placeholder capacity; openEngineLocked() resizes to one second at the
    // negotiated rate. Constructing at zero would make resize() the only thing
    // standing between a mis-sequenced start() and a division by zero.
    , mInputRing(48000 * 2) {
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

void CoreAudioBackend::setFullDuplexEnabled(bool enable) {
    // Two effects, on purpose:
    //
    // 1. The request itself, applied on the next start() — same contract as
    //    OboeBackend, where this is also just a flag read at stream-open time.
    //    Whether to open a capture stream at all has to be decided before the
    //    session is configured: an app that never wants input should never ask
    //    the user for the microphone, and playAndRecord changes output routing.
    //
    // 2. Live gating of delivery when capture is ALREADY running. Mode changes
    //    call this while the engine streams (setFullDuplexEnabled(mode == 2)),
    //    and detaching the sink node at runtime would mean reconfiguring the
    //    audio session — an audible glitch on the output path. So capture keeps
    //    running and only the handoff to the callback flips.
    {
        std::lock_guard<std::mutex> lock(mStreamMutex);
        mFullDuplexRequested = enable;
    }

    mDeliverInput.store(enable, std::memory_order_release);

    if (enable && !mCaptureActive.load(std::memory_order_acquire) &&
        mIsRunning.load(std::memory_order_acquire)) {
        LOGW("Full duplex requested while running without a capture stream — "
             "takes effect on the next start()");
    }
}

float CoreAudioBackend::getInputLatencyMs() const {
    return mInputLatencyMs.load(std::memory_order_acquire);
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
    // Before start() this is the *request*, not a fact — whether a capture stream
    // actually opens depends on microphone access, which is only known at start().
    info.isFullDuplex    = mFullDuplexRequested;
    info.backendType     = BackendType::COREAUDIO;
    info.deviceName      = "Core Audio";
    return info;
}

// =============================================================================
// Engine setup / teardown (called with mStreamMutex held)
// =============================================================================

BackendResult CoreAudioBackend::openEngineLocked() {
    // ---- 1. Negotiate the session (iOS only; macOS has no AVAudioSession) ----
    const bool wantCapture = mFullDuplexRequested;

    double negotiatedSampleRate = mRequestedSampleRate > 0 ? mRequestedSampleRate : 48000;
    double ioBufferDuration     = 0.0;   // seconds
    float  sessionOutputLatency = 0.0f;  // seconds
    float  sessionInputLatency  = 0.0f;  // seconds

#if TARGET_OS_IOS
    {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError* err = nil;

        // playAndRecord only when capture was actually asked for: it prompts the
        // user for the microphone and, without defaultToSpeaker, routes output to
        // the receiver instead of the speaker. An output-only app should pay
        // neither price.
        //
        // Note the overlap with AudioSessionManager (WA-3.4, Kotlin): the host app
        // may also configure the session. The options below mirror it so whichever
        // runs last leaves the same category — but the backend never *downgrades*
        // a recording session to playback, which is the case that would silently
        // kill capture.
        BOOL categoryOk;
        if (wantCapture) {
            categoryOk = [session setCategory:AVAudioSessionCategoryPlayAndRecord
                                  withOptions:(AVAudioSessionCategoryOptionDefaultToSpeaker |
                                               AVAudioSessionCategoryOptionAllowBluetoothA2DP)
                                        error:&err];
        } else {
            categoryOk = [session setCategory:AVAudioSessionCategoryPlayback error:&err];
        }
        if (!categoryOk) {
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
        sessionInputLatency  = (float)session.inputLatency;
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

    // Capture buffers. One scratch per RT thread — the sink and render blocks run
    // on different CoreAudio threads and must never share a scratch. The ring is
    // one second of stereo, far more than the phase offset between the two
    // callbacks needs; the slack is what absorbs a late capture block instead of
    // turning it into a dropout.
    mCaptureScratch.assign(static_cast<size_t>(mMaxFrames) * 2, 0.0f);
    mInputScratch.assign(static_cast<size_t>(mMaxFrames) * 2, 0.0f);
    mInputRing.resize(static_cast<size_t>(std::lround(negotiatedSampleRate)) * 2);
    mCapturePrimed.store(false, std::memory_order_release);
    mCaptureActive.store(false, std::memory_order_release);

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

        // ---- Pull one block of captured audio, if there is one ----
        // A null inputData means "no input this block", which AudioEngine reads
        // as "not in INPUT_FX mode". So this must be stable, not per-block noise:
        // the ring has to hold a full block once (primed) before delivery starts,
        // and from then on an underrun yields silence rather than a null.
        const float* inputData = nullptr;
        if (backend->mDeliverInput.load(std::memory_order_acquire) &&
            backend->mCaptureActive.load(std::memory_order_acquire)) {

            const size_t needed = static_cast<size_t>(frames) * 2;

            if (!backend->mCapturePrimed.load(std::memory_order_acquire)) {
                if (backend->mInputRing.availableToRead() >= needed) {
                    backend->mCapturePrimed.store(true, std::memory_order_release);
                }
            }

            if (backend->mCapturePrimed.load(std::memory_order_acquire)) {
                // read() fills with silence and returns false on underrun, which
                // is exactly the wanted behaviour — keep the mode, drop audio.
                backend->mInputRing.read(backend->mInputScratch.data(), needed);
                inputData = backend->mInputScratch.data();
            }
        }

        if (numBuffers == 1) {
            // Interleaved: the engine honored our format — hand the buffer
            // straight to the mixer, zero conversion.
            float* out = static_cast<float*>(outputData->mBuffers[0].mData);
            cb->onAudioReady(out, inputData, frames);
        } else {
            // Deinterleaved fallback: mix into the pre-allocated interleaved
            // scratch, then split into the L/R planar buffers.
            float* scratch = backend->mScratchInterleaved.data();
            cb->onAudioReady(scratch, inputData, frames);

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

    // ---- 5b. Capture branch (full duplex) ----
    // Failure here is never fatal: an app whose user denied the microphone, or a
    // simulator with no input device, must still get its output. Capture stays
    // off and isCaptureActive() reports the truth.
    //
    // The @try is not defensive padding. `engine.inputNode` and `connect:` report
    // "there is no usable input" by RAISING, not by returning an error — so
    // without it, a user who denied the microphone would take the whole process
    // down instead of losing capture. Nothing inside allocates C++ state that a
    // raise would leak: the sink node is ARC-managed and the flags are set last.
    if (wantCapture) @try {
        AVAudioInputNode* input = engine.inputNode;
        AVAudioFormat* inputFormat = [input inputFormatForBus:0];

        const double inRate     = inputFormat.sampleRate;
        const AVAudioChannelCount inChannels = inputFormat.channelCount;

        if (inRate <= 0.0 || inChannels == 0) {
            // The documented tell for "there is no usable input device": the
            // hardware format comes back all zeros rather than an error.
            LOGW("Capture requested but the input format is empty "
                 "(rate=%.0f, channels=%u) — no microphone access. Output only.",
                 inRate, (unsigned)inChannels);
        } else {
            const int captureChannels = (int)inChannels;

            AVAudioSinkNodeReceiverBlock captureBlock =
                ^OSStatus(const AudioTimeStamp* /*timestamp*/,
                          AVAudioFrameCount frameCount,
                          const AudioBufferList* inputBuffers) {

                backend->mActiveCallbacks.fetch_add(1, std::memory_order_acquire);

                int frames = static_cast<int>(frameCount);
                if (frames > backend->mMaxFrames) {
                    frames = backend->mMaxFrames;  // defensive; matches the scratch
                }

                if (frames > 0 && !backend->mStopping.load(std::memory_order_acquire)) {
                    float* scratch = backend->mCaptureScratch.data();
                    const UInt32 inBuffers = inputBuffers->mNumberBuffers;

                    // Normalise whatever the OS hands us to interleaved stereo:
                    // that is the only shape the rest of the engine speaks. The
                    // built-in iPhone mic is mono, so the duplicate path is the
                    // common one, not the exotic one.
                    if (inBuffers == 1 && captureChannels == 1) {
                        const float* src =
                            static_cast<const float*>(inputBuffers->mBuffers[0].mData);
                        for (int i = 0; i < frames; ++i) {
                            scratch[2 * i]     = src[i];
                            scratch[2 * i + 1] = src[i];
                        }
                    } else if (inBuffers == 1) {
                        // Interleaved with >= 2 channels: take the first two.
                        const float* src =
                            static_cast<const float*>(inputBuffers->mBuffers[0].mData);
                        for (int i = 0; i < frames; ++i) {
                            scratch[2 * i]     = src[i * captureChannels];
                            scratch[2 * i + 1] = src[i * captureChannels + 1];
                        }
                    } else {
                        // Planar: one buffer per channel. Mono planar duplicates.
                        const float* left =
                            static_cast<const float*>(inputBuffers->mBuffers[0].mData);
                        const float* right = (inBuffers >= 2)
                            ? static_cast<const float*>(inputBuffers->mBuffers[1].mData)
                            : left;
                        for (int i = 0; i < frames; ++i) {
                            scratch[2 * i]     = left[i];
                            scratch[2 * i + 1] = right[i];
                        }
                    }

                    // Overflow drops the block instead of blocking. The reader is
                    // the output RT thread; making the capture thread wait for it
                    // would trade a dropout for an xrun on both paths.
                    backend->mInputRing.write(scratch, static_cast<size_t>(frames) * 2);
                }

                if (backend->mActiveCallbacks.fetch_sub(1, std::memory_order_release) == 1 &&
                    backend->mStopping.load(std::memory_order_acquire)) {
                    backend->mStopCondition.notify_all();
                }
                return noErr;
            };

            AVAudioSinkNode* sinkNode =
                [[AVAudioSinkNode alloc] initWithReceiverBlock:captureBlock];
            mImpl->sinkNode = sinkNode;

            [engine attachNode:sinkNode];
            // Connect with the hardware's own format — forcing ours here is what
            // makes AVAudioEngine throw on a mic that is 44.1 kHz mono.
            [engine connect:input to:sinkNode format:inputFormat];

            mCaptureActive.store(true, std::memory_order_release);
            LOGI("Capture attached: %.0f Hz, %u ch", inRate, (unsigned)inChannels);
        }
    } @catch (NSException* e) {
        LOGW("Capture setup raised (%s) — continuing output-only",
             e.reason ? e.reason.UTF8String : "unknown");
        mImpl->sinkNode = nil;
        mCaptureActive.store(false, std::memory_order_release);
    }

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

    const bool captureLive = mCaptureActive.load(std::memory_order_acquire);

    // Input latency is the hardware capture path plus the ring's phase offset:
    // the sink block writes a block that the render block reads on its next turn,
    // which is one buffer. Reporting only the session value would understate the
    // round trip by exactly the amount this design costs.
    const float inLatencyMs = captureLive
        ? (sessionInputLatency * 1000.0f + ioBufferMs)
        : 0.0f;
    mInputLatencyMs.store(inLatencyMs, std::memory_order_release);

    mCachedStreamInfo.sampleRate      = (int)std::lround(negotiatedSampleRate);
    mCachedStreamInfo.channelCount    = 2;
    mCachedStreamInfo.framesPerBuffer = negotiatedFrames;
    mCachedStreamInfo.format          = AudioFormat::FLOAT_32;
    mCachedStreamInfo.outputLatencyMs = outLatencyMs;
    mCachedStreamInfo.inputLatencyMs  = inLatencyMs;
    mCachedStreamInfo.isFullDuplex    = captureLive;
    mCachedStreamInfo.backendType     = BackendType::COREAUDIO;
    mCachedStreamInfo.deviceName      = "Core Audio";
    mStreamInfoValid.store(true);

    LOGI("=== CoreAudio STREAM OPENED ===");
    LOGI("  Sample rate:      %d Hz", mCachedStreamInfo.sampleRate);
    LOGI("  Frames/buffer:    %d", negotiatedFrames);
    LOGI("  Output latency:   %.2f ms", outLatencyMs);
    LOGI("  Capture:          %s", captureLive ? "active" : "off");
    if (captureLive) {
        LOGI("  Input latency:    %.2f ms", inLatencyMs);
    }
    LOGI("===============================");

    return BackendResult::OK;
}

void CoreAudioBackend::closeEngineLocked() {
    if (mImpl->engine != nil) {
        [mImpl->engine stop];
    }

    // Capture goes down first so nothing reads a half-torn-down state: the flag
    // is what both RT blocks consult before touching the ring.
    mCaptureActive.store(false, std::memory_order_release);
    mCapturePrimed.store(false, std::memory_order_release);
    mInputRing.clear();

    // Dropping the strong refs releases the engine, its nodes and their blocks
    // (ARC). Done off the RT path.
    mImpl->sinkNode   = nil;
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
