/**
 * BackendManager.h
 *
 * Manages audio backend selection and lifecycle.
 *
 * Responsibilities:
 * - Create and manage backend instances (Oboe, LibUSB)
 * - Handle backend switching (USB connect/disconnect)
 * - Provide automatic fallback to Oboe if USB fails
 * - Thread-safe backend access
 *
 * Usage:
 *   auto& manager = BackendManager::getInstance();
 *   manager.setCallback(&myCallback);
 *   manager.selectBackend(BackendType::OBOE);
 *   manager.start();
 *
 * USB Flow:
 *   1. UsbAudioManager.kt detects USB device
 *   2. JNI calls initializeUsbBackend(fd, usbfsPath)
 *   3. BackendManager creates LibusbBackend (future)
 *   4. On disconnect, fallbackToOboe() is called
 */

#pragma once

#include "IAudioBackend.h"
#include <memory>
#include <mutex>
#include <atomic>
#include <functional>

namespace watermelon_audio {

// Forward declarations
class OboeBackend;
class LibusbBackend;

/**
 * BackendManager
 *
 * Manager for audio backends. Constructible (Phase 0D: no longer singleton-only).
 * WmaEngine creates and owns its BackendManager instance.
 *
 * Thread Safety:
 * - getInstance(): Thread-safe, returns global instance
 * - selectBackend(): Thread-safe with mutex
 * - getCurrentBackend(): Returns pointer, caller must not store long-term
 * - start/stop: Thread-safe with mutex
 */
class BackendManager {
public:
    BackendManager();
    ~BackendManager();

    /**
     * Get the global instance (for legacy code that hasn't been migrated).
     * If setGlobalInstance() was called, returns that. Otherwise creates a default.
     */
    static BackendManager& getInstance();

    /**
     * Set the global instance pointer. Called by WmaEngine on creation.
     * Pass nullptr to clear (called on WmaEngine destruction).
     * Does NOT take ownership — caller must ensure lifetime.
     */
    static void setGlobalInstance(BackendManager* instance);

    // Prevent copy/move
    BackendManager(const BackendManager&) = delete;
    BackendManager& operator=(const BackendManager&) = delete;

    // =========================================================================
    // Backend Selection
    // =========================================================================

    /**
     * Select which backend to use.
     *
     * If the engine is running, it will be stopped before switching
     * and restarted with the new backend.
     *
     * @param type Backend type to use
     * @return true if backend was successfully selected
     */
    bool selectBackend(BackendType type);

    /**
     * Get the currently selected backend type.
     */
    BackendType getCurrentType() const {
        return mCurrentType.load(std::memory_order_acquire);
    }

    /**
     * Get the current backend instance.
     *
     * @return Pointer to current backend, or nullptr if none selected.
     * @warning Do not store this pointer - it may become invalid after backend switch.
     */
    IAudioBackend* getCurrentBackend();

    // =========================================================================
    // Callback Management
    // =========================================================================

    /**
     * Set the audio callback for all backends.
     *
     * This must be called before start().
     * The callback will be passed to whichever backend is active.
     *
     * @param callback Pointer to callback handler
     */
    void setCallback(IAudioCallback* callback);

    /**
     * Get the current callback.
     */
    IAudioCallback* getCallback() const { return mCallback; }

    // =========================================================================
    // Lifecycle Management
    // =========================================================================

    /**
     * Start the current backend.
     *
     * @return Result of start operation
     */
    BackendResult start();

    /**
     * Stop the current backend.
     */
    void stop();

    /**
     * Check if the current backend is running.
     */
    bool isRunning() const;

    /**
     * Get stream info from the current backend.
     */
    StreamInfo getStreamInfo() const;

    // =========================================================================
    // Configuration
    // =========================================================================

    /**
     * Set sample rate for backends.
     * Must be called before start().
     */
    void setSampleRate(int sampleRate);

    /**
     * Set buffer size for backends.
     * Must be called before start().
     */
    void setBufferSize(int framesPerBuffer);

    /**
     * Enable/disable full-duplex mode.
     * Must be called before start().
     */
    void setFullDuplexEnabled(bool enable);

    // =========================================================================
    // USB Support (Future)
    // =========================================================================

    /**
     * Initialize USB backend from Android file descriptor.
     *
     * Called from JNI when a USB audio device is connected.
     * Will switch from Oboe to LibUSB backend automatically.
     *
     * @param fd         File descriptor from UsbDeviceConnection
     * @param usbfsPath  Path to usbfs device (e.g., "/dev/bus/usb/001/002")
     * @return true if USB backend was initialized successfully
     */
    bool initializeUsbBackend(int fd, const char* usbfsPath);

    /**
     * Fallback to Oboe backend.
     *
     * Called when USB device is disconnected or USB backend fails.
     * Will attempt to maintain audio continuity.
     */
    void fallbackToOboe();

    /**
     * Check if USB backend is available.
     *
     * @return true if USB backend was successfully initialized
     */
    bool isUsbBackendAvailable() const {
        return mUsbBackendAvailable.load(std::memory_order_acquire);
    }

    /**
     * Get direct access to LibusbBackend.
     *
     * Used for USB-specific operations like getting transfer stats.
     * Only valid after successful initializeUsbBackend().
     *
     * @return Pointer to LibusbBackend, or nullptr if not available.
     */
    LibusbBackend* getLibusbBackend();

    // =========================================================================
    // Event Callbacks
    // =========================================================================

    using BackendChangedCallback = std::function<void(BackendType oldType, BackendType newType)>;
    using ErrorCallback = std::function<void(BackendError error)>;

    /**
     * Set callback for backend changes.
     */
    void setOnBackendChanged(BackendChangedCallback callback) {
        std::lock_guard<std::mutex> lock(mMutex);
        mOnBackendChanged = std::move(callback);
    }

    /**
     * Set callback for backend errors.
     */
    void setOnError(ErrorCallback callback) {
        std::lock_guard<std::mutex> lock(mMutex);
        mOnError = std::move(callback);
    }

private:
    // Mutex for thread-safe operations
    mutable std::mutex mMutex;

    // Backend instances
    std::unique_ptr<OboeBackend> mOboeBackend;
    std::unique_ptr<LibusbBackend> mLibusbBackend;

    // Current active backend
    IAudioBackend* mActiveBackend = nullptr;
    std::atomic<BackendType> mCurrentType{BackendType::NONE};

    // Configuration
    IAudioCallback* mCallback = nullptr;
    int mSampleRate = 0;
    int mBufferSize = 0;
    bool mFullDuplexEnabled = false;

    // USB state
    std::atomic<bool> mUsbBackendAvailable{false};

    // Event callbacks
    BackendChangedCallback mOnBackendChanged;
    ErrorCallback mOnError;

    // Was running before backend switch?
    bool mWasRunning = false;

    // Internal helpers
    void notifyBackendChanged(BackendType oldType, BackendType newType);
    void notifyError(BackendError error);
    void applyConfigToBackend(IAudioBackend* backend);
};

} // namespace watermelon_audio
