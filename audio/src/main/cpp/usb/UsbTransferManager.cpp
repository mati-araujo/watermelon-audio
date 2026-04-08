/**
 * UsbTransferManager.cpp
 *
 * Implementation of USB isochronous transfer management.
 */

#include "UsbTransferManager.h"
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

namespace noisypad {
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
    mFeedbackEnabled = enabled;
    if (endpoint) {
        mFeedbackEndpoint = *endpoint;
    } else {
        mFeedbackEndpoint.reset();
    }
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
    int numPackets = mConfig.packetsPerTransfer;

    // Separate packet sizes for output vs input
    int outputPacketSize = mConfig.bytesPerPacket();
    int inputPacketSize = mConfig.inputBytesPerPacket();

    LOGI("Allocating transfers: outputPacketSize=%d, inputPacketSize=%d",
         outputPacketSize, inputPacketSize);

    // Allocate output transfers
    if (mOutputInterface) {
        for (int i = 0; i < mConfig.numTransfers; ++i) {
            auto ctx = std::make_unique<IsoTransfer>();
            ctx->manager = this;
            ctx->isOutput = true;
            ctx->packetCount = numPackets;
            ctx->buffer.resize(static_cast<size_t>(mConfig.bytesPerTransfer()));

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

            // Set individual packet sizes for OUTPUT
            libusb_set_iso_packet_lengths(ctx->transfer, static_cast<unsigned int>(outputPacketSize));

            mOutputTransfers.push_back(std::move(ctx));
        }
    }

    // Allocate input transfers (using INPUT-specific sizes)
    if (mInputInterface) {
        int inputTransferSize = mConfig.inputBytesPerTransfer();
        LOGI("Allocating input transfers: size=%d bytes (%d packets x %d bytes/packet)",
             inputTransferSize, numPackets, inputPacketSize);

        for (int i = 0; i < mConfig.numTransfers; ++i) {
            auto ctx = std::make_unique<IsoTransfer>();
            ctx->manager = this;
            ctx->isOutput = false;
            ctx->packetCount = numPackets;
            // FIX: Use input-specific buffer size
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

            // FIX: Set individual packet sizes for INPUT
            libusb_set_iso_packet_lengths(ctx->transfer, static_cast<unsigned int>(inputPacketSize));

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
            libusb_set_iso_packet_lengths(mFeedbackTransfer, 4);
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
}

void UsbTransferManager::handleFeedbackComplete(libusb_transfer* transfer) {
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        // Parse feedback data
        if (transfer->iso_packet_desc[0].actual_length >= 3) {
            UacVersion version = UacVersion::UAC_1_0;
            int length = transfer->iso_packet_desc[0].actual_length;
            if (length >= 4) {
                version = UacVersion::UAC_2_0;
            }
            mClockController->processFeedback(mFeedbackBuffer.data(), length, version);
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
    int samplesPerPacket = mConfig.framesPerPacket * mConfig.channelCount;
    int bytesPerSample = AudioFormatConverter::getBytesPerSample(mConfig.pcmFormat);

    // Get adjusted frame count from clock controller
    int nominalFrames = mConfig.framesPerPacket;
    int adjustedFrames = mClockController->getAdjustedFrameCount(nominalFrames);

    // Read from ring buffer
    size_t samplesNeeded = static_cast<size_t>(adjustedFrames * mConfig.channelCount *
                                                 ctx->packetCount);
    bool success = mOutputRingBuffer->read(mFloatBuffer.data(), samplesNeeded);

    if (!success) {
        // Underrun - fill with silence
        std::memset(mFloatBuffer.data(), 0, samplesNeeded * sizeof(float));
    }

    // Convert to PCM format
    mFormatConverter.floatToPcm(
        mFloatBuffer.data(),
        ctx->buffer.data(),
        samplesNeeded,
        mConfig.pcmFormat
    );

    // Update packet sizes (may vary for clock adjustment)
    // Also reset actual_length - required before resubmission on some devices
    int bytesPerPacket = adjustedFrames * mConfig.channelCount * bytesPerSample;
    for (int i = 0; i < ctx->packetCount; ++i) {
        ctx->transfer->iso_packet_desc[i].length = static_cast<unsigned int>(bytesPerPacket);
        ctx->transfer->iso_packet_desc[i].actual_length = 0;
        ctx->transfer->iso_packet_desc[i].status = LIBUSB_TRANSFER_COMPLETED;
    }

    mStats.packetsSubmitted.fetch_add(static_cast<uint64_t>(ctx->packetCount));

    return success;
}

bool UsbTransferManager::processInputTransfer(IsoTransfer* ctx) {
    // FIX: Use input-specific bytes per sample based on input bit depth
    int bytesPerSample = mConfig.inputBitDepth / 8;

    // Process each packet
    uint8_t* bufPtr = ctx->buffer.data();
    size_t totalSamples = 0;

    // FIX: Use input-specific packet size (may differ from output)
    int inputPacketSize = mConfig.inputBytesPerPacket();

    for (int i = 0; i < ctx->packetCount; ++i) {
        auto& desc = ctx->transfer->iso_packet_desc[i];
        if (desc.status == LIBUSB_TRANSFER_COMPLETED && desc.actual_length > 0) {
            int samplesInPacket = desc.actual_length / bytesPerSample;

            // Convert PCM to float
            // FIX: Use inputPacketSize for buffer offset, not output bytesPerPacket
            mFormatConverter.pcmToFloat(
                bufPtr + (i * inputPacketSize),
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

    while (!mStopRequested.load(std::memory_order_acquire) &&
           !mDeviceDisconnected.load(std::memory_order_acquire)) {
        // Handle USB events
        int result = libusb_handle_events_timeout_completed(mContext, &timeout, nullptr);
        if (result != LIBUSB_SUCCESS && result != LIBUSB_ERROR_TIMEOUT) {
            LOGE("libusb event error: %s", libusb_error_name(result));
            if (result == LIBUSB_ERROR_NO_DEVICE) {
                reportError(UsbAudioError::DEVICE_DISCONNECTED, "Device disconnected");
                break;
            }
        }

        // Watchdog: Check periodically for device responsiveness
        uint64_t now = getCurrentTimeMs();
        if (now - lastWatchdogCheck >= WATCHDOG_CHECK_INTERVAL_MS) {
            lastWatchdogCheck = now;
            if (checkWatchdog()) {
                LOGW("Watchdog detected device unresponsive, triggering disconnect");
                reportError(UsbAudioError::DEVICE_DISCONNECTED, "Device unresponsive (watchdog timeout)");
                break;
            }
        }

        // Check if all transfers have completed (during stop)
        if (mStopRequested.load() &&
            mOutputPendingCount.load() == 0 &&
            mInputPendingCount.load() == 0) {
            break;
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
} // namespace noisypad
