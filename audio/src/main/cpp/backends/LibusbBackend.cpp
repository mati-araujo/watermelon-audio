/**
 * LibusbBackend.cpp
 *
 * Implementation of USB Audio backend using libusb.
 */

#include "LibusbBackend.h"
#include "../usb/UsbConstants.h"
#include "../usb/SampleRateRequest.h"
#include "../usb/ClockSourceRangeParser.h"
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

namespace watermelon_audio {

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

    // Claim the AudioControl interface. In UAC 2.0 all class-specific
    // requests (clock source SET_CUR, feature unit volume/mute, clock
    // selector, etc.) are routed to the control interface, so it MUST be
    // claimed before any of those requests can succeed. Without this,
    // every class-specific interface request fails with LIBUSB_ERROR_IO
    // — which is what caused both the UsbVolumeControl fallback to
    // digital ("SET_CUR volume failed") and the sample rate negotiation
    // failure in stage 1's first device tests.
    //
    // For UAC 1.0 it's not strictly required (sample rate is endpoint-
    // recipient there), but claiming it anyway makes the volume control
    // path work consistently and costs nothing.
    if (!claimControlInterface()) {
        LOGW("Failed to claim AudioControl interface %d — class-specific "
             "requests (volume, clock source, sample rate UAC2) will fail "
             "with LIBUSB_ERROR_IO. Continuing anyway because some devices "
             "accept them without an explicit claim.",
             mUsbDevice->controlInterface);
    }

    // Stage 3: query clock source RANGE to populate real sample rates
    // for UAC2 devices. No-op on UAC1. Must run AFTER the control interface
    // is claimed so interface-recipient requests can reach their target.
    populateClockSourceRates();

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
    mSelectedPlaybackFormat.reset();
    mSelectedCaptureFormat.reset();

    bool needsPlayback = (mStreamingMode == UsbStreamingMode::PLAYBACK_ONLY ||
                          mStreamingMode == UsbStreamingMode::FULL_DUPLEX);
    bool needsCapture = (mStreamingMode == UsbStreamingMode::CAPTURE_ONLY ||
                         mStreamingMode == UsbStreamingMode::FULL_DUPLEX);

    LOGI("selectBestInterfaces: mode=%s, needsPlayback=%d, needsCapture=%d",
         mStreamingMode == UsbStreamingMode::PLAYBACK_ONLY ? "PLAYBACK_ONLY" :
         mStreamingMode == UsbStreamingMode::CAPTURE_ONLY ? "CAPTURE_ONLY" : "FULL_DUPLEX",
         needsPlayback, needsCapture);

    // Helper: log every altsetting of a direction so operators can see
    // exactly what the device offered vs what we picked.
    auto logAltsettings = [](const char* label,
                              const std::vector<usb::UsbStreamingInterface>& list) {
        LOGI("Available %s altsettings (%zu):", label, list.size());
        for (size_t i = 0; i < list.size(); ++i) {
            const auto& iface = list[i];
            const auto& fmt = iface.primaryFormat();
            char rateBuf[128] = {0};
            if (fmt.hasContinuousRates) {
                snprintf(rateBuf, sizeof(rateBuf), "%d-%d",
                         fmt.minSampleRate, fmt.maxSampleRate);
            } else if (!fmt.sampleRates.empty()) {
                size_t pos = 0;
                for (size_t j = 0; j < fmt.sampleRates.size() &&
                                   pos + 8 < sizeof(rateBuf); ++j) {
                    pos += snprintf(rateBuf + pos, sizeof(rateBuf) - pos,
                                    "%s%d", (j == 0 ? "" : ","),
                                    fmt.sampleRates[j]);
                }
            } else {
                snprintf(rateBuf, sizeof(rateBuf), "unknown");
            }
            LOGI("  [%zu] IF%d Alt%d: %dch/%dbit (%zu fmt) rates={%s} ep=0x%02x fb=%s",
                 i, iface.interfaceNumber, iface.alternateSetting,
                 fmt.channels, fmt.bitResolution, iface.formats.size(), rateBuf,
                 iface.dataEndpoint.address,
                 iface.feedbackEndpoint ? "yes" : "no");
        }
    };

    // Build a preference from the current requested state, optionally
    // overlaid with a user-provided preference (from setStreamPreference).
    usb::StreamPreference pref = mUserPreference.value_or(
        usb::StreamPreference::defaultPro());
    pref.requiredSampleRate = mRequestedSampleRate;

    // Playback
    if (needsPlayback && !mUsbDevice->playbackInterfaces.empty()) {
        logAltsettings("playback", mUsbDevice->playbackInterfaces);
        auto match = usb::AltsettingSelector::pickPlayback(*mUsbDevice, pref);
        if (match) {
            mSelectedPlayback = *match->altsetting;
            mSelectedPlaybackFormat = *match->format;
            LOGI("Selected playback: IF%d Alt%d, %dHz, %dch, %dbit, score=%.3f",
                 match->altsetting->interfaceNumber,
                 match->altsetting->alternateSetting,
                 mRequestedSampleRate,
                 match->format->channels, match->format->bitResolution,
                 match->score);
        } else {
            // Last-resort: nothing scored. Fall back to the first entry and
            // let the transfer manager surface an error if it really doesn't
            // work.
            mSelectedPlayback = mUsbDevice->playbackInterfaces[0];
            mSelectedPlaybackFormat = mSelectedPlayback->primaryFormat();
            if (!mSelectedPlaybackFormat->sampleRates.empty()) {
                mRequestedSampleRate = mSelectedPlaybackFormat->sampleRates[0];
            } else if (mSelectedPlaybackFormat->hasContinuousRates) {
                mRequestedSampleRate = std::min(48000,
                    mSelectedPlaybackFormat->maxSampleRate);
            }
            LOGW("No scoring playback candidate; using IF%d Alt%d as "
                 "last-resort fallback (%dch/%dbit @ %dHz)",
                 mSelectedPlayback->interfaceNumber,
                 mSelectedPlayback->alternateSetting,
                 mSelectedPlaybackFormat->channels,
                 mSelectedPlaybackFormat->bitResolution,
                 mRequestedSampleRate);
        }
    }

    // Capture
    if (needsCapture && !mUsbDevice->captureInterfaces.empty()) {
        logAltsettings("capture", mUsbDevice->captureInterfaces);
        // Capture devices commonly expose mono inputs (e.g. GHW USB AUDIO has
        // a single 1ch/16bit capture altsetting). The default playback pref
        // requires minChannels=2 which would falsely reject these. Use a
        // capture-specific copy with minChannels relaxed to 1.
        usb::StreamPreference capturePref = pref;
        capturePref.minChannels = 1;
        auto match = usb::AltsettingSelector::pickCapture(*mUsbDevice, capturePref);
        if (match) {
            mSelectedCapture = *match->altsetting;
            mSelectedCaptureFormat = *match->format;
            LOGI("Selected capture: IF%d Alt%d, %dHz, %dch, %dbit, score=%.3f",
                 match->altsetting->interfaceNumber,
                 match->altsetting->alternateSetting,
                 mRequestedSampleRate,
                 match->format->channels, match->format->bitResolution,
                 match->score);
        } else {
            mSelectedCapture = mUsbDevice->captureInterfaces[0];
            mSelectedCaptureFormat = mSelectedCapture->primaryFormat();
            if (mStreamingMode == UsbStreamingMode::CAPTURE_ONLY) {
                if (!mSelectedCaptureFormat->sampleRates.empty()) {
                    mRequestedSampleRate = mSelectedCaptureFormat->sampleRates[0];
                }
            }
            LOGW("No scoring capture candidate; using IF%d Alt%d as "
                 "last-resort fallback (%dch/%dbit @ %dHz)",
                 mSelectedCapture->interfaceNumber,
                 mSelectedCapture->alternateSetting,
                 mSelectedCaptureFormat->channels,
                 mSelectedCaptureFormat->bitResolution,
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

bool LibusbBackend::claimControlInterface() {
    if (!mDeviceHandle || !mUsbDevice) {
        return false;
    }
    if (mControlInterfaceClaimed) {
        return true;
    }

    const int ifNum = mUsbDevice->controlInterface;

    // Android's usbfs needs auto-detach for most devices; be defensive
    // about the result because not every libusb backend supports the option.
    int r = libusb_set_auto_detach_kernel_driver(mDeviceHandle, 1);
    if (r != LIBUSB_SUCCESS && r != LIBUSB_ERROR_NOT_SUPPORTED) {
        LOGW("libusb_set_auto_detach_kernel_driver failed: %s",
             libusb_error_name(r));
    }

    // Detach an active kernel driver if present. LIBUSB_ERROR_NOT_SUPPORTED
    // is fine — it means the platform doesn't expose the concept.
    int active = libusb_kernel_driver_active(mDeviceHandle, ifNum);
    if (active == 1) {
        int d = libusb_detach_kernel_driver(mDeviceHandle, ifNum);
        if (d != LIBUSB_SUCCESS) {
            LOGW("Failed to detach kernel driver from control interface %d: %s",
                 ifNum, libusb_error_name(d));
        } else {
            LOGI("Detached kernel driver from control interface %d", ifNum);
        }
    }

    r = libusb_claim_interface(mDeviceHandle, ifNum);
    if (r != LIBUSB_SUCCESS) {
        LOGE("libusb_claim_interface(%d) for AudioControl failed: %s",
             ifNum, libusb_error_name(r));
        mLastLibusbError.store(r);
        return false;
    }

    mControlInterfaceClaimed = true;
    LOGI("Claimed AudioControl interface %d", ifNum);
    return true;
}

// ============================================================================
// Stage 3: Clock Source RANGE query
// ============================================================================
//
// UAC 2.0 clock sources advertise their supported sample rates via a RANGE
// control request (spec 5.2.1):
//   bmRequestType = 0xA1  (D2H | Class | Interface)
//   bRequest      = 0x02  (RANGE)
//   wValue        = CS_SAM_FREQ_CONTROL << 8  = 0x0100
//   wIndex        = (clockSourceId << 8) | controlInterface
//
// The response is variable-length: 2 bytes wNumSubRanges followed by
// numSubRanges * 12 bytes of (dMIN, dMAX, dRES) u32 LE triplets.
//
// Before stage 3 this was a TODO stub (UsbDescriptorParser.cpp:950) that
// hardcoded 44100..192000. That made the Kotlin capability snapshot lie
// about what the device actually supports. Now we query for real.
void LibusbBackend::populateClockSourceRates() {
    if (!mDeviceHandle || !mUsbDevice) return;
    if (mUsbDevice->uacVersion != 2) return;  // UAC1 has no clock graph
    if (mUsbDevice->clockSources.empty()) return;

    const uint8_t controlIface = mUsbDevice->controlInterface;

    for (auto& cs : mUsbDevice->clockSources) {
        if (!cs.canControlFrequency) {
            LOGD("Clock source %d has no frequency control bit, skipping RANGE",
                 cs.clockId);
            continue;
        }

        // Clear any stale fallback rates from the old stub path.
        cs.sampleRates.clear();
        cs.hasContinuousRates = false;
        cs.minSampleRate = 0;
        cs.maxSampleRate = 0;

        const uint16_t wValue = static_cast<uint16_t>(
            usb::UAC2_CS_SAM_FREQ_CONTROL << 8);
        const uint16_t wIndex = static_cast<uint16_t>(
            (cs.clockId << 8) | controlIface);

        // Read the full payload in one shot. Sub-ranges are 12 bytes each;
        // 32 sub-ranges cover virtually any real device (typically ≤ 6).
        // Overallocation is fine — libusb returns actual length.
        constexpr size_t kMaxSubRanges = 32;
        constexpr size_t kBufSize = 2 + kMaxSubRanges * 12;
        std::array<uint8_t, kBufSize> buf{};

        int r = libusb_control_transfer(
            mDeviceHandle,
            /*bmRequestType*/ 0xA1,  // D2H | Class | Interface
            /*bRequest*/      usb::UAC2_REQUEST_RANGE,
            /*wValue*/        wValue,
            /*wIndex*/        wIndex,
            buf.data(),
            static_cast<uint16_t>(buf.size()),
            /*timeout*/       1000);

        if (r < 2) {
            LOGW("Clock source %d: RANGE request failed (%d: %s), "
                 "rates will stay empty",
                 cs.clockId, r,
                 (r < 0 ? libusb_error_name(r) : "short response"));
            continue;
        }

        auto ranges = usb::parseClockRangeResponse(
            buf.data(), static_cast<size_t>(r));
        if (ranges.empty()) {
            LOGW("Clock source %d: RANGE decoded to 0 sub-ranges", cs.clockId);
            continue;
        }

        usb::applyRangesToClockSource(ranges, cs);

        LOGI("Clock source %d: %zu sub-ranges → %zu discrete rates, "
             "continuous=%d, min=%d, max=%d",
             cs.clockId, ranges.size(), cs.sampleRates.size(),
             cs.hasContinuousRates ? 1 : 0,
             cs.minSampleRate, cs.maxSampleRate);
    }
}

// ============================================================================
// Sample Rate Negotiation
// ============================================================================
//
// USB Audio Class devices need an explicit class-specific control transfer
// to set their internal clock to the desired rate. Without this, the device
// stays at its power-on default and the host's iso packet sizing is wrong
// (or right by accident only). Two protocols, two recipients:
//
//  - UAC 1.0: endpoint-recipient request to the data endpoint.
//      bmRequestType = 0x22, bRequest = SET_CUR (0x01),
//      wValue = SAMPLING_FREQ_CONTROL << 8, wIndex = endpoint address,
//      data = 3-byte little-endian rate.
//
//  - UAC 2.0: interface-recipient request to the clock source unit.
//      bmRequestType = 0x21, bRequest = CUR (0x01),
//      wValue = CS_SAM_FREQ_CONTROL << 8, wIndex = (clockSrcId<<8) | controlIface,
//      data = 4-byte little-endian rate.
//
// In both cases we follow with a GET_CUR to detect coercion (devices may
// snap the rate to the nearest supported value). A failed GET_CUR is logged
// but not fatal — some devices STALL GET while accepting SET.

bool LibusbBackend::configureSampleRate() {
    if (!mDeviceHandle || !mUsbDevice) {
        LOGE("configureSampleRate: device not initialized");
        return false;
    }

    const int version = mUsbDevice->uacVersion;
    const uint32_t requested = static_cast<uint32_t>(mRequestedSampleRate);

    if (version == 1) {
        if (!mSelectedPlayback && !mSelectedCapture) {
            LOGE("configureSampleRate UAC1: no selected interface");
            return false;
        }
        const uint8_t epAddress = mSelectedPlayback
            ? mSelectedPlayback->dataEndpoint.address
            : mSelectedCapture->dataEndpoint.address;

        // SET_CUR
        auto setReq = usb::buildUac1SetSampleRateRequest(epAddress, requested);
        int r = libusb_control_transfer(
            mDeviceHandle,
            setReq.bmRequestType,
            setReq.bRequest,
            setReq.wValue,
            setReq.wIndex,
            setReq.payload.data(),
            static_cast<uint16_t>(setReq.payload.size()),
            /*timeout*/ 1000);
        if (r < 0) {
            LOGE("UAC1 SET_CUR sample rate failed for EP 0x%02x (%u Hz): %s",
                 epAddress, requested, libusb_error_name(r));
            mLastLibusbError.store(r);
            return false;
        }

        // GET_CUR (verification, non-fatal on STALL)
        auto getReq = usb::buildUac1GetSampleRateRequest(epAddress);
        std::array<uint8_t, 3> readback{};
        int g = libusb_control_transfer(
            mDeviceHandle,
            getReq.bmRequestType,
            getReq.bRequest,
            getReq.wValue,
            getReq.wIndex,
            readback.data(),
            static_cast<uint16_t>(readback.size()),
            /*timeout*/ 1000);
        if (g == 3) {
            uint32_t actual = usb::decodeUac1SampleRateResponse(readback);
            if (actual != requested) {
                LOGW("UAC1 device coerced sample rate %u Hz -> %u Hz",
                     requested, actual);
                mRequestedSampleRate = static_cast<int>(actual);
            }
            LOGI("Rate negotiation: UAC1 EP 0x%02x req=%u actual=%u",
                 epAddress, requested, actual);
        } else {
            LOGW("UAC1 GET_CUR sample rate readback failed: %s (continuing)",
                 g < 0 ? libusb_error_name(g) : "short response");
            LOGI("Rate negotiation: UAC1 EP 0x%02x req=%u (unverified)",
                 epAddress, requested);
        }
        return true;
    }

    if (version == 2) {
        if (mUsbDevice->clockSources.empty()) {
            LOGE("UAC2 device has no parsed clock sources; cannot negotiate "
                 "sample rate");
            return false;
        }

        const uint8_t controlIface = mUsbDevice->controlInterface;

        // Stage 3: resolve the clock source(s) actually feeding the selected
        // streaming terminal(s). Devices like UGREEN CM720 expose two clock
        // sources (id=27 for the mic input terminal, id=30 for the playback
        // output terminal). We need to SET_CUR on each one — stage 1 only
        // set clockSources.front() which was wrong for full-duplex devices
        // with asymmetric clocks.
        std::vector<uint8_t> clockIdsToSet;
        auto addUnique = [&](uint8_t id) {
            if (id == 0) return;
            for (uint8_t existing : clockIdsToSet) {
                if (existing == id) return;
            }
            clockIdsToSet.push_back(id);
        };

        if (mSelectedPlayback) {
            addUnique(mUsbDevice->resolveClockSourceId(mSelectedPlayback->terminalLink));
        }
        if (mSelectedCapture) {
            addUnique(mUsbDevice->resolveClockSourceId(mSelectedCapture->terminalLink));
        }

        if (clockIdsToSet.empty()) {
            // Fall back to clockSources.front() if resolution failed — e.g.
            // terminals don't have clockSourceId populated for some reason.
            LOGW("Could not resolve clock source for any selected terminal; "
                 "falling back to clockSources.front() (clockId=%d)",
                 mUsbDevice->clockSources.front().clockId);
            clockIdsToSet.push_back(mUsbDevice->clockSources.front().clockId);
        }

        // Lambda: run SET_CUR + GET_CUR for a single clock source.
        // Stage 3 fix: GET_CUR FIRST. If the device is already at the
        // requested rate, skip SET_CUR entirely. Some UAC2 devices
        // (UGREEN CM720) trigger an internal clock-domain re-sync on
        // every SET_CUR even when the value is unchanged, which produces
        // audible noise on the playback output during the resync window.
        // For clock sources that only support a single rate (e.g. UGREEN
        // mic clock id=27 = 48000-only), every SET_CUR is redundant.
        auto setOneClock = [&](uint8_t clockId) -> bool {
            // Step 1: GET_CUR — what rate is the clock currently at?
            auto getReq = usb::buildUac2GetSampleRateRequest(clockId, controlIface);
            std::array<uint8_t, 4> currentReadback{};
            int curRead = libusb_control_transfer(
                mDeviceHandle,
                getReq.bmRequestType,
                getReq.bRequest,
                getReq.wValue,
                getReq.wIndex,
                currentReadback.data(),
                static_cast<uint16_t>(currentReadback.size()),
                /*timeout*/ 1000);
            if (curRead == 4) {
                uint32_t current = usb::decodeUac2SampleRateResponse(currentReadback);
                if (current == requested) {
                    LOGI("Rate negotiation: UAC2 clockSrc=%d already at %u Hz "
                         "(skipping SET_CUR to avoid resync glitch)",
                         clockId, requested);
                    return true;
                }
                LOGI("Rate negotiation: UAC2 clockSrc=%d currently %u, will SET to %u",
                     clockId, current, requested);
            }
            // GET_CUR failure is non-fatal — fall through to SET_CUR which
            // will fail clearly if the device truly can't accept the rate.

            // Step 2: SET_CUR — only reached if the rate actually needs to change
            auto setReq = usb::buildUac2SetSampleRateRequest(
                clockId, controlIface, requested);
            int r = libusb_control_transfer(
                mDeviceHandle,
                setReq.bmRequestType,
                setReq.bRequest,
                setReq.wValue,
                setReq.wIndex,
                setReq.payload.data(),
                static_cast<uint16_t>(setReq.payload.size()),
                /*timeout*/ 1000);
            if (r < 0) {
                LOGE("UAC2 SET_CUR sample rate failed for clockSrc %d (%u Hz): %s",
                     clockId, requested, libusb_error_name(r));
                mLastLibusbError.store(r);
                return false;
            }

            // GET_CUR verification (re-use the same getReq shape from the
            // pre-check above; declare under a different name to avoid
            // shadowing the outer scope).
            auto verifyReq = usb::buildUac2GetSampleRateRequest(clockId, controlIface);
            std::array<uint8_t, 4> readback{};
            int g = libusb_control_transfer(
                mDeviceHandle,
                verifyReq.bmRequestType,
                verifyReq.bRequest,
                verifyReq.wValue,
                verifyReq.wIndex,
                readback.data(),
                static_cast<uint16_t>(readback.size()),
                /*timeout*/ 1000);
            if (g == 4) {
                uint32_t actual = usb::decodeUac2SampleRateResponse(readback);
                if (actual != requested) {
                    LOGW("UAC2 device coerced sample rate %u Hz -> %u Hz on clockSrc %d",
                         requested, actual, clockId);
                    mRequestedSampleRate = static_cast<int>(actual);
                }
                LOGI("Rate negotiation: UAC2 clockSrc=%d req=%u actual=%u",
                     clockId, requested, actual);
            } else {
                LOGW("UAC2 GET_CUR sample rate readback failed on clockSrc %d: %s (continuing)",
                     clockId, g < 0 ? libusb_error_name(g) : "short response");
                LOGI("Rate negotiation: UAC2 clockSrc=%d req=%u (unverified)",
                     clockId, requested);
            }
            return true;
        };

        for (uint8_t clockId : clockIdsToSet) {
            if (!setOneClock(clockId)) {
                return false;
            }
        }
        return true;
    }

    LOGE("configureSampleRate: unknown UAC version %d", version);
    return false;
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

    // Note: sample rate negotiation (configureSampleRate) used to run here,
    // but it has to happen AFTER the transfer manager has claimed the
    // streaming interface and put it in its active altsetting — otherwise
    // UAC 1.0 endpoint-recipient SET_CUR fails with LIBUSB_ERROR_IO because
    // the target endpoint only exists when the device is in alt > 0. The
    // transfer manager invokes it via the clock config hook registered in
    // setupTransferManager() below.

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
        const int outputChannels = mSelectedPlaybackFormat ? mSelectedPlaybackFormat->channels
                                : mSelectedPlayback ? mSelectedPlayback->primaryFormat().channels : 0;
        const int inputChannels = mSelectedCaptureFormat ? mSelectedCaptureFormat->channels
                                : mSelectedCapture ? mSelectedCapture->primaryFormat().channels : 0;

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

    // Stop DSP thread. Burst-release the wake semaphore so any pending
    // try_acquire_for(5ms) returns immediately instead of waiting out the
    // safety timeout. The releases saturate at the semaphore's capacity.
    mDspRunning.store(false);
    for (int i = 0; i < 8; ++i) {
        mDspWake.release();
    }
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
    info.channelCount = mSelectedPlaybackFormat ? mSelectedPlaybackFormat->channels
                      : mSelectedPlayback ? mSelectedPlayback->primaryFormat().channels : 2;
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

    // Tell the transfer manager which UAC version we're talking to. This
    // controls the feedback transfer's iso packet length (3 bytes for UAC1
    // 10.14, 4 bytes for UAC2 16.16) and the version forwarded to the
    // ClockController. Without this the manager would default to UAC2 and
    // misparse feedback packets from UAC1 devices.
    mTransferManager->setUacVersion(
        mUsbDevice->uacVersion == 2
            ? UacVersion::UAC_2_0
            : UacVersion::UAC_1_0);

    // Configure transfer parameters from the selected format (not just
    // primaryFormat, because AltsettingSelector may have picked a non-primary
    // format within a multi-format altsetting).
    usb::TransferConfig config;
    config.sampleRate = mRequestedSampleRate;

    // Use the explicitly selected format when available (set by selectBestInterfaces),
    // otherwise fall back to primaryFormat() for backward compatibility.
    const auto& outFmt = mSelectedPlaybackFormat
        ? *mSelectedPlaybackFormat : configInterface->primaryFormat();
    config.channelCount = outFmt.channels;
    config.bitDepth = outFmt.bitResolution;

    // FIX: Configure input parameters separately (may differ from output)
    if (mSelectedCapture) {
        const auto& inFmt = mSelectedCaptureFormat
            ? *mSelectedCaptureFormat : mSelectedCapture->primaryFormat();
        config.inputChannelCount = inFmt.channels;
        config.inputBitDepth = inFmt.bitResolution;
        LOGI("Input config: %d channels, %d-bit (output: %d channels, %d-bit)",
             config.inputChannelCount, config.inputBitDepth,
             config.channelCount, config.bitDepth);
    } else {
        // No capture, use same as output (default)
        config.inputChannelCount = config.channelCount;
        config.inputBitDepth = config.bitDepth;
    }

    // Set PCM format based on bit depth. Output and input can use different
    // wire formats on the same device — e.g. the GHW USB AUDIO exposes
    // 2ch/24-bit playback alongside 1ch/16-bit capture, so we need to track
    // both formats independently and NEVER reuse the output format for the
    // input converter. Doing so garbles the input byte-for-byte (the S24_3LE
    // reader walks 3 bytes per sample over data that was laid out as 2-byte
    // S16 samples, interpreting every 1.5 real samples as one "24-bit" one
    // and reading 50% past the end of each packet).
    auto bitDepthToFormat = [](int depth) {
        switch (depth) {
            case 16: return usb::PcmFormat::PCM_S16_LE;
            case 24: return usb::PcmFormat::PCM_S24_3LE;
            case 32: return usb::PcmFormat::PCM_S32_LE;
            default: return usb::PcmFormat::PCM_S16_LE;
        }
    };
    config.pcmFormat = bitDepthToFormat(config.bitDepth);
    config.inputPcmFormat = bitDepthToFormat(config.inputBitDepth);

    // Determine the actual USB speed of the attached device. UAC 2.0 almost
    // always runs at USB 2.0 high-speed, where iso endpoints are polled once
    // per 125 µs microframe (8 packets per 1 ms SOF frame). UAC 1.0 is
    // full-speed: 1 iso packet per 1 ms frame. The iso packet sizing has to
    // match that cadence exactly — a high-speed device that receives a
    // full-speed-sized packet (8× too large) consumes it 8× too fast per
    // microframe and produces grossly distorted audio, which is what we saw
    // on the first real UAC 2.0 test device (UGREEN CM720).
    libusb_device* dev = libusb_get_device(mDeviceHandle);
    const int speed = dev ? libusb_get_device_speed(dev) : LIBUSB_SPEED_FULL;
    const bool isHighSpeed = (speed == LIBUSB_SPEED_HIGH ||
                               speed == LIBUSB_SPEED_SUPER ||
                               speed == LIBUSB_SPEED_SUPER_PLUS);
    const int packetsPerMs = isHighSpeed ? 8 : 1;
    const char* speedName =
        (speed == LIBUSB_SPEED_LOW)        ? "LOW"   :
        (speed == LIBUSB_SPEED_FULL)       ? "FULL"  :
        (speed == LIBUSB_SPEED_HIGH)       ? "HIGH"  :
        (speed == LIBUSB_SPEED_SUPER)      ? "SUPER" :
        (speed == LIBUSB_SPEED_SUPER_PLUS) ? "SUPER+" :
                                              "UNKNOWN";

    // Frames per iso packet. At 48 kHz:
    //   full-speed: 48000 / 1000 = 48 frames/packet
    //   high-speed: 48000 / 8000 = 6  frames/packet  (microframes)
    // Non-integer rates (44.1 kHz, 88.2 kHz) truncate here; the clock
    // controller's fractional accumulator compensates via the feedback
    // endpoint when available.
    config.framesPerPacket = mRequestedSampleRate / (packetsPerMs * 1000);
    // Keep ~8 ms of audio per libusb transfer in both speed classes so the
    // event-loop cadence is constant regardless of USB speed.
    config.packetsPerTransfer = 8 * packetsPerMs;  // 8 on FS, 64 on HS
    config.numTransfers = 3;                       // Triple buffering

    LOGI("USB speed: %s (libusb=%d) → %d packets/ms, %d frames/packet, "
         "%d packets/xfer",
         speedName, speed, packetsPerMs,
         config.framesPerPacket, config.packetsPerTransfer);

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

    // Register the data-ready notifier so the USB event thread wakes the
    // DSP loop on every transfer completion. Replaces the previous 200µs
    // polling sleep with futex-backed signaling.
    mTransferManager->setDataReadyCallback([this]() {
        // counting_semaphore::release is wait-free in the common case
        // (no waiters or single waiter) and saturates at the capacity.
        mDspWake.release();
    });

    // Register the clock configuration hook. The transfer manager invokes
    // it once, synchronously, during start() — after claim_interface and
    // set_interface_alt_setting for all streaming interfaces, but before
    // any iso transfer is allocated or submitted. That is the earliest
    // moment where the device's active endpoints actually exist on the
    // wire, so endpoint-recipient requests like the UAC 1.0 sampling
    // frequency SET_CUR reach a live target instead of failing with IO.
    mTransferManager->setClockConfigHook([this]() {
        return configureSampleRate();
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

    // Per-window counters for the periodic WMA_AUDIT log. They cover the
    // last ~300 DSP callbacks and help diagnose "no sound in input_fx mode"
    // situations: if inputReadOk stays at 0 despite mSelectedCapture being
    // set, we know readInput is failing (ring underrun from the USB side);
    // if inputReadOk is climbing but the last observed peak is 0, the USB
    // transfers are delivering silence from the device.
    int inputReadOkCount = 0;
    int inputReadFailCount = 0;
    float lastInputPeakObserved = 0.0f;
    size_t lastInputAvailBefore = 0;
    // Output peak observed after the audio callback fills outputBuffer,
    // before it goes through writeOutput → ring buffer → format converter
    // → iso transfer → DAC. Lets us distinguish "engine produced bad
    // output" (high outPeak before USB processing) from "USB pipeline
    // corrupted clean output" (engine outPeak normal but DAC plays noise).
    float lastOutputPeakObserved = 0.0f;

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

        // Wait for the USB event thread to signal that data is available
        // (or that output space was freed) before checking the ring buffers.
        // The 5ms timeout is a safety net: if the device stalls, the
        // disconnect detection above will catch it within 500ms via the
        // watchdog. We don't want to block forever in case a wake is missed.
        //
        // Drain any backlog of pending wakes in one shot — counting_semaphore
        // collapses naturally because we just re-check the readiness state
        // after waking, and one wake is enough to cover N completed transfers.
        (void)mDspWake.try_acquire_for(std::chrono::milliseconds(5));
        if (!mDspRunning.load(std::memory_order_acquire)) {
            break;
        }

        bool outputReady = !mSelectedPlayback ||
            (mTransferManager->getOutputBufferAvailable() >= outputSamples);
        bool inputReady = !mSelectedCapture ||
            (mTransferManager->getInputBufferAvailable() >= inputSamples);

        if (!outputReady || !inputReady) {
            // Spurious or premature wake — go back to waiting. No sleep:
            // try_acquire_for above is the throttle.
            continue;
        }

        // Get input data if capture is enabled
        const float* inputPtr = nullptr;
        if (mSelectedCapture) {
            // Snapshot ring availability before the read so the periodic
            // diagnostic log can report whether the ring was starving.
            lastInputAvailBefore = mTransferManager->getInputBufferAvailable();
            if (mTransferManager->readInput(inputBuffer.data(), inputSamples)) {
                ++inputReadOkCount;
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

                // Track the peak of the most recent valid block for the
                // diagnostic log (cheap: bounded scan over the stereo tail).
                {
                    const size_t scanLimit = std::min(stereoSamples, static_cast<size_t>(128));
                    float peak = 0.0f;
                    for (size_t i = 0; i < scanLimit; ++i) {
                        float a = std::abs(stereoSrc[i]);
                        if (a > peak) peak = a;
                    }
                    lastInputPeakObserved = peak;
                }
            } else if (mDspHasValidInput) {
                ++inputReadFailCount;
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
                    wma::logMessage(wma::LogLevel::WARN, "WMA_AUDIT",
                        "USB_INPUT_UNDERRUN: using faded last block (%d)", underrunLogCount);
                }
            } else {
                // readInput failed AND we have no previous block to fade
                // from. inputPtr stays nullptr — the callback will see
                // "no input this block". Count it so the diagnostic log
                // can surface a persistent starve before anyone notices
                // the silence.
                ++inputReadFailCount;
            }
        }

        // Call audio callback
        float* outputPtr = mSelectedPlayback ? outputBuffer.data() : nullptr;

        // DIAGNOSTIC: Log input state periodically (every ~300 DSP callbacks
        // ≈ 1.6 s at 48 kHz / 256-frame blocks). The extra fields — ring
        // availability before the read, readInput success/fail ratio for
        // the window, and the peak of the last valid input block — are the
        // minimum set needed to diagnose "no sound in input_fx mode":
        //   - fail >> ok  → the USB input ring is starving (device not
        //                    delivering packets, or processInputTransfer is
        //                    failing to write to the ring)
        //   - ok >> fail, peak ≈ 0 → the device IS delivering packets, but
        //                             they are silence (nothing plugged in,
        //                             or wrong altsetting / channel layout)
        //   - ok >> fail, peak > 0  → input path is healthy, the issue is
        //                              somewhere in the user callback
        static int usbDspDiagCount = 0;
        if (++usbDspDiagCount >= 300) {
            const int ioTotal = inputReadOkCount + inputReadFailCount;
            wma::logMessage(wma::LogLevel::INFO, "WMA_AUDIT",
                "USB_DSP: hasCapture=%d inputPtr=%p outputPtr=%p "
                "frames=%d streamMode=%d | read ok=%d fail=%d ratio=%.2f "
                "ringAvailPre=%zu lastInPeak=%.5f lastOutPeak=%.5f",
                mSelectedCapture.has_value() ? 1 : 0, inputPtr, outputPtr,
                framesPerBlock, static_cast<int>(mStreamingMode),
                inputReadOkCount, inputReadFailCount,
                ioTotal > 0 ? static_cast<float>(inputReadOkCount) / ioTotal : 0.0f,
                lastInputAvailBefore, lastInputPeakObserved,
                lastOutputPeakObserved);
            usbDspDiagCount = 0;
            inputReadOkCount = 0;
            inputReadFailCount = 0;
            lastOutputPeakObserved = 0.0f;
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

        // Sample the engine output peak BEFORE writeOutput. Lets us
        // diagnose "engine produced this noise" vs "USB pipeline garbled
        // clean engine output". Bounded scan (≤ 512 samples = ~256 frames)
        // so we don't add measurable RT cost.
        if (outputPtr && outputSamples > 0) {
            float peak = 0.0f;
            const size_t scanSamples = std::min(outputSamples, size_t(512));
            for (size_t i = 0; i < scanSamples; ++i) {
                float a = std::fabs(outputPtr[i]);
                if (a > peak) peak = a;
            }
            if (peak > lastOutputPeakObserved) {
                lastOutputPeakObserved = peak;
            }
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

    // Collect from all playback interfaces (iterating all formats per altsetting)
    for (const auto& iface : mUsbDevice->playbackInterfaces) {
        for (const auto& fmt : iface.formats) {
            // Sample rates
            for (int rate : fmt.sampleRates) {
                if (std::find(caps.supportedSampleRates.begin(),
                             caps.supportedSampleRates.end(), rate) ==
                    caps.supportedSampleRates.end()) {
                    caps.supportedSampleRates.push_back(rate);
                }
            }

            // Bit depths
            int depth = fmt.bitResolution;
            if (depth > 0 && std::find(caps.supportedBitDepths.begin(),
                         caps.supportedBitDepths.end(), depth) ==
                caps.supportedBitDepths.end()) {
                caps.supportedBitDepths.push_back(depth);
            }

            // Channels
            caps.maxChannelsOutput = std::max(caps.maxChannelsOutput,
                                              static_cast<int>(fmt.channels));
        }

        // Feedback support
        if (iface.feedbackEndpoint) {
            caps.supportsFeedback = true;
        }
    }

    // Input channels
    for (const auto& iface : mUsbDevice->captureInterfaces) {
        for (const auto& fmt : iface.formats) {
            caps.maxChannelsInput = std::max(caps.maxChannelsInput,
                                             static_cast<int>(fmt.channels));
        }
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

    // Release the AudioControl interface if we claimed it. Skip the call
    // when the device is physically disconnected — the handle is invalid
    // and libusb_release_interface can crash on Android usbfs in that state.
    if (mControlInterfaceClaimed && mDeviceHandle && !mDeviceDisconnected.load()) {
        const int ifNum = mUsbDevice ? mUsbDevice->controlInterface : 0;
        int r = libusb_release_interface(mDeviceHandle, ifNum);
        if (r != LIBUSB_SUCCESS) {
            LOGW("Failed to release control interface %d: %s",
                 ifNum, libusb_error_name(r));
        } else {
            LOGI("Released AudioControl interface %d", ifNum);
        }
    }
    mControlInterfaceClaimed = false;

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

} // namespace watermelon_audio
