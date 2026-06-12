// Fase 0.5 — Host-side latency math (hallazgo L7)

#include <gtest/gtest.h>

#include "../UsbLatencyMath.h"

using watermelon_audio::usb::computeOutputLatencyMs;
using watermelon_audio::usb::computeInputLatencyMs;

TEST(UsbLatencyMathTest, OutputFullSpeedTypical) {
    // 48 kHz stereo, ring holds ~32 ms (1536 frames * 2 ch = 3072 samples),
    // 3 transfers in flight of 8 packets * 48 frames = 384 frames each.
    // ring: 1536 frames = 32 ms; inflight: 3 * 384 * 0.5 = 576 frames = 12 ms.
    const float ms = computeOutputLatencyMs(
        /*ringSamples=*/3072, /*channels=*/2, /*pending=*/3,
        /*framesPerTransfer=*/384, /*sampleRate=*/48000);
    EXPECT_NEAR(ms, 32.0f + 12.0f, 0.01f);
}

TEST(UsbLatencyMathTest, OutputNoPendingIsJustRing) {
    const float ms = computeOutputLatencyMs(960, 2, 0, 384, 48000);
    EXPECT_NEAR(ms, 10.0f, 0.01f);  // 480 frames / 48000 * 1000
}

TEST(UsbLatencyMathTest, InputTypical) {
    // 480 frames ring (10 ms) + half a 384-frame transfer (4 ms).
    const float ms = computeInputLatencyMs(960, 2, 384, 48000);
    EXPECT_NEAR(ms, 10.0f + 4.0f, 0.01f);
}

TEST(UsbLatencyMathTest, HighSpeedScaling) {
    // HS bInterval=1: 8000 packets/s, 6 frames/packet, transfer of 64 packets
    // = 384 frames. Same numeric latency as FS for the same frame counts.
    const float ms = computeOutputLatencyMs(0, 2, 1, 384, 48000);
    EXPECT_NEAR(ms, 4.0f, 0.01f);  // only inflight: 192 frames / 48000
}

TEST(UsbLatencyMathTest, GuardsInvalidInputs) {
    EXPECT_EQ(computeOutputLatencyMs(3072, 2, 3, 384, 0), 0.0f);
    EXPECT_EQ(computeOutputLatencyMs(3072, 0, 3, 384, 48000), 0.0f);
    EXPECT_EQ(computeInputLatencyMs(960, 0, 384, 48000), 0.0f);
}
