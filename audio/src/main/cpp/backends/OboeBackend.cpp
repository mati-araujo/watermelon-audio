/**
 * OboeBackend.cpp
 *
 * Implementation of IAudioBackend using Google's Oboe library.
 */

#include "OboeBackend.h"
#include "../platform/Logger.h"
#include "../platform/Platform.h"

#define LOG_TAG "OboeBackend"
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) wma::logMessage(wma::LogLevel::WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)

namespace watermelon_audio {

// =============================================================================
// Constructor / Destructor
// =============================================================================

OboeBackend::OboeBackend() {
    LOGI("OboeBackend created");
}

OboeBackend::~OboeBackend() {
    if (mIsRunning.load()) {
        stop();
    }
    LOGI("OboeBackend destroyed");
}

// =============================================================================
// IAudioBackend Implementation
// =============================================================================

BackendResult OboeBackend::start() {
    std::lock_guard<std::mutex> lock(mStreamMutex);

    if (mIsRunning.load()) {
        LOGW("Already running");
        return BackendResult::ERROR_ALREADY_RUNNING;
    }

    if (!mCallback) {
        LOGE("No callback set");
        return BackendResult::ERROR_NOT_INITIALIZED;
    }

    // Enable denormal flush for audio optimization
    enableDenormalFlush();

    // Open output stream
    BackendResult result = openOutputStream();
    if (result != BackendResult::OK) {
        return result;
    }

    // Open input stream if full-duplex enabled
    if (mFullDuplexEnabled) {
        result = openInputStream();
        if (result != BackendResult::OK) {
            LOGW("Failed to open input stream, continuing without full-duplex");
            // Continue without input - not a fatal error
        }
    }

    // Start output stream
    oboe::Result oboeResult = mOutputStream->requestStart();
    if (oboeResult != oboe::Result::OK) {
        LOGE("Failed to start output stream: %s", oboe::convertToText(oboeResult));
        closeStreams();
        return BackendResult::ERROR_STREAM_FAILED;
    }

    // Start input stream if available
    if (mInputStream) {
        oboeResult = mInputStream->requestStart();
        if (oboeResult != oboe::Result::OK) {
            LOGW("Failed to start input stream: %s", oboe::convertToText(oboeResult));
            mInputStream->close();
            mInputStream.reset();
        }
    }

    mIsRunning.store(true);
    mIsPaused.store(false);
    mStreamError.store(false);
    mStreamInfoValid.store(false);

    LOGI("OboeBackend started successfully");
    return BackendResult::OK;
}

void OboeBackend::stop() {
    std::unique_lock<std::mutex> lock(mStreamMutex);

    if (!mIsRunning.load()) {
        return;
    }

    LOGI("Stopping OboeBackend...");

    // Stop streams first to prevent new callbacks
    if (mOutputStream) {
        mOutputStream->stop();
    }
    if (mInputStream) {
        mInputStream->stop();
    }

    // Wait for active callbacks to finish
    auto timeout = std::chrono::milliseconds(1000);
    bool finished = mStopCondition.wait_for(lock, timeout, [this] {
        return mActiveCallbacks.load() == 0;
    });

    if (!finished) {
        LOGW("Timeout waiting for callbacks to finish");
    }

    // Close streams
    closeStreams();

    mIsRunning.store(false);
    mIsPaused.store(false);
    mStreamInfoValid.store(false);

    LOGI("OboeBackend stopped");
}

void OboeBackend::pause() {
    std::lock_guard<std::mutex> lock(mStreamMutex);

    if (!mIsRunning.load() || mIsPaused.load()) {
        return;
    }

    if (mOutputStream) {
        mOutputStream->pause();
    }
    if (mInputStream) {
        mInputStream->pause();
    }

    mIsPaused.store(true);
    LOGI("OboeBackend paused");
}

void OboeBackend::resume() {
    std::lock_guard<std::mutex> lock(mStreamMutex);

    if (!mIsRunning.load() || !mIsPaused.load()) {
        return;
    }

    if (mOutputStream) {
        mOutputStream->start();
    }
    if (mInputStream) {
        mInputStream->start();
    }

    mIsPaused.store(false);
    LOGI("OboeBackend resumed");
}

void OboeBackend::setCallback(IAudioCallback* callback) {
    mCallback = callback;
}

void OboeBackend::setSampleRate(int sampleRate) {
    mRequestedSampleRate = sampleRate;
}

void OboeBackend::setBufferSize(int framesPerBuffer) {
    mRequestedBufferSize = framesPerBuffer;
}

void OboeBackend::setFullDuplexEnabled(bool enable) {
    mFullDuplexEnabled = enable;
}

StreamInfo OboeBackend::getStreamInfo() const {
    if (mStreamInfoValid.load()) {
        return mCachedStreamInfo;
    }

    if (!mOutputStream) {
        return StreamInfo{};
    }

    // Update cache
    const_cast<OboeBackend*>(this)->updateStreamInfo();
    return mCachedStreamInfo;
}

bool OboeBackend::isRunning() const {
    return mIsRunning.load() && !mIsPaused.load();
}

float OboeBackend::getOutputLatencyMs() const {
    if (!mOutputStream) {
        return 0.0f;
    }

    auto result = mOutputStream->calculateLatencyMillis();
    if (result.error() == oboe::Result::OK) {
        return static_cast<float>(result.value());
    }

    // Fallback: estimate from buffer size
    int bufferSize = mOutputStream->getBufferSizeInFrames();
    int sampleRate = mOutputStream->getSampleRate();
    if (sampleRate > 0) {
        return (static_cast<float>(bufferSize) / sampleRate) * 1000.0f;
    }

    return 0.0f;
}

float OboeBackend::getInputLatencyMs() const {
    if (!mInputStream) {
        return 0.0f;
    }

    auto result = mInputStream->calculateLatencyMillis();
    if (result.error() == oboe::Result::OK) {
        return static_cast<float>(result.value());
    }

    return 0.0f;
}

int32_t OboeBackend::getXRunCount() const {
    if (!mOutputStream) {
        return 0;
    }
    auto result = mOutputStream->getXRunCount();
    return (result.error() == oboe::Result::OK) ? result.value() : 0;
}

void OboeBackend::clearError() {
    mStreamError.store(false);
    mLastErrorCode.store(0);
}

// =============================================================================
// Oboe Callback Implementation
// =============================================================================

oboe::DataCallbackResult OboeBackend::onAudioReady(
    oboe::AudioStream* oboeStream,
    void* audioData,
    int32_t numFrames) {

    // Track active callbacks for safe shutdown
    mActiveCallbacks.fetch_add(1);

    // Early exit if paused or no callback
    if (mIsPaused.load() || !mCallback) {
        // Fill with silence
        std::memset(audioData, 0, numFrames * oboeStream->getChannelCount() * sizeof(float));
        mActiveCallbacks.fetch_sub(1);
        if (mActiveCallbacks.load() == 0) {
            mStopCondition.notify_all();
        }
        return oboe::DataCallbackResult::Continue;
    }

    // Get input data if available (full-duplex)
    const float* inputData = nullptr;
    // Note: Full-duplex input handling would go here
    // For now, we only support output

    // Call the audio callback
    auto result = mCallback->onAudioReady(
        static_cast<float*>(audioData),
        inputData,
        numFrames
    );

    mActiveCallbacks.fetch_sub(1);
    if (mActiveCallbacks.load() == 0) {
        mStopCondition.notify_all();
    }

    return (result == IAudioCallback::Result::CONTINUE)
        ? oboe::DataCallbackResult::Continue
        : oboe::DataCallbackResult::Stop;
}

void OboeBackend::onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) {
    LOGE("Stream error after close: %s", oboe::convertToText(error));

    mStreamError.store(true);
    mLastErrorCode.store(static_cast<int>(error));

    // Notify callback of error
    if (mCallback) {
        BackendError backendError = BackendError::FATAL;

        if (error == oboe::Result::ErrorDisconnected) {
            backendError = BackendError::DEVICE_DISCONNECTED;
        }

        mCallback->onBackendError(backendError);
    }
}

void OboeBackend::onErrorBeforeClose(oboe::AudioStream* stream, oboe::Result error) {
    LOGW("Stream error before close: %s", oboe::convertToText(error));

    mStreamError.store(true);
    mLastErrorCode.store(static_cast<int>(error));
}

// =============================================================================
// Internal Methods
// =============================================================================

BackendResult OboeBackend::openOutputStream() {
    oboe::AudioStreamBuilder builder;

    builder.setDirection(oboe::Direction::Output)
           ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
           ->setSharingMode(oboe::SharingMode::Shared)  // Allow device routing changes
           ->setFormat(oboe::AudioFormat::Float)
           ->setChannelCount(oboe::ChannelCount::Stereo)
           ->setCallback(this)
           ->setErrorCallback(this);

    // Apply requested sample rate
    if (mRequestedSampleRate > 0) {
        builder.setSampleRate(mRequestedSampleRate);
        LOGI("Using requested sample rate: %d Hz", mRequestedSampleRate);
    }

    // Open stream
    oboe::Result result = builder.openStream(mOutputStream);
    if (result != oboe::Result::OK) {
        LOGE("Failed to open output stream: %s", oboe::convertToText(result));
        return BackendResult::ERROR_STREAM_FAILED;
    }

    // Configure buffer size
    int framesPerBurst = mOutputStream->getFramesPerBurst();
    int targetBufferSize = (mRequestedBufferSize > 0)
        ? mRequestedBufferSize
        : framesPerBurst * 2;  // Double buffering

    // Ensure minimum buffer size
    if (targetBufferSize < 256) {
        targetBufferSize = 256;
    }

    mOutputStream->setBufferSizeInFrames(targetBufferSize);
    int actualBufferSize = mOutputStream->getBufferSizeInFrames();

    // Log stream info
    LOGI("=== OUTPUT STREAM OPENED ===");
    LOGI("  Sample rate: %d Hz", mOutputStream->getSampleRate());
    LOGI("  Channel count: %d", mOutputStream->getChannelCount());
    LOGI("  Sharing mode: %s",
         mOutputStream->getSharingMode() == oboe::SharingMode::Shared ? "Shared" : "Exclusive");
    LOGI("  Performance mode: %s",
         mOutputStream->getPerformanceMode() == oboe::PerformanceMode::LowLatency ? "LowLatency" : "Other");
    LOGI("  Device ID: %d", mOutputStream->getDeviceId());
    LOGI("  Frames per burst: %d", framesPerBurst);
    LOGI("  Buffer size: %d frames", actualBufferSize);
    LOGI("  Estimated latency: %.1f ms",
         (float)actualBufferSize / mOutputStream->getSampleRate() * 1000.0f);
    LOGI("==============================");

    return BackendResult::OK;
}

BackendResult OboeBackend::openInputStream() {
    if (!mOutputStream) {
        return BackendResult::ERROR_NOT_INITIALIZED;
    }

    oboe::AudioStreamBuilder builder;

    builder.setDirection(oboe::Direction::Input)
           ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
           ->setSharingMode(oboe::SharingMode::Shared)
           ->setFormat(oboe::AudioFormat::Float)
           ->setChannelCount(oboe::ChannelCount::Stereo)
           ->setSampleRate(mOutputStream->getSampleRate());  // Match output

    oboe::Result result = builder.openStream(mInputStream);
    if (result != oboe::Result::OK) {
        LOGE("Failed to open input stream: %s", oboe::convertToText(result));
        return BackendResult::ERROR_STREAM_FAILED;
    }

    LOGI("Input stream opened at %d Hz", mInputStream->getSampleRate());
    return BackendResult::OK;
}

void OboeBackend::closeStreams() {
    if (mOutputStream) {
        mOutputStream->close();
        mOutputStream.reset();
    }
    if (mInputStream) {
        mInputStream->close();
        mInputStream.reset();
    }
}

void OboeBackend::updateStreamInfo() {
    if (!mOutputStream) {
        return;
    }

    mCachedStreamInfo.sampleRate = mOutputStream->getSampleRate();
    mCachedStreamInfo.channelCount = mOutputStream->getChannelCount();
    mCachedStreamInfo.framesPerBuffer = mOutputStream->getBufferSizeInFrames();
    mCachedStreamInfo.format = AudioFormat::FLOAT_32;
    mCachedStreamInfo.outputLatencyMs = getOutputLatencyMs();
    mCachedStreamInfo.inputLatencyMs = getInputLatencyMs();
    mCachedStreamInfo.isFullDuplex = (mInputStream != nullptr);
    mCachedStreamInfo.backendType = BackendType::OBOE;
    mCachedStreamInfo.deviceName = "Oboe (AAudio/OpenSL)";

    mStreamInfoValid.store(true);
}

void OboeBackend::enableDenormalFlush() {
    wma::platform::flushDenormals();
}

} // namespace watermelon_audio
