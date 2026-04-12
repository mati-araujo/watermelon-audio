/**
 * UsbSnapshotCodec.h
 *
 * Binary serialization of a UsbAudioDevice topology into a compact,
 * versionable byte array suitable for passing across JNI as a ByteArray.
 *
 * The Kotlin decoder lives in commonMain UsbSnapshotCodec.kt and mirrors
 * this format exactly. Both sides share the same v1 wire layout.
 *
 * Format v1 overview:
 *   [0]       version byte (0x01)
 *   [1..4]    total length (u32 LE, including header)
 *   [5..6]    vendorId (u16 LE)
 *   [7..8]    productId (u16 LE)
 *   [9]       uacVersion (u8)
 *   [10..]    length-prefixed UTF-8 strings (product, manufacturer, serial)
 *   ...       playback altsettings
 *   ...       capture altsettings
 *   ...       clock sources
 *   ...       feature units
 *
 * Stage 2 — USB Audio Discovery & Directed Selection.
 */

#pragma once

#include "UsbAudioTypes.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace watermelon_audio {
namespace usb {

static constexpr uint8_t SNAPSHOT_FORMAT_VERSION = 0x01;

/**
 * Encode a UsbAudioDevice into a binary snapshot.
 */
inline std::vector<uint8_t> encodeSnapshot(const UsbAudioDevice& device) {
    std::vector<uint8_t> buf;
    buf.reserve(512);  // typical size

    // ===== Helpers =====
    auto writeU8 = [&](uint8_t v) { buf.push_back(v); };
    auto writeU16 = [&](uint16_t v) {
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };
    auto writeU32 = [&](uint32_t v) {
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };
    auto writeString = [&](const std::string& s) {
        uint16_t len = static_cast<uint16_t>(std::min(s.size(), size_t(65535)));
        writeU16(len);
        buf.insert(buf.end(), s.data(), s.data() + len);
    };

    auto writeFormat = [&](const UsbAudioFormat& fmt) {
        writeU8(fmt.channels);
        writeU8(fmt.bitResolution);
        writeU8(fmt.subframeSize);
        writeU8(fmt.hasContinuousRates ? 1 : 0);
        writeU32(static_cast<uint32_t>(fmt.minSampleRate));
        writeU32(static_cast<uint32_t>(fmt.maxSampleRate));
        writeU8(static_cast<uint8_t>(std::min(fmt.sampleRates.size(), size_t(255))));
        for (size_t i = 0; i < fmt.sampleRates.size() && i < 255; ++i) {
            writeU32(static_cast<uint32_t>(fmt.sampleRates[i]));
        }
    };

    auto writeAltsetting = [&](const UsbStreamingInterface& alt) {
        writeU8(alt.interfaceNumber);
        writeU8(alt.alternateSetting);
        // Sync type from endpoint attributes
        uint8_t syncType = (alt.dataEndpoint.attributes >> 2) & 0x03;
        writeU8(syncType);
        // Flags: bit0 = hasFeedback, bit1 = implicitFeedback
        uint8_t flags = 0;
        if (alt.feedbackEndpoint.has_value()) flags |= 0x01;
        if (alt.feedbackEndpoint.has_value() && alt.feedbackEndpoint->isImplicit) flags |= 0x02;
        writeU8(flags);
        writeU8(alt.dataEndpoint.address);
        writeU8(alt.terminalLink);
        // Formats
        writeU8(static_cast<uint8_t>(std::min(alt.formats.size(), size_t(255))));
        for (const auto& fmt : alt.formats) {
            writeFormat(fmt);
        }
    };

    // ===== Header =====
    writeU8(SNAPSHOT_FORMAT_VERSION);
    // Placeholder for total length (will patch at end)
    size_t lengthOffset = buf.size();
    writeU32(0);

    // ===== Device info =====
    writeU16(device.deviceInfo.vendorId);
    writeU16(device.deviceInfo.productId);
    writeU8(device.uacVersion);
    writeString(device.deviceInfo.product);
    writeString(device.deviceInfo.manufacturer);
    writeString(device.deviceInfo.serialNumber);

    // ===== Playback altsettings =====
    writeU16(static_cast<uint16_t>(device.playbackInterfaces.size()));
    for (const auto& alt : device.playbackInterfaces) {
        writeAltsetting(alt);
    }

    // ===== Capture altsettings =====
    writeU16(static_cast<uint16_t>(device.captureInterfaces.size()));
    for (const auto& alt : device.captureInterfaces) {
        writeAltsetting(alt);
    }

    // ===== Clock sources =====
    writeU8(static_cast<uint8_t>(std::min(device.clockSources.size(), size_t(255))));
    for (const auto& cs : device.clockSources) {
        writeU8(cs.clockId);
        writeU8(static_cast<uint8_t>(cs.type));
        writeU8(cs.syncedToSof ? 1 : 0);
        writeU8(cs.canControlFrequency ? 1 : 0);
        writeU8(cs.hasValidityControl ? 1 : 0);
    }

    // ===== Feature units =====
    writeU8(static_cast<uint8_t>(std::min(device.featureUnits.size(), size_t(255))));
    for (const auto& fu : device.featureUnits) {
        writeU8(fu.unitId);
        writeU8(fu.sourceId);
        writeU8(fu.numChannels);
        // Master volume/mute flags (using UAC version from device)
        uint8_t masterFlags = 0;
        if (fu.hasVolumeControl(0, device.uacVersion)) masterFlags |= 0x01;
        if (fu.hasMuteControl(0, device.uacVersion)) masterFlags |= 0x02;
        writeU8(masterFlags);
        // Per-channel volume flags (packed as bytes, one per channel)
        writeU8(fu.numChannels);
        for (uint8_t ch = 1; ch <= fu.numChannels; ++ch) {
            uint8_t chFlags = 0;
            if (fu.hasVolumeControl(ch, device.uacVersion)) chFlags |= 0x01;
            if (fu.hasMuteControl(ch, device.uacVersion)) chFlags |= 0x02;
            writeU8(chFlags);
        }
    }

    // ===== Patch total length =====
    uint32_t totalLength = static_cast<uint32_t>(buf.size());
    buf[lengthOffset + 0] = static_cast<uint8_t>(totalLength & 0xFF);
    buf[lengthOffset + 1] = static_cast<uint8_t>((totalLength >> 8) & 0xFF);
    buf[lengthOffset + 2] = static_cast<uint8_t>((totalLength >> 16) & 0xFF);
    buf[lengthOffset + 3] = static_cast<uint8_t>((totalLength >> 24) & 0xFF);

    return buf;
}

/**
 * Decode a binary snapshot back into a UsbAudioDevice.
 * Used for C++ round-trip testing. The Kotlin decoder is separate.
 * Returns nullopt if the buffer is malformed.
 */
inline std::optional<UsbAudioDevice> decodeSnapshot(const uint8_t* data, size_t length) {
    if (length < 10) return std::nullopt;

    size_t pos = 0;

    auto readU8 = [&]() -> uint8_t {
        if (pos >= length) return 0;
        return data[pos++];
    };
    auto readU16 = [&]() -> uint16_t {
        if (pos + 2 > length) { pos = length; return 0; }
        uint16_t v = static_cast<uint16_t>(data[pos])
                   | (static_cast<uint16_t>(data[pos + 1]) << 8);
        pos += 2;
        return v;
    };
    auto readU32 = [&]() -> uint32_t {
        if (pos + 4 > length) { pos = length; return 0; }
        uint32_t v = static_cast<uint32_t>(data[pos])
                   | (static_cast<uint32_t>(data[pos + 1]) << 8)
                   | (static_cast<uint32_t>(data[pos + 2]) << 16)
                   | (static_cast<uint32_t>(data[pos + 3]) << 24);
        pos += 4;
        return v;
    };
    auto readString = [&]() -> std::string {
        uint16_t len = readU16();
        if (pos + len > length) { pos = length; return {}; }
        std::string s(reinterpret_cast<const char*>(data + pos), len);
        pos += len;
        return s;
    };

    auto readFormat = [&]() -> UsbAudioFormat {
        UsbAudioFormat fmt;
        fmt.channels = readU8();
        fmt.bitResolution = readU8();
        fmt.subframeSize = readU8();
        fmt.hasContinuousRates = readU8() != 0;
        fmt.minSampleRate = static_cast<int>(readU32());
        fmt.maxSampleRate = static_cast<int>(readU32());
        uint8_t numRates = readU8();
        for (int i = 0; i < numRates; ++i) {
            fmt.sampleRates.push_back(static_cast<int>(readU32()));
        }
        return fmt;
    };

    auto readAltsetting = [&]() -> UsbStreamingInterface {
        UsbStreamingInterface alt;
        alt.interfaceNumber = readU8();
        alt.alternateSetting = readU8();
        uint8_t syncType = readU8();
        // Reconstruct endpoint attributes (sync type in bits 3:2)
        alt.dataEndpoint.attributes = 0x01 | (syncType << 2);  // isochronous + sync
        uint8_t flags = readU8();
        alt.dataEndpoint.address = readU8();
        alt.terminalLink = readU8();
        if (flags & 0x01) {
            UsbFeedbackEndpoint fb;
            fb.isImplicit = (flags & 0x02) != 0;
            alt.feedbackEndpoint = fb;
        }
        uint8_t numFormats = readU8();
        for (int i = 0; i < numFormats; ++i) {
            alt.formats.push_back(readFormat());
        }
        return alt;
    };

    // Version
    uint8_t version = readU8();
    if (version != SNAPSHOT_FORMAT_VERSION) return std::nullopt;

    // Total length (skip, we use actual buffer length)
    readU32();

    UsbAudioDevice device;
    device.deviceInfo.vendorId = readU16();
    device.deviceInfo.productId = readU16();
    device.uacVersion = readU8();
    device.deviceInfo.product = readString();
    device.deviceInfo.manufacturer = readString();
    device.deviceInfo.serialNumber = readString();

    // Playback
    uint16_t numPlayback = readU16();
    for (int i = 0; i < numPlayback && pos < length; ++i) {
        device.playbackInterfaces.push_back(readAltsetting());
    }

    // Capture
    uint16_t numCapture = readU16();
    for (int i = 0; i < numCapture && pos < length; ++i) {
        device.captureInterfaces.push_back(readAltsetting());
    }

    // Clock sources
    uint8_t numClocks = readU8();
    for (int i = 0; i < numClocks && pos < length; ++i) {
        UsbClockSource cs;
        cs.clockId = readU8();
        cs.type = static_cast<ClockSourceType>(readU8());
        cs.syncedToSof = readU8() != 0;
        cs.canControlFrequency = readU8() != 0;
        cs.hasValidityControl = readU8() != 0;
        device.clockSources.push_back(cs);
    }

    // Feature units
    uint8_t numFUs = readU8();
    for (int i = 0; i < numFUs && pos < length; ++i) {
        UsbFeatureUnit fu;
        fu.unitId = readU8();
        fu.sourceId = readU8();
        fu.numChannels = readU8();
        readU8();  // masterFlags (encoded; we don't re-derive channelControls)
        uint8_t numCh = readU8();
        for (int ch = 0; ch < numCh; ++ch) {
            readU8();  // perChannelFlags
        }
        device.featureUnits.push_back(fu);
    }

    if (pos > length) return std::nullopt;
    return device;
}

}  // namespace usb
}  // namespace watermelon_audio
