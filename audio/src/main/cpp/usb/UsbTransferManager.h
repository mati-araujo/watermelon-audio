/**
 * UsbTransferManager.h
 *
 * USB Isochronous Transfer Manager for Audio Streaming
 *
 * Manages low-latency USB audio transfers using libusb:
 * - Triple buffering for glitch-free audio
 * - Isochronous transfer scheduling
 * - Clock synchronization with feedback endpoints
 * - Error handling and recovery
 *
 * Threading Model:
 * - USB event loop runs on dedicated high-priority thread
 * - DSP thread fills ring buffer asynchronously
 * - Transfer callbacks run on USB event thread
 * - Statistics can be read from any thread (atomic)
 *
 * Design Goals:
 * - Latency < 10ms round-trip
 * - Zero-copy where possible
 * - Graceful handling of underruns/overruns
 * - Hot-plug safe (device disconnect detection)
 */

#pragma once

#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <memory>
#include <vector>
#include <array>

#include <libusb.h>

#include "UsbAudioTypes.h"
#include "AudioFormatConverter.h"
#include "UsbLatencyProfiler.h"
#include "AdaptiveBufferController.h"
#include "../dsp/LockFreeRingBuffer.h"
#include "../backends/ClockController.h"

namespace watermelon_audio {
namespace usb {

// ============================================================================
// Forward Declarations
// ============================================================================

class UsbTransferManager;

// ============================================================================
// Transfer Configuration
// ============================================================================

/**
 * Configuration for USB transfer manager.
 */
struct TransferConfig {
    // Buffer configuration
    int framesPerPacket = 48;           // Audio frames per USB packet (1ms at 48kHz)
    int packetsPerTransfer = 8;         // Packets per libusb transfer
    int numTransfers = 3;               // Triple buffering

    // Audio format (output)
    int sampleRate = 48000;
    int channelCount = 2;               // Output channel count
    int bitDepth = 24;
    PcmFormat pcmFormat = PcmFormat::PCM_S24_3LE;

    // Audio format (input) - can differ from output
    int inputChannelCount = 2;          // Input channel count (may differ from output)
    int inputBitDepth = 24;             // Input bit depth (may differ from output)

    // Ring buffer sizing
    // Ring buffer decouples USB transfers from DSP processing.
    // Larger buffer = more headroom for USB jitter, NOT more latency.
    // 100ms provides excellent compatibility across Android devices while
    // using minimal memory (~38KB per buffer at 48kHz stereo).
    int ringBufferMs = 200;             // Ring buffer duration in milliseconds

    // Timing
    int transferTimeoutMs = 100;        // Timeout for USB transfers

    // ========== Output (playback) calculations ==========
    int bytesPerFrame() const {
        return channelCount * (bitDepth / 8);
    }

    int bytesPerPacket() const {
        return framesPerPacket * bytesPerFrame();
    }

    int bytesPerTransfer() const {
        return bytesPerPacket() * packetsPerTransfer;
    }

    int ringBufferFrames() const {
        return (sampleRate * ringBufferMs) / 1000;
    }

    int ringBufferSamples() const {
        return ringBufferFrames() * channelCount;
    }

    // ========== Input (capture) calculations ==========
    int inputBytesPerFrame() const {
        return inputChannelCount * (inputBitDepth / 8);
    }

    int inputBytesPerPacket() const {
        return framesPerPacket * inputBytesPerFrame();
    }

    int inputBytesPerTransfer() const {
        return inputBytesPerPacket() * packetsPerTransfer;
    }

    int inputRingBufferSamples() const {
        return ringBufferFrames() * inputChannelCount;
    }
};

// ============================================================================
// Transfer Statistics
// ============================================================================

/**
 * Real-time transfer statistics.
 */
struct TransferStatistics {
    // Packet counters
    std::atomic<uint64_t> packetsSubmitted{0};
    std::atomic<uint64_t> packetsCompleted{0};
    std::atomic<uint64_t> packetsErrors{0};

    // Buffer health
    std::atomic<uint64_t> underruns{0};
    std::atomic<uint64_t> overruns{0};

    // Latency tracking
    std::atomic<float> currentLatencyMs{0.0f};
    std::atomic<float> avgLatencyMs{0.0f};

    // Ring buffer state
    std::atomic<int> ringBufferLevel{0};       // Current fill level (samples)
    std::atomic<float> ringBufferFillPct{0.0f}; // Fill percentage

    void reset() {
        packetsSubmitted.store(0);
        packetsCompleted.store(0);
        packetsErrors.store(0);
        underruns.store(0);
        overruns.store(0);
        currentLatencyMs.store(0.0f);
        avgLatencyMs.store(0.0f);
        ringBufferLevel.store(0);
        ringBufferFillPct.store(0.0f);
    }
};

// ============================================================================
// Transfer Callbacks
// ============================================================================

/**
 * Callback types for transfer events.
 */
using TransferErrorCallback = std::function<void(UsbAudioError error, const char* message)>;
using TransferStatsCallback = std::function<void(const TransferStatistics& stats)>;

// ============================================================================
// UsbTransferManager Class
// ============================================================================

class UsbTransferManager {
public:
    /**
     * Construct a transfer manager.
     *
     * @param deviceHandle  libusb device handle (from libusb_open or wrap_fd)
     * @param context       libusb context (required for Android wrapped fd)
     */
    UsbTransferManager(libusb_device_handle* deviceHandle, libusb_context* context);
    ~UsbTransferManager();

    // Non-copyable
    UsbTransferManager(const UsbTransferManager&) = delete;
    UsbTransferManager& operator=(const UsbTransferManager&) = delete;

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * Configure the transfer manager.
     *
     * Must be called before start(). Can only be called while stopped.
     *
     * @param config  Transfer configuration
     * @return true on success
     */
    bool configure(const TransferConfig& config);

    /**
     * Set the output streaming interface.
     *
     * @param interface  Interface parsed from USB descriptors
     * @return true on success
     */
    bool setOutputInterface(const UsbStreamingInterface& interface);

    /**
     * Set the input streaming interface (for full-duplex).
     *
     * @param interface  Interface parsed from USB descriptors
     * @return true on success
     */
    bool setInputInterface(const UsbStreamingInterface& interface);

    /**
     * Enable/disable feedback endpoint processing.
     *
     * Required for asynchronous USB audio devices.
     *
     * @param enabled  true to enable feedback processing
     * @param endpoint Feedback endpoint info (from descriptor parser)
     */
    void setFeedbackEnabled(bool enabled, const UsbFeedbackEndpoint* endpoint = nullptr);

    /**
     * Tell the transfer manager which UAC version the device implements.
     *
     * Required to (a) parse the feedback endpoint correctly (UAC1 = 3-byte
     * 10.14, UAC2 = 4-byte 16.16) and (b) configure the iso packet length
     * of the feedback transfer.
     *
     * Must be called before start(). Defaults to UNKNOWN, in which case
     * the manager assumes UAC2 packet length (4) for backward compatibility.
     */
    void setUacVersion(UacVersion version);

    /**
     * Register a hook invoked once during start(), AFTER claim_interface +
     * set_interface_alt_setting for all configured interfaces, but BEFORE
     * the iso transfers are allocated and submitted.
     *
     * This is the safe point to issue class-specific control transfers that
     * require the target endpoint or interface to already be in its active
     * state — notably SET_CUR to the audio sampling frequency control
     * (endpoint-recipient in UAC 1.0, interface-recipient in UAC 2.0).
     * Running those requests any earlier races with the device still sitting
     * in altsetting 0 where the streaming endpoints don't exist yet, which
     * produces LIBUSB_ERROR_IO on the wire.
     *
     * If the hook returns false, start() fails and the stream is not
     * brought up.
     */
    using ClockConfigHook = std::function<bool()>;
    void setClockConfigHook(ClockConfigHook hook) {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        mClockConfigHook = std::move(hook);
    }

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /**
     * Start USB transfers.
     *
     * This claims the interface, selects the correct alternate setting,
     * pre-fills the output buffer, and starts the transfer loop.
     *
     * @return true on success
     */
    bool start();

    /**
     * Stop USB transfers.
     *
     * Cancels pending transfers and releases the interface.
     * Blocks until all transfers are completed or cancelled.
     */
    void stop();

    /**
     * Check if transfers are running.
     */
    bool isRunning() const { return mIsRunning.load(std::memory_order_acquire); }

    /**
     * Check if device was disconnected.
     * When true, no more USB operations should be attempted.
     */
    bool isDeviceDisconnected() const { return mDeviceDisconnected.load(std::memory_order_acquire); }

    // ========================================================================
    // Audio Data Interface
    // ========================================================================

    /**
     * Write audio samples to the output ring buffer.
     *
     * Called from DSP thread with float samples.
     * This is the producer side of the ring buffer.
     *
     * @param samples      Float samples (interleaved stereo)
     * @param numSamples   Number of samples to write
     * @return true if all samples were written, false on overflow
     */
    bool writeOutput(const float* samples, size_t numSamples);

    /**
     * Read audio samples from the input ring buffer.
     *
     * Called from DSP thread to get captured audio.
     * This is the consumer side of the input ring buffer.
     *
     * @param samples      Buffer to receive float samples
     * @param numSamples   Number of samples to read
     * @return true if all samples were read, false on underrun
     */
    bool readInput(float* samples, size_t numSamples);

    /**
     * Get available samples in output ring buffer.
     */
    size_t getOutputBufferAvailable() const;

    /**
     * Get available samples in input ring buffer.
     */
    size_t getInputBufferAvailable() const;

    // ========================================================================
    // Callbacks
    // ========================================================================

    /**
     * Set error callback.
     *
     * Called when a USB error occurs. May be called from USB thread.
     */
    void setErrorCallback(TransferErrorCallback callback) {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        mErrorCallback = std::move(callback);
    }

    /**
     * Set statistics callback.
     *
     * Called periodically with updated statistics.
     */
    void setStatsCallback(TransferStatsCallback callback) {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        mStatsCallback = std::move(callback);
    }

    /**
     * Set a notifier invoked from the USB event thread whenever data is
     * consumable (input ring filled) or output space is freed (output
     * transfer completed). Used by LibusbBackend's DSP thread to wake
     * from a counting_semaphore wait without busy-polling.
     *
     * The callback runs on the libusb event thread; it must be wait-free
     * and bounded. A `semaphore.release(1)` is the canonical use case.
     */
    void setDataReadyCallback(std::function<void()> callback) {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        mDataReadyCallback = std::move(callback);
    }

    // ========================================================================
    // Statistics
    // ========================================================================

    /**
     * Get current transfer statistics.
     */
    const TransferStatistics& getStatistics() const { return mStats; }

    /**
     * Get clock controller for monitoring.
     */
    ClockController* getClockController() { return mClockController.get(); }

    /**
     * Get latency profiler for detailed timing analysis.
     */
    UsbLatencyProfiler& getLatencyProfiler() { return mLatencyProfiler; }
    const UsbLatencyProfiler& getLatencyProfiler() const { return mLatencyProfiler; }

    /**
     * Get comprehensive profiling statistics.
     * NOT lock-free - only call from non-RT thread.
     */
    UsbProfilingStats getProfilingStats() const { return mLatencyProfiler.getStatistics(); }

    // ========================================================================
    // Adaptive Buffer Control
    // ========================================================================

    /**
     * Get the adaptive buffer controller.
     */
    AdaptiveBufferController* getBufferController() { return mBufferController.get(); }
    const AdaptiveBufferController* getBufferController() const { return mBufferController.get(); }

    /**
     * Reconfigure buffer size while running.
     *
     * This method should only be called when the ring buffer is at a low
     * fill level to minimize audio glitches. The caller is responsible for
     * coordinating the resize.
     *
     * @param newBufferMs  New ring buffer size in milliseconds
     * @return true on success
     */
    bool reconfigureBufferSize(int newBufferMs);

    /**
     * Get current ring buffer size in milliseconds.
     */
    int getCurrentBufferMs() const { return mConfig.ringBufferMs; }

private:
    // ========================================================================
    // Internal Transfer Handling
    // ========================================================================

    /**
     * Single isochronous transfer context.
     */
    struct IsoTransfer {
        libusb_transfer* transfer = nullptr;
        std::vector<uint8_t> buffer;
        UsbTransferManager* manager = nullptr;
        bool isOutput = true;
        int packetCount = 0;
        uint64_t profilingToken = 0;  // Token for latency profiler
    };

    // Allocate and free transfers
    bool allocateTransfers();
    void freeTransfers();

    // Transfer callbacks
    static void LIBUSB_CALL outputTransferCallback(libusb_transfer* transfer);
    static void LIBUSB_CALL inputTransferCallback(libusb_transfer* transfer);
    static void LIBUSB_CALL feedbackTransferCallback(libusb_transfer* transfer);

    // Process completed transfers
    void handleOutputComplete(IsoTransfer* ctx, libusb_transfer* transfer);
    void handleInputComplete(IsoTransfer* ctx, libusb_transfer* transfer);
    void handleFeedbackComplete(libusb_transfer* transfer);

    // Fill output transfer buffer from ring buffer
    bool fillOutputTransfer(IsoTransfer* ctx);

    // Empty input transfer buffer to ring buffer
    bool processInputTransfer(IsoTransfer* ctx);

    // Submit a transfer
    bool submitTransfer(libusb_transfer* transfer);

    // USB event loop
    void eventLoopThread();

    // Claim/release interface
    bool claimInterface(int interfaceNum);
    void releaseInterface(int interfaceNum);

    // Select alternate setting
    bool setAlternateSetting(int interfaceNum, int altSetting);

    // Error handling
    void reportError(UsbAudioError error, const char* message);

    // ========================================================================
    // State
    // ========================================================================

    libusb_device_handle* mDeviceHandle = nullptr;
    libusb_context* mContext = nullptr;

    TransferConfig mConfig;
    std::atomic<bool> mIsRunning{false};
    std::atomic<bool> mStopRequested{false};
    std::atomic<bool> mDeviceDisconnected{false};  // Set when device is physically disconnected

    // Streaming interfaces
    std::optional<UsbStreamingInterface> mOutputInterface;
    std::optional<UsbStreamingInterface> mInputInterface;

    // Feedback
    bool mFeedbackEnabled = false;
    std::optional<UsbFeedbackEndpoint> mFeedbackEndpoint;
    libusb_transfer* mFeedbackTransfer = nullptr;
    std::array<uint8_t, 8> mFeedbackBuffer{};

    // UAC version of the connected device. Determines feedback packet
    // length (3 bytes for UAC1, 4 for UAC2) and is forwarded to the
    // ClockController so processFeedback() doesn't have to guess.
    UacVersion mUacVersion = UacVersion::UNKNOWN;

    // Output transfers (playback)
    std::vector<std::unique_ptr<IsoTransfer>> mOutputTransfers;
    std::atomic<int> mOutputPendingCount{0};

    // Input transfers (capture)
    std::vector<std::unique_ptr<IsoTransfer>> mInputTransfers;
    std::atomic<int> mInputPendingCount{0};

    // Ring buffers (float samples)
    std::unique_ptr<LockFreeRingBuffer> mOutputRingBuffer;
    std::unique_ptr<LockFreeRingBuffer> mInputRingBuffer;

    // Format conversion
    AudioFormatConverter mFormatConverter;

    // Temp buffers for format conversion
    std::vector<float> mFloatBuffer;
    std::vector<uint8_t> mPcmBuffer;

    // Clock synchronization
    std::unique_ptr<ClockController> mClockController;

    // Event loop thread
    std::thread mEventThread;

    // Claimed interfaces
    std::vector<int> mClaimedInterfaces;

    // Statistics
    TransferStatistics mStats;

    // Callbacks
    std::mutex mCallbackMutex;
    TransferErrorCallback mErrorCallback;
    TransferStatsCallback mStatsCallback;
    std::function<void()> mDataReadyCallback;
    ClockConfigHook mClockConfigHook;

    // Wake the DSP thread (if a notifier is registered) after a transfer
    // completes. Invoked from the USB event thread; must be wait-free.
    void notifyDataReady() {
        // No lock here on the hot path: the callback is set/cleared from
        // setup/teardown, never concurrently with stream operation.
        // Worst case under racing teardown is one wasted call into a
        // valid std::function — std::counting_semaphore::release tolerates
        // that. We accept this in exchange for zero per-transfer locking.
        if (mDataReadyCallback) mDataReadyCallback();
    }

    // ========================================================================
    // Watchdog for device disconnect detection
    // ========================================================================

    // Watchdog configuration
    static constexpr int WATCHDOG_TIMEOUT_MS = 500;       // P1-8: 500ms (reduced from 2s for faster recovery)
    static constexpr int WATCHDOG_CHECK_INTERVAL_MS = 50;  // Check every 50ms (was 100ms)
    static constexpr int MAX_CONSECUTIVE_ERRORS = 10;     // Max errors before declaring disconnected

    // Watchdog state
    std::atomic<uint64_t> mLastCompletedTimeMs{0};        // Timestamp of last successful transfer
    std::atomic<int> mConsecutiveErrors{0};               // Count of consecutive transfer errors

    // Get current time in milliseconds
    static uint64_t getCurrentTimeMs();

    // Check watchdog and return true if device appears disconnected
    bool checkWatchdog();

    // ========================================================================
    // Latency Profiling
    // ========================================================================

    UsbLatencyProfiler mLatencyProfiler;

    // ========================================================================
    // Adaptive Buffer Control
    // ========================================================================

    std::unique_ptr<AdaptiveBufferController> mBufferController;
};

} // namespace usb
} // namespace watermelon_audio
