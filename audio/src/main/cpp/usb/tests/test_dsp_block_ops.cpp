/**
 * test_dsp_block_ops.cpp — pins the pure block transforms extracted from the USB
 * duplex DSP loop (usb/DspBlockOps.h). These cover hallazgo H6 (splice/fade were
 * inline and untested) and guard the bit-identity of the Etapa 5 extraction.
 */

#include <gtest/gtest.h>

#include <vector>

#include "usb/DspBlockOps.h"

using namespace watermelon_audio::usb;

namespace {

// --------------------------------------------------------------------------
// monoToStereo
// --------------------------------------------------------------------------

TEST(DspBlockOps, MonoToStereoDefaultGainDuplicatesWithMinus3dB) {
    const std::vector<float> mono = {1.0f, -0.5f, 0.25f};
    std::vector<float> stereo(mono.size() * 2, 999.0f);

    monoToStereo(mono.data(), stereo.data(), static_cast<int>(mono.size()));

    for (size_t f = 0; f < mono.size(); ++f) {
        const float expected = mono[f] * 0.707f;  // kMonoToStereoGain
        EXPECT_FLOAT_EQ(stereo[f * 2], expected);
        EXPECT_FLOAT_EQ(stereo[f * 2 + 1], expected);  // L == R
    }
}

TEST(DspBlockOps, MonoToStereoHonorsCustomGain) {
    const std::vector<float> mono = {2.0f, 4.0f};
    std::vector<float> stereo(4, 0.0f);

    monoToStereo(mono.data(), stereo.data(), 2, 0.5f);

    EXPECT_FLOAT_EQ(stereo[0], 1.0f);
    EXPECT_FLOAT_EQ(stereo[1], 1.0f);
    EXPECT_FLOAT_EQ(stereo[2], 2.0f);
    EXPECT_FLOAT_EQ(stereo[3], 2.0f);
}

// --------------------------------------------------------------------------
// spliceCrossfadeFrames / spliceDeclickHead
// --------------------------------------------------------------------------

TEST(DspBlockOps, CrossfadeFramesCappedAt48) {
    EXPECT_EQ(spliceCrossfadeFrames(4), 4);
    EXPECT_EQ(spliceCrossfadeFrames(48), 48);
    EXPECT_EQ(spliceCrossfadeFrames(256), 48);
}

TEST(DspBlockOps, SpliceDeclickRampsHeadFromHold) {
    // 4 frames -> xf = 4. w = (f+1)/4 = 0.25, 0.5, 0.75, 1.0.
    const float hL = 10.0f, hR = 20.0f;
    std::vector<float> block = {1, 2,  3, 4,  5, 6,  7, 8};

    spliceDeclickHead(block.data(), 4, hL, hR);

    // out = w*block + (1-w)*hold
    EXPECT_FLOAT_EQ(block[0], 0.25f * 1 + 0.75f * hL);
    EXPECT_FLOAT_EQ(block[1], 0.25f * 2 + 0.75f * hR);
    EXPECT_FLOAT_EQ(block[2], 0.50f * 3 + 0.50f * hL);
    EXPECT_FLOAT_EQ(block[3], 0.50f * 4 + 0.50f * hR);
    EXPECT_FLOAT_EQ(block[4], 0.75f * 5 + 0.25f * hL);
    EXPECT_FLOAT_EQ(block[5], 0.75f * 6 + 0.25f * hR);
    EXPECT_FLOAT_EQ(block[6], 7.0f);  // f=3 -> w=1 -> unchanged
    EXPECT_FLOAT_EQ(block[7], 8.0f);
}

TEST(DspBlockOps, SpliceDeclickLeavesFramesPastCrossfadeUntouched) {
    // 50 frames -> xf = 48. Frames 48 and 49 must be untouched.
    std::vector<float> block(50 * 2, 3.0f);
    spliceDeclickHead(block.data(), 50, -1.0f, -1.0f);
    EXPECT_FLOAT_EQ(block[48 * 2], 3.0f);
    EXPECT_FLOAT_EQ(block[48 * 2 + 1], 3.0f);
    EXPECT_FLOAT_EQ(block[49 * 2], 3.0f);
    EXPECT_FLOAT_EQ(block[49 * 2 + 1], 3.0f);
    // Frame 0 was crossfaded from hold=-1 (w=1/48).
    const float w0 = 1.0f / 48.0f;
    EXPECT_FLOAT_EQ(block[0], w0 * 3.0f + (1.0f - w0) * -1.0f);
}

// --------------------------------------------------------------------------
// fadeBlockToSilence
// --------------------------------------------------------------------------

TEST(DspBlockOps, FadeRampsPerSampleTowardZero) {
    // 2 frames -> n = 4. fade = 1 - i/4: 1.0, 0.75, 0.5, 0.25.
    std::vector<float> block = {1, 1, 1, 1};
    fadeBlockToSilence(block.data(), 2);
    EXPECT_FLOAT_EQ(block[0], 1.0f);
    EXPECT_FLOAT_EQ(block[1], 0.75f);   // R of frame 0 differs from L (per-sample)
    EXPECT_FLOAT_EQ(block[2], 0.5f);
    EXPECT_FLOAT_EQ(block[3], 0.25f);   // never reaches exactly 0
}

// --------------------------------------------------------------------------
// Composed underrun sequence: capture hold -> fade -> declick head.
// Mirrors the LibusbBackend underrun path exactly.
// --------------------------------------------------------------------------

TEST(DspBlockOps, UnderrunFadeThenDeclickMatchesReference) {
    const int frames = 4;
    const size_t n = static_cast<size_t>(frames) * 2;  // 8
    std::vector<float> block(n, 1.0f);

    const float holdL = block[n - 2];  // 1.0 (captured BEFORE fade)
    const float holdR = block[n - 1];  // 1.0
    fadeBlockToSilence(block.data(), frames);
    spliceDeclickHead(block.data(), frames, holdL, holdR);

    // Hand-computed reference (see header math).
    const std::vector<float> expected = {
        1.0f, 0.96875f, 0.875f, 0.8125f, 0.625f, 0.53125f, 0.25f, 0.125f};
    for (size_t i = 0; i < n; ++i) {
        EXPECT_NEAR(block[i], expected[i], 1e-6f) << "sample " << i;
    }
}

}  // namespace
