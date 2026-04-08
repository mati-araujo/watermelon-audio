/**
 * LibusbBackend.cpp
 *
 * Implementation of USB Audio backend using libusb.
 */

#include "LibusbBackend.h"
#include "../utils/ThreadUtils.h"
#include "../utils/MemoryUtils.h"
#include "../platform/Logger.h"
#include <cstring>
#include <algorithm>
#include <chrono>
#include <vector>

#define LOG_TAG "LibusbBackend"
#undef LOGI
#undef LOGW
#undef LOGE
#undef LOGD
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) wma::logMessage(wma::LogLevel::WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) wma::logMessage(wma::LogLevel::DEBUG, LOG_TAG, __VA_ARGS__)

namespace noisypad {

// ============================================================================
// Constructor / Destructor
// ============================================================================

LibusbBackend::LibusbBackend() {
    LOGI("LibusbBackend created");
}

LibusbBackend::~LibusbBackend() {
    stop();
    cleanup();
    LOGI("LibusbBackend destroyed");
}

// ============================================================================
// USB Initialization
// ============================================================================

bool LibusbBackend::initializeFromFileDescriptor(int fd, const char* usbfsPath) {
    std::lock_guard<std::mutex> lock(mMutex);

    if (mDeviceReady.load()) {
        LOGW("Already initialized, cleaning up first");
        cleanup();
    }

    LOGI("Initializing from fd=%d, path=%s", fd, usbfsPath ? usbfsPath : "null");

    // On Android, we must disable device discovery before libusb_init()
    // because Android doesn't allow direct USB enumeration.
    // We use wrapped file descriptors from Android's USB API instead.
    int result = libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    if (result != LIBUSB_SUCCESS) {
        LOGW("libusb_set_option(NO_DEVICE_DISCOVERY) returned: %s (continuing anyway)",
             libusb_error_name(result));
        // Don't fail here - some libusb versions may not support this option
    }

    // Initialize libusb context
    result = libusb_init(&mContext);
    if (result != LIBUSB_SUCCESS) {
        LOGE("Failed to init libusb: %s", libusb_error_name(result));
        mLastLibusbError.store(result);
        return false;
    }
    mOwnsContext = true;

    // Wrap the Android file descriptor
    result = libusb_wrap_sys_device(mContext, fd, &mDeviceHandle);
    if (result != LIBUSB_SUCCESS) {
        LOGE("Failed to wrap fd: %s", libusb_error_name(result));
        mLastLibusbError.store(result);
        libusb_exit(mContext);
        mContext = nullptr;
        return false;
    }

    // Store device info
    mDeviceInfo.fileDescriptor = fd;
    mDeviceInfo.usbfsPath = usbfsPath ? usbfsPath : "";

    // Get device descriptor
    libusb_device* dev = libusb_get_device(mDeviceHandle);
    if (dev) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(dev, &desc) == LIBUSB_SUCCESS) {
            mDeviceInfo.vendorId = desc.idVendor;
            mDeviceInfo.productId = desc.idProduct;
            mDeviceInfo.bcdDevice = desc.bcdDevice;
            mDeviceInfo.deviceClass = desc.bDeviceClass;
            mDeviceInfo.deviceSubClass = desc.bDeviceSubClass;
            mDeviceInfo.deviceProtocol = desc.bDeviceProtocol;

            LOGI("USB Device: VID=0x%04X, PID=0x%04X", desc.idVendor, desc.idProduct);

            // Get string descriptors
            uint8_t buffer[256];
            if (desc.iManufacturer) {
                int len = libusb_get_string_descriptor_ascii(mDeviceHandle,
                    desc.iManufacturer, buffer, sizeof(buffer));
                if (len > 0) {
                    mDeviceInfo.manufacturer = std::string(reinterpret_cast<char*>(buffer), len);
                    LOGI("Manufacturer: %s", mDeviceInfo.manufacturer.c_str());
                }
            }
            if (desc.iProduct) {
                int len = libusb_get_string_descriptor_ascii(mDeviceHandle,
                    desc.iProduct, buffer, sizeof(buffer));
                if (len > 0) {
                    mDeviceInfo.product = std::string(reinterpret_cast<char*>(buffer), len);
                    LOGI("Product: %s", mDeviceInfo.product.c_str());
                }
            }
            if (desc.iSerialNumber) {
                int len = libusb_get_string_descriptor_ascii(mDeviceHandle,
                    desc.iSerialNumber, buffer, sizeof(buffer));
                if (len > 0) {
                    mDeviceInfo.serialNumber = std::string(reinterpret_cast<char*>(buffer), len);
                }
            }
        }
    }

    // Parse audio descriptors
    if (!parseDeviceDescriptors()) {
        LOGE("Failed to parse device descriptors");
        cleanup();
        return false;
    }

    // Select best interfaces for streaming
    if (!selectBestInterfaces()) {
        LOGE("No suitable audio interfaces found");
        cleanup();
        return false;
    }

    // Initialize volume controls based on Feature Unit descriptors
    initializeVolumeControls();

    mDeviceReady.store(true);
    LOGI("USB device initialized successfully");
    return true;
}

bool LibusbBackend::parseDeviceDescriptors() {
    libusb_device* dev = libusb_get_device(mDeviceHandle);
    if (!dev) {
        return false;
    }

    // First, get the parsed config descriptor to know the total length
    libusb_config_descriptor* configDesc = nullptr;
    int result = libusb_get_active_config_descriptor(dev, &configDesc);
    if (result != LIBUSB_SUCCESS) {
        // Try first config if no active
        result = libusb_get_config_descriptor(dev, 0, &configDesc);
        if (result != LIBUSB_SUCCESS) {
            LOGE("Failed to get config descriptor: %s", libusb_error_name(result));
            return false;
        }
    }

    uint16_t totalLength = configDesc->wTotalLength;
    uint8_t configValue = configDesc->bConfigurationValue;
    libusb_free_config_descriptor(configDesc);

    LOGI("Config descriptor: totalLength=%d, configValue=%d", totalLength, configValue);

    // Now fetch the RAW configuration descriptor bytes using control transfer
    // USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE = 0x80
    // USB_REQ_GET_DESCRIPTOR = 0x06
    // Descriptor type CONFIGURATION = 0x02
    std::vector<uint8_t> rawDescriptor(totalLength);

    result = libusb_control_transfer(
        mDeviceHandle,
        0x80,                           // bmRequestType: Device-to-host, Standard, Device
        0x06,                           // bRequest: GET_DESCRIPTOR
        (0x02 << 8) | 0,                // wValue: Descriptor type (Configuration) | Index
        0,                              // wIndex: Language ID (0 for config descriptor)
        rawDescriptor.data(),
        totalLength,
        1000                            // Timeout in ms
    );

    if (result < 0) {
        LOGE("Failed to get raw config descriptor: %s", libusb_error_name(result));
        return false;
    }

    if (result != totalLength) {
        LOGW("Got %d bytes, expected %d", result, totalLength);
    }

    LOGI("Got raw config descriptor: %d bytes", result);

    // Parse using our USB Audio descriptor parser
    usb::UsbDescriptorParser parser;
    auto parseResult = parser.parse(rawDescriptor.data(), result, mDeviceInfo);

    if (!parseResult) {
        LOGE("USB Audio descriptor parsing failed");
        return false;
    }

    mUsbDevice = std::move(parseResult);

    LOGI("Parsed USB Audio device:");
    LOGI("  UAC Version: %d", mUsbDevice->uacVersion);
    LOGI("  Playback interfaces: %zu", mUsbDevice->playbackInterfaces.size());
    LOGI("  Capture interfaces: %zu", mUsbDevice->captureInterfaces.size());

    return true;
}

bool LibusbBackend::selectBestInterfaces() {
    if (!mUsbDevice) {
        return false;
    }

    // Clear previous selections
    mSelectedPlayback.reset();
    mSelectedCapture.reset();

    bool needsPlayback = (mStreamingMode == UsbStreamingMode::PLAYBACK_ONLY ||
                          mStreamingMode == UsbStreamingMode::FULL_DUPLEX);
    bool needsCapture = (mStreamingMode == UsbStreamingMode::CAPTURE_ONLY ||
                         mStreamingMode == UsbStreamingMode::FULL_DUPLEX);

    LOGI("selectBestInterfaces: mode=%s, needsPlayback=%d, needsCapture=%d",
         mStreamingMode == UsbStreamingMode::PLAYBACK_ONLY ? "PLAYBACK_ONLY" :
         mStreamingMode == UsbStreamingMode::CAPTURE_ONLY ? "CAPTURE_ONLY" : "FULL_DUPLEX",
         needsPlayback, needsCapture);

    // Find best playback interface matching requested sample rate
    if (needsPlayback && !mUsbDevice->playbackInterfaces.empty()) {
        // Try to find interface with requested sample rate
        for (const auto& iface : mUsbDevice->playbackInterfaces) {
            if (iface.format.supportsSampleRate(mRequestedSampleRate)) {
                mSelectedPlayback = iface;
                LOGI("Selected playback: IF%d Alt%d, %dHz, %dch, %dbit",
                     iface.interfaceNumber, iface.alternateSetting,
                     mRequestedSampleRate, iface.format.channels, iface.format.bitResolution);
                break;
            }
        }

        // Fallback to first available
        if (!mSelectedPlayback && !mUsbDevice->playbackInterfaces.empty()) {
            mSelectedPlayback = mUsbDevice->playbackInterfaces[0];
            // Use first available sample rate
            if (!mSelectedPlayback->format.sampleRates.empty()) {
                mRequestedSampleRate = mSelectedPlayback->format.sampleRates[0];
            } else if (mSelectedPlayback->format.hasContinuousRates) {
                mRequestedSampleRate = std::min(48000, mSelectedPlayback->format.maxSampleRate);
            }
            LOGI("Fallback playback: IF%d Alt%d, %dHz",
                 mSelectedPlayback->interfaceNumber, mSelectedPlayback->alternateSetting,
                 mRequestedSampleRate);
        }
    }

    // Find best capture interface
    if (needsCapture && !mUsbDevice->captureInterfaces.empty()) {
        for (const auto& iface : mUsbDevice->captureInterfaces) {
            if (iface.format.supportsSampleRate(mRequestedSampleRate)) {
                mSelectedCapture = iface;
                LOGI("Selected capture: IF%d Alt%d, %dHz, %dch, %dbit",
                     iface.interfaceNumber, iface.alternateSetting,
                     mRequestedSampleRate, iface.format.channels, iface.format.bitResolution);
                break;
            }
        }

        // Fallback to first available capture if no matching sample rate
        if (!mSelectedCapture && !mUsbDevice->captureInterfaces.empty()) {
            mSelectedCapture = mUsbDevice->captureInterfaces[0];
            // If capture-only mode, use capture's sample rate
            if (mStreamingMode == UsbStreamingMode::CAPTURE_ONLY) {
                if (!mSelectedCapture->format.sampleRates.empty()) {
                    mRequestedSampleRate = mSelectedCapture->format.sampleRates[0];
                }
            }
            LOGI("Fallback capture: IF%d Alt%d, %dHz",
                 mSelectedCapture->interfaceNumber, mSelectedCapture->alternateSetting,
                 mRequestedSampleRate);
        }
    }

    // Validate we have the required interfaces
    bool success = true;
    if (needsPlayback && !mSelectedPlayback.has_value()) {
        LOGE("Playback required but no playback interface available");
        success = false;
    }
    if (needsCapture && !mSelectedCapture.has_value()) {
        LOGE("Capture required but no capture interface available");
        success = false;
    }

    return success;
}

// ============================================================================
// IAudioBackend Implementation
// ============================================================================

BackendResult LibusbBackend::start() {
    std::lock_guard<std::mutex> lock(mMutex);

    if (mIsRunning.load()) {
        return BackendResult::ERROR_ALREADY_RUNNING;
    }

    if (!mDeviceReady.load()) {
        LOGE("USB device not initialized");
        return BackendResult::ERROR_NOT_INITIALIZED;
    }

    if (!mCallback) {
        LOGE("No audio callback set");
        return BackendResult::ERROR_INVALID_CONFIG;
    }

    // Re-select interfaces with current mFullDuplexEnabled setting
    // This is needed because streaming mode may have changed since initialization
    if (!selectBestInterfaces()) {
        LOGE("No suitable audio interfaces found for current mode");
        return BackendResult::ERROR_INVALID_CONFIG;
    }

    LOGI("Starting with full-duplex=%s, capture selected=%s",
         mFullDuplexEnabled ? "true" : "false",
         mSelectedCapture.has_value() ? "yes" : "no");

    // FIX: Tear down any existing transfer manager from a previous stop/start cycle.
    // stop() pauses the transfer manager but doesn't destroy it. If we don't
    // clean it up, setupTransferManager() creates a new one with stale USB state,
    // causing SIGSEGV in libusb_submit_transfer.
    if (mTransferManager) {
        LOGI("Tearing down previous transfer manager before restart");
        teardownTransferManager();
    }

    // Setup transfer manager
    if (!setupTransferManager()) {
        return BackendResult::ERROR_USB_INIT_FAILED;
    }

    // Start transfer manager
    if (!mTransferManager->start()) {
        LOGE("Failed to start transfer manager");
        teardownTransferManager();
        return BackendResult::ERROR_STREAM_FAILED;
    }

    // Pre-allocate DSP buffers BEFORE starting the RT thread (P0-4 fix)
    {
        const int framesPerBlock = mRequestedBufferSize;
        const int outputChannels = mSelectedPlayback ? mSelectedPlayback->format.channels : 0;
        const int inputChannels = mSelectedCapture ? mSelectedCapture->format.channels : 0;

        // Track actual samples per block (used for read/write sizes in RT thread)
        mDspOutputSamples = static_cast<size_t>(framesPerBlock * std::max(outputChannels, 2));
        mDspInputSamples = static_cast<size_t>(framesPerBlock * std::max(inputChannels, 1));

        // Allocate with actual sizes (not max)
        mDspOutputBuffer.resize(mDspOutputSamples);
        mDspInputBuffer.resize(mDspInputSamples);
        mDspNeedsMonoToStereo = (inputChannels == 1);
        if (mDspNeedsMonoToStereo) {
            mDspStereoInputBuffer.resize(static_cast<size_t>(framesPerBlock * 2));
            MemoryUtils::prepareVectorForRealtime(mDspStereoInputBuffer);
        }
        // Last valid input block for underrun protection (always stereo for callback)
        mDspLastValidInput.resize(static_cast<size_t>(framesPerBlock * 2), 0.0f);
        mDspHasValidInput = false;
        MemoryUtils::prepareVectorForRealtime(mDspLastValidInput);
        MemoryUtils::prepareVectorForRealtime(mDspOutputBuffer);
        MemoryUtils::prepareVectorForRealtime(mDspInputBuffer);

        LOGI("Pre-allocated DSP buffers: frames=%d, outCh=%d(%zu samples), inCh=%d(%zu samples), monoToStereo=%d",
             framesPerBlock, outputChannels, mDspOutputSamples, inputChannels, mDspInputSamples, mDspNeedsMonoToStereo);
    }

    // Start DSP thread
    // Thread priority and CPU affinity are configured from within the thread
    mDspRunning.store(true);
    mDspThread = std::thread(&LibusbBackend::dspThreadFunc, this);

    mIsRunning.store(true);
    mIsPaused.store(false);

    LOGI("LibusbBackend started");
    return BackendResult::OK;
}

void LibusbBackend::stop() {
    std::lock_guard<std::mutex> lock(mMutex);

    if (!mIsRunning.load()) {
        return;
    }

    LOGI("Stopping LibusbBackend...");

    // Stop DSP thread
    mDspRunning.store(false);
    if (mDspThread.joinable()) {
        mDspThread.join();
    }

    // Stop transfer manager and check if device was disconnected
    if (mTransferManager) {
        mTransferManager->stop();
        // Propagate disconnection flag from transfer manager
        if (mTransferManager->isDeviceDisconnected()) {
            LOGW("Device was disconnected during operation");
            mDeviceDisconnected.store(true);
        }
    }

    mIsRunning.store(false);
    mIsPaused.store(false);

    LOGI("LibusbBackend stopped, deviceDisconnected=%d", mDeviceDisconnected.load());
}

void LibusbBackend::pause() {
    if (mIsRunning.load() && !mIsPaused.load()) {
        mIsPaused.store(true);
        LOGI("LibusbBackend paused");
    }
}

void LibusbBackend::resume() {
    if (mIsRunning.load() && mIsPaused.load()) {
        mIsPaused.store(false);
        LOGI("LibusbBackend resumed");
    }
}

void LibusbBackend::setCallback(IAudioCallback* callback) {
    mCallback = callback;
}

void LibusbBackend::setSampleRate(int sampleRate) {
    mRequestedSampleRate = sampleRate;
}

void LibusbBackend::setBufferSize(int framesPerBuffer) {
    mRequestedBufferSize = framesPerBuffer;
}

void LibusbBackend::setFullDuplexEnabled(bool enable) {
    mFullDuplexEnabled = enable;
    // Update streaming mode to match legacy API
    mStreamingMode = enable ? UsbStreamingMode::FULL_DUPLEX : UsbStreamingMode::PLAYBACK_ONLY;
}

void LibusbBackend::setStreamingMode(UsbStreamingMode mode) {
    mStreamingMode = mode;
    // Update legacy flag for compatibility
    mFullDuplexEnabled = (mode == UsbStreamingMode::FULL_DUPLEX);
    LOGI("Streaming mode set to: %s",
         mode == UsbStreamingMode::PLAYBACK_ONLY ? "PLAYBACK_ONLY" :
         mode == UsbStreamingMode::CAPTURE_ONLY ? "CAPTURE_ONLY" : "FULL_DUPLEX");
}

StreamInfo LibusbBackend::getStreamInfo() const {
    std::lock_guard<std::mutex> lock(mStreamInfoMutex);

    if (!mDeviceReady.load()) {
        return StreamInfo{};
    }

    StreamInfo info;
    info.sampleRate = mRequestedSampleRate;
    info.channelCount = mSelectedPlayback ? mSelectedPlayback->format.channels : 2;
    info.framesPerBuffer = mRequestedBufferSize;
    info.format = AudioFormat::FLOAT_32;
    info.backendType = BackendType::LIBUSB;

    // Calculate latency
    if (mTransferManager) {
        auto& stats = mTransferManager->getStatistics();
        info.outputLatencyMs = stats.currentLatencyMs.load();
    }

    // USB info
    info.usbVendorId = mDeviceInfo.vendorId;
    info.usbProductId = mDeviceInfo.productId;
    info.deviceName = mDeviceInfo.product;

    // Full-duplex
    info.isFullDuplex = mSelectedCapture.has_value();

    return info;
}

bool LibusbBackend::isRunning() const {
    return mIsRunning.load();
}

float LibusbBackend::getOutputLatencyMs() const {
    if (mTransferManager) {
        return mTransferManager->getStatistics().currentLatencyMs.load();
    }
    // Estimate based on buffer config
    return static_cast<float>(mRequestedBufferSize) /
           static_cast<float>(mRequestedSampleRate) * 1000.0f;
}

float LibusbBackend::getInputLatencyMs() const {
    if (mSelectedCapture && mTransferManager) {
        // Similar to output for symmetric full-duplex
        return mTransferManager->getStatistics().currentLatencyMs.load();
    }
    return 0.0f;
}

bool LibusbBackend::supportsFullDuplex() const {
    if (mUsbDevice) {
        return mUsbDevice->isFullDuplex();
    }
    return false;
}

bool LibusbBackend::hasCapture() const {
    if (mUsbDevice) {
        return mUsbDevice->hasCapture();
    }
    return false;
}

int LibusbBackend::getUacVersion() const {
    if (mUsbDevice) {
        return mUsbDevice->uacVersion;
    }
    return 0;
}

bool LibusbBackend::getUsbDeviceInfo(int* vendorId, int* productId) const {
    if (!mDeviceReady.load()) {
        return false;
    }
    if (vendorId) *vendorId = mDeviceInfo.vendorId;
    if (productId) *productId = mDeviceInfo.productId;
    return true;
}

// ============================================================================
// Transfer Manager Setup
// ============================================================================

bool LibusbBackend::setupTransferManager() {
    // Determine which interface to use for configuration
    const usb::UsbStreamingInterface* configInterface = nullptr;

    if (mSelectedPlayback) {
        configInterface = &(*mSelectedPlayback);
    } else if (mSelectedCapture) {
        configInterface = &(*mSelectedCapture);
    } else {
        LOGE("No streaming interface selected");
        return false;
    }

    mTransferManager = std::make_unique<usb::UsbTransferManager>(mDeviceHandle, mContext);

    // Configure transfer parameters from the primary interface (output)
    usb::TransferConfig config;
    config.sampleRate = mRequestedSampleRate;
    config.channelCount = configInterface->format.channels;
    config.bitDepth = configInterface->format.bitResolution;

    // FIX: Configure input parameters separately (may differ from output)
    if (mSelectedCapture) {
        config.inputChannelCount = mSelectedCapture->format.channels;
        config.inputBitDepth = mSelectedCapture->format.bitResolution;
        LOGI("Input config: %d channels, %d-bit (output: %d channels, %d-bit)",
             config.inputChannelCount, config.inputBitDepth,
             config.channelCount, config.bitDepth);
    } else {
        // No capture, use same as output (default)
        config.inputChannelCount = config.channelCount;
        config.inputBitDepth = config.bitDepth;
    }

    // Set PCM format based on bit depth
    switch (config.bitDepth) {
        case 16:
            config.pcmFormat = usb::PcmFormat::PCM_S16_LE;
            break;
        case 24:
            config.pcmFormat = usb::PcmFormat::PCM_S24_3LE;
            break;
        case 32:
            config.pcmFormat = usb::PcmFormat::PCM_S32_LE;
            break;
        default:
            config.pcmFormat = usb::PcmFormat::PCM_S16_LE;
    }

    // Calculate frames per packet (1ms at given sample rate)
    config.framesPerPacket = mRequestedSampleRate / 1000;
    config.packetsPerTransfer = 8;  // 8ms per transfer
    config.numTransfers = 3;        // Triple buffering

    // Ring buffer size: start with reduced default (100ms instead of 200ms)
    // Adaptive buffer controller may adjust this based on system performance
    config.ringBufferMs = 100;

    if (!mTransferManager->configure(config)) {
        LOGE("Failed to configure transfer manager");
        return false;
    }

    // Set output interface if playback is enabled
    if (mSelectedPlayback) {
        if (!mTransferManager->setOutputInterface(*mSelectedPlayback)) {
            LOGE("Failed to set output interface");
            return false;
        }

        // Enable feedback if async endpoint
        if (mSelectedPlayback->feedbackEndpoint) {
            mTransferManager->setFeedbackEnabled(true, &(*mSelectedPlayback->feedbackEndpoint));
        }
    }

    // Set input interface if capture is enabled
    if (mSelectedCapture) {
        if (!mTransferManager->setInputInterface(*mSelectedCapture)) {
            LOGE("Failed to set input interface");
            return false;
        }
    }

    // Set error callback
    mTransferManager->setErrorCallback([this](usb::UsbAudioError error, const char* msg) {
        handleTransferError(error, msg);
    });

    return true;
}

void LibusbBackend::teardownTransferManager() {
    if (mTransferManager) {
        mTransferManager->stop();
        // Propagate disconnection flag before destroying
        if (mTransferManager->isDeviceDisconnected()) {
            mDeviceDisconnected.store(true);
        }
        mTransferManager.reset();
    }
}

// ============================================================================
// DSP Thread
// ============================================================================

void LibusbBackend::dspThreadFunc() {
    LOGI("DSP thread started, mode=%s",
         mStreamingMode == UsbStreamingMode::PLAYBACK_ONLY ? "PLAYBACK_ONLY" :
         mStreamingMode == UsbStreamingMode::CAPTURE_ONLY ? "CAPTURE_ONLY" : "FULL_DUPLEX");

    // Configure this thread for real-time audio from within the thread
    ThreadUtils::setCurrentThreadRealtime("UsbDspThread", ThreadUtils::Priority::REALTIME);

    // Pin this thread to a performance core (big core on ARM big.LITTLE)
    int numCpus = ThreadUtils::getNumCpus();
    if (numCpus >= 4) {
        int targetCore = numCpus - 2;
        if (ThreadUtils::setCurrentThreadCpuAffinity(targetCore)) {
            LOGI("DSP thread pinned to core %d (of %d)", targetCore, numCpus);
        }
    }

    // Use pre-allocated buffers (allocated in start() before thread launch — P0-4 fix)
    auto& outputBuffer = mDspOutputBuffer;
    auto& inputBuffer = mDspInputBuffer;
    auto& stereoInputBuffer = mDspStereoInputBuffer;
    const bool needsMonoToStereo = mDspNeedsMonoToStereo;
    const int framesPerBlock = mRequestedBufferSize;
    const size_t outputSamples = mDspOutputSamples;  // Exact samples for output write
    const size_t inputSamples = mDspInputSamples;    // Exact samples for input read

    // Adaptive buffer evaluation counter
    int callbackCount = 0;
    const int ADAPTIVE_EVAL_INTERVAL = 100;

    // Error tracking for disconnect detection (P0-2 fix)
    int consecutiveWriteErrors = 0;
    static constexpr int MAX_CONSECUTIVE_ERRORS = 10;

    while (mDspRunning.load(std::memory_order_acquire)) {
        // P0-2: Check for device disconnection
        if (mTransferManager && mTransferManager->isDeviceDisconnected()) {
            LOGI("Device disconnected detected in DSP thread, exiting");
            mDeviceDisconnected.store(true, std::memory_order_release);
            if (mCallback) {
                mCallback->onBackendError(BackendError::DEVICE_DISCONNECTED);
            }
            break;
        }

        // Check for pending buffer resize
        if (mBufferResizePending.load(std::memory_order_acquire)) {
            performBufferResize();
        }

        // Periodic adaptive buffer evaluation
        if (mAdaptiveBufferingEnabled.load(std::memory_order_relaxed) &&
            mTransferManager && ++callbackCount >= ADAPTIVE_EVAL_INTERVAL) {
            callbackCount = 0;

            auto* controller = mTransferManager->getBufferController();
            if (controller) {
                auto stats = mTransferManager->getProfilingStats();
                controller->updateFromProfiler(stats);

                auto recommendation = controller->evaluate();
                if (recommendation != usb::AdaptiveBufferController::Recommendation::NO_CHANGE) {
                    int newBufferMs = controller->getRecommendedBufferMs();
                    requestBufferResize(newBufferMs);
                }
            }
        }

        // P1-3: Paused state — drain/fill with short sleep (reduced from 1ms to 200µs)
        if (mIsPaused.load()) {
            if (mSelectedPlayback) {
                std::fill(outputBuffer.begin(), outputBuffer.begin() + outputSamples, 0.0f);
                mTransferManager->writeOutput(outputBuffer.data(), outputSamples);
            }
            if (mSelectedCapture) {
                mTransferManager->readInput(inputBuffer.data(), inputSamples);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }

        // Check ALL buffer availability in a single gate to avoid
        // wasting a full sleep cycle when one buffer is ready but the other isn't.
        {
            bool outputReady = !mSelectedPlayback ||
                (mTransferManager->getOutputBufferAvailable() >= outputSamples);
            bool inputReady = !mSelectedCapture ||
                (mTransferManager->getInputBufferAvailable() >= inputSamples);

            if (!outputReady || !inputReady) {
                // Sleep proportional to how much data we're missing.
                // At 48kHz stereo, 1 sample ≈ 10.4µs. Missing a full block (512 samples)
                // means ~5.3ms until data arrives. Sleep for ~1/4 of a block period
                // to balance responsiveness vs CPU usage.
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
            }
        }

        // Get input data if capture is enabled
        const float* inputPtr = nullptr;
        if (mSelectedCapture) {
            if (mTransferManager->readInput(inputBuffer.data(), inputSamples)) {
                // Apply digital input volume/mute if not using hardware control
                if (!isUsingHardwareInputVolume()) {
                    bool inputMuted = mDigitalInputMute.load(std::memory_order_relaxed);
                    float inputVol = inputMuted ? 0.0f : mDigitalInputVolume.load(std::memory_order_relaxed);

                    if (inputVol < 0.999f) {
                        for (size_t i = 0; i < inputSamples; ++i) {
                            inputBuffer[i] *= inputVol;
                        }
                    }
                }

                // Convert mono input to stereo for AudioEngine callback (P1-2: with -3dB)
                if (needsMonoToStereo) {
                    constexpr float monoGain = 0.707f; // -3dB to prevent clipping
                    for (int i = 0; i < framesPerBlock; ++i) {
                        float sample = inputBuffer[static_cast<size_t>(i)] * monoGain;
                        stereoInputBuffer[static_cast<size_t>(i * 2)] = sample;
                        stereoInputBuffer[static_cast<size_t>(i * 2 + 1)] = sample;
                    }
                    inputPtr = stereoInputBuffer.data();
                } else {
                    inputPtr = inputBuffer.data();
                }

                // Save last valid stereo input for underrun protection
                const float* stereoSrc = needsMonoToStereo ? stereoInputBuffer.data() : inputBuffer.data();
                size_t stereoSamples = static_cast<size_t>(framesPerBlock * 2);
                std::memcpy(mDspLastValidInput.data(), stereoSrc, stereoSamples * sizeof(float));
                mDspHasValidInput = true;
            } else if (mDspHasValidInput) {
                // Underrun: fade the last valid block to silence with a linear ramp.
                // This produces a smooth tail instead of a repeated transient or hard cut.
                size_t totalStereoSamples = static_cast<size_t>(framesPerBlock * 2);
                for (size_t i = 0; i < totalStereoSamples; ++i) {
                    float fade = 1.0f - (static_cast<float>(i) / static_cast<float>(totalStereoSamples));
                    mDspLastValidInput[i] *= fade;
                }
                inputPtr = mDspLastValidInput.data();
                // Next underrun will produce silence (data is faded to zero)

                static int underrunLogCount = 0;
                if (++underrunLogCount <= 5) {
                    wma::logMessage(wma::LogLevel::WARN, "INPUTFX_DIAG",
                        "USB_INPUT_UNDERRUN: using faded last block (%d)", underrunLogCount);
                }
            }
        }

        // Call audio callback
        float* outputPtr = mSelectedPlayback ? outputBuffer.data() : nullptr;

        // DIAGNOSTIC: Log input state periodically
        static int usbDspDiagCount = 0;
        if (++usbDspDiagCount >= 300) {
            float inputPeak = 0.0f;
            if (inputPtr != nullptr) {
                int samples = std::min(framesPerBlock * 2, 64);
                for (int i = 0; i < samples; ++i) {
                    float abs = std::abs(inputPtr[i]);
                    if (abs > inputPeak) inputPeak = abs;
                }
            }
            wma::logMessage(wma::LogLevel::INFO, "INPUTFX_DIAG",
                "USB_DSP: hasCapture=%d, inputPtr=%p, outputPtr=%p, "
                "frames=%d, inputPeak=%.5f, streamMode=%d",
                mSelectedCapture.has_value() ? 1 : 0, inputPtr, outputPtr,
                framesPerBlock, inputPeak,
                static_cast<int>(mStreamingMode));
            usbDspDiagCount = 0;
        }

        if (mCallback) {
            auto& profiler = mTransferManager->getLatencyProfiler();
            if (profiler.isEnabled()) {
                profiler.onDspCallbackStart();
            }

            auto result = mCallback->onAudioReady(
                outputPtr,
                inputPtr,
                framesPerBlock
            );

            if (profiler.isEnabled()) {
                profiler.onDspCallbackEnd();
            }

            if (result == IAudioCallback::Result::STOP) {
                LOGI("Callback requested stop");
                break;
            }
        } else if (outputPtr) {
            std::fill(outputBuffer.begin(), outputBuffer.begin() + outputSamples, 0.0f);
        }

        // Write to output buffer if playback is enabled
        if (mSelectedPlayback) {
            if (!isUsingHardwareOutputVolume()) {
                bool outputMuted = mDigitalOutputMute.load(std::memory_order_relaxed);
                float outputVol = outputMuted ? 0.0f : mDigitalOutputVolume.load(std::memory_order_relaxed);

                if (outputVol < 0.999f) {
                    for (size_t i = 0; i < outputSamples; ++i) {
                        outputBuffer[i] *= outputVol;
                    }
                }
            }

            // P0-2: Track consecutive write errors for disconnect detection
            if (!mTransferManager->writeOutput(outputBuffer.data(), outputSamples)) {
                if (++consecutiveWriteErrors > MAX_CONSECUTIVE_ERRORS) {
                    LOGE("Too many consecutive write errors (%d), possible disconnect",
                         consecutiveWriteErrors);
                    if (mCallback) {
                        mCallback->onBackendError(BackendError::UNDERRUN);
                    }
                    consecutiveWriteErrors = 0;
                }
            } else {
                consecutiveWriteErrors = 0;
            }
        }
    }

    LOGI("DSP thread stopped");
}

// ============================================================================
// Query Methods
// ============================================================================

const usb::TransferStatistics* LibusbBackend::getTransferStats() const {
    if (mTransferManager) {
        return &mTransferManager->getStatistics();
    }
    return nullptr;
}

ClockController* LibusbBackend::getClockController() {
    if (mTransferManager) {
        return mTransferManager->getClockController();
    }
    return nullptr;
}

usb::UsbProfilingStats LibusbBackend::getProfilingStats() const {
    if (mTransferManager) {
        return mTransferManager->getProfilingStats();
    }
    return usb::UsbProfilingStats{};
}

usb::UsbLatencyProfiler* LibusbBackend::getLatencyProfiler() {
    if (mTransferManager) {
        return &mTransferManager->getLatencyProfiler();
    }
    return nullptr;
}

LibusbBackend::DeviceCapabilities LibusbBackend::getCapabilities() const {
    DeviceCapabilities caps;

    if (!mUsbDevice) {
        return caps;
    }

    // Collect from all playback interfaces
    for (const auto& iface : mUsbDevice->playbackInterfaces) {
        // Sample rates
        for (int rate : iface.format.sampleRates) {
            if (std::find(caps.supportedSampleRates.begin(),
                         caps.supportedSampleRates.end(), rate) ==
                caps.supportedSampleRates.end()) {
                caps.supportedSampleRates.push_back(rate);
            }
        }

        // Bit depths
        int depth = iface.format.bitResolution;
        if (std::find(caps.supportedBitDepths.begin(),
                     caps.supportedBitDepths.end(), depth) ==
            caps.supportedBitDepths.end()) {
            caps.supportedBitDepths.push_back(depth);
        }

        // Channels
        caps.maxChannelsOutput = std::max(caps.maxChannelsOutput,
                                          static_cast<int>(iface.format.channels));

        // Feedback support
        if (iface.feedbackEndpoint) {
            caps.supportsFeedback = true;
        }
    }

    // Input channels
    for (const auto& iface : mUsbDevice->captureInterfaces) {
        caps.maxChannelsInput = std::max(caps.maxChannelsInput,
                                         static_cast<int>(iface.format.channels));
    }

    // Sort for consistent ordering
    std::sort(caps.supportedSampleRates.begin(), caps.supportedSampleRates.end());
    std::sort(caps.supportedBitDepths.begin(), caps.supportedBitDepths.end());

    // Volume control capabilities
    if (mOutputVolumeControl) {
        const auto& volCaps = mOutputVolumeControl->getCapabilities();
        caps.hasOutputVolumeControl = volCaps.hasVolumeControl || true;  // Always true (digital fallback)
        caps.hasOutputMuteControl = volCaps.hasMuteControl || true;
        caps.isUsingHardwareOutputVolume = mOutputVolumeControl->isUsingHardwareVolume();
        caps.outputVolumeMinDb = volCaps.volumeToDb(volCaps.minVolume);
        caps.outputVolumeMaxDb = volCaps.volumeToDb(volCaps.maxVolume);
    } else {
        // Digital fallback always available
        caps.hasOutputVolumeControl = true;
        caps.hasOutputMuteControl = true;
        caps.isUsingHardwareOutputVolume = false;
    }

    if (mInputVolumeControl) {
        const auto& volCaps = mInputVolumeControl->getCapabilities();
        caps.hasInputVolumeControl = volCaps.hasVolumeControl || true;
        caps.hasInputMuteControl = volCaps.hasMuteControl || true;
        caps.isUsingHardwareInputVolume = mInputVolumeControl->isUsingHardwareVolume();
        caps.inputVolumeMinDb = volCaps.volumeToDb(volCaps.minVolume);
        caps.inputVolumeMaxDb = volCaps.volumeToDb(volCaps.maxVolume);
    } else if (!mUsbDevice->captureInterfaces.empty()) {
        // Digital fallback for capture
        caps.hasInputVolumeControl = true;
        caps.hasInputMuteControl = true;
        caps.isUsingHardwareInputVolume = false;
    }

    return caps;
}

void LibusbBackend::setUsbErrorCallback(ErrorCallback callback) {
    mErrorCallback = std::move(callback);
}

// ============================================================================
// Error Handling
// ============================================================================

void LibusbBackend::handleTransferError(usb::UsbAudioError error, const char* message) {
    LOGE("USB Transfer error: %s", message);

    // Mark device as disconnected to prevent cleanup from crashing
    if (error == usb::UsbAudioError::DEVICE_DISCONNECTED) {
        LOGW("Device disconnected, marking flag to skip libusb_close");
        mDeviceDisconnected.store(true);
    }

    // Notify callback
    if (mCallback) {
        BackendError backendError = BackendError::TRANSFER_ERROR;
        if (error == usb::UsbAudioError::DEVICE_DISCONNECTED) {
            backendError = BackendError::DEVICE_DISCONNECTED;
        }
        mCallback->onBackendError(backendError);
    }

    // Notify USB error callback
    if (mErrorCallback) {
        mErrorCallback(error, message);
    }
}

// ============================================================================
// Cleanup
// ============================================================================

void LibusbBackend::cleanup() {
    LOGI("cleanup() called, deviceDisconnected=%d", mDeviceDisconnected.load());

    teardownTransferManager();

    // Only close device handle if device wasn't disconnected
    // When device is physically disconnected, the handle is already invalid
    if (mDeviceHandle && !mDeviceDisconnected.load()) {
        LOGI("Closing device handle");
        libusb_close(mDeviceHandle);
    }
    mDeviceHandle = nullptr;

    // Only exit context if device wasn't disconnected
    if (mContext && mOwnsContext && !mDeviceDisconnected.load()) {
        LOGI("Exiting libusb context");
        libusb_exit(mContext);
    }
    mContext = nullptr;
    mOwnsContext = false;

    mUsbDevice.reset();
    mSelectedPlayback.reset();
    mSelectedCapture.reset();
    mDeviceReady.store(false);
    mDeviceDisconnected.store(false);
}

// ============================================================================
// Adaptive Buffer Control
// ============================================================================

void LibusbBackend::setAdaptiveBufferingEnabled(bool enabled) {
    mAdaptiveBufferingEnabled.store(enabled, std::memory_order_release);

    if (mTransferManager) {
        auto* controller = mTransferManager->getBufferController();
        if (controller) {
            controller->setEnabled(enabled);
        }
    }

    LOGI("Adaptive buffering %s", enabled ? "enabled" : "disabled");
}

bool LibusbBackend::isAdaptiveBufferingEnabled() const {
    return mAdaptiveBufferingEnabled.load(std::memory_order_acquire);
}

int LibusbBackend::getCurrentBufferMs() const {
    if (mTransferManager) {
        return mTransferManager->getCurrentBufferMs();
    }
    return 100;  // Default
}

bool LibusbBackend::requestBufferResize(int newBufferMs) {
    if (!mTransferManager) {
        return false;
    }

    // Clamp to valid range
    newBufferMs = std::clamp(newBufferMs, 50, 200);

    if (newBufferMs == getCurrentBufferMs()) {
        return true;  // No change needed
    }

    // Queue the resize request
    mPendingBufferMs.store(newBufferMs, std::memory_order_release);
    mBufferResizePending.store(true, std::memory_order_release);

    LOGI("Buffer resize requested: %d ms", newBufferMs);
    return true;
}

usb::AdaptiveBufferController* LibusbBackend::getBufferController() {
    if (mTransferManager) {
        return mTransferManager->getBufferController();
    }
    return nullptr;
}

void LibusbBackend::performBufferResize() {
    int newBufferMs = mPendingBufferMs.load(std::memory_order_acquire);
    mBufferResizePending.store(false, std::memory_order_release);

    if (!mTransferManager) {
        return;
    }

    LOGI("Performing buffer resize: %d ms -> %d ms",
         mTransferManager->getCurrentBufferMs(), newBufferMs);

    // P0-3 fix: No sleeping in RT thread. Just attempt the resize immediately.
    // The ring buffer implementation should handle concurrent access safely.
    // A brief audio glitch is preferable to 50-100ms of jitter from sleeping.
    if (mTransferManager->reconfigureBufferSize(newBufferMs)) {
        LOGI("Buffer resize complete: %d ms", newBufferMs);
    } else {
        LOGE("Buffer resize failed");
    }
}

// ============================================================================
// Volume Control
// ============================================================================

void LibusbBackend::initializeVolumeControls() {
    if (!mUsbDevice || !mDeviceHandle) {
        LOGD("Volume control: No device, using digital only");
        return;
    }

    // Find Feature Unit for output volume
    const auto* outputFU = mUsbDevice->findOutputVolumeFeatureUnit();
    if (outputFU) {
        bool hasVolume = outputFU->hasAnyVolumeControl(mUsbDevice->uacVersion);
        bool hasMute = outputFU->hasAnyMuteControl(mUsbDevice->uacVersion);

        if (hasVolume || hasMute) {
            mOutputVolumeControl = std::make_unique<usb::UsbVolumeControl>(
                mDeviceHandle, mUsbDevice->controlInterface);

            bool hwAvailable = mOutputVolumeControl->initialize(
                outputFU->unitId, hasVolume, hasMute,
                true,  // isOutput
                mUsbDevice->uacVersion);

            LOGI("Output volume control: HW=%d, Volume=%d, Mute=%d, FU=%d",
                 hwAvailable, hasVolume, hasMute, outputFU->unitId);
        }
    }

    // Find Feature Unit for input volume
    const auto* inputFU = mUsbDevice->findInputVolumeFeatureUnit();
    if (inputFU && !mUsbDevice->captureInterfaces.empty()) {
        bool hasVolume = inputFU->hasAnyVolumeControl(mUsbDevice->uacVersion);
        bool hasMute = inputFU->hasAnyMuteControl(mUsbDevice->uacVersion);

        // Only create if different from output FU or output doesn't exist
        if ((hasVolume || hasMute) &&
            (!outputFU || inputFU->unitId != outputFU->unitId)) {
            mInputVolumeControl = std::make_unique<usb::UsbVolumeControl>(
                mDeviceHandle, mUsbDevice->controlInterface);

            bool hwAvailable = mInputVolumeControl->initialize(
                inputFU->unitId, hasVolume, hasMute,
                false,  // isOutput
                mUsbDevice->uacVersion);

            LOGI("Input volume control: HW=%d, Volume=%d, Mute=%d, FU=%d",
                 hwAvailable, hasVolume, hasMute, inputFU->unitId);
        }
    }
}

void LibusbBackend::setOutputVolume(float volume) {
    volume = std::clamp(volume, 0.0f, 1.0f);

    // Always update digital volume (used as fallback or when no HW control)
    mDigitalOutputVolume.store(volume, std::memory_order_relaxed);

    // Try hardware volume if available
    if (mOutputVolumeControl) {
        mOutputVolumeControl->setVolume(volume);
    }

    LOGD("Set output volume: %.2f", volume);
}

float LibusbBackend::getOutputVolume() const {
    if (mOutputVolumeControl) {
        return mOutputVolumeControl->getVolume();
    }
    return mDigitalOutputVolume.load(std::memory_order_relaxed);
}

void LibusbBackend::setInputVolume(float volume) {
    volume = std::clamp(volume, 0.0f, 1.0f);

    mDigitalInputVolume.store(volume, std::memory_order_relaxed);

    if (mInputVolumeControl) {
        mInputVolumeControl->setVolume(volume);
    }

    LOGD("Set input volume: %.2f", volume);
}

float LibusbBackend::getInputVolume() const {
    if (mInputVolumeControl) {
        return mInputVolumeControl->getVolume();
    }
    return mDigitalInputVolume.load(std::memory_order_relaxed);
}

void LibusbBackend::setOutputMute(bool muted) {
    mDigitalOutputMute.store(muted, std::memory_order_relaxed);

    if (mOutputVolumeControl) {
        mOutputVolumeControl->setMute(muted);
    }

    LOGD("Set output mute: %d", muted);
}

bool LibusbBackend::isOutputMuted() const {
    if (mOutputVolumeControl) {
        return mOutputVolumeControl->isMuted();
    }
    return mDigitalOutputMute.load(std::memory_order_relaxed);
}

void LibusbBackend::setInputMute(bool muted) {
    mDigitalInputMute.store(muted, std::memory_order_relaxed);

    if (mInputVolumeControl) {
        mInputVolumeControl->setMute(muted);
    }

    LOGD("Set input mute: %d", muted);
}

bool LibusbBackend::isInputMuted() const {
    if (mInputVolumeControl) {
        return mInputVolumeControl->isMuted();
    }
    return mDigitalInputMute.load(std::memory_order_relaxed);
}

bool LibusbBackend::isUsingHardwareOutputVolume() const {
    return mOutputVolumeControl && mOutputVolumeControl->isUsingHardwareVolume();
}

bool LibusbBackend::isUsingHardwareInputVolume() const {
    return mInputVolumeControl && mInputVolumeControl->isUsingHardwareVolume();
}

} // namespace noisypad
