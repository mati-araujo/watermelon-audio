/**
 * UsbDescriptorParser.h
 *
 * Parser for USB Audio Class (UAC) 1.0 and 2.0 descriptors.
 *
 * This parser extracts audio capabilities from USB device descriptors,
 * including supported sample rates, bit depths, channels, and endpoint
 * configurations.
 *
 * UAC 2.0 additions:
 * - Clock Source/Selector/Multiplier descriptors
 * - Enhanced terminal descriptors with clock references
 * - New format type descriptors
 *
 * References:
 * - USB Audio Class 1.0 Specification
 * - USB Audio Class 2.0 Specification
 * - Universal Serial Bus Specification 2.0
 */

#pragma once

#include "UsbConstants.h"
#include "UsbAudioTypes.h"
#include <cstdint>
#include <vector>
#include <optional>
#include <string>

namespace noisypad {
namespace usb {

/**
 * Parser for USB Audio Class descriptors.
 *
 * Usage:
 *   UsbDescriptorParser parser;
 *   auto result = parser.parse(descriptorData, descriptorLength);
 *   if (result.has_value()) {
 *       UsbAudioDevice device = result.value();
 *       // Use device capabilities...
 *   }
 */
class UsbDescriptorParser {
public:
    UsbDescriptorParser() = default;
    ~UsbDescriptorParser() = default;

    // Non-copyable, movable
    UsbDescriptorParser(const UsbDescriptorParser&) = delete;
    UsbDescriptorParser& operator=(const UsbDescriptorParser&) = delete;
    UsbDescriptorParser(UsbDescriptorParser&&) = default;
    UsbDescriptorParser& operator=(UsbDescriptorParser&&) = default;

    /**
     * Parse USB configuration descriptors.
     *
     * @param data Raw descriptor data from libusb_get_config_descriptor or similar
     * @param length Length of descriptor data in bytes
     * @param deviceInfo Basic device info (vendorId, productId, etc.)
     * @return Parsed UsbAudioDevice if successful, std::nullopt on error
     */
    std::optional<UsbAudioDevice> parse(
        const uint8_t* data,
        size_t length,
        const UsbDeviceInfo& deviceInfo);

    /**
     * Parse only to check if device is a USB Audio device.
     *
     * @param data Raw descriptor data
     * @param length Length of descriptor data
     * @return true if device has USB Audio Class interfaces
     */
    bool isAudioDevice(const uint8_t* data, size_t length);

    /**
     * Get the last parse error description.
     */
    const std::string& getLastError() const { return mLastError; }

    /**
     * Get UAC version detected in last parse (1 or 2).
     */
    uint8_t getDetectedUacVersion() const { return mDetectedUacVersion; }

private:
    // ========== Descriptor Header Structures ==========

    // Standard USB descriptor header
    struct DescriptorHeader {
        uint8_t bLength;
        uint8_t bDescriptorType;
    } __attribute__((packed));

    // Configuration descriptor
    struct ConfigDescriptor {
        uint8_t  bLength;
        uint8_t  bDescriptorType;
        uint16_t wTotalLength;
        uint8_t  bNumInterfaces;
        uint8_t  bConfigurationValue;
        uint8_t  iConfiguration;
        uint8_t  bmAttributes;
        uint8_t  bMaxPower;
    } __attribute__((packed));

    // Interface descriptor
    struct InterfaceDescriptor {
        uint8_t bLength;
        uint8_t bDescriptorType;
        uint8_t bInterfaceNumber;
        uint8_t bAlternateSetting;
        uint8_t bNumEndpoints;
        uint8_t bInterfaceClass;
        uint8_t bInterfaceSubClass;
        uint8_t bInterfaceProtocol;
        uint8_t iInterface;
    } __attribute__((packed));

    // Endpoint descriptor
    struct EndpointDescriptor {
        uint8_t  bLength;
        uint8_t  bDescriptorType;
        uint8_t  bEndpointAddress;
        uint8_t  bmAttributes;
        uint16_t wMaxPacketSize;
        uint8_t  bInterval;
    } __attribute__((packed));

    // ========== UAC 1.0 Class-Specific Descriptors ==========

    // Audio Control Interface Header (UAC 1.0 Table 4-2)
    struct ACHeaderDescriptor {
        uint8_t  bLength;
        uint8_t  bDescriptorType;
        uint8_t  bDescriptorSubtype;
        uint16_t bcdADC;              // Audio Device Class spec version
        uint16_t wTotalLength;
        uint8_t  bInCollection;       // Number of AudioStreaming interfaces
        // Followed by bInCollection bytes of interface numbers
    } __attribute__((packed));

    // Input Terminal Descriptor (UAC 1.0 Table 4-3)
    struct InputTerminalDescriptor {
        uint8_t  bLength;
        uint8_t  bDescriptorType;
        uint8_t  bDescriptorSubtype;
        uint8_t  bTerminalID;
        uint16_t wTerminalType;
        uint8_t  bAssocTerminal;
        uint8_t  bNrChannels;
        uint16_t wChannelConfig;
        uint8_t  iChannelNames;
        uint8_t  iTerminal;
    } __attribute__((packed));

    // Output Terminal Descriptor (UAC 1.0 Table 4-4)
    struct OutputTerminalDescriptor {
        uint8_t  bLength;
        uint8_t  bDescriptorType;
        uint8_t  bDescriptorSubtype;
        uint8_t  bTerminalID;
        uint16_t wTerminalType;
        uint8_t  bAssocTerminal;
        uint8_t  bSourceID;
        uint8_t  iTerminal;
    } __attribute__((packed));

    // Feature Unit Descriptor (UAC 1.0 Table 4-7)
    // Note: Variable length, bmaControls has (ch+1) entries
    struct FeatureUnitDescriptor {
        uint8_t bLength;
        uint8_t bDescriptorType;
        uint8_t bDescriptorSubtype;
        uint8_t bUnitID;
        uint8_t bSourceID;
        uint8_t bControlSize;
        // Followed by (bControlSize * (bNrChannels + 1)) bytes of controls
        // Followed by iFeature string index
    } __attribute__((packed));

    // Audio Streaming Interface Descriptor (UAC 1.0 Table 4-19)
    struct ASGeneralDescriptor {
        uint8_t  bLength;
        uint8_t  bDescriptorType;
        uint8_t  bDescriptorSubtype;
        uint8_t  bTerminalLink;
        uint8_t  bDelay;
        uint16_t wFormatTag;
    } __attribute__((packed));

    // Format Type I Descriptor (UAC 1.0 Table 2-1)
    struct FormatTypeIDescriptor {
        uint8_t bLength;
        uint8_t bDescriptorType;
        uint8_t bDescriptorSubtype;
        uint8_t bFormatType;
        uint8_t bNrChannels;
        uint8_t bSubframeSize;
        uint8_t bBitResolution;
        uint8_t bSamFreqType;  // Number of sample rates (0 = continuous)
        // Followed by either:
        // - If bSamFreqType == 0: tLowerSamFreq (3 bytes) + tUpperSamFreq (3 bytes)
        // - If bSamFreqType > 0: bSamFreqType * tSamFreq (3 bytes each)
    } __attribute__((packed));

    // Audio Endpoint Descriptor (UAC 1.0 Table 4-21)
    struct AudioEndpointDescriptor {
        uint8_t bLength;
        uint8_t bDescriptorType;
        uint8_t bDescriptorSubtype;
        uint8_t bmAttributes;
        uint8_t bLockDelayUnits;
        uint16_t wLockDelay;
    } __attribute__((packed));

    // ========== UAC 2.0 Class-Specific Descriptors ==========

    // Clock Source Descriptor (UAC 2.0 Table 4-6)
    struct UAC2ClockSourceDescriptor {
        uint8_t  bLength;
        uint8_t  bDescriptorType;
        uint8_t  bDescriptorSubtype;
        uint8_t  bClockID;
        uint8_t  bmAttributes;       // Clock type and sync to SOF
        uint8_t  bmControls;         // Clock frequency and validity controls
        uint8_t  bAssocTerminal;
        uint8_t  iClockSource;
    } __attribute__((packed));

    // Clock Selector Descriptor (UAC 2.0 Table 4-7)
    struct UAC2ClockSelectorDescriptor {
        uint8_t  bLength;
        uint8_t  bDescriptorType;
        uint8_t  bDescriptorSubtype;
        uint8_t  bClockID;
        uint8_t  bNrInPins;
        // Followed by bNrInPins bytes of baCSourceID
        // Followed by bmControls (1 byte)
        // Followed by iClockSelector (1 byte)
    } __attribute__((packed));

    // Clock Multiplier Descriptor (UAC 2.0 Table 4-8)
    struct UAC2ClockMultiplierDescriptor {
        uint8_t  bLength;
        uint8_t  bDescriptorType;
        uint8_t  bDescriptorSubtype;
        uint8_t  bClockID;
        uint8_t  bCSourceID;
        uint8_t  bmControls;
        uint8_t  iClockMultiplier;
    } __attribute__((packed));

    // Input Terminal Descriptor (UAC 2.0 Table 4-9)
    struct UAC2InputTerminalDescriptor {
        uint8_t  bLength;
        uint8_t  bDescriptorType;
        uint8_t  bDescriptorSubtype;
        uint8_t  bTerminalID;
        uint16_t wTerminalType;
        uint8_t  bAssocTerminal;
        uint8_t  bCSourceID;        // Clock source ID (UAC 2.0 specific)
        uint8_t  bNrChannels;
        uint32_t bmChannelConfig;   // 32-bit channel config (vs 16-bit in UAC 1.0)
        uint8_t  iChannelNames;
        uint16_t bmControls;
        uint8_t  iTerminal;
    } __attribute__((packed));

    // Output Terminal Descriptor (UAC 2.0 Table 4-10)
    struct UAC2OutputTerminalDescriptor {
        uint8_t  bLength;
        uint8_t  bDescriptorType;
        uint8_t  bDescriptorSubtype;
        uint8_t  bTerminalID;
        uint16_t wTerminalType;
        uint8_t  bAssocTerminal;
        uint8_t  bSourceID;
        uint8_t  bCSourceID;        // Clock source ID (UAC 2.0 specific)
        uint16_t bmControls;
        uint8_t  iTerminal;
    } __attribute__((packed));

    // Feature Unit Descriptor (UAC 2.0 Table 4-13)
    // Note: Variable length, bmaControls are 4 bytes per channel in UAC 2.0
    struct UAC2FeatureUnitDescriptor {
        uint8_t  bLength;
        uint8_t  bDescriptorType;
        uint8_t  bDescriptorSubtype;
        uint8_t  bUnitID;
        uint8_t  bSourceID;
        // Followed by bmaControls (4 bytes per channel + 1 for master)
        // Followed by iFeature string index
    } __attribute__((packed));

    // Audio Streaming Interface Descriptor (UAC 2.0 Table 4-27)
    struct UAC2ASGeneralDescriptor {
        uint8_t  bLength;
        uint8_t  bDescriptorType;
        uint8_t  bDescriptorSubtype;
        uint8_t  bTerminalLink;
        uint8_t  bmControls;
        uint8_t  bFormatType;
        uint32_t bmFormats;         // 32-bit format bitfield
        uint8_t  bNrChannels;
        uint32_t bmChannelConfig;
        uint8_t  iChannelNames;
    } __attribute__((packed));

    // Format Type I Descriptor (UAC 2.0 Table 2-2)
    struct UAC2FormatTypeIDescriptor {
        uint8_t  bLength;
        uint8_t  bDescriptorType;
        uint8_t  bDescriptorSubtype;
        uint8_t  bFormatType;
        uint8_t  bSubslotSize;      // Renamed from bSubframeSize in UAC 1.0
        uint8_t  bBitResolution;
        // Note: Sample rates are obtained via Clock Source in UAC 2.0
    } __attribute__((packed));

    // Audio Data Endpoint Descriptor (UAC 2.0 Table 4-34)
    struct UAC2AudioEndpointDescriptor {
        uint8_t  bLength;
        uint8_t  bDescriptorType;
        uint8_t  bDescriptorSubtype;
        uint8_t  bmAttributes;
        uint8_t  bmControls;
        uint8_t  bLockDelayUnits;
        uint16_t wLockDelay;
    } __attribute__((packed));

    // ========== Parsing Methods ==========

    // Main parsing entry point after validation
    bool parseDescriptors(const uint8_t* data, size_t length, UsbAudioDevice& device);

    // Parse different descriptor types
    bool parseConfigDescriptor(const uint8_t* data, size_t length, UsbAudioDevice& device);
    bool parseInterfaceDescriptor(const uint8_t* data, size_t remaining, UsbAudioDevice& device);
    bool parseAudioControlInterface(const uint8_t* data, size_t remaining, UsbAudioDevice& device);
    bool parseAudioStreamingInterface(const uint8_t* data, size_t remaining,
                                      UsbStreamingInterface& streaming);
    bool parseEndpointDescriptor(const uint8_t* data, UsbEndpointInfo& endpoint);

    // Parse UAC 1.0 class-specific descriptors
    bool parseACHeader(const uint8_t* data, UsbAudioDevice& device);
    bool parseInputTerminal(const uint8_t* data, UsbAudioDevice& device);
    bool parseOutputTerminal(const uint8_t* data, UsbAudioDevice& device);
    bool parseFeatureUnit(const uint8_t* data, UsbAudioDevice& device);
    bool parseASGeneral(const uint8_t* data, UsbStreamingInterface& streaming);
    bool parseFormatType(const uint8_t* data, UsbAudioFormat& format);
    bool parseAudioEndpoint(const uint8_t* data, UsbStreamingInterface& streaming);

    // Parse UAC 2.0 class-specific descriptors
    bool parseUAC2ACHeader(const uint8_t* data, UsbAudioDevice& device);
    bool parseUAC2ClockSource(const uint8_t* data, UsbAudioDevice& device);
    bool parseUAC2ClockSelector(const uint8_t* data, UsbAudioDevice& device);
    bool parseUAC2ClockMultiplier(const uint8_t* data, UsbAudioDevice& device);
    bool parseUAC2InputTerminal(const uint8_t* data, UsbAudioDevice& device);
    bool parseUAC2OutputTerminal(const uint8_t* data, UsbAudioDevice& device);
    bool parseUAC2FeatureUnit(const uint8_t* data, UsbAudioDevice& device);
    bool parseUAC2ASGeneral(const uint8_t* data, UsbStreamingInterface& streaming);
    bool parseUAC2FormatType(const uint8_t* data, UsbAudioFormat& format);
    bool parseUAC2AudioEndpoint(const uint8_t* data, UsbStreamingInterface& streaming);

    // Query clock source sample rates via control request
    bool queryClockSourceSampleRates(uint8_t clockId, UsbAudioDevice& device);

    // ========== Helper Methods ==========

    // Read a 3-byte sample frequency value (little-endian)
    static uint32_t readSampleFrequency(const uint8_t* data);

    // Read 16-bit little-endian value
    static uint16_t readUint16LE(const uint8_t* data);

    // Validate descriptor bounds
    bool checkBounds(size_t offset, size_t size, size_t totalLength) const;

    // Set error message
    void setError(const std::string& error);

    // ========== State ==========

    std::string mLastError;
    uint8_t mDetectedUacVersion = 1;

    // Parsing context
    struct ParsingContext {
        uint8_t currentInterfaceNumber = 0;
        uint8_t currentAlternateSetting = 0;
        uint8_t currentInterfaceClass = 0;
        uint8_t currentInterfaceSubClass = 0;
        bool inAudioControl = false;
        bool inAudioStreaming = false;
        UsbStreamingInterface currentStreaming;
    };

    ParsingContext mContext;
};

} // namespace usb
} // namespace noisypad
