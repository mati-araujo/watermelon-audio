// Validates the equal-power pan LUT: clamp behavior at the extremes,
// the L² + R² = 1 invariant across the table, and monotonic interpolation.
#include <gtest/gtest.h>
#include "PanLUT.h"

#include <cmath>

TEST(PanLUT, ClampsOutOfRangeInputs) {
    const auto& lut = wm::EqualPowerPanLUT::instance();
    auto left  = lut.lookup(-2.5f);
    auto right = lut.lookup( 2.5f);
    auto edgeL = lut.lookup(-1.0f);
    auto edgeR = lut.lookup( 1.0f);

    // Out-of-range must clamp to the same entry as the boundary value.
    EXPECT_FLOAT_EQ(left.l,  edgeL.l);
    EXPECT_FLOAT_EQ(left.r,  edgeL.r);
    EXPECT_FLOAT_EQ(right.l, edgeR.l);
    EXPECT_FLOAT_EQ(right.r, edgeR.r);
}

TEST(PanLUT, EqualPowerInvariantHoldsAcrossTable) {
    const auto& lut = wm::EqualPowerPanLUT::instance();
    for (int i = -100; i <= 100; ++i) {
        const float pan = static_cast<float>(i) * 0.01f;  // -1.0 .. +1.0
        const auto p = lut.lookup(pan);
        const float power = p.l * p.l + p.r * p.r;
        // 256-entry table sampled at arbitrary positions — slack ~1e-3
        EXPECT_NEAR(power, 1.0f, 2e-3f) << "pan=" << pan;
    }
}

TEST(PanLUT, ExtremesAreFullyPanned) {
    const auto& lut = wm::EqualPowerPanLUT::instance();
    auto fullL = lut.lookup(-1.0f);
    auto fullR = lut.lookup( 1.0f);
    // Full left: L≈1, R≈0
    EXPECT_NEAR(fullL.l, 1.0f, 1e-4f);
    EXPECT_NEAR(fullL.r, 0.0f, 1e-4f);
    // Full right: L≈0, R≈1
    EXPECT_NEAR(fullR.l, 0.0f, 1e-4f);
    EXPECT_NEAR(fullR.r, 1.0f, 1e-4f);
}

TEST(PanLUT, CentreIsMinusThreeDb) {
    const auto& lut = wm::EqualPowerPanLUT::instance();
    auto centre = lut.lookup(0.0f);
    // -3 dB ≈ 1/√2 ≈ 0.7071
    EXPECT_NEAR(centre.l, 0.70710678f, 5e-3f);
    EXPECT_NEAR(centre.r, 0.70710678f, 5e-3f);
}

TEST(PanLUT, MonotonicAcrossRange) {
    const auto& lut = wm::EqualPowerPanLUT::instance();
    float prevL = lut.lookup(-1.0f).l;
    float prevR = lut.lookup(-1.0f).r;
    for (int i = -99; i <= 100; ++i) {
        const float pan = static_cast<float>(i) * 0.01f;
        const auto p = lut.lookup(pan);
        // panL decreases monotonically, panR increases monotonically.
        EXPECT_LE(p.l, prevL + 1e-4f) << "pan=" << pan;
        EXPECT_GE(p.r, prevR - 1e-4f) << "pan=" << pan;
        prevL = p.l;
        prevR = p.r;
    }
}