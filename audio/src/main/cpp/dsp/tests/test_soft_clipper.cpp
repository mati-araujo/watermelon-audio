#include <gtest/gtest.h>
#include "SoftClipper.h"
#include <cmath>

TEST(SoftClipper, OutputIsBounded) {
    SoftClipper clipper;

    // SoftClipper saturates but may exceed ±1.0 (it's soft, not hard limiting)
    // Output should be bounded within some reasonable range (e.g., ±1.5)
    for (float input = -100.0f; input <= 100.0f; input += 0.5f) {
        float output = clipper.process(input);
        EXPECT_GE(output, -2.0f) << "Extreme below at input=" << input;
        EXPECT_LE(output, 2.0f) << "Extreme above at input=" << input;
        EXPECT_TRUE(std::isfinite(output)) << "NaN/Inf at input=" << input;
    }
}

TEST(SoftClipper, PassesSmallSignals) {
    SoftClipper clipper;

    // Small signals should pass relatively unaffected
    for (float input = -0.3f; input <= 0.3f; input += 0.01f) {
        float output = clipper.process(input);
        EXPECT_NEAR(output, input, 0.1f) << "Distortion at small input=" << input;
    }
}

TEST(SoftClipper, StereoProcessingReducesPeaks) {
    SoftClipper clipper;

    float buffer[] = {2.0f, -2.0f, 5.0f, -5.0f, 0.1f, -0.1f};
    float origPeak = 5.0f;

    clipper.processStereo(buffer, 3);

    // Soft clipper reduces peaks but doesn't hard limit
    for (int i = 0; i < 6; i++) {
        EXPECT_TRUE(std::isfinite(buffer[i]));
        EXPECT_LT(std::abs(buffer[i]), origPeak) << "Peak not reduced at " << i;
    }
}

TEST(SoftClipper, PreservesSign) {
    SoftClipper clipper;

    EXPECT_GT(clipper.process(0.5f), 0.0f);
    EXPECT_LT(clipper.process(-0.5f), 0.0f);
    EXPECT_FLOAT_EQ(clipper.process(0.0f), 0.0f);
}
