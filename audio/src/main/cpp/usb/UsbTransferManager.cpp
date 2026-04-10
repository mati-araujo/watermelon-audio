/**
 * UsbTransferManager.cpp
 *
 * Implementation of USB isochronous transfer management.
 */

#include "UsbTransferManager.h"
#include "UsbConstants.h"
#include "../utils/ThreadUtils.h"
#include "../utils/MemoryUtils.h"
#include "../platform/Logger.h"
#include <cstring>
#include <algorithm>
#include <chrono>

#define LOG_TAG "UsbTransferManager"
#undef LOGI
#undef LOGW
#undef LOGE
#undef LOGD
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) wma::logMessage(wma::LogLevel::WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) wma::logMessage(wma::LogLevel::DEBUG, LOG_TAG, __VA_ARGS__)

namespace watermelon_audio {
namespace usb {

// ============================================================================
// Constructor / Destructor
// ============================================================================

UsbTransferManager::UsbTransferManager(libusb_device_handle* deviceHandle, libusb_context* context)
    : mDeviceHandle(deviceHandle)
    , mContext(context)
{
    // On Android with wrapped file descriptors, we MUST use the correct context
    // for event handling. Using nullptr (default context) won't work.
    if (!mContext) {
        LOGW("UsbTransferManager created with null context - event handling may fail");
    }

    // Initialize clock controller with default sample rate
    mClockController = std::make_unique<ClockController>(48000);
}

UsbTransferManager::~UsbTransferManager() {
    stop();
    freeTransfers();
}

// ============================================================================
// Configuration
// ============================================================================

bool UsbTransferManager::configure(const TransferConfig& config) {
    if (mIsRunning.load()) {
        LOGE("Cannot configure while running");
        return false;
    }

    mConfig = config;

    // Update clock controller sample rate
    mClockController->setNominalSampleRate(config.sampleRate);

    // Allocate ring buffers (separate sizes for input vs output)
    size_t outputRingBufferSize = static_cast<size_t>(config.ringBufferSamples());
    size_t inputRingBufferSize = static_cast<size_t>(config.inputRingBufferSamples());

    LOGI("Ring buffer config: ringBufferMs=%d, outputSize=%zu samples (%zu bytes), inputSize=%zu samples (%zu bytes)",
         config.ringBufferMs,
         outputRingBufferSize, outputRingBufferSize * sizeof(float),
         inputRingBufferSize, inputRingBufferSize * sizeof(float));

    mOutputRingBuffer = std::make_unique<LockFreeRingBuffer>(outputRingBufferSize);
    mInputRingBuffer = std::make_unique<LockFreeRingBuffer>(inputRingBufferSize);

    // Lock ring buffers in memory to prevent page faults during audio streaming
    MemoryUtils::prepareForRealtime(mOutputRingBuffer->data(), mOutputRingBuffer->sizeBytes());
    MemoryUtils::prepareForRealtime(mInputRingBuffer->data(), mInputRingBuffer->sizeBytes());

    // Allocate temp buffers for format conversion (use max of input/output)
    size_t maxOutputSamples = static_cast<size_t>(config.framesPerPacket *
                                                   config.packetsPerTransfer *
                                                   config.channelCount);
    size_t maxInputSamples = static_cast<size_t>(config.framesPerPacket *
                                                  config.packetsPerTransfer *
                                                  config.inputChannelCount);
    size_t maxSamples = std::max(maxOutputSamples, maxInputSamples);
    mFloatBuffer.resize(maxSamples);

    // PCM buffer needs to be large enough for both directions
    size_t maxPcmSize = std::max(static_cast<size_t>(config.bytesPerTransfer()),
                                  static_cast<size_t>(config.inputBytesPerTransfer()));
    mPcmBuffer.resize(maxPcmSize);

    // Lock temp buffers in memory to prevent page faults during USB transfers
    MemoryUtils::prepareVectorForRealtime(mFloatBuffer);
    MemoryUtils::prepareVectorForRealtime(mPcmBuffer);

    LOGI("Configured: %dHz, out=%dch/%dbit, in=%dch/%dbit, %d frames/packet, %d packets/xfer",
         config.sampleRate, config.channelCount, config.bitDepth,
         config.inputChannelCount, config.inputBitDepth,
         config.framesPerPacket, config.packetsPerTransfer);

    // Configure latency profiler
    mLatencyProfiler.configureFromTransfer(
        config.framesPerPacket,
        config.packetsPerTransfer,
        config.sampleRate
    );

    // Initialize adaptive buffer controller
    mBufferController = std::make_unique<AdaptiveBufferController>();
    AdaptiveBufferController::Config bufferConfig;
    bufferConfig.defaultBufferMs = config.ringBufferMs;
    mBufferController->configure(bufferConfig);
    mBufferController->setCurrentBufferMs(config.ringBufferMs);

    return true;
}

bool UsbTransferManager::setOutputInterface(const UsbStreamingInterface& interface) {
    if (mIsRunning.load()) {
        LOGE("Cannot set interface while running");
        return false;
    }

    mOutputInterface = interface;
    LOGI("Output interface set: IF%d Alt%d, EP 0x%02X",
         interface.interfaceNumber, interface.alternateSetting,
         interface.dataEndpoint.address);

    return true;
}

bool UsbTransferManager::setInputInterface(const UsbStreamingInterface& interface) {
    if (mIsRunning.load()) {
        LOGE("Cannot set interface while running");
        return false;
    }

    mInputInterface = interface;
    LOGI("Input interface set: IF%d Alt%d, EP 0x%02X",
         interface.interfaceNumber, interface.alternateSetting,
         interface.dataEndpoint.address);

    return true;
}

void UsbTransferManager::setFeedbackEnabled(bool enabled, const UsbFeedbackEndpoint* endpoint) {
    if (!enabled || endpoint == nullptr) {
        mFeedbackEnabled = enabled;
        if (!endpoint) mFeedbackEndpoint.reset();
        return;
    }

    // Validate the endpoint really is an explicit feedback endpoint:
    //   - isochronous transfer type (bmAttributes 1:0 == 01)
    //   - usage type = feedback (bmAttributes 5:4 == 01)
    //   - direction IN
    // The parser is supposed to enforce this in stage 1, but defending
    // here keeps the transfer manager honest if someone hands it the
    // wrong endpoint.
    const uint8_t attrs = endpoint->endpoint.attributes;
    const bool isIso = (attrs & 0x03) == 0x01;
    const bool isFeedbackUsage = ((attrs >> 4) & 0x03) == 0x01;
    const bool isInput = (endpoint->endpoint.address & 0x80) != 0;

    if (endpoint->isImplicit) {
        // Implicit feedback rides on the data endpoint and is handled by
        // mining iso_packet_desc.actual_length, not by a separate transfer.
        // We do not allocate a feedback transfer in this case; just remember
        // the flag for the clock controller's accounting.
        mFeedbackEnabled = false;
        mFeedbackEndpoint.reset();
        LOGI("Feedback endpoint is implicit on data EP 0x%02x — clock sync "
             "via packet timing (no dedicated transfer)",
             endpoint->endpoint.address);
        return;
    }

    if (!isIso || !isFeedbackUsage || !isInput) {
        LOGW("Refusing to enable feedback on EP 0x%02x: "
             "iso=%d feedbackUsage=%d in=%d (attrs=0x%02x)",
             endpoint->endpoint.address, isIso, isFeedbackUsage, isInput, attrs);
        mFeedbackEnabled = false;
        mFeedbackEndpoint.reset();
        return;
    }

    mFeedbackEnabled = true;
    mFeedbackEndpoint = *endpoint;
    LOGI("Feedback endpoint enabled: addr=0x%02x maxPkt=%d interval=%d",
         endpoint->endpoint.address,
         endpoint->endpoint.maxPacketSize,
         endpoint->endpoint.interval);
}

void UsbTransferManager::setUacVersion(UacVersion version) {
    mUacVersion = version;
    if (mClockController) {
        mClockController->setUacVersion(version);
    }
    LOGI("UsbTransferManager UAC version set to %d",
         version == UacVersion::UAC_1_0 ? 1 :
         version == UacVersion::UAC_2_0 ? 2 : 0);
}

// ============================================================================
// Lifecycle
// ============================================================================

bool UsbTransferManager::start() {
    if (mIsRunning.load()) {
        LOGW("Already running");
        return true;
    }

    if (!mOutputInterface && !mInputInterface) {
        LOGE("No interfaces configured");
        return false;
    }

    mStopRequested.store(false);
    mStats.reset();
    mLatencyProfiler.reset();

    // Initialize watchdog
    mLastCompletedTimeMs.store(getCurrentTimeMs(), std::memory_order_release);
    mConsecutiveErrors.store(0, std::memory_order_release);

    // Claim interfaces and set alternate settings
    if (mOutputInterface) {
        if (!claimInterface(mOutputInterface->interfaceNumber)) {
            return false;
        }
        if (!setAlternateSetting(mOutputInterface->interfaceNumber,
                                  mOutputInterface->alternateSetting)) {
            return false;
        }
    }

    if (mInputInterface) {
        if (!claimInterface(mInputInterface->interfaceNumber)) {
            return false;
        }
        if (!setAlternateSetting(mInputInterface->interfaceNumber,
                                  mInputInterface->alternateSetting)) {
            return false;
        }
    }

    // Now that the streaming interface is claimed and in its active
    // altsetting (so its isochronous endpoints exist on the wire), run
    // any device-specific control transfers that depend on that state.
    // In practice this is where the sample rate SET_CUR happens — see
    // LibusbBackend::configureSampleRate() and the hook registered in
    // setupTransferManager(). Doing this earlier (before setAlternateSetting)
    // races with the device still sitting in alt 0 and produces
    // LIBUSB_ERROR_IO on endpoint-recipient UAC 1.0 requests.
    {
        ClockConfigHook hookCopy;
        {
            std::lock_guard<std::mutex> lock(mCallbackMutex);
            hookCopy = mClockConfigHook;
        }
        if (hookCopy) {
            if (!hookCopy()) {
                LOGE("Clock config hook failed");
                stop();
                return false;
            }
        }
    }

    // Allocate transfers
    if (!allocateTransfers()) {
        LOGE("Failed to allocate transfers");
        stop();
        return false;
    }

    // Pre-fill output ring buffer with silence to prevent initial underrun
    size_t prefillSamples = static_cast<size_t>(mConfig.framesPerPacket *
                                                  mConfig.packetsPerTransfer *
                                                  mConfig.numTransfers *
                                                  mConfig.channelCount * 2);
    std::vector<float> silence(prefillSamples, 0.0f);
    mOutputRingBuffer->write(silence.data(), silence.size());

    mIsRunning.store(true, std::memory_order_release);

    // Submit initial output transfers
    for (auto& ctx : mOutputTransfers) {
        if (!fillOutputTransfer(ctx.get())) {
            LOGW("Initial fill underrun (expected)");
        }
        // Profile: record initial submission
        if (mLatencyProfiler.isEnabled()) {
            ctx->profilingToken = mLatencyProfiler.onOutputSubmitted();
        }
        if (!submitTransfer(ctx->transfer)) {
            LOGE("Failed to submit initial output transfer");
            stop();
            return false;
        }
        mOutputPendingCount.fetch_add(1);
    }

    // Submit initial input transfers
    for (auto& ctx : mInputTransfers) {
        // Profile: record initial submission
        if (mLatencyProfiler.isEnabled()) {
            ctx->profilingToken = mLatencyProfiler.onInputSubmitted();
        }
        if (!submitTransfer(ctx->transfer)) {
            LOGE("Failed to submit initial input transfer");
            stop();
            return false;
        }
        mInputPendingCount.fetch_add(1);
    }

    // Submit feedback transfer if enabled
    if (mFeedbackEnabled && mFeedbackTransfer) {
        if (!submitTransfer(mFeedbackTransfer)) {
            LOGW("Failed to submit feedback transfer");
            // Non-fatal, continue without feedback
        }
    }

    // Start event loop thread
    // Thread priority and CPU affinity are configured from within the thread
    mEventThread = std::thread(&UsbTransferManager::eventLoopThread, this);

    LOGI("Started USB transfers");
    return true;
}

void UsbTransferManager::stop() {
    if (!mIsRunning.load()) {
        return;
    }

    LOGI("Stopping USB transfers...");

    mStopRequested.store(true, std::memory_order_release);

    // Cancel pending transfers - only if we have a valid device handle
    // and device wasn't disconnected. libusb_cancel_transfer can crash
    // if device was physically disconnected
    bool deviceValid = (mDeviceHandle != nullptr) && !mDeviceDisconnected.load(std::memory_order_acquire);

    if (deviceValid) {
        for (auto& ctx : mOutputTransfers) {
            if (ctx && ctx->transfer) {
                int result = libusb_cancel_transfer(ctx->transfer);
                if (result != LIBUSB_SUCCESS && result != LIBUSB_ERROR_NOT_FOUND) {
                    LOGW("Failed to cancel output transfer: %s", libusb_error_name(result));
                }
            }
        }
        for (auto& ctx : mInputTransfers) {
            if (ctx && ctx->transfer) {
                int result = libusb_cancel_transfer(ctx->transfer);
                if (result != LIBUSB_SUCCESS && result != LIBUSB_ERROR_NOT_FOUND) {
                    LOGW("Failed to cancel input transfer: %s", libusb_error_name(result));
                }
            }
        }
        if (mFeedbackTransfer) {
            int result = libusb_cancel_transfer(mFeedbackTransfer);
            if (result != LIBUSB_SUCCESS && result != LIBUSB_ERROR_NOT_FOUND) {
                LOGW("Failed to cancel feedback transfer: %s", libusb_error_name(result));
            }
        }
    } else {
        LOGW("Device handle is null, skipping transfer cancellation");
    }

    // Wait for event thread to finish
    if (mEventThread.joinable()) {
        mEventThread.join();
    }

    // Release interfaces - only if device is valid
    if (deviceValid) {
        for (int ifNum : mClaimedInterfaces) {
            releaseInterface(ifNum);
        }
    }
    mClaimedInterfaces.clear();

    mIsRunning.store(false, std::memory_order_release);

    // Clear ring buffers
    if (mOutputRingBuffer) mOutputRingBuffer->clear();
    if (mInputRingBuffer) mInputRingBuffer->clear();

    LOGI("Stopped USB transfers. Stats: submitted=%llu, completed=%llu, errors=%llu, underruns=%llu",
         (unsigned long long)mStats.packetsSubmitted.load(),
         (unsigned long long)mStats.packetsCompleted.load(),
         (unsigned long long)mStats.packetsErrors.load(),
         (unsigned long long)mStats.underruns.load());
}

// ============================================================================
// Audio Data Interface
// ============================================================================

bool UsbTransferManager::writeOutput(const float* samples, size_t numSamples) {
    if (!mOutputRingBuffer) {
        return false;
    }

    bool success = mOutputRingBuffer->write(samples, numSamples);

    // Update statistics
    int level = static_cast<int>(mOutputRingBuffer->availableToRead());
    mStats.ringBufferLevel.store(level, std::memory_order_relaxed);
    float fillPct = static_cast<float>(level) / static_cast<float>(mOutputRingBuffer->capacity());
    mStats.ringBufferFillPct.store(fillPct * 100.0f, std::memory_order_relaxed);

    if (!success) {
        mStats.overruns.fetch_add(1, std::memory_order_relaxed);
    }

    return success;
}

bool UsbTransferManager::readInput(float* samples, size_t numSamples) {
    if (!mInputRingBuffer) {
        return false;
    }
    return mInputRingBuffer->read(samples, numSamples);
}

size_t UsbTransferManager::getOutputBufferAvailable() const {
    return mOutputRingBuffer ? mOutputRingBuffer->availableToWrite() : 0;
}

size_t UsbTransferManager::getInputBufferAvailable() const {
    return mInputRingBuffer ? mInputRingBuffer->availableToRead() : 0;
}

// ============================================================================
// Transfer Allocation
// ============================================================================

bool UsbTransferManager::allocateTransfers() {
    const int numPackets = mConfig.packetsPerTransfer;

    // Nominal iso packet size derived from the negotiated stream config
    // (sampleRate, channels, bitDepth). This is what the host EXPECTS to
    // send / receive per packet in steady state.
    const int outputPacketSizeNominal = mConfig.bytesPerPacket();
    const int inputPacketSizeNominal = mConfig.inputBytesPerPacket();

    // Effective upper bound declared by each endpoint in its descriptor.
    // Async devices typically declare a wMaxPacketSize *larger* than the
    // nominal to reserve room for +1/+2 frames of drift compensation, and
    // high-speed endpoints can declare additional transactions per
    // microframe (bits 12:11 of wMaxPacketSize) that multiply the base
    // size up to 3×. If we allocate the iso packet buffer at the nominal
    // size only, packets that legitimately carry more bytes than nominal
    // get truncated silently by the host controller — audible as
    // distortion, lost samples, or subtle crackle on real hardware.
    const int outputEndpointMax = mOutputInterface
        ? mOutputInterface->dataEndpoint.effectiveMaxBytesPerPacket()
        : 0;
    const int inputEndpointMax = mInputInterface
        ? mInputInterface->dataEndpoint.effectiveMaxBytesPerPacket()
        : 0;

    // Output additionally needs headroom for the clock controller's
    // per-packet frame adjustment. getAdjustedFrameCount() clamps the
    // adjustment to ± CLOCK_ADJUST_FRAMES_MAX from nominal, so the largest
    // packet length we might set is nominalFrames + CLOCK_ADJUST_FRAMES_MAX
    // frames. Without this headroom, a slot sized exactly at nominal can
    // be told to carry a larger length via libusb_set_iso_packet_lengths
    // and the host controller writes past the end of the slot — another
    // silent overflow producing audible clicks at high drift.
    //
    // CLOCK_ADJUST_FRAMES_MAX must match the clamp value used in
    // ClockController::getAdjustedFrameCount(); keeping it defensive here
    // means this file owns the buffer sizing, not the clock controller.
    constexpr int CLOCK_ADJUST_FRAMES_MAX = 4;
    const int outputClockMarginBytes = mOutputInterface
        ? CLOCK_ADJUST_FRAMES_MAX
            * mConfig.channelCount
            * (mConfig.bitDepth / 8)
        : 0;

    // Final slot sizes stored as members so fillOutputTransfer and
    // processInputTransfer can use them as their per-packet stride into
    // the buffer (matches what libusb_fill_iso_transfer reserves).
    mOutputSlotBytes = mOutputInterface
        ? std::max({outputPacketSizeNominal + outputClockMarginBytes,
                    outputEndpointMax,
                    outputPacketSizeNominal})
        : 0;
    mInputSlotBytes = mInputInterface
        ? std::max(inputPacketSizeNominal, inputEndpointMax)
        : 0;

    if (mOutputInterface) {
        LOGI("Allocating output transfers: nominal=%d, clockMargin=+%d, "
             "endpoint wMaxPacketSize=0x%04x (effective=%d), slot=%d bytes/packet%s",
             outputPacketSizeNominal, outputClockMarginBytes,
             mOutputInterface->dataEndpoint.maxPacketSize, outputEndpointMax,
             mOutputSlotBytes,
             (mOutputSlotBytes > outputPacketSizeNominal)
                 ? " (grew past nominal)"
                 : "");
    }
    if (mInputInterface) {
        LOGI("Allocating input transfers:  nominal=%d, "
             "endpoint wMaxPacketSize=0x%04x (effective=%d), slot=%d bytes/packet%s",
             inputPacketSizeNominal,
             mInputInterface->dataEndpoint.maxPacketSize, inputEndpointMax,
             mInputSlotBytes,
             (mInputSlotBytes > inputPacketSizeNominal)
                 ? " (grew past nominal — endpoint accepts larger packets)"
                 : "");
    }

    // Allocate output transfers
    if (mOutputInterface) {
        const int outputTransferSize = mOutputSlotBytes * numPackets;
        for (int i = 0; i < mConfig.numTransfers; ++i) {
            auto ctx = std::make_unique<IsoTransfer>();
            ctx->manager = this;
            ctx->isOutput = true;
            ctx->packetCount = numPackets;
            ctx->buffer.resize(static_cast<size_t>(outputTransferSize));

            ctx->transfer = libusb_alloc_transfer(numPackets);
            if (!ctx->transfer) {
                LOGE("Failed to allocate output transfer %d", i);
                return false;
            }

            // Fill isochronous transfer
            libusb_fill_iso_transfer(
                ctx->transfer,
                mDeviceHandle,
                mOutputInterface->dataEndpoint.address,
                ctx->buffer.data(),
                static_cast<int>(ctx->buffer.size()),
                numPackets,
                outputTransferCallback,
                ctx.get(),
                mConfig.transferTimeoutMs
            );

            // Set individual packet sizes for OUTPUT. On output this is the
            // upper bound the host is allowed to push per packet; the
            // clock controller's per-packet frame adjustment (in
            // fillOutputTransfer) writes a smaller length at runtime.
            libusb_set_iso_packet_lengths(ctx->transfer,
                static_cast<unsigned int>(mOutputSlotBytes));

            mOutputTransfers.push_back(std::move(ctx));
        }
    }

    // Allocate input transfers (using INPUT-specific slot size)
    if (mInputInterface) {
        const int inputTransferSize = mInputSlotBytes * numPackets;
        LOGI("Allocating input transfers: size=%d bytes (%d packets × %d bytes/packet)",
             inputTransferSize, numPackets, mInputSlotBytes);

        for (int i = 0; i < mConfig.numTransfers; ++i) {
            auto ctx = std::make_unique<IsoTransfer>();
            ctx->manager = this;
            ctx->isOutput = false;
            ctx->packetCount = numPackets;
            ctx->buffer.resize(static_cast<size_t>(inputTransferSize));

            ctx->transfer = libusb_alloc_transfer(numPackets);
            if (!ctx->transfer) {
                LOGE("Failed to allocate input transfer %d", i);
                return false;
            }

            libusb_fill_iso_transfer(
                ctx->transfer,
                mDeviceHandle,
                mInputInterface->dataEndpoint.address,
                ctx->buffer.data(),
                static_cast<int>(ctx->buffer.size()),
                numPackets,
                inputTransferCallback,
                ctx.get(),
                mConfig.transferTimeoutMs
            );

            // Set individual packet sizes for INPUT. The device can fill
            // up to this many bytes per packet; actual_length in each
            // iso_packet_desc tells us how many it actually sent.
            libusb_set_iso_packet_lengths(ctx->transfer,
                static_cast<unsigned int>(mInputSlotBytes));

            mInputTransfers.push_back(std::move(ctx));
        }
    }

    // Allocate feedback transfer
    if (mFeedbackEnabled && mFeedbackEndpoint) {
        mFeedbackTransfer = libusb_alloc_transfer(1);
        if (mFeedbackTransfer) {
            libusb_fill_iso_transfer(
                mFeedbackTransfer,
                mDeviceHandle,
                mFeedbackEndpoint->endpoint.address,
                mFeedbackBuffer.data(),
                static_cast<int>(mFeedbackBuffer.size()),
                1,
                feedbackTransferCallback,
                this,
                mConfig.transferTimeoutMs
            );
            // UAC 1.0 (full-speed): 10.14 fixed point, 3 bytes per packet.
            // UAC 2.0 (high-speed): 16.16 fixed point, 4 bytes per packet.
            // If the version was never set we default to UAC2 to preserve
            // historical behavior.
            const int feedbackLen = (mUacVersion == UacVersion::UAC_1_0)
                ? UAC_FEEDBACK_LENGTH_UAC1
                : UAC_FEEDBACK_LENGTH_UAC2;
            libusb_set_iso_packet_lengths(
                mFeedbackTransfer,
                static_cast<unsigned int>(feedbackLen));
            LOGI("Feedback iso transfer allocated: packetLen=%d (UAC%d)",
                 feedbackLen,
                 mUacVersion == UacVersion::UAC_1_0 ? 1 :
                 mUacVersion == UacVersion::UAC_2_0 ? 2 : 0);
        }
    }

    return true;
}

void UsbTransferManager::freeTransfers() {
    for (auto& ctx : mOutputTransfers) {
        if (ctx->transfer) {
            libusb_free_transfer(ctx->transfer);
            ctx->transfer = nullptr;
        }
    }
    mOutputTransfers.clear();

    for (auto& ctx : mInputTransfers) {
        if (ctx->transfer) {
            libusb_free_transfer(ctx->transfer);
            ctx->transfer = nullptr;
        }
    }
    mInputTransfers.clear();

    if (mFeedbackTransfer) {
        libusb_free_transfer(mFeedbackTransfer);
        mFeedbackTransfer = nullptr;
    }
}

// ============================================================================
// Transfer Callbacks
// ============================================================================

void LIBUSB_CALL UsbTransferManager::outputTransferCallback(libusb_transfer* transfer) {
    auto* ctx = static_cast<IsoTransfer*>(transfer->user_data);
    if (ctx && ctx->manager) {
        ctx->manager->handleOutputComplete(ctx, transfer);
    }
}

void LIBUSB_CALL UsbTransferManager::inputTransferCallback(libusb_transfer* transfer) {
    auto* ctx = static_cast<IsoTransfer*>(transfer->user_data);
    if (ctx && ctx->manager) {
        ctx->manager->handleInputComplete(ctx, transfer);
    }
}

void LIBUSB_CALL UsbTransferManager::feedbackTransferCallback(libusb_transfer* transfer) {
    auto* manager = static_cast<UsbTransferManager*>(transfer->user_data);
    if (manager) {
        manager->handleFeedbackComplete(transfer);
    }
}

void UsbTransferManager::handleOutputComplete(IsoTransfer* ctx, libusb_transfer* transfer) {
    mOutputPendingCount.fetch_sub(1);

    // Profile: record transfer completion
    if (mLatencyProfiler.isEnabled()) {
        mLatencyProfiler.onOutputCompleted(ctx->profilingToken);
    }

    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        // Check individual packet status
        int completedPackets = 0;
        for (int i = 0; i < ctx->packetCount; ++i) {
            if (transfer->iso_packet_desc[i].status == LIBUSB_TRANSFER_COMPLETED) {
                completedPackets++;
            }
        }
        mStats.packetsCompleted.fetch_add(static_cast<uint64_t>(completedPackets));

        // Watchdog: Update last completed time and reset error counter
        mLastCompletedTimeMs.store(getCurrentTimeMs(), std::memory_order_release);
        mConsecutiveErrors.store(0, std::memory_order_release);

        // Adaptive buffer: track successful output transfer
        if (mBufferController) {
            mBufferController->onTransferComplete();
        }

    } else if (transfer->status == LIBUSB_TRANSFER_CANCELLED) {
        // Expected during stop
        return;

    } else if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        reportError(UsbAudioError::DEVICE_DISCONNECTED, "Device disconnected");
        return;

    } else {
        mStats.packetsErrors.fetch_add(1);
        mConsecutiveErrors.fetch_add(1, std::memory_order_release);  // Watchdog: track errors
        LOGW("Output transfer error: %d", transfer->status);
    }

    // Resubmit if still running and device not disconnected
    if (!mStopRequested.load(std::memory_order_acquire) &&
        !mDeviceDisconnected.load(std::memory_order_acquire)) {
        if (!fillOutputTransfer(ctx)) {
            mStats.underruns.fetch_add(1, std::memory_order_relaxed);
            // Adaptive buffer: track underrun
            if (mBufferController) {
                mBufferController->onUnderrun();
            }
        }
        // Profile: record new transfer submission
        if (mLatencyProfiler.isEnabled()) {
            ctx->profilingToken = mLatencyProfiler.onOutputSubmitted();
        }
        if (submitTransfer(transfer)) {
            mOutputPendingCount.fetch_add(1);
        }
    }

    // Output ring drained one transfer's worth -> wake the DSP thread so
    // it can refill it without polling. Wait-free release on the futex.
    notifyDataReady();
}

void UsbTransferManager::handleInputComplete(IsoTransfer* ctx, libusb_transfer* transfer) {
    mInputPendingCount.fetch_sub(1);

    // Profile: record transfer completion
    if (mLatencyProfiler.isEnabled()) {
        mLatencyProfiler.onInputCompleted(ctx->profilingToken);
    }

    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        processInputTransfer(ctx);

        // Watchdog: Update last completed time and reset error counter
        mLastCompletedTimeMs.store(getCurrentTimeMs(), std::memory_order_release);
        mConsecutiveErrors.store(0, std::memory_order_release);

    } else if (transfer->status == LIBUSB_TRANSFER_CANCELLED) {
        return;

    } else if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        reportError(UsbAudioError::DEVICE_DISCONNECTED, "Device disconnected");
        return;

    } else {
        mStats.packetsErrors.fetch_add(1);
        mConsecutiveErrors.fetch_add(1, std::memory_order_release);  // Watchdog: track errors
    }

    // Resubmit if still running and device not disconnected
    if (!mStopRequested.load(std::memory_order_acquire) &&
        !mDeviceDisconnected.load(std::memory_order_acquire)) {
        // Profile: record new transfer submission
        if (mLatencyProfiler.isEnabled()) {
            ctx->profilingToken = mLatencyProfiler.onInputSubmitted();
        }
        if (submitTransfer(transfer)) {
            mInputPendingCount.fetch_add(1);
        }
    }

    // Input ring just got fresh samples -> wake the DSP thread so it can
    // consume them. The output handler does the same; either wake suffices
    // because the DSP loop re-checks both buffers after waking.
    notifyDataReady();
}

void UsbTransferManager::handleFeedbackComplete(libusb_transfer* transfer) {
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        // Use the explicit UAC version we were told about at setup time
        // instead of inferring from packet length (which used to misclassify
        // UAC1 devices that happened to send 4-byte aligned packets).
        // Default to UAC2 if the version was never set, matching the legacy
        // behavior.
        const UacVersion version = (mUacVersion == UacVersion::UAC_1_0)
            ? UacVersion::UAC_1_0
            : UacVersion::UAC_2_0;
        const int expectedLen = (version == UacVersion::UAC_1_0)
            ? UAC_FEEDBACK_LENGTH_UAC1
            : UAC_FEEDBACK_LENGTH_UAC2;
        const int actualLen = transfer->iso_packet_desc[0].actual_length;
        if (actualLen >= expectedLen) {
            mClockController->processFeedback(
                mFeedbackBuffer.data(), actualLen, version);
        }

    } else if (transfer->status == LIBUSB_TRANSFER_CANCELLED) {
        return;

    } else if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        mDeviceDisconnected.store(true, std::memory_order_release);
        return;
    }

    // Resubmit if still running and device not disconnected
    if (!mStopRequested.load(std::memory_order_acquire) &&
        !mDeviceDisconnected.load(std::memory_order_acquire)) {
        submitTransfer(transfer);
    }
}

// ============================================================================
// Transfer Data Handling
// ============================================================================

bool UsbTransferManager::fillOutputTransfer(IsoTransfer* ctx) {
    const int bytesPerSample = AudioFormatConverter::getBytesPerSample(mConfig.pcmFormat);

    // Get adjusted frame count from the clock controller. This may be
    // ± a few frames from nominal depending on the async feedback — the
    // slot size reserved at allocate time accounts for the upper bound.
    const int nominalFrames = mConfig.framesPerPacket;
    const int adjustedFrames = mClockController->getAdjustedFrameCount(nominalFrames);

    const int samplesPerPacket = adjustedFrames * mConfig.channelCount;
    const int bytesPerPacket = samplesPerPacket * bytesPerSample;
    const size_t samplesNeeded = static_cast<size_t>(samplesPerPacket * ctx->packetCount);

    // Read the whole transfer worth of samples from the ring in one go.
    bool success = mOutputRingBuffer->read(mFloatBuffer.data(), samplesNeeded);
    if (!success) {
        // Underrun - fill the temp buffer with silence so every packet
        // we submit is well-formed (audible dropout rather than an error).
        std::memset(mFloatBuffer.data(), 0, samplesNeeded * sizeof(float));
    }

    // Write each packet into ITS OWN slot inside ctx->buffer. Slots are
    // `mOutputSlotBytes` apart — which may be larger than `bytesPerPacket`
    // when the endpoint declares a larger wMaxPacketSize or when we reserved
    // headroom for the clock adjustment. Writing samples contiguously at
    // the start of the buffer (the previous behavior) places packet i's
    // data at offset i*bytesPerPacket, but libusb reads slot i from
    // i*slotBytes — any mismatch between those two strides corrupts the
    // stream on the wire, which is exactly the kind of subtle distortion
    // that shows up only on devices whose endpoint differs from nominal.
    uint8_t* bufPtr = ctx->buffer.data();
    for (int p = 0; p < ctx->packetCount; ++p) {
        const size_t srcSampleOffset = static_cast<size_t>(p * samplesPerPacket);
        uint8_t* slotPtr = bufPtr + static_cast<ptrdiff_t>(p) * mOutputSlotBytes;
        mFormatConverter.floatToPcm(
            mFloatBuffer.data() + srcSampleOffset,
            slotPtr,
            static_cast<size_t>(samplesPerPacket),
            mConfig.pcmFormat);

        // Update per-packet length and reset status before resubmission.
        ctx->transfer->iso_packet_desc[p].length = static_cast<unsigned int>(bytesPerPacket);
        ctx->transfer->iso_packet_desc[p].actual_length = 0;
        ctx->transfer->iso_packet_desc[p].status = LIBUSB_TRANSFER_COMPLETED;
    }

    mStats.packetsSubmitted.fetch_add(static_cast<uint64_t>(ctx->packetCount));

    return success;
}

bool UsbTransferManager::processInputTransfer(IsoTransfer* ctx) {
    int bytesPerSample = mConfig.inputBitDepth / 8;

    // Process each packet
    uint8_t* bufPtr = ctx->buffer.data();
    size_t totalSamples = 0;

    // The per-packet stride in the buffer is the slot size libusb reserved
    // at allocation time, not the nominal bytesPerPacket — these can differ
    // when the endpoint's wMaxPacketSize is larger than nominal (async USB
    // devices with drift headroom). Using the nominal stride would read
    // from a shifted position inside slot i, producing garbled audio on
    // any device where the two differ.
    const int slotBytes = mInputSlotBytes;

    for (int i = 0; i < ctx->packetCount; ++i) {
        auto& desc = ctx->transfer->iso_packet_desc[i];
        if (desc.status == LIBUSB_TRANSFER_COMPLETED && desc.actual_length > 0) {
            int samplesInPacket = desc.actual_length / bytesPerSample;

            // Convert PCM to float starting at the beginning of this slot.
            // desc.actual_length tells us how much of the slot the device
            // actually filled this time around — bytes past that point in
            // the same slot are stale and must not be read.
            mFormatConverter.pcmToFloat(
                bufPtr + (i * slotBytes),
                mFloatBuffer.data() + totalSamples,
                static_cast<size_t>(samplesInPacket),
                mConfig.pcmFormat
            );

            totalSamples += static_cast<size_t>(samplesInPacket);
        }
    }

    // Write to input ring buffer
    if (totalSamples > 0) {
        if (!mInputRingBuffer->write(mFloatBuffer.data(), totalSamples)) {
            mStats.overruns.fetch_add(1);
            return false;
        }
    }

    mStats.packetsCompleted.fetch_add(static_cast<uint64_t>(ctx->packetCount));
    return true;
}

bool UsbTransferManager::submitTransfer(libusb_transfer* transfer) {
    int result = libusb_submit_transfer(transfer);
    if (result != LIBUSB_SUCCESS) {
        LOGE("Failed to submit transfer: %s (EP=0x%02X, len=%d, type=%d)",
             libusb_error_name(result),
             transfer->endpoint,
             transfer->length,
             transfer->type);

        // On LIBUSB_ERROR_IO, report as device error
        if (result == LIBUSB_ERROR_IO || result == LIBUSB_ERROR_NO_DEVICE) {
            reportError(UsbAudioError::DEVICE_DISCONNECTED, "Device disconnected");
        }
        return false;
    }
    return true;
}

// ============================================================================
// Event Loop
// ============================================================================

void UsbTransferManager::eventLoopThread() {
    LOGI("USB event loop started");

    // Configure this thread for high priority audio I/O
    ThreadUtils::setCurrentThreadRealtime("UsbEventLoop", ThreadUtils::Priority::HIGH);

    // Pin to a different core than the DSP thread if possible
    int numCpus = ThreadUtils::getNumCpus();
    if (numCpus >= 4) {
        int targetCore = numCpus - 1;  // Last core for event loop
        if (ThreadUtils::setCurrentThreadCpuAffinity(targetCore)) {
            LOGI("Event loop thread pinned to core %d (of %d)", targetCore, numCpus);
        }
    }

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 1000; // P1-1: 1ms timeout (reduced from 10ms for lower latency)

    uint64_t lastWatchdogCheck = getCurrentTimeMs();

    // Deadline for the drain phase of stop(): once mStopRequested is set we
    // keep iterating until every pending transfer has produced its CANCELLED
    // callback, but never longer than this so we can't deadlock if a
    // callback is somehow lost.
    constexpr uint64_t DRAIN_DEADLINE_MS = 500;
    uint64_t drainStartMs = 0;

    while (true) {
        if (mDeviceDisconnected.load(std::memory_order_acquire)) {
            break;
        }

        // Handle USB events
        int result = libusb_handle_events_timeout_completed(mContext, &timeout, nullptr);
        if (result != LIBUSB_SUCCESS && result != LIBUSB_ERROR_TIMEOUT) {
            LOGE("libusb event error: %s", libusb_error_name(result));
            if (result == LIBUSB_ERROR_NO_DEVICE) {
                reportError(UsbAudioError::DEVICE_DISCONNECTED, "Device disconnected");
                break;
            }
        }

        // Watchdog: Check periodically for device responsiveness (only while
        // the stream is actually running — during the drain phase we expect
        // no new transfers to complete).
        if (!mStopRequested.load(std::memory_order_acquire)) {
            uint64_t now = getCurrentTimeMs();
            if (now - lastWatchdogCheck >= WATCHDOG_CHECK_INTERVAL_MS) {
                lastWatchdogCheck = now;
                if (checkWatchdog()) {
                    LOGW("Watchdog detected device unresponsive, triggering disconnect");
                    reportError(UsbAudioError::DEVICE_DISCONNECTED,
                                "Device unresponsive (watchdog timeout)");
                    break;
                }
            }
        }

        // Exit condition — this is the only place we leave the loop during
        // a graceful stop(). We deliberately keep running after mStopRequested
        // goes true until every pending transfer has delivered its CANCELLED
        // callback, because libusb keeps those in its flying_transfers list
        // until the callback fires. Freeing the transfer structs (which
        // happens after stop() returns) while libusb still has pointers to
        // them causes use-after-free crashes in the next libusb_control_transfer
        // that happens to process events on the same context — which is
        // exactly what stage 1's sample-rate negotiation does on every
        // backend restart.
        if (mStopRequested.load(std::memory_order_acquire)) {
            if (drainStartMs == 0) {
                drainStartMs = getCurrentTimeMs();
            }
            const int outPending = mOutputPendingCount.load(std::memory_order_acquire);
            const int inPending = mInputPendingCount.load(std::memory_order_acquire);
            if (outPending == 0 && inPending == 0) {
                break;
            }
            // Safety timeout: if a cancelled callback never arrives after
            // half a second, give up rather than blocking the backend
            // shutdown forever. Leaks a transfer struct in the worst case,
            // which is still preferable to a deadlocked teardown.
            if (getCurrentTimeMs() - drainStartMs > DRAIN_DEADLINE_MS) {
                LOGW("Drain deadline exceeded after stop(): "
                     "output pending=%d input pending=%d — breaking anyway",
                     outPending, inPending);
                break;
            }
        }
    }

    LOGI("USB event loop stopped, disconnected=%d", mDeviceDisconnected.load());
}

// ============================================================================
// Interface Management
// ============================================================================

bool UsbTransferManager::claimInterface(int interfaceNum) {
    // Check if already claimed
    for (int claimed : mClaimedInterfaces) {
        if (claimed == interfaceNum) {
            return true;
        }
    }

    // Enable auto-detach of kernel driver (Android often needs this)
    int result = libusb_set_auto_detach_kernel_driver(mDeviceHandle, 1);
    if (result != LIBUSB_SUCCESS && result != LIBUSB_ERROR_NOT_SUPPORTED) {
        LOGW("libusb_set_auto_detach_kernel_driver failed: %s", libusb_error_name(result));
    }

    // Detach kernel driver if attached
    result = libusb_kernel_driver_active(mDeviceHandle, interfaceNum);
    if (result == 1) {
        result = libusb_detach_kernel_driver(mDeviceHandle, interfaceNum);
        if (result != LIBUSB_SUCCESS) {
            LOGW("Failed to detach kernel driver: %s", libusb_error_name(result));
            // Continue anyway - auto_detach might handle it
        } else {
            LOGI("Detached kernel driver from interface %d", interfaceNum);
        }
    } else if (result < 0) {
        LOGW("libusb_kernel_driver_active failed: %s", libusb_error_name(result));
    }

    // Claim interface
    result = libusb_claim_interface(mDeviceHandle, interfaceNum);
    if (result != LIBUSB_SUCCESS) {
        LOGE("Failed to claim interface %d: %s", interfaceNum, libusb_error_name(result));
        return false;
    }

    mClaimedInterfaces.push_back(interfaceNum);
    LOGI("Claimed interface %d", interfaceNum);
    return true;
}

void UsbTransferManager::releaseInterface(int interfaceNum) {
    // Set to zero-bandwidth alternate setting first
    libusb_set_interface_alt_setting(mDeviceHandle, interfaceNum, 0);

    int result = libusb_release_interface(mDeviceHandle, interfaceNum);
    if (result != LIBUSB_SUCCESS) {
        LOGW("Failed to release interface %d: %s", interfaceNum, libusb_error_name(result));
    } else {
        LOGI("Released interface %d", interfaceNum);
    }
}

bool UsbTransferManager::setAlternateSetting(int interfaceNum, int altSetting) {
    int result = libusb_set_interface_alt_setting(mDeviceHandle, interfaceNum, altSetting);
    if (result != LIBUSB_SUCCESS) {
        LOGE("Failed to set alt setting %d on interface %d: %s",
             altSetting, interfaceNum, libusb_error_name(result));
        return false;
    }
    LOGI("Set interface %d to alt setting %d", interfaceNum, altSetting);
    return true;
}

// ============================================================================
// Error Handling
// ============================================================================

void UsbTransferManager::reportError(UsbAudioError error, const char* message) {
    // Mark device as disconnected to prevent further USB operations
    if (error == UsbAudioError::DEVICE_DISCONNECTED) {
        mDeviceDisconnected.store(true, std::memory_order_release);
        LOGW("Device disconnected, stopping further USB operations");
    }

    std::lock_guard<std::mutex> lock(mCallbackMutex);
    if (mErrorCallback) {
        mErrorCallback(error, message);
    }
    LOGE("USB Error: %s", message);
}

// ============================================================================
// Watchdog Implementation
// ============================================================================

uint64_t UsbTransferManager::getCurrentTimeMs() {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    return static_cast<uint64_t>(ms);
}

bool UsbTransferManager::checkWatchdog() {
    uint64_t now = getCurrentTimeMs();
    uint64_t lastCompleted = mLastCompletedTimeMs.load(std::memory_order_acquire);

    // If we haven't received any completed transfers yet, use a longer grace period
    if (lastCompleted == 0) {
        return false;
    }

    uint64_t elapsed = now - lastCompleted;

    // Check for timeout (no successful transfers for too long)
    if (elapsed > WATCHDOG_TIMEOUT_MS) {
        LOGW("Watchdog: No successful transfers for %llu ms (threshold: %d ms)",
             (unsigned long long)elapsed, WATCHDOG_TIMEOUT_MS);
        return true;
    }

    // Check for too many consecutive errors
    int errors = mConsecutiveErrors.load(std::memory_order_acquire);
    if (errors >= MAX_CONSECUTIVE_ERRORS) {
        LOGW("Watchdog: %d consecutive transfer errors (threshold: %d)",
             errors, MAX_CONSECUTIVE_ERRORS);
        return true;
    }

    return false;
}

// ============================================================================
// Adaptive Buffer Reconfiguration
// ============================================================================

bool UsbTransferManager::reconfigureBufferSize(int newBufferMs) {
    // Clamp to valid range
    int minMs = 50;
    int maxMs = 200;
    newBufferMs = std::clamp(newBufferMs, minMs, maxMs);

    if (newBufferMs == mConfig.ringBufferMs) {
        return true;  // No change needed
    }

    int oldBufferMs = mConfig.ringBufferMs;
    mConfig.ringBufferMs = newBufferMs;

    // Calculate new ring buffer sizes
    size_t newOutputSize = static_cast<size_t>(mConfig.ringBufferSamples());
    size_t newInputSize = static_cast<size_t>(mConfig.inputRingBufferSamples());

    LOGI("Reconfiguring buffer: %dms -> %dms (output=%zu samples, input=%zu samples)",
         oldBufferMs, newBufferMs, newOutputSize, newInputSize);

    // Resize ring buffers
    // Note: resize() clears the buffer content, which may cause a brief audio gap
    if (mOutputRingBuffer) {
        mOutputRingBuffer->resize(newOutputSize);
        MemoryUtils::prepareForRealtime(mOutputRingBuffer->data(), mOutputRingBuffer->sizeBytes());
    }

    if (mInputRingBuffer) {
        mInputRingBuffer->resize(newInputSize);
        MemoryUtils::prepareForRealtime(mInputRingBuffer->data(), mInputRingBuffer->sizeBytes());
    }

    // Update buffer controller
    if (mBufferController) {
        mBufferController->setCurrentBufferMs(newBufferMs);
    }

    // Pre-fill with silence to prevent initial underrun after resize
    if (mOutputRingBuffer && mIsRunning.load()) {
        size_t prefillSamples = static_cast<size_t>(mConfig.framesPerPacket *
                                                      mConfig.packetsPerTransfer *
                                                      mConfig.numTransfers *
                                                      mConfig.channelCount);
        std::vector<float> silence(prefillSamples, 0.0f);
        mOutputRingBuffer->write(silence.data(), silence.size());
    }

    LOGI("Buffer reconfiguration complete: %dms", newBufferMs);
    return true;
}

} // namespace usb
} // namespace watermelon_audio
