/**
 * BackendManager.cpp
 *
 * Implementation of the backend manager singleton.
 */

#include "BackendManager.h"
#include "PlatformBackends.h"
#include "SplitBackend.h"
#include "../platform/Logger.h"

#define LOG_TAG "BackendManager"
#undef LOGI
#undef LOGW
#undef LOGE
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) wma::logMessage(wma::LogLevel::WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)

namespace watermelon_audio {

// =============================================================================
// Global Instance (Phase 0D: settable, no longer static-local only)
// =============================================================================

static std::atomic<BackendManager*> g_globalInstance{nullptr};

BackendManager& BackendManager::getInstance() {
    BackendManager* instance = g_globalInstance.load(std::memory_order_acquire);
    if (instance) return *instance;

    // Fallback: create a default static instance (backward compat for JNI code
    // that calls getInstance() before WmaEngine is created)
    static BackendManager defaultInstance;
    return defaultInstance;
}

void BackendManager::setGlobalInstance(BackendManager* instance) {
    g_globalInstance.store(instance, std::memory_order_release);
}

BackendManager::BackendManager() {
    LOGI("BackendManager initialized");

    // The platform decides what this is; the manager only sees IAudioBackend.
    mSystemBackend = createSystemAudioBackend();
    if (!mSystemBackend) {
        LOGW("No built-in audio backend on this platform — selectBackend(OBOE) will fail");
    }

    // The USB backend is created on demand, when a device is handed to us.
}

BackendManager::~BackendManager() {
    stop();
    LOGI("BackendManager destroyed");
}

// =============================================================================
// Backend Selection
// =============================================================================

bool BackendManager::selectBackend(BackendType type) {
    std::lock_guard<std::mutex> lock(mMutex);

    BackendType oldType = mCurrentType.load(std::memory_order_acquire);

    if (oldType == type) {
        LOGI("Backend already selected: %s", backendTypeToString(type));
        return true;
    }

    // Check if engine was running
    mWasRunning = (mActiveBackend != nullptr && mActiveBackend->isRunning());

    // Stop current backend if running
    if (mWasRunning && mActiveBackend) {
        LOGI("Stopping current backend before switch");
        mActiveBackend->stop();
    }

    // Select new backend
    IAudioBackend* newBackend = nullptr;

    switch (type) {
        case BackendType::OBOE:
            if (!mSystemBackend) {
                LOGE("No built-in audio backend available on this platform");
                return false;
            }
            newBackend = mSystemBackend.get();
            break;

        case BackendType::LIBUSB:
            if (mUsbBackend && mUsbBackendAvailable.load()) {
                newBackend = mUsbBackend.get();
            } else if (mSystemBackend) {
                LOGW("USB backend not available, falling back to the built-in backend");
                newBackend = mSystemBackend.get();
                type = BackendType::OBOE;
            } else {
                LOGE("USB backend not available and no built-in backend to fall back to");
                return false;
            }
            break;

        case BackendType::SPLIT:
            if (mSplitBackend) {
                newBackend = mSplitBackend.get();
            } else {
                LOGW("Split backend not configured");
                return false;
            }
            break;

        case BackendType::NONE:
            newBackend = nullptr;
            break;

        default:
            LOGE("Unknown backend type: %d", static_cast<int>(type));
            return false;
    }

    // Apply configuration to new backend
    if (newBackend) {
        applyConfigToBackend(newBackend);
    }

    mActiveBackend = newBackend;
    mCurrentType.store(type, std::memory_order_release);

    LOGI("Backend selected: %s", backendTypeToString(type));

    // Notify listeners
    notifyBackendChanged(oldType, type);

    // Restart if was running
    if (mWasRunning && mActiveBackend) {
        LOGI("Restarting backend after switch");
        BackendResult result = mActiveBackend->start();
        if (result != BackendResult::OK) {
            LOGE("Failed to restart backend: %s", backendResultToString(result));
            return false;
        }
    }

    return true;
}

IAudioBackend* BackendManager::getCurrentBackend() {
    std::lock_guard<std::mutex> lock(mMutex);
    return mActiveBackend;
}

// =============================================================================
// Callback Management
// =============================================================================

void BackendManager::setCallback(IAudioCallback* callback) {
    std::lock_guard<std::mutex> lock(mMutex);

    mCallback = callback;

    // Apply to current backend
    if (mActiveBackend) {
        mActiveBackend->setCallback(callback);
    }
}

// =============================================================================
// Lifecycle Management
// =============================================================================

BackendResult BackendManager::start() {
    std::lock_guard<std::mutex> lock(mMutex);

    if (!mActiveBackend) {
        LOGE("No backend selected");
        return BackendResult::ERROR_NOT_INITIALIZED;
    }

    if (!mCallback) {
        LOGE("No callback set");
        return BackendResult::ERROR_NOT_INITIALIZED;
    }

    // Ensure callback is set
    mActiveBackend->setCallback(mCallback);

    BackendResult result = mActiveBackend->start();

    if (result != BackendResult::OK) {
        LOGE("Failed to start backend: %s", backendResultToString(result));
    } else {
        LOGI("Backend started: %s", backendTypeToString(mCurrentType.load()));
    }

    return result;
}

void BackendManager::stop() {
    std::lock_guard<std::mutex> lock(mMutex);

    if (mActiveBackend && mActiveBackend->isRunning()) {
        mActiveBackend->stop();
        LOGI("Backend stopped");
    }
}

bool BackendManager::isRunning() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mActiveBackend && mActiveBackend->isRunning();
}

StreamInfo BackendManager::getStreamInfo() const {
    std::lock_guard<std::mutex> lock(mMutex);

    if (mActiveBackend) {
        return mActiveBackend->getStreamInfo();
    }

    return StreamInfo{};
}

// =============================================================================
// Configuration
// =============================================================================

void BackendManager::setSampleRate(int sampleRate) {
    std::lock_guard<std::mutex> lock(mMutex);

    mSampleRate = sampleRate;

    if (mActiveBackend) {
        mActiveBackend->setSampleRate(sampleRate);
    }
}

void BackendManager::setBufferSize(int framesPerBuffer) {
    std::lock_guard<std::mutex> lock(mMutex);

    mBufferSize = framesPerBuffer;

    if (mActiveBackend) {
        mActiveBackend->setBufferSize(framesPerBuffer);
    }
}

void BackendManager::setFullDuplexEnabled(bool enable) {
    // The mode requester never restarts a running stream: a mode change must not
    // punch an audible gap into playback.
    requestCapture(CaptureRequester::MODE, enable, /*allowRestart=*/false);
}

bool BackendManager::isCaptureLive() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mActiveBackend && mActiveBackend->isRunning() &&
           mActiveBackend->getStreamInfo().isFullDuplex;
}

bool BackendManager::requestCapture(CaptureRequester who, bool want, bool allowRestart) {
    // Every path that does NOT need a reopen returns from inside this block, so
    // reaching the code after it *is* the decision to reopen.
    {
        std::lock_guard<std::mutex> lock(mMutex);

        if (who == CaptureRequester::MODE) {
            mCaptureRequestedByMode = want;
        } else {
            mCaptureRequestedByInputNode = want;
        }

        const bool effective = mCaptureRequestedByMode || mCaptureRequestedByInputNode;
        mFullDuplexEnabled = effective;

        if (!mActiveBackend) {
            return false;
        }

        // Always push the request: it is what the backend reads at its next
        // start(), and applyConfigToBackend() replays it if the backend is
        // recreated. Backends that can honor a change live (CoreAudio flips
        // delivery of an already-attached capture stream) do it inside here.
        mActiveBackend->setFullDuplexEnabled(effective);

        if (!mActiveBackend->isRunning()) {
            return false;
        }

        const bool live = mActiveBackend->getStreamInfo().isFullDuplex;

        if (live == effective) {
            return live;
        }

        if (!effective) {
            // Asked to stop capturing and the stream still carries input. Not
            // worth a restart: the backend has already stopped delivering it, so
            // the only cost is some capture work nobody consumes. Trading that
            // for an audible gap would be a bad deal.
            return false;
        }

        if (!allowRestart) {
            LOGW("Capture requested on a running stream that has none — takes "
                 "effect on the next start()");
            return false;
        }
    }

    // Reopen OUTSIDE the lock: start()/stop() take mMutex themselves, and stop()
    // additionally blocks until in-flight RT callbacks drain.
    LOGI("Reopening the stream to add a capture path");
    stop();

    if (start() == BackendResult::OK) {
        const bool live = isCaptureLive();
        if (!live) {
            LOGW("Stream reopened but capture is still not live — most likely "
                 "microphone access was denied");
        }
        return live;
    }

    // The reopen failed with capture on. Falling back to no capture is the only
    // way out that leaves the user with audio instead of silence.
    LOGE("Failed to reopen with capture — retrying without it");
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mCaptureRequestedByInputNode = false;
        mFullDuplexEnabled = mCaptureRequestedByMode;
        if (mActiveBackend) {
            mActiveBackend->setFullDuplexEnabled(mFullDuplexEnabled);
        }
    }
    if (start() != BackendResult::OK) {
        LOGE("Fallback start failed too — the stream is down");
    }
    return false;
}

void BackendManager::setLatencyProfile(usb::UsbLatencyProfile profile) {
    std::lock_guard<std::mutex> lock(mMutex);

    mLatencyProfile = profile;

    // Apply immediately to the existing USB backend (takes effect at its next
    // start). Persisted in mLatencyProfile so applyConfigToBackend re-applies it
    // if the backend is later recreated — same lifecycle as the streaming mode.
    if (mUsbBackend) {
        mUsbBackend->setUsbLatencyProfile(profile);
    }
}

// =============================================================================
// USB Support
// =============================================================================

bool BackendManager::initializeUsbBackend(int fd, const char* usbfsPath) {
    std::lock_guard<std::mutex> lock(mMutex);

    LOGI("Initializing USB backend: fd=%d, path=%s", fd, usbfsPath);

    // Clean up existing USB backend if any
    if (mSplitBackend) {
        mSplitBackend->stop();
        mSplitBackend.reset();
        if (mCurrentType.load(std::memory_order_acquire) == BackendType::SPLIT) {
            mActiveBackend = nullptr;
            mCurrentType.store(BackendType::NONE, std::memory_order_release);
        }
    }
    if (mUsbBackend) {
        mUsbBackend->stop();
        mUsbBackend.reset();
    }

    mUsbBackend = createUsbAudioBackend();
    if (!mUsbBackend) {
        LOGE("USB audio is not supported on this platform");
        mUsbBackendAvailable.store(false, std::memory_order_release);
        return false;
    }

    // Initialize with file descriptor from Android
    if (!mUsbBackend->initializeFromFileDescriptor(fd, usbfsPath)) {
        LOGE("Failed to initialize USB backend");
        mUsbBackend.reset();
        mUsbBackendAvailable.store(false, std::memory_order_release);
        return false;
    }

    // Set up the error callback for automatic fallback on disconnect
    mUsbBackend->setErrorCallback([this](BackendError error, const char* message) {
        LOGW("USB error callback: %s", message);
        if (error == BackendError::DEVICE_DISCONNECTED) {
            LOGI("Device disconnected detected, triggering automatic fallback");
            // Don't call fallbackToOboe() directly from callback to avoid deadlock
            // Instead, post to a handler or use a flag
            // For now, we'll notify the error callback which can be handled by JNI
            notifyError(BackendError::DEVICE_DISCONNECTED);
        }
    });

    // Apply current configuration
    applyConfigToBackend(mUsbBackend.get());

    mUsbBackendAvailable.store(true, std::memory_order_release);
    LOGI("USB backend initialized successfully");

    return true;
}

bool BackendManager::createSplitBackend(BackendType inputType, BackendType outputType) {
    std::lock_guard<std::mutex> lock(mMutex);

    IAudioBackend* input = resolveBackendForSplit(inputType);
    IAudioBackend* output = resolveBackendForSplit(outputType);

    if (!input || !output || input == output) {
        LOGW("Cannot create Split backend: invalid endpoints input=%d output=%d",
             static_cast<int>(inputType), static_cast<int>(outputType));
        return false;
    }

    const bool wasRunning = (mActiveBackend != nullptr && mActiveBackend->isRunning());
    if (wasRunning && mActiveBackend) {
        mActiveBackend->stop();
    }

    mSplitBackend = std::make_unique<SplitBackend>(*input, *output);
    applyConfigToBackend(mSplitBackend.get());

    if (wasRunning) {
        mActiveBackend = mSplitBackend.get();
        mCurrentType.store(BackendType::SPLIT, std::memory_order_release);
        BackendResult result = mActiveBackend->start();
        if (result != BackendResult::OK) {
            LOGE("Failed to start Split backend: %s", backendResultToString(result));
            mSplitBackend.reset();
            mActiveBackend = nullptr;
            mCurrentType.store(BackendType::NONE, std::memory_order_release);
            return false;
        }
    }

    LOGI("Split backend configured: input=%s output=%s",
         backendTypeToString(inputType), backendTypeToString(outputType));
    return true;
}

void BackendManager::fallbackToOboe() {
    LOGI("Falling back to Oboe backend");

    mUsbBackendAvailable.store(false, std::memory_order_release);

    // Switch to Oboe
    selectBackend(BackendType::OBOE);

    // Clean up LibUSB backend
    if (mSplitBackend) {
        mSplitBackend->stop();
        mSplitBackend.reset();
    }
    if (mUsbBackend) {
        mUsbBackend->stop();
        mUsbBackend.reset();
    }
}

LibusbBackend* BackendManager::getLibusbBackend() {
    std::lock_guard<std::mutex> lock(mMutex);
    return asLibusbBackend(mUsbBackend.get());
}

// =============================================================================
// Internal Helpers
// =============================================================================

void BackendManager::notifyBackendChanged(BackendType oldType, BackendType newType) {
    if (mOnBackendChanged) {
        // Call outside lock to prevent deadlock
        auto callback = mOnBackendChanged;
        // Note: We're inside the lock here, be careful with callback
        // In production, consider posting to a queue
        callback(oldType, newType);
    }
}

void BackendManager::notifyError(BackendError error) {
    if (mOnError) {
        mOnError(error);
    }
}

void BackendManager::applyConfigToBackend(IAudioBackend* backend) {
    if (!backend) return;

    if (mCallback) {
        backend->setCallback(mCallback);
    }
    if (mSampleRate > 0) {
        backend->setSampleRate(mSampleRate);
    }
    if (mBufferSize > 0) {
        backend->setBufferSize(mBufferSize);
    }
    backend->setFullDuplexEnabled(mFullDuplexEnabled);

    // USB latency profile (Fase 1). Re-applied here so a freshly created or
    // reactivated USB backend picks up the persisted profile. Pushed
    // unconditionally: the interface default is a no-op, so backends without
    // USB latency knobs ignore it and no type test is needed.
    backend->setUsbLatencyProfile(mLatencyProfile);
}

IAudioBackend* BackendManager::resolveBackendForSplit(BackendType type) const {
    switch (type) {
        case BackendType::OBOE:
            return mSystemBackend.get();
        case BackendType::LIBUSB:
            return (mUsbBackend && mUsbBackendAvailable.load(std::memory_order_acquire))
                ? mUsbBackend.get()
                : nullptr;
        default:
            return nullptr;
    }
}

} // namespace watermelon_audio
