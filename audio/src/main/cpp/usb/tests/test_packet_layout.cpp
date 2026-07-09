/**
 * test_packet_layout.cpp — pins usb::computeOutputPacketLayout, the ISO output
 * packet sizing + slot clamp extracted from UsbTransferManager::fillOutputTransfer.
 * The clamp is a correctness invariant (a byte length past the reserved slot makes
 * the linux_usbfs kernel walk offsets out of our allocation -> distortion), so its
 * exact integer arithmetic — including truncating re-derivation — is frozen here.
 */

#include <gtest/gtest.h>

#include "usb/PacketLayout.h"

using namespace watermelon_audio::usb;

namespace {

TEST(PacketLayout, NoClampWhenWithinSlot) {
    // 48 frames * 2ch * 2 bytes (S16) = 192 bytes < slot.
    const auto l = computeOutputPacketLayout(
        /*adjustedFrames*/ 48, /*channelCount*/ 2, /*bytesPerSample*/ 2,
        /*slotBytes*/ 100000, /*packetCount*/ 8);
    EXPECT_EQ(l.adjustedFrames, 48);
    EXPECT_EQ(l.samplesPerPacket, 96);
    EXPECT_EQ(l.bytesPerPacket, 192);
    EXPECT_EQ(l.samplesNeeded, static_cast<size_t>(96 * 8));
}

TEST(PacketLayout, ClampsAndReDerivesWhenOverSlot) {
    // 100 frames * 2ch * 3 bytes (S24) = 600 bytes > slot 192.
    const auto l = computeOutputPacketLayout(100, 2, 3, 192, 4);
    EXPECT_EQ(l.bytesPerPacket, 192);
    EXPECT_EQ(l.samplesPerPacket, 64);       // 192 / 3
    EXPECT_EQ(l.adjustedFrames, 32);         // 64 / 2
    EXPECT_EQ(l.samplesNeeded, static_cast<size_t>(64 * 4));
}

TEST(PacketLayout, ExactBoundaryDoesNotClamp) {
    // bytes == slot (192) is not > slot, so no clamp / re-derive.
    const auto l = computeOutputPacketLayout(32, 2, 3, 192, 2);
    EXPECT_EQ(l.adjustedFrames, 32);
    EXPECT_EQ(l.samplesPerPacket, 64);
    EXPECT_EQ(l.bytesPerPacket, 192);
    EXPECT_EQ(l.samplesNeeded, static_cast<size_t>(64 * 2));
}

TEST(PacketLayout, ClampReDeriveTruncatesTowardZero) {
    // slot 190 not divisible by bytesPerSample/channels -> integer truncation
    // must be preserved exactly (matches the original inline math).
    const auto l = computeOutputPacketLayout(100, 2, 3, 190, 5);
    EXPECT_EQ(l.bytesPerPacket, 190);
    EXPECT_EQ(l.samplesPerPacket, 63);       // 190 / 3 = 63 (trunc)
    EXPECT_EQ(l.adjustedFrames, 31);         // 63 / 2 = 31 (trunc)
    EXPECT_EQ(l.samplesNeeded, static_cast<size_t>(63 * 5));
}

}  // namespace
