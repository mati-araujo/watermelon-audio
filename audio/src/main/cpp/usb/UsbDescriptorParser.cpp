/**
 * UsbDescriptorParser.cpp
 *
 * Implementation of USB Audio Class 1.0 descriptor parser.
 */

#include "UsbDescriptorParser.h"
#include "../platform/Logger.h"
#include <cstring>
#include <algorithm>

#define LOG_TAG "UsbDescriptorParser"
#define LOGD(...) wma::logMessage(wma::LogLevel::DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) wma::logMessage(wma::LogLevel::WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)

namespace watermelon_audio {
namespace usb {

// ============================================================================
// Public API
// ============================================================================

std::optional<UsbAudioDevice> UsbDescriptorParser::parse(
    const uint8_t* data,
    size_t length,
    const UsbDeviceInfo& deviceInfo) {

    if (data == nullptr || length < sizeof(ConfigDescriptor)) {
        setError("Invalid descriptor data or length too short");
        return std::nullopt;
    }

    // Reset state
    mLastError.clear();
    mDetectedUacVersion = 1;
    mContext = ParsingContext{};

    // Create device with basic info
    UsbAudioDevice device;
    device.deviceInfo = deviceInfo;

    // Parse all descriptors
    if (!parseDescriptors(data, length, device)) {
        return std::nullopt;
    }

    // Validate we found audio interfaces
    if (device.playbackInterfaces.empty() && device.captureInterfaces.empty()) {
        setError("No audio streaming interfaces found");
        return std::nullopt;
    }

    LOGI("Successfully parsed USB Audio device: VID=%04x PID=%04x",
         deviceInfo.vendorId, deviceInfo.productId);
    LOGI("  Playback interfaces: %zu, Capture interfaces: %zu",
         device.playbackInterfaces.size(), device.captureInterfaces.size());

    return device;
}

bool UsbDescriptorParser::isAudioDevice(const uint8_t* data, size_t length) {
    if (data == nullptr || length < sizeof(ConfigDescriptor)) {
        return false;
    }

    size_t offset = 0;
    while (offset + 2 <= length) {
        const auto* header = reinterpret_cast<const DescriptorHeader*>(data + offset);

        if (header->bLength < 2 || offset + header->bLength > length) {
            break;
        }

        // Check for Audio interface
        if (header->bDescriptorType == USB_DT_INTERFACE &&
            header->bLength >= sizeof(InterfaceDescriptor)) {

            const auto* iface = reinterpret_cast<const InterfaceDescriptor*>(data + offset);
            if (iface->bInterfaceClass == USB_CLASS_AUDIO) {
                return true;
            }
        }

        offset += header->bLength;
    }

    return false;
}

// ============================================================================
// Main Parsing Logic
// ============================================================================

bool UsbDescriptorParser::parseDescriptors(
    const uint8_t* data,
    size_t length,
    UsbAudioDevice& device) {

    // First, parse configuration descriptor
    if (!parseConfigDescriptor(data, length, device)) {
        return false;
    }

    // Parse remaining descriptors
    size_t offset = sizeof(ConfigDescriptor);

    while (offset + 2 <= length) {
        const auto* header = reinterpret_cast<const DescriptorHeader*>(data + offset);

        // Validate descriptor length
        if (header->bLength < 2) {
            LOGW("Invalid descriptor length %d at offset %zu", header->bLength, offset);
            break;
        }

        if (offset + header->bLength > length) {
            LOGW("Descriptor extends beyond buffer at offset %zu", offset);
            break;
        }

        const uint8_t* descData = data + offset;
        size_t remaining = length - offset;

        switch (header->bDescriptorType) {
            case USB_DT_INTERFACE:
                if (!parseInterfaceDescriptor(descData, remaining, device)) {
                    LOGW("Failed to parse interface descriptor at offset %zu", offset);
                }
                break;

            case USB_DT_ENDPOINT:
                if (mContext.inAudioStreaming) {
                    UsbEndpointInfo endpoint;
                    if (parseEndpointDescriptor(descData, endpoint)) {
                        // Route the endpoint based on its usage type bits
                        // (bmAttributes 5:4 — see UsbConstants.h:185-188).
                        const uint8_t usage = endpoint.attributes & 0x30;
                        if (endpoint.isIsochronous() &&
                            usage == USB_ENDPOINT_USAGE_FEEDBACK &&
                            endpoint.isInput()) {
                            // Explicit feedback endpoint: separate iso IN
                            // endpoint that carries the device's actual rate.
                            UsbFeedbackEndpoint fb;
                            fb.endpoint = endpoint;
                            fb.isImplicit = false;
                            mContext.currentStreaming.feedbackEndpoint = fb;
                            LOGI("Feedback endpoint detected (explicit): "
                                 "addr=0x%02x maxPkt=%d interval=%d (IF%d Alt%d)",
                                 endpoint.address, endpoint.maxPacketSize,
                                 endpoint.interval,
                                 mContext.currentStreaming.interfaceNumber,
                                 mContext.currentStreaming.alternateSetting);
                        } else {
                            // Data endpoint (audio samples).
                            mContext.currentStreaming.dataEndpoint = endpoint;
                            // If this data endpoint also signals implicit
                            // feedback timing, mark it so the transfer manager
                            // can extract drift from packet completion.
                            if (endpoint.isIsochronous() &&
                                usage == USB_ENDPOINT_USAGE_IMPLICIT_FB) {
                                UsbFeedbackEndpoint fb;
                                fb.endpoint = endpoint;
                                fb.isImplicit = true;
                                mContext.currentStreaming.feedbackEndpoint = fb;
                                LOGI("Implicit feedback detected on data EP "
                                     "0x%02x (IF%d Alt%d)",
                                     endpoint.address,
                                     mContext.currentStreaming.interfaceNumber,
                                     mContext.currentStreaming.alternateSetting);
                            }
                        }
                    }
                }
                break;

            case UAC_CS_INTERFACE:
                if (mContext.inAudioControl) {
                    parseAudioControlInterface(descData, remaining, device);
                } else if (mContext.inAudioStreaming) {
                    parseAudioStreamingInterface(descData, remaining, mContext.currentStreaming);
                }
                break;

            case UAC_CS_ENDPOINT:
                if (mContext.inAudioStreaming) {
                    parseAudioEndpoint(descData, mContext.currentStreaming);
                }
                break;

            default:
                // Skip unknown descriptors
                break;
        }

        offset += header->bLength;
    }

    // Save the last streaming interface if valid
    if (mContext.inAudioStreaming &&
        !mContext.currentStreaming.formats.empty() &&
        mContext.currentStreaming.formats[0].channels > 0 &&
        mContext.currentStreaming.dataEndpoint.maxPacketSize > 0) {

        if (mContext.currentStreaming.isPlayback()) {
            device.playbackInterfaces.push_back(mContext.currentStreaming);
        } else if (mContext.currentStreaming.isCapture()) {
            device.captureInterfaces.push_back(mContext.currentStreaming);
        }
    }

    return true;
}

bool UsbDescriptorParser::parseConfigDescriptor(
    const uint8_t* data,
    size_t length,
    UsbAudioDevice& device) {

    if (length < sizeof(ConfigDescriptor)) {
        setError("Configuration descriptor too short");
        return false;
    }

    const auto* config = reinterpret_cast<const ConfigDescriptor*>(data);

    if (config->bDescriptorType != USB_DT_CONFIG) {
        setError("Not a configuration descriptor");
        return false;
    }

    uint16_t totalLength = readUint16LE(reinterpret_cast<const uint8_t*>(&config->wTotalLength));

    LOGD("Configuration descriptor: %d interfaces, total length %d",
         config->bNumInterfaces, totalLength);

    return true;
}

bool UsbDescriptorParser::parseInterfaceDescriptor(
    const uint8_t* data,
    size_t remaining,
    UsbAudioDevice& device) {

    if (remaining < sizeof(InterfaceDescriptor)) {
        return false;
    }

    const auto* iface = reinterpret_cast<const InterfaceDescriptor*>(data);

    // Save previous streaming interface if valid
    if (mContext.inAudioStreaming &&
        !mContext.currentStreaming.formats.empty() &&
        mContext.currentStreaming.formats[0].channels > 0 &&
        mContext.currentStreaming.dataEndpoint.maxPacketSize > 0) {

        if (mContext.currentStreaming.isPlayback()) {
            device.playbackInterfaces.push_back(mContext.currentStreaming);
        } else if (mContext.currentStreaming.isCapture()) {
            device.captureInterfaces.push_back(mContext.currentStreaming);
        }
    }

    // Reset context for new interface
    mContext.currentInterfaceNumber = iface->bInterfaceNumber;
    mContext.currentAlternateSetting = iface->bAlternateSetting;
    mContext.currentInterfaceClass = iface->bInterfaceClass;
    mContext.currentInterfaceSubClass = iface->bInterfaceSubClass;
    mContext.inAudioControl = false;
    mContext.inAudioStreaming = false;
    mContext.currentStreaming = UsbStreamingInterface{};

    if (iface->bInterfaceClass != USB_CLASS_AUDIO) {
        return true; // Not an audio interface, skip
    }

    // Detect UAC version from protocol
    if (iface->bInterfaceProtocol == UAC_VERSION_2) {
        mDetectedUacVersion = 2;
        device.uacVersion = 2;
    }

    switch (iface->bInterfaceSubClass) {
        case UAC_SUBCLASS_AUDIOCONTROL:
            mContext.inAudioControl = true;
            device.controlInterface = iface->bInterfaceNumber;
            LOGD("Found Audio Control interface: %d", iface->bInterfaceNumber);
            break;

        case UAC_SUBCLASS_AUDIOSTREAMING:
            mContext.inAudioStreaming = true;
            mContext.currentStreaming.interfaceNumber = iface->bInterfaceNumber;
            mContext.currentStreaming.alternateSetting = iface->bAlternateSetting;
            LOGD("Found Audio Streaming interface: %d alt %d (endpoints: %d)",
                 iface->bInterfaceNumber, iface->bAlternateSetting, iface->bNumEndpoints);
            break;

        case UAC_SUBCLASS_MIDISTREAMING:
            LOGD("Found MIDI Streaming interface: %d (skipping)", iface->bInterfaceNumber);
            break;

        default:
            break;
    }

    return true;
}

bool UsbDescriptorParser::parseAudioControlInterface(
    const uint8_t* data,
    size_t remaining,
    UsbAudioDevice& device) {

    if (remaining < 3) {
        return false;
    }

    uint8_t subtype = data[2];

    // UAC 2.0 uses different descriptor subtypes for some units
    if (mDetectedUacVersion == 2) {
        switch (subtype) {
            case UAC_AC_HEADER:
                return parseUAC2ACHeader(data, device);

            case UAC_AC_INPUT_TERMINAL:
                return parseUAC2InputTerminal(data, device);

            case UAC_AC_OUTPUT_TERMINAL:
                return parseUAC2OutputTerminal(data, device);

            case UAC_AC_FEATURE_UNIT:
                return parseUAC2FeatureUnit(data, device);

            case UAC2_AC_CLOCK_SOURCE:
                return parseUAC2ClockSource(data, device);

            case UAC2_AC_CLOCK_SELECTOR:
                return parseUAC2ClockSelector(data, device);

            case UAC2_AC_CLOCK_MULTIPLIER:
                return parseUAC2ClockMultiplier(data, device);

            case UAC_AC_MIXER_UNIT:
            case UAC_AC_SELECTOR_UNIT:
            case UAC2_AC_EFFECT_UNIT:
            case UAC2_AC_PROCESSING_UNIT_V2:
            case UAC2_AC_EXTENSION_UNIT_V2:
            case UAC2_AC_SAMPLE_RATE_CONVERTER:
                LOGD("Skipping UAC 2.0 AC descriptor subtype: 0x%02x", subtype);
                break;

            default:
                LOGD("Unknown UAC 2.0 AC descriptor subtype: 0x%02x", subtype);
                break;
        }
        return true;
    }

    // UAC 1.0 parsing
    switch (subtype) {
        case UAC_AC_HEADER:
            return parseACHeader(data, device);

        case UAC_AC_INPUT_TERMINAL:
            return parseInputTerminal(data, device);

        case UAC_AC_OUTPUT_TERMINAL:
            return parseOutputTerminal(data, device);

        case UAC_AC_FEATURE_UNIT:
            return parseFeatureUnit(data, device);

        case UAC_AC_MIXER_UNIT:
        case UAC_AC_SELECTOR_UNIT:
        case UAC_AC_PROCESSING_UNIT:
        case UAC_AC_EXTENSION_UNIT:
            // Skip for now, not critical for basic functionality
            LOGD("Skipping AC descriptor subtype: 0x%02x", subtype);
            break;

        default:
            LOGD("Unknown AC descriptor subtype: 0x%02x", subtype);
            break;
    }

    return true;
}

bool UsbDescriptorParser::parseAudioStreamingInterface(
    const uint8_t* data,
    size_t remaining,
    UsbStreamingInterface& streaming) {

    if (remaining < 3) {
        return false;
    }

    uint8_t subtype = data[2];

    // UAC 2.0 uses the same subtype codes but different structures
    if (mDetectedUacVersion == 2) {
        switch (subtype) {
            case UAC_AS_GENERAL:
                return parseUAC2ASGeneral(data, streaming);

            case UAC_AS_FORMAT_TYPE:
                // UAC2: AS_GENERAL always comes first and pushes a format entry.
                // FORMAT_TYPE fills the last entry. If formats is somehow empty
                // (malformed descriptor order), push a default so we don't crash.
                if (streaming.formats.empty()) {
                    streaming.formats.emplace_back();
                }
                return parseUAC2FormatType(data, streaming.formats.back());

            case UAC_AS_FORMAT_SPECIFIC:
                // Not commonly used, skip
                break;

            default:
                LOGD("Unknown UAC 2.0 AS descriptor subtype: 0x%02x", subtype);
                break;
        }
        return true;
    }

    // UAC 1.0 parsing
    switch (subtype) {
        case UAC_AS_GENERAL:
            return parseASGeneral(data, streaming);

        case UAC_AS_FORMAT_TYPE:
            // UAC1: AS_GENERAL comes first and pushes a format entry.
            // FORMAT_TYPE fills the last entry with channels, bit depth, rates.
            // If a second FORMAT_TYPE appears (rare but spec-legal), push a new
            // entry so we don't overwrite the first.
            if (streaming.formats.empty()) {
                streaming.formats.emplace_back();
            }
            return parseFormatType(data, streaming.formats.back());

        case UAC_AS_FORMAT_SPECIFIC:
            // Not commonly used, skip
            break;

        default:
            LOGD("Unknown AS descriptor subtype: 0x%02x", subtype);
            break;
    }

    return true;
}

// ============================================================================
// UAC Class-Specific Descriptor Parsing
// ============================================================================

bool UsbDescriptorParser::parseACHeader(const uint8_t* data, UsbAudioDevice& device) {
    if (data[0] < sizeof(ACHeaderDescriptor)) {
        return false;
    }

    const auto* header = reinterpret_cast<const ACHeaderDescriptor*>(data);

    uint16_t bcdADC = readUint16LE(reinterpret_cast<const uint8_t*>(&header->bcdADC));

    // Detect UAC version from bcdADC
    if ((bcdADC & 0xFF00) == 0x0200) {
        mDetectedUacVersion = 2;
        device.uacVersion = 2;
        LOGD("Detected UAC 2.0 from bcdADC: 0x%04x", bcdADC);
    } else {
        mDetectedUacVersion = 1;
        device.uacVersion = 1;
        LOGD("Detected UAC 1.0 from bcdADC: 0x%04x", bcdADC);
    }

    LOGD("AC Header: bcdADC=0x%04x, bInCollection=%d",
         bcdADC, header->bInCollection);

    return true;
}

bool UsbDescriptorParser::parseInputTerminal(const uint8_t* data, UsbAudioDevice& device) {
    if (data[0] < sizeof(InputTerminalDescriptor)) {
        return false;
    }

    const auto* term = reinterpret_cast<const InputTerminalDescriptor*>(data);

    uint16_t termType = readUint16LE(reinterpret_cast<const uint8_t*>(&term->wTerminalType));

    device.inputTerminalIds.push_back(term->bTerminalID);

    // Stage 3: also store the full terminal record. UAC1 has no clock source
    // reference, so clockSourceId stays 0 — resolveClockSourceId() falls
    // through to the UAC1 path (endpoint-recipient SET_CUR).
    UsbInputTerminal rec;
    rec.terminalId = term->bTerminalID;
    rec.terminalType = termType;
    rec.clockSourceId = 0;
    rec.numChannels = term->bNrChannels;
    device.inputTerminals.push_back(rec);

    LOGD("Input Terminal: ID=%d, Type=0x%04x, Channels=%d",
         term->bTerminalID, termType, term->bNrChannels);

    return true;
}

bool UsbDescriptorParser::parseOutputTerminal(const uint8_t* data, UsbAudioDevice& device) {
    if (data[0] < sizeof(OutputTerminalDescriptor)) {
        return false;
    }

    const auto* term = reinterpret_cast<const OutputTerminalDescriptor*>(data);

    uint16_t termType = readUint16LE(reinterpret_cast<const uint8_t*>(&term->wTerminalType));

    device.outputTerminalIds.push_back(term->bTerminalID);

    // Stage 3: store full record. UAC1 clockSourceId = 0.
    UsbOutputTerminal rec;
    rec.terminalId = term->bTerminalID;
    rec.terminalType = termType;
    rec.sourceId = term->bSourceID;
    rec.clockSourceId = 0;
    device.outputTerminals.push_back(rec);

    LOGD("Output Terminal: ID=%d, Type=0x%04x, SourceID=%d",
         term->bTerminalID, termType, term->bSourceID);

    return true;
}

bool UsbDescriptorParser::parseFeatureUnit(const uint8_t* data, UsbAudioDevice& device) {
    if (data[0] < sizeof(FeatureUnitDescriptor)) {
        return false;
    }

    const auto* unit = reinterpret_cast<const FeatureUnitDescriptor*>(data);
    uint8_t controlSize = unit->bControlSize;
    if (controlSize == 0) controlSize = 1;  // Default to 1 byte

    // Calculate number of control entries (master + N channels)
    // Descriptor length = 7 + (numChannels + 1) * controlSize + 1(iFeature)
    uint8_t descriptorLen = data[0];
    if (descriptorLen < 8) {
        LOGW("Feature Unit descriptor too short: %d bytes", descriptorLen);
        return false;
    }

    uint8_t numControlEntries = (descriptorLen - 7 - 1) / controlSize;
    if (numControlEntries == 0) {
        LOGW("Feature Unit has no control entries");
        return false;
    }

    UsbFeatureUnit featureUnit;
    featureUnit.unitId = unit->bUnitID;
    featureUnit.sourceId = unit->bSourceID;
    featureUnit.numChannels = numControlEntries > 0 ? numControlEntries - 1 : 0;

    // Parse control bitmaps for each channel (master + N logical channels)
    // Controls start after bControlSize field (offset 6)
    const uint8_t* controlData = data + 6;

    for (uint8_t i = 0; i < numControlEntries; ++i) {
        uint32_t controls = 0;
        // Read controlSize bytes for this channel's controls
        for (uint8_t b = 0; b < controlSize && b < 4; ++b) {
            controls |= static_cast<uint32_t>(controlData[i * controlSize + b]) << (b * 8);
        }
        featureUnit.channelControls.push_back(controls);
    }

    // Log control capabilities
    bool hasMute = featureUnit.hasMuteControl(0, 1);
    bool hasVolume = featureUnit.hasVolumeControl(0, 1);

    LOGD("Feature Unit: ID=%d, SourceID=%d, ControlSize=%d, Channels=%d, Mute=%d, Volume=%d",
         unit->bUnitID, unit->bSourceID, controlSize, featureUnit.numChannels,
         hasMute, hasVolume);

    // Log per-channel controls if more than master
    if (featureUnit.numChannels > 0) {
        for (uint8_t ch = 0; ch <= featureUnit.numChannels; ++ch) {
            LOGD("  Channel %d controls: 0x%08x (Mute=%d, Volume=%d)",
                 ch, featureUnit.channelControls[ch],
                 featureUnit.hasMuteControl(ch, 1),
                 featureUnit.hasVolumeControl(ch, 1));
        }
    }

    device.featureUnits.push_back(featureUnit);
    return true;
}

bool UsbDescriptorParser::parseASGeneral(const uint8_t* data, UsbStreamingInterface& streaming) {
    if (data[0] < sizeof(ASGeneralDescriptor)) {
        return false;
    }

    const auto* general = reinterpret_cast<const ASGeneralDescriptor*>(data);

    streaming.terminalLink = general->bTerminalLink;

    // Push the first format entry for this altsetting. The subsequent
    // FORMAT_TYPE descriptor will fill in channels, bit depth, etc.
    streaming.formats.emplace_back();
    streaming.formats.back().formatTag = readUint16LE(reinterpret_cast<const uint8_t*>(&general->wFormatTag));

    LOGD("AS General: TerminalLink=%d, FormatTag=0x%04x, Delay=%d",
         general->bTerminalLink, streaming.formats.back().formatTag, general->bDelay);

    return true;
}

bool UsbDescriptorParser::parseFormatType(const uint8_t* data, UsbAudioFormat& format) {
    if (data[0] < sizeof(FormatTypeIDescriptor)) {
        return false;
    }

    const auto* fmt = reinterpret_cast<const FormatTypeIDescriptor*>(data);

    format.formatType = fmt->bFormatType;
    format.channels = fmt->bNrChannels;
    format.subframeSize = fmt->bSubframeSize;
    format.bitResolution = fmt->bBitResolution;

    LOGD("Format Type I: Channels=%d, SubframeSize=%d, BitRes=%d, SamFreqType=%d",
         fmt->bNrChannels, fmt->bSubframeSize, fmt->bBitResolution, fmt->bSamFreqType);

    // Parse sample rates
    const uint8_t* freqData = data + sizeof(FormatTypeIDescriptor);
    size_t expectedLen = sizeof(FormatTypeIDescriptor);

    if (fmt->bSamFreqType == 0) {
        // Continuous sample rate range
        format.hasContinuousRates = true;
        expectedLen += 6; // Two 3-byte frequencies

        if (data[0] >= expectedLen) {
            format.minSampleRate = static_cast<int>(readSampleFrequency(freqData));
            format.maxSampleRate = static_cast<int>(readSampleFrequency(freqData + 3));

            LOGD("  Continuous rates: %d - %d Hz",
                 format.minSampleRate, format.maxSampleRate);
        }
    } else {
        // Discrete sample rates
        format.hasContinuousRates = false;
        expectedLen += fmt->bSamFreqType * 3;

        if (data[0] >= expectedLen) {
            for (int i = 0; i < fmt->bSamFreqType; i++) {
                int sampleRate = static_cast<int>(readSampleFrequency(freqData + (i * 3)));
                format.sampleRates.push_back(sampleRate);
                LOGD("  Sample rate %d: %d Hz", i, sampleRate);
            }
        }
    }

    return true;
}

bool UsbDescriptorParser::parseEndpointDescriptor(const uint8_t* data, UsbEndpointInfo& endpoint) {
    if (data[0] < sizeof(EndpointDescriptor)) {
        return false;
    }

    const auto* ep = reinterpret_cast<const EndpointDescriptor*>(data);

    endpoint.address = ep->bEndpointAddress;
    endpoint.attributes = ep->bmAttributes;
    endpoint.maxPacketSize = readUint16LE(reinterpret_cast<const uint8_t*>(&ep->wMaxPacketSize));
    endpoint.interval = ep->bInterval;

    LOGD("Endpoint: Address=0x%02x, Attr=0x%02x, MaxPacket=%d, Interval=%d",
         endpoint.address, endpoint.attributes, endpoint.maxPacketSize, endpoint.interval);

    // Log sync type for isochronous endpoints
    if (endpoint.isIsochronous()) {
        const char* syncType = "Unknown";
        if (endpoint.isAsync()) syncType = "Async";
        else if (endpoint.isAdaptive()) syncType = "Adaptive";
        else if (endpoint.isSynchronous()) syncType = "Sync";

        LOGD("  Isochronous endpoint: %s, %s",
             endpoint.isInput() ? "IN" : "OUT", syncType);
    }

    return true;
}

bool UsbDescriptorParser::parseAudioEndpoint(const uint8_t* data, UsbStreamingInterface& streaming) {
    if (data[0] < sizeof(AudioEndpointDescriptor)) {
        return false;
    }

    const auto* ep = reinterpret_cast<const AudioEndpointDescriptor*>(data);

    // bmAttributes contains control information
    // Bit 0: Sampling Frequency control
    // Bit 1: Pitch control
    // Bit 7: MaxPacketsOnly

    bool hasSamplingFreqControl = (ep->bmAttributes & 0x01) != 0;
    bool hasPitchControl = (ep->bmAttributes & 0x02) != 0;
    bool maxPacketsOnly = (ep->bmAttributes & 0x80) != 0;

    LOGD("Audio Endpoint: bmAttr=0x%02x (SamFreq=%d, Pitch=%d, MaxPkt=%d)",
         ep->bmAttributes, hasSamplingFreqControl, hasPitchControl, maxPacketsOnly);

    return true;
}

// ============================================================================
// UAC 2.0 Parsing Methods
// ============================================================================

bool UsbDescriptorParser::parseUAC2ACHeader(const uint8_t* data, UsbAudioDevice& device) {
    // UAC 2.0 Header is similar but has different bcdADC field
    if (data[0] < 9) {  // Minimum length for UAC 2.0 header
        return false;
    }

    uint16_t bcdADC = readUint16LE(data + 3);
    uint8_t bCategory = data[5];

    LOGD("UAC 2.0 AC Header: bcdADC=0x%04x, bCategory=0x%02x", bcdADC, bCategory);

    device.uacVersion = 2;
    mDetectedUacVersion = 2;

    return true;
}

bool UsbDescriptorParser::parseUAC2ClockSource(const uint8_t* data, UsbAudioDevice& device) {
    if (data[0] < sizeof(UAC2ClockSourceDescriptor)) {
        return false;
    }

    const auto* desc = reinterpret_cast<const UAC2ClockSourceDescriptor*>(data);

    UsbClockSource clockSource;
    clockSource.clockId = desc->bClockID;
    clockSource.type = static_cast<ClockSourceType>(desc->bmAttributes & 0x03);
    clockSource.syncedToSof = (desc->bmAttributes & UAC2_CLOCK_SOURCE_SYNCED_TO_SOF) != 0;
    clockSource.canControlFrequency = (desc->bmControls & UAC2_CLOCK_FREQ_CONTROL_MASK) != 0;
    clockSource.hasValidityControl = (desc->bmControls & UAC2_CLOCK_VALIDITY_CONTROL_MASK) != 0;
    clockSource.assocTerminal = desc->bAssocTerminal;

    LOGD("UAC 2.0 Clock Source: ID=%d, Type=%d, SyncToSOF=%d, FreqCtrl=%d",
         clockSource.clockId, static_cast<int>(clockSource.type),
         clockSource.syncedToSof, clockSource.canControlFrequency);

    device.clockSources.push_back(clockSource);
    return true;
}

bool UsbDescriptorParser::parseUAC2ClockSelector(const uint8_t* data, UsbAudioDevice& device) {
    if (data[0] < sizeof(UAC2ClockSelectorDescriptor)) {
        return false;
    }

    const auto* desc = reinterpret_cast<const UAC2ClockSelectorDescriptor*>(data);

    UsbClockSelector clockSelector;
    clockSelector.clockId = desc->bClockID;

    // Parse source IDs (variable length)
    for (int i = 0; i < desc->bNrInPins && (5 + i) < data[0]; ++i) {
        clockSelector.sourceIds.push_back(data[5 + i]);
    }

    // Controls byte is after the source IDs
    if (5 + desc->bNrInPins < data[0]) {
        uint8_t bmControls = data[5 + desc->bNrInPins];
        clockSelector.canControlSelector = (bmControls & 0x03) >= 0x02;
    }

    LOGD("UAC 2.0 Clock Selector: ID=%d, NrInPins=%d", clockSelector.clockId, desc->bNrInPins);

    device.clockSelectors.push_back(clockSelector);
    return true;
}

bool UsbDescriptorParser::parseUAC2ClockMultiplier(const uint8_t* data, UsbAudioDevice& device) {
    if (data[0] < sizeof(UAC2ClockMultiplierDescriptor)) {
        return false;
    }

    const auto* desc = reinterpret_cast<const UAC2ClockMultiplierDescriptor*>(data);

    UsbClockMultiplier clockMultiplier;
    clockMultiplier.clockId = desc->bClockID;
    clockMultiplier.sourceId = desc->bCSourceID;
    clockMultiplier.canControlNumerator = (desc->bmControls & 0x03) != 0;
    clockMultiplier.canControlDenominator = (desc->bmControls & 0x0C) != 0;

    LOGD("UAC 2.0 Clock Multiplier: ID=%d, SourceID=%d", clockMultiplier.clockId, clockMultiplier.sourceId);

    device.clockMultipliers.push_back(clockMultiplier);
    return true;
}

bool UsbDescriptorParser::parseUAC2InputTerminal(const uint8_t* data, UsbAudioDevice& device) {
    if (data[0] < sizeof(UAC2InputTerminalDescriptor)) {
        return false;
    }

    const auto* desc = reinterpret_cast<const UAC2InputTerminalDescriptor*>(data);

    uint16_t termType = readUint16LE(reinterpret_cast<const uint8_t*>(&desc->wTerminalType));

    device.inputTerminalIds.push_back(desc->bTerminalID);

    // Stage 3: store full terminal record with clock source linkage.
    UsbInputTerminal rec;
    rec.terminalId = desc->bTerminalID;
    rec.terminalType = termType;
    rec.clockSourceId = desc->bCSourceID;
    rec.numChannels = desc->bNrChannels;
    device.inputTerminals.push_back(rec);

    LOGD("UAC 2.0 Input Terminal: ID=%d, Type=0x%04x, Channels=%d, ClockSrc=%d",
         desc->bTerminalID, termType, desc->bNrChannels, desc->bCSourceID);

    return true;
}

bool UsbDescriptorParser::parseUAC2OutputTerminal(const uint8_t* data, UsbAudioDevice& device) {
    if (data[0] < sizeof(UAC2OutputTerminalDescriptor)) {
        return false;
    }

    const auto* desc = reinterpret_cast<const UAC2OutputTerminalDescriptor*>(data);

    uint16_t termType = readUint16LE(reinterpret_cast<const uint8_t*>(&desc->wTerminalType));

    device.outputTerminalIds.push_back(desc->bTerminalID);

    // Stage 3: store full terminal record with clock source linkage.
    UsbOutputTerminal rec;
    rec.terminalId = desc->bTerminalID;
    rec.terminalType = termType;
    rec.sourceId = desc->bSourceID;
    rec.clockSourceId = desc->bCSourceID;
    device.outputTerminals.push_back(rec);

    LOGD("UAC 2.0 Output Terminal: ID=%d, Type=0x%04x, SourceID=%d, ClockSrc=%d",
         desc->bTerminalID, termType, desc->bSourceID, desc->bCSourceID);

    return true;
}

bool UsbDescriptorParser::parseUAC2FeatureUnit(const uint8_t* data, UsbAudioDevice& device) {
    if (data[0] < sizeof(UAC2FeatureUnitDescriptor)) {
        return false;
    }

    const auto* desc = reinterpret_cast<const UAC2FeatureUnitDescriptor*>(data);
    uint8_t descriptorLen = data[0];

    // UAC 2.0 Feature Unit format:
    // bLength (1) + bDescriptorType (1) + bDescriptorSubtype (1) +
    // bUnitID (1) + bSourceID (1) + bmaControls[] (4 bytes each) + iFeature (1)
    // Minimum length = 6 + 4 (at least master controls) + 1 = 11 bytes

    if (descriptorLen < 11) {
        LOGW("UAC 2.0 Feature Unit descriptor too short: %d bytes", descriptorLen);
        return false;
    }

    // Calculate number of control entries (master + N channels)
    // Length = 6 + (numChannels + 1) * 4 + 1
    uint8_t numControlEntries = (descriptorLen - 6 - 1) / 4;
    if (numControlEntries == 0) {
        LOGW("UAC 2.0 Feature Unit has no control entries");
        return false;
    }

    UsbFeatureUnit featureUnit;
    featureUnit.unitId = desc->bUnitID;
    featureUnit.sourceId = desc->bSourceID;
    featureUnit.numChannels = numControlEntries > 0 ? numControlEntries - 1 : 0;

    // Parse 4-byte control bitmaps for each channel
    // Controls start after bSourceID (offset 5)
    const uint8_t* controlData = data + 5;

    for (uint8_t i = 0; i < numControlEntries; ++i) {
        uint32_t controls = 0;
        std::memcpy(&controls, controlData + i * 4, 4);
        featureUnit.channelControls.push_back(controls);
    }

    // Log control capabilities (UAC 2.0 uses 2-bit fields)
    bool hasMute = featureUnit.hasMuteControl(0, 2);
    bool hasVolume = featureUnit.hasVolumeControl(0, 2);

    LOGD("UAC 2.0 Feature Unit: ID=%d, SourceID=%d, Channels=%d, Mute=%d, Volume=%d",
         desc->bUnitID, desc->bSourceID, featureUnit.numChannels, hasMute, hasVolume);

    // Log per-channel controls if more than master
    if (featureUnit.numChannels > 0) {
        for (uint8_t ch = 0; ch <= featureUnit.numChannels; ++ch) {
            LOGD("  Channel %d controls: 0x%08x (Mute=%d, Volume=%d)",
                 ch, featureUnit.channelControls[ch],
                 featureUnit.hasMuteControl(ch, 2),
                 featureUnit.hasVolumeControl(ch, 2));
        }
    }

    device.featureUnits.push_back(featureUnit);
    return true;
}

bool UsbDescriptorParser::parseUAC2ASGeneral(const uint8_t* data, UsbStreamingInterface& streaming) {
    if (data[0] < sizeof(UAC2ASGeneralDescriptor)) {
        return false;
    }

    const auto* desc = reinterpret_cast<const UAC2ASGeneralDescriptor*>(data);

    streaming.terminalLink = desc->bTerminalLink;

    // Push a format entry; FORMAT_TYPE descriptor will fill subslotSize/bitRes.
    streaming.formats.emplace_back();
    auto& fmt = streaming.formats.back();
    fmt.formatType = desc->bFormatType;
    fmt.channels = desc->bNrChannels;

    // bmFormats is a 32-bit field in UAC 2.0
    uint32_t bmFormats;
    std::memcpy(&bmFormats, &desc->bmFormats, sizeof(bmFormats));

    // Check for PCM format (bit 0)
    if (bmFormats & 0x01) {
        fmt.formatTag = UAC_FORMAT_TYPE_I_PCM;
    }

    LOGD("UAC 2.0 AS General: TerminalLink=%d, FormatType=%d, Channels=%d, bmFormats=0x%08x",
         desc->bTerminalLink, desc->bFormatType, desc->bNrChannels, bmFormats);

    return true;
}

bool UsbDescriptorParser::parseUAC2FormatType(const uint8_t* data, UsbAudioFormat& format) {
    if (data[0] < sizeof(UAC2FormatTypeIDescriptor)) {
        return false;
    }

    const auto* desc = reinterpret_cast<const UAC2FormatTypeIDescriptor*>(data);

    format.formatType = desc->bFormatType;
    format.subframeSize = desc->bSubslotSize;
    format.bitResolution = desc->bBitResolution;

    LOGD("UAC 2.0 Format Type I: SubslotSize=%d, BitRes=%d",
         desc->bSubslotSize, desc->bBitResolution);

    // Note: In UAC 2.0, sample rates are obtained via Clock Source control requests,
    // not from the format descriptor. We'll need to query this separately.

    return true;
}

bool UsbDescriptorParser::parseUAC2AudioEndpoint(const uint8_t* data, UsbStreamingInterface& streaming) {
    if (data[0] < sizeof(UAC2AudioEndpointDescriptor)) {
        return false;
    }

    const auto* desc = reinterpret_cast<const UAC2AudioEndpointDescriptor*>(data);

    LOGD("UAC 2.0 Audio Endpoint: bmAttr=0x%02x, bmControls=0x%02x",
         desc->bmAttributes, desc->bmControls);

    return true;
}

bool UsbDescriptorParser::queryClockSourceSampleRates(uint8_t clockId, UsbAudioDevice& device) {
    // This would require a USB control transfer to query sample rates from clock source
    // For now, we'll populate common sample rates as a fallback
    // TODO: Implement actual USB control transfer query

    for (auto& cs : device.clockSources) {
        if (cs.clockId == clockId) {
            // Add common sample rates as fallback
            cs.sampleRates = {44100, 48000, 88200, 96000, 176400, 192000};
            cs.hasContinuousRates = false;
            LOGD("Clock Source %d: Using default sample rates (control transfer not implemented)",
                 clockId);
            return true;
        }
    }
    return false;
}

// ============================================================================
// Helper Methods
// ============================================================================

uint32_t UsbDescriptorParser::readSampleFrequency(const uint8_t* data) {
    // USB Audio sample frequencies are 3 bytes, little-endian
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16);
}

uint16_t UsbDescriptorParser::readUint16LE(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

bool UsbDescriptorParser::checkBounds(size_t offset, size_t size, size_t totalLength) const {
    return (offset + size) <= totalLength;
}

void UsbDescriptorParser::setError(const std::string& error) {
    mLastError = error;
    LOGE("Parse error: %s", error.c_str());
}

// ============================================================================
// UsbAudioFormat toString implementation
// ============================================================================

std::string UsbAudioFormat::toString() const {
    std::string result;

    // Format type
    switch (formatTag) {
        case UAC_FORMAT_TYPE_I_PCM:
            result = "PCM";
            break;
        case UAC_FORMAT_TYPE_I_PCM8:
            result = "PCM8";
            break;
        case UAC_FORMAT_TYPE_I_IEEE_FLOAT:
            result = "IEEE Float";
            break;
        case UAC_FORMAT_TYPE_I_ALAW:
            result = "A-Law";
            break;
        case UAC_FORMAT_TYPE_I_MULAW:
            result = "u-Law";
            break;
        default:
            result = "Unknown(0x" + std::to_string(formatTag) + ")";
            break;
    }

    // Add details
    result += " " + std::to_string(channels) + "ch";
    result += " " + std::to_string(bitResolution) + "bit";

    // Sample rates
    if (hasContinuousRates) {
        result += " " + std::to_string(minSampleRate) + "-" + std::to_string(maxSampleRate) + "Hz";
    } else if (!sampleRates.empty()) {
        result += " [";
        for (size_t i = 0; i < sampleRates.size(); i++) {
            if (i > 0) result += ", ";
            result += std::to_string(sampleRates[i]);
        }
        result += "]Hz";
    }

    return result;
}

} // namespace usb
} // namespace watermelon_audio
