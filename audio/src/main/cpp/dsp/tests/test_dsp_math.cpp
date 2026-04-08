#include <gtest/gtest.h>
#include "DSPMath.h"
#include <cmath>

TEST(DSPMath, SoftClipApproximation) {
    // softClip uses tanh — should be close to std::tanh
    for (float x = -3.0f; x <= 3.0f; x += 0.1f) {
        float result = DSPMath::softClip(x);
        float ref = std::tanh(x);
        EXPECT_NEAR(result, ref, 0.001f) << "at x=" << x;
    }
}

TEST(DSPMath, SoftClipOutputRange) {
    for (float x = -100.0f; x <= 100.0f; x += 0.5f) {
        float y = DSPMath::softClip(x);
        EXPECT_GE(y, -1.0f);
        EXPECT_LE(y, 1.0f);
    }
}

TEST(DSPMath, HardClip) {
    EXPECT_FLOAT_EQ(DSPMath::hardClip(0.5f), 0.5f);
    EXPECT_FLOAT_EQ(DSPMath::hardClip(-0.5f), -0.5f);
    EXPECT_FLOAT_EQ(DSPMath::hardClip(2.0f), 1.0f);
    EXPECT_FLOAT_EQ(DSPMath::hardClip(-2.0f), -1.0f);
}

TEST(DSPMath, LinearInterpolation) {
    EXPECT_FLOAT_EQ(DSPMath::lerp(0.0f, 10.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(DSPMath::lerp(0.0f, 10.0f, 1.0f), 10.0f);
    EXPECT_FLOAT_EQ(DSPMath::lerp(0.0f, 10.0f, 0.5f), 5.0f);
}

TEST(DSPMath, DbToLinear) {
    EXPECT_NEAR(DSPMath::dbToLinear(0.0f), 1.0f, 0.001f);
    EXPECT_NEAR(DSPMath::dbToLinear(-6.02f), 0.5f, 0.01f);
    EXPECT_NEAR(DSPMath::dbToLinear(-20.0f), 0.1f, 0.01f);
}

TEST(DSPMath, LinearToDb) {
    EXPECT_NEAR(DSPMath::linearToDb(1.0f), 0.0f, 0.001f);
    EXPECT_NEAR(DSPMath::linearToDb(0.5f), -6.02f, 0.1f);
    EXPECT_FLOAT_EQ(DSPMath::linearToDb(0.0f), -100.0f);  // Practical minimum
}

TEST(DSPMath, OnePoleCoefficient) {
    // Instant change when time constant is 0
    EXPECT_FLOAT_EQ(DSPMath::onePoleCoefficient(0.0f, 48000.0f), 1.0f);

    // Longer time constant = smaller coefficient
    float fast = DSPMath::onePoleCoefficient(1.0f, 48000.0f);
    float slow = DSPMath::onePoleCoefficient(100.0f, 48000.0f);
    EXPECT_GT(fast, slow);

    // Coefficient should be in (0, 1]
    float coeff = DSPMath::onePoleCoefficient(10.0f, 48000.0f);
    EXPECT_GT(coeff, 0.0f);
    EXPECT_LE(coeff, 1.0f);
}
