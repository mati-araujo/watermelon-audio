/**
 * OboeBackend.h
 *
 * Oboe-based audio backend implementation.
 * Wraps Google's Oboe library to provide low-latency audio I/O.
 *
 * This backend uses AAudio (on Android 8.1+) or OpenSL ES as fallback.
 * It's the primary backend for built-in audio output and Bluetooth.
 *
 * Thread Safety:
 * - Configuration methods: Call from UI thread before start()
 * - start/stop/pause/resume: Thread-safe, can be called from any thread
 * - Audio callback: Called from high-priority audio thread
 */

#pragma once

#include "IAudioBackend.h"
#include <oboe/Oboe.h>
#include <memory>
#include <mutex>
#include <atomic>
#include <condition_variable>

namespace watermelon_audio {

/**
 * OboeBackend
 *
 * Implementation of IAudioBackend using Google's Oboe library.
 *
 * Features:
 * - Automatic selection of AAudio or OpenSL ES
 * - Low-latency performance mode
 * - Automatic device routing (headphones, Bluetooth, etc.)
 * - Stream error recovery
 * - Full-duplex support (when hardware supports it)
 */
class OboeBackend : public IAudioBackend,
                    public oboe::AudioStreamCallback {
public:
    OboeBackend();
    ~OboeBackend() override;

    // Prevent copy/move (stream ownership)
    OboeBackend(const OboeBackend&) = delete;
    OboeBackend& operator=(const OboeBackend&) = delete;
    OboeBackend(OboeBackend&&) = delete;
    OboeBackend& operator=(OboeBackend&&) = delete;

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
    BackendType getType() const override { return BackendType::OBOE; }
    bool supportsFullDuplex() const override { return true; }
    bool supportsPause() const override { return true; }

    // USB methods not supported by Oboe backend
    bool initializeFromFileDescriptor(int fd, const char* usbfsPath) override {
        return false;  // Not a USB backend
    }

    // =========================================================================
    // Oboe AudioStreamCallback Implementation
    // =========================================================================

    oboe::DataCallbackResult onAudioReady(
        oboe::AudioStream* oboeStream,
        void* audioData,
        int32_t numFrames) override;

    void onErrorAfterClose(
        oboe::AudioStream* stream,
        oboe::Result error) override;

    void onErrorBeforeClose(
        oboe::AudioStream* stream,
        oboe::Result error) override;

    // =========================================================================
    // Oboe-Specific Methods
    // =========================================================================

    /**
     * Get the underlying Oboe stream for advanced configuration.
     * @return Pointer to AudioStream, or nullptr if not running.
     */
    oboe::AudioStream* getOboeStream() const { return mOutputStream.get(); }

    /**
     * Get XRun (underrun/overrun) count since stream start.
     */
    int32_t getXRunCount() const;

    /**
     * Check if stream has encountered an error.
     */
    bool hasStreamError() const { return mStreamError.load(); }

    /**
     * Get last error code (Oboe Result).
     */
    int getLastErrorCode() const { return mLastErrorCode.load(); }

    /**
     * Clear error state.
     */
    void clearError();

private:
    // Configuration (set before start)
    int mRequestedSampleRate = 0;      // 0 = auto-select
    int mRequestedBufferSize = 0;      // 0 = auto-select
    bool mFullDuplexEnabled = false;

    // Callback handler
    IAudioCallback* mCallback = nullptr;

    // Output stream
    std::shared_ptr<oboe::AudioStream> mOutputStream;

    // Input stream (for full-duplex)
    std::shared_ptr<oboe::AudioStream> mInputStream;

    // State management
    std::atomic<bool> mIsRunning{false};
    std::atomic<bool> mIsPaused{false};
    std::mutex mStreamMutex;
    std::condition_variable mStopCondition;
    std::atomic<int> mActiveCallbacks{0};

    // Error state
    std::atomic<bool> mStreamError{false};
    std::atomic<int> mLastErrorCode{0};

    // Stream info cache
    mutable StreamInfo mCachedStreamInfo;
    mutable std::atomic<bool> mStreamInfoValid{false};

    // Internal methods
    BackendResult openOutputStream();
    BackendResult openInputStream();
    void closeStreams();
    void updateStreamInfo();

    // Denormal flush for audio optimization
    void enableDenormalFlush();
};

} // namespace watermelon_audio
