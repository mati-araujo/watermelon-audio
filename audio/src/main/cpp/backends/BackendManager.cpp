/**
 * BackendManager.cpp
 *
 * Implementation of the backend manager singleton.
 */

#include "BackendManager.h"
#include "OboeBackend.h"
#include "LibusbBackend.h"
#include "../usb/UsbAudioTypes.h"
#include "../platform/Logger.h"

#define LOG_TAG "BackendManager"
#undef LOGI
#undef LOGW
#undef LOGE
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) wma::logMessage(wma::LogLevel::WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)

namespace noisypad {

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

    // Create Oboe backend (always available)
    mOboeBackend = std::make_unique<OboeBackend>();

    // LibUSB backend will be created on-demand when USB device connects
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
            newBackend = mOboeBackend.get();
            break;

        case BackendType::LIBUSB:
            if (mLibusbBackend && mUsbBackendAvailable.load()) {
                newBackend = mLibusbBackend.get();
            } else {
                LOGW("LibUSB backend not available, falling back to Oboe");
                newBackend = mOboeBackend.get();
                type = BackendType::OBOE;
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
    std::lock_guard<std::mutex> lock(mMutex);

    mFullDuplexEnabled = enable;

    if (mActiveBackend) {
        mActiveBackend->setFullDuplexEnabled(enable);
    }
}

// =============================================================================
// USB Support
// =============================================================================

bool BackendManager::initializeUsbBackend(int fd, const char* usbfsPath) {
    std::lock_guard<std::mutex> lock(mMutex);

    LOGI("Initializing USB backend: fd=%d, path=%s", fd, usbfsPath);

    // Clean up existing USB backend if any
    if (mLibusbBackend) {
        mLibusbBackend->stop();
        mLibusbBackend.reset();
    }

    // Create new LibusbBackend
    mLibusbBackend = std::make_unique<LibusbBackend>();

    // Initialize with file descriptor from Android
    if (!mLibusbBackend->initializeFromFileDescriptor(fd, usbfsPath)) {
        LOGE("Failed to initialize LibUSB backend");
        mLibusbBackend.reset();
        mUsbBackendAvailable.store(false, std::memory_order_release);
        return false;
    }

    // Set up USB error callback for automatic fallback on disconnect
    mLibusbBackend->setUsbErrorCallback([this](usb::UsbAudioError error, const char* message) {
        LOGW("USB error callback: %s", message);
        if (error == usb::UsbAudioError::DEVICE_DISCONNECTED) {
            LOGI("Device disconnected detected, triggering automatic fallback to Oboe");
            // Don't call fallbackToOboe() directly from callback to avoid deadlock
            // Instead, post to a handler or use a flag
            // For now, we'll notify the error callback which can be handled by JNI
            notifyError(BackendError::DEVICE_DISCONNECTED);
        }
    });

    // Apply current configuration
    applyConfigToBackend(mLibusbBackend.get());

    mUsbBackendAvailable.store(true, std::memory_order_release);
    LOGI("USB backend initialized successfully");

    return true;
}

void BackendManager::fallbackToOboe() {
    LOGI("Falling back to Oboe backend");

    mUsbBackendAvailable.store(false, std::memory_order_release);

    // Switch to Oboe
    selectBackend(BackendType::OBOE);

    // Clean up LibUSB backend
    if (mLibusbBackend) {
        mLibusbBackend->stop();
        mLibusbBackend.reset();
    }
}

LibusbBackend* BackendManager::getLibusbBackend() {
    std::lock_guard<std::mutex> lock(mMutex);
    return mLibusbBackend.get();
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
}

} // namespace noisypad
