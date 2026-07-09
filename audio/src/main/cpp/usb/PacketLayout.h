#pragma once

/**
 * PacketLayout.h
 *
 * Pure, host-testable extraction of the ISO output packet sizing math from
 * UsbTransferManager::fillOutputTransfer. Given the clock-adjusted frame count
 * for a packet, it derives the per-packet sample/byte counts and clamps them to
 * the slot the kernel reserved at allocation time.
 *
 * Why this matters: linux_usbfs walks the transfer buffer by SUMMING the
 * per-packet iso_packet_desc[].length values, so a byte length that exceeds the
 * reserved slot makes the kernel compute offsets past our allocation — every
 * packet but the first then reads stale/adjacent memory and the stream decodes
 * as brutal distortion on real devices. The clamp is a correctness invariant,
 * not a nicety; extracting it lets the host suite pin it (Etapa 5).
 *
 * Bit-identical to the previous inline code; the only non-pure input
 * (getAdjustedFrameCount, from the clock controller) stays at the call site.
 */

#include <cstddef>

namespace watermelon_audio {
namespace usb {

struct OutputPacketLayout {
    int         adjustedFrames;    // possibly reduced by the slot clamp
    int         samplesPerPacket;  // adjustedFrames * channelCount (post-clamp)
    int         bytesPerPacket;    // <= slotBytes
    std::size_t samplesNeeded;     // samplesPerPacket * packetCount
};

/**
 * Derive the output packet layout for one ISO transfer.
 *
 * @param adjustedFrames clock-adjusted frames for this packet (may exceed nominal)
 * @param channelCount   output channels (>= 1)
 * @param bytesPerSample bytes per PCM sample (>= 1)
 * @param slotBytes      per-packet slot reserved at allocation (the ceiling)
 * @param packetCount    packets in this transfer
 */
inline OutputPacketLayout computeOutputPacketLayout(
        int adjustedFrames, int channelCount, int bytesPerSample,
        int slotBytes, int packetCount) {
    int samplesPerPacket = adjustedFrames * channelCount;
    int bytesPerPacket = samplesPerPacket * bytesPerSample;
    if (bytesPerPacket > slotBytes) {
        // Clamp to the allocated headroom and re-derive so the kernel's offsets
        // (which sum lengths) stay inside our allocation.
        bytesPerPacket = slotBytes;
        samplesPerPacket = bytesPerPacket / bytesPerSample;
        adjustedFrames = samplesPerPacket / channelCount;
    }
    const std::size_t samplesNeeded =
        static_cast<std::size_t>(samplesPerPacket * packetCount);
    return {adjustedFrames, samplesPerPacket, bytesPerPacket, samplesNeeded};
}

}  // namespace usb
}  // namespace watermelon_audio
