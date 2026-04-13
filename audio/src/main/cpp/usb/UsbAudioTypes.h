/**
 * UsbAudioTypes.h
 *
 * Data types for USB Audio driver implementation.
 *
 * Contains structures for:
 * - USB device descriptors
 * - Audio streaming configuration
 * - Device state management
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace watermelon_audio {
namespace usb {

// ============================================================================
// Forward Declarations
// ============================================================================

struct UsbDeviceInfo;
struct UsbEndpointInfo;
struct UsbAudioFormat;
struct UsbStreamingInterface;
struct UsbAudioDevice;

// ============================================================================
// USB Device Information
// ============================================================================

/**
 * Basic USB device identification info.
 */
struct UsbDeviceInfo {
    uint16_t vendorId = 0;
    uint16_t productId = 0;
    uint16_t bcdDevice = 0;        // Device release number
    uint8_t deviceClass = 0;
    uint8_t deviceSubClass = 0;
    uint8_t deviceProtocol = 0;

    std::string manufacturer;
    std::string product;
    std::string serialNumber;

    // Android-specific
    int fileDescriptor = -1;       // From UsbDeviceConnection
    std::string usbfsPath;         // e.g., "/dev/bus/usb/001/002"

    bool isValid() const {
        return fileDescriptor >= 0 && vendorId != 0;
    }
};

// ============================================================================
// USB Endpoint Information
// ============================================================================

/**
 * USB endpoint configuration.
 *
 * `maxPacketSize` holds the raw wMaxPacketSize field as it appears in the
 * USB endpoint descriptor (16 bits, little-endian). In USB 2.0 high-speed
 * that field is multiplexed: bits 10:0 are the per-transaction byte size,
 * and bits 12:11 encode "additional transactions per microframe" (0, 1, or
 * 2 → 1, 2, or 3 transactions per polling slot). In USB 1.1 full-speed and
 * low-speed, bits 12:11 are always 0, so the raw value equals the effective
 * max. Use effectiveMaxBytesPerPacket() to get the decoded upper bound the
 * device can actually deliver per iso packet slot.
 */
struct UsbEndpointInfo {
    uint8_t address = 0;           // Endpoint address (with direction bit)
    uint8_t attributes = 0;        // Transfer type, sync type, usage type
    uint16_t maxPacketSize = 0;    // Raw wMaxPacketSize (multiplexed in HS)
    uint8_t interval = 0;          // Polling interval

    // Derived properties
    bool isInput() const { return (address & 0x80) != 0; }
    bool isOutput() const { return (address & 0x80) == 0; }
    uint8_t endpointNumber() const { return address & 0x0F; }

    // Transfer type
    bool isIsochronous() const { return (attributes & 0x03) == 0x01; }
    bool isBulk() const { return (attributes & 0x03) == 0x02; }

    // Sync type (for isochronous)
    bool isAsync() const { return ((attributes >> 2) & 0x03) == 0x01; }
    bool isAdaptive() const { return ((attributes >> 2) & 0x03) == 0x02; }
    bool isSynchronous() const { return ((attributes >> 2) & 0x03) == 0x03; }

    /**
     * Effective maximum bytes per iso packet slot, decoded from the raw
     * multiplexed wMaxPacketSize field.
     *
     * USB 2.0 §9.6.6:
     *   wMaxPacketSize:
     *     bits 10:0  — max packet size per transaction
     *     bits 12:11 — additional transactions per microframe (0..2)
     *     bits 15:13 — reserved, must be 0
     *
     * For a high-speed endpoint that declares (base=1024, additional=2),
     * libusb and the USB host controller treat each polling slot as being
     * able to carry up to 1024 * 3 = 3072 bytes. The iso packet buffer
     * allocation for such a slot must be sized accordingly, otherwise
     * device-sent bytes get truncated silently.
     *
     * Full-speed endpoints (bits 12:11 == 0) get the same value back as
     * the raw field, so this helper is safe to call unconditionally.
     */
    int effectiveMaxBytesPerPacket() const {
        const uint16_t base = static_cast<uint16_t>(maxPacketSize & 0x07FF);
        const uint16_t additional = static_cast<uint16_t>((maxPacketSize >> 11) & 0x03);
        return static_cast<int>(base) * static_cast<int>(additional + 1);
    }
};

/**
 * USB Audio feedback endpoint info (for async endpoints).
 */
struct UsbFeedbackEndpoint {
    UsbEndpointInfo endpoint;
    bool isImplicit = false;       // Implicit feedback from data endpoint
};

// ============================================================================
// Audio Format Information
// ============================================================================

/**
 * Supported audio format from UAC descriptor.
 */
struct UsbAudioFormat {
    uint8_t formatType = 0;        // UAC_FORMAT_TYPE_I, II, III
    uint16_t formatTag = 0;        // PCM, PCM8, IEEE_FLOAT, etc.
    uint8_t channels = 0;
    uint8_t bitResolution = 0;     // Bits per sample (16, 24, 32)
    uint8_t subframeSize = 0;      // Bytes per sample slot

    // Supported sample rates
    std::vector<int> sampleRates;
    bool hasContinuousRates = false;
    int minSampleRate = 0;
    int maxSampleRate = 0;

    // Utility methods
    int bytesPerFrame() const {
        return channels * subframeSize;
    }

    bool supportsSampleRate(int rate) const {
        if (hasContinuousRates) {
            return rate >= minSampleRate && rate <= maxSampleRate;
        }
        for (int sr : sampleRates) {
            if (sr == rate) return true;
        }
        return false;
    }

    std::string toString() const;
};

// ============================================================================
// Audio Streaming Interface
// ============================================================================

/**
 * Represents one alternate setting of an AudioStreaming interface.
 *
 * A single altsetting may declare multiple Format Type I descriptors
 * (e.g. S24_3LE + S32_LE). Stage 1 stored only a scalar `format`
 * which silently overwrote earlier entries. Stage 2 stores a vector.
 */
struct UsbStreamingInterface {
    uint8_t interfaceNumber = 0;
    uint8_t alternateSetting = 0;

    // Audio formats supported by this alt setting (may be >1)
    std::vector<UsbAudioFormat> formats;

    // Data endpoint
    UsbEndpointInfo dataEndpoint;

    // Feedback endpoint (optional, for async)
    std::optional<UsbFeedbackEndpoint> feedbackEndpoint;

    // Terminal ID this interface connects to
    uint8_t terminalLink = 0;

    // Is this a playback (output) or capture (input) interface?
    bool isPlayback() const { return dataEndpoint.isOutput(); }
    bool isCapture() const { return dataEndpoint.isInput(); }

    /**
     * Convenience accessor for the primary (first) format.
     * Most altsettings declare exactly one format; this avoids
     * having to write formats[0] everywhere.
     * Returns a default-constructed format if the vector is empty.
     */
    const UsbAudioFormat& primaryFormat() const {
        static const UsbAudioFormat empty{};
        return formats.empty() ? empty : formats[0];
    }
};

// ============================================================================
// UAC 2.0 Clock Source Information
// ============================================================================

/**
 * Clock source type for UAC 2.0 devices.
 */
enum class ClockSourceType : uint8_t {
    EXTERNAL = 0,              // External clock
    INTERNAL_FIXED = 1,        // Internal fixed clock
    INTERNAL_VARIABLE = 2,     // Internal variable clock
    INTERNAL_PROGRAMMABLE = 3  // Internal programmable clock
};

/**
 * UAC 2.0 Clock Source descriptor data.
 */
struct UsbClockSource {
    uint8_t clockId = 0;           // Clock source ID
    ClockSourceType type = ClockSourceType::INTERNAL_FIXED;
    bool syncedToSof = false;      // Synchronized to USB SOF
    bool canControlFrequency = false;
    bool hasValidityControl = false;
    uint8_t assocTerminal = 0;     // Associated terminal
    std::string name;              // Clock source name (from string descriptor)

    // Sample rates supported by this clock source
    std::vector<int> sampleRates;
    bool hasContinuousRates = false;
    int minSampleRate = 0;
    int maxSampleRate = 0;

    bool supportsSampleRate(int rate) const {
        if (hasContinuousRates) {
            return rate >= minSampleRate && rate <= maxSampleRate;
        }
        for (int sr : sampleRates) {
            if (sr == rate) return true;
        }
        return false;
    }
};

/**
 * UAC 2.0 Clock Selector descriptor data.
 */
struct UsbClockSelector {
    uint8_t clockId = 0;           // Clock selector ID
    std::vector<uint8_t> sourceIds; // IDs of clock sources this can select from
    bool canControlSelector = false;
};

/**
 * UAC 2.0 Clock Multiplier descriptor data.
 */
struct UsbClockMultiplier {
    uint8_t clockId = 0;           // Clock multiplier ID
    uint8_t sourceId = 0;          // Source clock ID
    bool canControlNumerator = false;
    bool canControlDenominator = false;
};

// ============================================================================
// Feature Unit (Volume/Mute Control)
// ============================================================================

/**
 * Feature Unit descriptor data for volume/mute control.
 *
 * Feature Units provide audio processing controls like volume, mute,
 * bass, treble, etc. The most common use is for volume and mute control.
 */
struct UsbFeatureUnit {
    uint8_t unitId = 0;            // Feature Unit ID
    uint8_t sourceId = 0;          // ID of the unit/terminal this FU gets input from

    // Control bitmap per channel: index 0 = master, 1+ = logical channels
    // For UAC 1.0: each entry is a byte (bits per control)
    // For UAC 2.0: each entry is 4 bytes (2 bits per control)
    std::vector<uint32_t> channelControls;

    // Number of logical channels (excluding master)
    uint8_t numChannels = 0;

    /**
     * Check if this Feature Unit has volume control for a channel.
     * @param channel Channel number (0 = master)
     * @param uacVersion UAC version (1 or 2)
     */
    bool hasVolumeControl(uint8_t channel = 0, uint8_t uacVersion = 1) const {
        if (channel >= channelControls.size()) return false;
        uint32_t controls = channelControls[channel];

        if (uacVersion == 2) {
            // UAC 2.0: bits 2-3 for volume (0x0C mask)
            return (controls & 0x0000000C) != 0;
        } else {
            // UAC 1.0: bit 1 for volume
            return (controls & (1 << 1)) != 0;
        }
    }

    /**
     * Check if this Feature Unit has mute control for a channel.
     * @param channel Channel number (0 = master)
     * @param uacVersion UAC version (1 or 2)
     */
    bool hasMuteControl(uint8_t channel = 0, uint8_t uacVersion = 1) const {
        if (channel >= channelControls.size()) return false;
        uint32_t controls = channelControls[channel];

        if (uacVersion == 2) {
            // UAC 2.0: bits 0-1 for mute (0x03 mask)
            return (controls & 0x00000003) != 0;
        } else {
            // UAC 1.0: bit 0 for mute
            return (controls & (1 << 0)) != 0;
        }
    }

    /**
     * Check if this FU has any volume control (master or any channel).
     */
    bool hasAnyVolumeControl(uint8_t uacVersion = 1) const {
        for (uint8_t ch = 0; ch < channelControls.size(); ++ch) {
            if (hasVolumeControl(ch, uacVersion)) return true;
        }
        return false;
    }

    /**
     * Check if this FU has any mute control.
     */
    bool hasAnyMuteControl(uint8_t uacVersion = 1) const {
        for (uint8_t ch = 0; ch < channelControls.size(); ++ch) {
            if (hasMuteControl(ch, uacVersion)) return true;
        }
        return false;
    }
};

// ============================================================================
// Audio Terminals (full records, stage 3)
// ============================================================================

/**
 * Parsed Input Terminal record.
 *
 * UAC 2.0 terminals carry a `bCSourceID` that points at a clock source/
 * selector/multiplier in the clock graph. Stage 1 dropped this on the
 * floor (only kept a bare id list); stage 3 needs it to route SET_CUR
 * sample rate requests to the correct clock.
 *
 * For UAC 1.0 devices `clockSourceId` is 0 — UAC1 has no explicit clock
 * graph and sample rate goes straight to the streaming endpoint.
 */
struct UsbInputTerminal {
    uint8_t  terminalId = 0;
    uint16_t terminalType = 0;
    uint8_t  clockSourceId = 0;   // UAC2 only
    uint8_t  numChannels = 0;
};

/**
 * Parsed Output Terminal record.
 */
struct UsbOutputTerminal {
    uint8_t  terminalId = 0;
    uint16_t terminalType = 0;
    uint8_t  sourceId = 0;        // data-path input (FU, mixer, etc)
    uint8_t  clockSourceId = 0;   // UAC2 only
};

// ============================================================================
// Complete USB Audio Device Representation
// ============================================================================

/**
 * Complete parsed representation of a USB Audio device.
 */
struct UsbAudioDevice {
    UsbDeviceInfo deviceInfo;

    // UAC version detected
    uint8_t uacVersion = 1;        // 1 = UAC 1.0, 2 = UAC 2.0

    // Audio Control interface info
    uint8_t controlInterface = 0;
    std::vector<uint8_t> inputTerminalIds;   // kept for legacy callers
    std::vector<uint8_t> outputTerminalIds;  // kept for legacy callers

    // Stage 3: full terminal records with clock-source linkage
    std::vector<UsbInputTerminal>  inputTerminals;
    std::vector<UsbOutputTerminal> outputTerminals;

    // UAC 2.0 clock topology
    std::vector<UsbClockSource> clockSources;
    std::vector<UsbClockSelector> clockSelectors;
    std::vector<UsbClockMultiplier> clockMultipliers;

    // Feature Units for volume/mute control
    std::vector<UsbFeatureUnit> featureUnits;

    // Streaming interfaces (one per alt setting with audio)
    std::vector<UsbStreamingInterface> playbackInterfaces;
    std::vector<UsbStreamingInterface> captureInterfaces;

    // Currently selected interfaces
    std::optional<UsbStreamingInterface> activePlayback;
    std::optional<UsbStreamingInterface> activeCapture;

    // Utility methods
    bool hasPlayback() const { return !playbackInterfaces.empty(); }
    bool hasCapture() const { return !captureInterfaces.empty(); }
    bool isFullDuplex() const { return hasPlayback() && hasCapture(); }

    // UAC 2.0 clock methods
    const UsbClockSource* findClockSource(uint8_t clockId) const {
        for (const auto& cs : clockSources) {
            if (cs.clockId == clockId) return &cs;
        }
        return nullptr;
    }

    /**
     * Resolve the clock source ID feeding a given streaming interface's
     * `terminalLink`. The terminalLink field of an AS General descriptor
     * points to the terminal attached to the streaming endpoint — for a
     * playback stream that's an Output Terminal (the DAC), for a capture
     * stream it's an Input Terminal (the mic/line-in).
     *
     * Returns 0 if the link can't be resolved (UAC 1.0, or mismatched topology).
     * In that case the caller should fall back to `clockSources.front()`.
     */
    uint8_t resolveClockSourceId(uint8_t terminalLinkId) const {
        if (terminalLinkId == 0) return 0;
        for (const auto& t : outputTerminals) {
            if (t.terminalId == terminalLinkId) return t.clockSourceId;
        }
        for (const auto& t : inputTerminals) {
            if (t.terminalId == terminalLinkId) return t.clockSourceId;
        }
        return 0;
    }

    // Feature Unit methods
    const UsbFeatureUnit* findFeatureUnit(uint8_t unitId) const {
        for (const auto& fu : featureUnits) {
            if (fu.unitId == unitId) return &fu;
        }
        return nullptr;
    }

    /**
     * Find a Feature Unit that controls an output terminal (for playback volume).
     * Returns the first FU with volume control that feeds into an output terminal.
     */
    const UsbFeatureUnit* findOutputVolumeFeatureUnit() const {
        for (const auto& fu : featureUnits) {
            if (fu.hasAnyVolumeControl(uacVersion)) {
                // Check if this FU is in the output path
                for (uint8_t termId : outputTerminalIds) {
                    // Simple heuristic: FU's unit ID is often numerically between
                    // the input terminal and output terminal it connects
                    // A more accurate approach would trace the full topology
                    if (fu.sourceId != 0) {
                        return &fu;
                    }
                }
            }
        }
        return featureUnits.empty() ? nullptr : &featureUnits[0];
    }

    /**
     * Find a Feature Unit for input/capture volume control.
     */
    const UsbFeatureUnit* findInputVolumeFeatureUnit() const {
        // For capture, the FU typically comes after the input terminal
        for (const auto& fu : featureUnits) {
            if (fu.hasAnyVolumeControl(uacVersion)) {
                for (uint8_t termId : inputTerminalIds) {
                    if (fu.sourceId == termId) {
                        return &fu;
                    }
                }
            }
        }
        // Fallback: return any FU with volume control
        for (const auto& fu : featureUnits) {
            if (fu.hasAnyVolumeControl(uacVersion)) {
                return &fu;
            }
        }
        return nullptr;
    }

    // Find best matching format
    std::optional<UsbStreamingInterface> findPlaybackFormat(
        int sampleRate, int channels, int bitDepth) const;
    std::optional<UsbStreamingInterface> findCaptureFormat(
        int sampleRate, int channels, int bitDepth) const;

    // ====== Aggregate capability helpers (stage 2) ======

    /** All distinct sample rates across all playback altsettings. */
    std::vector<int> playbackSampleRates() const {
        std::vector<int> rates;
        for (const auto& iface : playbackInterfaces) {
            for (const auto& fmt : iface.formats) {
                for (int r : fmt.sampleRates) {
                    if (std::find(rates.begin(), rates.end(), r) == rates.end())
                        rates.push_back(r);
                }
            }
        }
        std::sort(rates.begin(), rates.end());
        return rates;
    }

    /** All distinct bit depths across all playback altsettings. */
    std::vector<int> playbackBitDepths() const {
        std::vector<int> depths;
        for (const auto& iface : playbackInterfaces) {
            for (const auto& fmt : iface.formats) {
                int d = fmt.bitResolution;
                if (d > 0 && std::find(depths.begin(), depths.end(), d) == depths.end())
                    depths.push_back(d);
            }
        }
        std::sort(depths.begin(), depths.end());
        return depths;
    }

    /** All distinct sample rates across all capture altsettings. */
    std::vector<int> captureSampleRates() const {
        std::vector<int> rates;
        for (const auto& iface : captureInterfaces) {
            for (const auto& fmt : iface.formats) {
                for (int r : fmt.sampleRates) {
                    if (std::find(rates.begin(), rates.end(), r) == rates.end())
                        rates.push_back(r);
                }
            }
        }
        std::sort(rates.begin(), rates.end());
        return rates;
    }

    /** All distinct bit depths across all capture altsettings. */
    std::vector<int> captureBitDepths() const {
        std::vector<int> depths;
        for (const auto& iface : captureInterfaces) {
            for (const auto& fmt : iface.formats) {
                int d = fmt.bitResolution;
                if (d > 0 && std::find(depths.begin(), depths.end(), d) == depths.end())
                    depths.push_back(d);
            }
        }
        std::sort(depths.begin(), depths.end());
        return depths;
    }
};

// ============================================================================
// Device State
// ============================================================================

/**
 * USB Audio device connection state.
 */
enum class UsbDeviceState {
    DISCONNECTED,      // No device connected
    CONNECTED,         // Device connected, not configured
    CONFIGURED,        // Device configured, ready to stream
    STREAMING,         // Actively streaming audio
    ERROR              // Error state
};

/**
 * USB Audio error types.
 */
enum class UsbAudioError {
    NONE,
    DEVICE_NOT_FOUND,
    PERMISSION_DENIED,
    UNSUPPORTED_FORMAT,
    DESCRIPTOR_PARSE_ERROR,
    TRANSFER_ERROR,
    TIMEOUT,
    DEVICE_DISCONNECTED,
    LIBUSB_ERROR,
    INTERNAL_ERROR
};

/**
 * USB transfer statistics.
 */
struct UsbTransferStats {
    uint64_t packetsTransferred = 0;
    uint64_t bytesTransferred = 0;
    uint64_t underruns = 0;
    uint64_t overruns = 0;
    uint64_t errors = 0;

    // Timing stats
    double avgLatencyMs = 0.0;
    double minLatencyMs = 0.0;
    double maxLatencyMs = 0.0;
};

// ============================================================================
// Callbacks
// ============================================================================

/**
 * Callback for USB device events.
 */
using UsbDeviceCallback = std::function<void(UsbDeviceState state, UsbAudioError error)>;

/**
 * Callback for USB audio data (output playback).
 * @param buffer Pointer to audio buffer to fill
 * @param frames Number of frames requested
 * @return Number of frames actually written
 */
using UsbAudioOutputCallback = std::function<int(float* buffer, int frames)>;

/**
 * Callback for USB audio data (input capture).
 * @param buffer Pointer to captured audio data
 * @param frames Number of frames available
 */
using UsbAudioInputCallback = std::function<void(const float* buffer, int frames)>;

// ============================================================================
// Configuration
// ============================================================================

/**
 * USB Audio stream configuration.
 */
struct UsbStreamConfig {
    int sampleRate = 48000;
    int channels = 2;
    int bitDepth = 24;             // Preferred bit depth

    // Buffer configuration
    int framesPerBuffer = 256;     // Frames per USB packet
    int numBuffers = 8;            // Number of buffers in ring

    // Advanced options
    bool enableFeedback = true;    // Use feedback for async endpoints
    int clockRecoveryMode = 0;     // 0=auto, 1=feedback, 2=adaptive
};

} // namespace usb
} // namespace watermelon_audio
