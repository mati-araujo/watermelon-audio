/**
 * LibusbBackend.h
 *
 * USB Audio backend implementation using libusb.
 *
 * This backend provides low-latency audio I/O for USB Audio Class devices
 * by bypassing Android's audio HAL and communicating directly with the
 * USB device via libusb.
 *
 * Target latency: <10ms round-trip
 *
 * Features:
 * - Direct USB Audio Class 1.0 communication
 * - Isochronous transfer scheduling
 * - Asynchronous clock synchronization via feedback endpoints
 * - Hot-plug detection and recovery
 * - Full-duplex support (device permitting)
 *
 * Thread Safety:
 * - Configuration methods: Call from UI thread before start()
 * - start/stop: Thread-safe via mutex
 * - Audio callbacks: RT-safe, called from DSP thread
 * - USB transfers: Managed by internal event thread
 */

#pragma once

#include "IAudioBackend.h"
#include "../usb/UsbAudioTypes.h"
#include "../usb/UsbTransferManager.h"
#include "../usb/UsbDescriptorParser.h"
#include "../usb/UsbVolumeControl.h"

#include <libusb.h>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>

namespace watermelon_audio {

// Forward declarations
namespace usb {
    class UsbTransferManager;
    class AudioFormatConverter;
}

/**
 * Streaming mode for USB audio.
 */
enum class UsbStreamingMode {
    PLAYBACK_ONLY = 0,   // Output only
    CAPTURE_ONLY = 1,    // Input only
    FULL_DUPLEX = 2      // Both input and output
};

/**
 * LibusbBackend
 *
 * Implementation of IAudioBackend using libusb for USB Audio Class devices.
 *
 * Usage:
 * 1. Create instance
 * 2. initializeFromFileDescriptor(fd, path) - From Android UsbDeviceConnection
 * 3. Configure (setSampleRate, setBufferSize, setStreamingMode, etc.)
 * 4. setCallback(callback)
 * 5. start()
 * 6. ... audio flows ...
 * 7. stop()
 *
 * The file descriptor is obtained from Android's UsbDeviceConnection.getFileDescriptor()
 * and allows libusb to communicate with the USB device directly.
 */
class LibusbBackend : public IAudioBackend {
public:
    LibusbBackend();
    ~LibusbBackend() override;

    // Prevent copy/move
    LibusbBackend(const LibusbBackend&) = delete;
    LibusbBackend& operator=(const LibusbBackend&) = delete;
    LibusbBackend(LibusbBackend&&) = delete;
    LibusbBackend& operator=(LibusbBackend&&) = delete;

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

    /**
     * Set the streaming mode (playback only, capture only, or full-duplex).
     * Must be called before start().
     */
    void setStreamingMode(UsbStreamingMode mode);

    StreamInfo getStreamInfo() const override;
    bool isRunning() const override;
    float getOutputLatencyMs() const override;
    float getInputLatencyMs() const override;

    BackendType getType() const override { return BackendType::LIBUSB; }
    bool supportsFullDuplex() const override;
    bool supportsPause() const override { return true; }

    /**
     * Check if the device has audio capture capability.
     */
    bool hasCapture() const;

    /**
     * Get the UAC version of the connected device.
     * @return 1 for UAC 1.0, 2 for UAC 2.0, 0 if not initialized
     */
    int getUacVersion() const;

    // =========================================================================
    // USB-Specific Methods (IAudioBackend overrides)
    // =========================================================================

    /**
     * Initialize from Android USB file descriptor.
     *
     * This is the primary initialization method for USB devices.
     * The file descriptor comes from Android's UsbDeviceConnection.getFileDescriptor().
     *
     * @param fd        File descriptor for USB device
     * @param usbfsPath Path to USB device in usbfs (e.g., "/dev/bus/usb/001/002")
     * @return true if initialization succeeded
     */
    bool initializeFromFileDescriptor(int fd, const char* usbfsPath) override;

    /**
     * Get USB device vendor/product IDs.
     */
    bool getUsbDeviceInfo(int* vendorId, int* productId) const override;

    // =========================================================================
    // LibusbBackend-Specific Methods
    // =========================================================================

    /**
     * Get parsed USB audio device information.
     */
    const usb::UsbAudioDevice* getUsbAudioDevice() const {
        return mUsbDevice ? &(*mUsbDevice) : nullptr;
    }

    /**
     * Get current USB transfer statistics.
     */
    const usb::TransferStatistics* getTransferStats() const;

    /**
     * Get detailed latency profiling statistics.
     * NOT lock-free - only call from non-RT thread.
     */
    usb::UsbProfilingStats getProfilingStats() const;

    /**
     * Get latency profiler for direct access.
     */
    usb::UsbLatencyProfiler* getLatencyProfiler();

    /**
     * Get clock controller for drift monitoring.
     */
    ClockController* getClockController();

    /**
     * Check if USB device is initialized and ready.
     */
    bool isUsbDeviceReady() const { return mDeviceReady.load(); }

    /**
     * Get last libusb error code.
     */
    int getLastLibusbError() const { return mLastLibusbError.load(); }

    /**
     * Sync type for USB endpoints.
     */
    enum class SyncType {
        NONE,
        ASYNC,
        ADAPTIVE,
        SYNC
    };

    /**
     * Device capabilities query.
     */
    struct DeviceCapabilities {
        std::vector<int> supportedSampleRates;
        std::vector<int> supportedBitDepths;
        int maxChannelsOutput = 0;
        int maxChannelsInput = 0;
        SyncType syncType = SyncType::NONE;
        bool supportsFeedback = false;

        // Volume control capabilities
        bool hasOutputVolumeControl = false;   // Output (playback) volume control available
        bool hasInputVolumeControl = false;    // Input (capture) volume control available
        bool hasOutputMuteControl = false;     // Output mute control available
        bool hasInputMuteControl = false;      // Input mute control available
        bool isUsingHardwareOutputVolume = false; // Currently using hardware (vs digital)
        bool isUsingHardwareInputVolume = false;

        // Volume range in dB (for UI display)
        float outputVolumeMinDb = -96.0f;
        float outputVolumeMaxDb = 0.0f;
        float inputVolumeMinDb = -96.0f;
        float inputVolumeMaxDb = 0.0f;
    };

    /**
     * Get device capabilities.
     * Only valid after successful initialization.
     */
    DeviceCapabilities getCapabilities() const;

    /**
     * Set error callback for USB-specific errors.
     */
    using ErrorCallback = std::function<void(usb::UsbAudioError, const char*)>;
    void setUsbErrorCallback(ErrorCallback callback);

    // =========================================================================
    // Volume Control
    // =========================================================================

    /**
     * Set output (playback) volume.
     * Uses hardware volume control if available, otherwise digital fallback.
     *
     * @param volume Linear volume 0.0 (silent) to 1.0 (max)
     */
    void setOutputVolume(float volume);

    /**
     * Get current output volume (0.0 - 1.0).
     */
    float getOutputVolume() const;

    /**
     * Set input (capture) volume.
     *
     * @param volume Linear volume 0.0 (silent) to 1.0 (max)
     */
    void setInputVolume(float volume);

    /**
     * Get current input volume (0.0 - 1.0).
     */
    float getInputVolume() const;

    /**
     * Set output mute state.
     */
    void setOutputMute(bool muted);

    /**
     * Check if output is muted.
     */
    bool isOutputMuted() const;

    /**
     * Set input mute state.
     */
    void setInputMute(bool muted);

    /**
     * Check if input is muted.
     */
    bool isInputMuted() const;

    /**
     * Check if using hardware volume control for output.
     */
    bool isUsingHardwareOutputVolume() const;

    /**
     * Check if using hardware volume control for input.
     */
    bool isUsingHardwareInputVolume() const;

    // =========================================================================
    // Adaptive Buffer Control
    // =========================================================================

    /**
     * Enable or disable adaptive buffer sizing.
     * When enabled, the buffer size will automatically adjust based on
     * underrun rate and system health metrics.
     */
    void setAdaptiveBufferingEnabled(bool enabled);

    /**
     * Check if adaptive buffering is enabled.
     */
    bool isAdaptiveBufferingEnabled() const;

    /**
     * Get current ring buffer size in milliseconds.
     */
    int getCurrentBufferMs() const;

    /**
     * Request a buffer resize.
     * The resize will be performed at the next safe opportunity.
     *
     * @param newBufferMs  New buffer size in milliseconds (50-200ms)
     * @return true if request was accepted
     */
    bool requestBufferResize(int newBufferMs);

    /**
     * Get the adaptive buffer controller.
     */
    usb::AdaptiveBufferController* getBufferController();

private:
    // =========================================================================
    // Internal State
    // =========================================================================

    // libusb handles
    libusb_context* mContext = nullptr;
    libusb_device_handle* mDeviceHandle = nullptr;
    bool mOwnsContext = false;

    // Device state
    std::atomic<bool> mDeviceReady{false};
    std::atomic<bool> mIsRunning{false};
    std::atomic<bool> mIsPaused{false};
    std::atomic<bool> mDeviceDisconnected{false};  // Flag for physical disconnection
    std::atomic<int> mLastLibusbError{0};

    // Configuration
    int mRequestedSampleRate = 48000;
    int mRequestedBufferSize = 256;
    bool mFullDuplexEnabled = false;  // Legacy, use mStreamingMode
    UsbStreamingMode mStreamingMode = UsbStreamingMode::PLAYBACK_ONLY;

    // USB device info
    usb::UsbDeviceInfo mDeviceInfo;
    std::optional<usb::UsbAudioDevice> mUsbDevice;

    // Selected interfaces for streaming
    std::optional<usb::UsbStreamingInterface> mSelectedPlayback;
    std::optional<usb::UsbStreamingInterface> mSelectedCapture;

    // Transfer manager
    std::unique_ptr<usb::UsbTransferManager> mTransferManager;

    // Audio callback
    IAudioCallback* mCallback = nullptr;
    ErrorCallback mErrorCallback;

    // DSP thread for audio processing
    std::thread mDspThread;
    std::atomic<bool> mDspRunning{false};

    // Stream info cache
    mutable StreamInfo mCachedStreamInfo;
    mutable std::mutex mStreamInfoMutex;

    // Thread synchronization
    std::mutex mMutex;

    // =========================================================================
    // Internal Methods
    // =========================================================================

    // USB initialization
    bool parseDeviceDescriptors();
    bool selectBestInterfaces();
    bool configureSampleRate();

    // Transfer management
    bool setupTransferManager();
    void teardownTransferManager();

    // DSP thread
    void dspThreadFunc();
    void processAudioBlock(int numFrames);

    // Error handling
    void handleTransferError(usb::UsbAudioError error, const char* message);

    // Cleanup
    void cleanup();

    // Volume control initialization
    void initializeVolumeControls();

    // =========================================================================
    // Volume Control State
    // =========================================================================

    // Volume controllers for hardware/digital volume
    std::unique_ptr<usb::UsbVolumeControl> mOutputVolumeControl;
    std::unique_ptr<usb::UsbVolumeControl> mInputVolumeControl;

    // Digital volume fallback (applied in DSP thread when no hardware control)
    std::atomic<float> mDigitalOutputVolume{1.0f};
    std::atomic<float> mDigitalInputVolume{1.0f};
    std::atomic<bool> mDigitalOutputMute{false};
    std::atomic<bool> mDigitalInputMute{false};

    // =========================================================================
    // Pre-allocated DSP Buffers (avoid allocations in RT thread)
    // =========================================================================

    std::vector<float> mDspOutputBuffer;
    std::vector<float> mDspInputBuffer;
    std::vector<float> mDspStereoInputBuffer;
    std::vector<float> mDspLastValidInput;  // Last valid stereo input block for underrun protection
    bool mDspHasValidInput = false;         // Whether mDspLastValidInput contains valid data
    bool mDspNeedsMonoToStereo = false;
    size_t mDspOutputSamples = 0;  // Actual samples per block for output
    size_t mDspInputSamples = 0;   // Actual samples per block for input

    // =========================================================================
    // Adaptive Buffer State
    // =========================================================================

    std::atomic<bool> mAdaptiveBufferingEnabled{false};
    std::atomic<bool> mBufferResizePending{false};
    std::atomic<int> mPendingBufferMs{0};

    // Perform the buffer resize (called from DSP thread when safe)
    void performBufferResize();
};

} // namespace watermelon_audio
