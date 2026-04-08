#include <gtest/gtest.h>
#include "ParameterSmoother.h"
#include <cmath>

TEST(ParameterSmoother, ConvergesToTarget) {
    ParameterSmoother smoother(0.99f);
    smoother.reset(0.0f);

    float value = 0.0f;
    for (int i = 0; i < 10000; i++) {
        value = smoother.process(1.0f);
    }

    EXPECT_NEAR(value, 1.0f, 0.001f);
}

TEST(ParameterSmoother, StartsAtResetValue) {
    ParameterSmoother smoother(0.99f);
    smoother.reset(0.5f);

    // First call with target=0.5 should return ~0.5
    float value = smoother.process(0.5f);
    EXPECT_FLOAT_EQ(value, 0.5f);
}

TEST(ParameterSmoother, NoOvershoot) {
    ParameterSmoother smoother(0.99f);
    smoother.reset(0.0f);

    for (int i = 0; i < 100000; i++) {
        float value = smoother.process(1.0f);
        EXPECT_GE(value, 0.0f) << "Undershoot at sample " << i;
        EXPECT_LE(value, 1.0f) << "Overshoot at sample " << i;
    }
}

TEST(ParameterSmoother, MonotonicIncrease) {
    ParameterSmoother smoother(0.99f);
    smoother.reset(0.0f);

    float prev = 0.0f;
    for (int i = 0; i < 1000; i++) {
        float value = smoother.process(1.0f);
        EXPECT_GE(value, prev - 1e-6f) << "Non-monotonic at sample " << i;
        prev = value;
    }
}

TEST(ParameterSmoother, FastCoefficientConvergesFaster) {
    ParameterSmoother fast(0.9f);   // Fast
    ParameterSmoother slow(0.999f); // Slow
    fast.reset(0.0f);
    slow.reset(0.0f);

    float fastVal = 0.0f, slowVal = 0.0f;
    for (int i = 0; i < 100; i++) {
        fastVal = fast.process(1.0f);
        slowVal = slow.process(1.0f);
    }

    EXPECT_GT(fastVal, slowVal);
}

TEST(ParameterSmoother, HandlesNegativeTarget) {
    ParameterSmoother smoother(0.99f);
    smoother.reset(1.0f);

    float value = 0.0f;
    for (int i = 0; i < 100000; i++) {
        value = smoother.process(-1.0f);
    }

    EXPECT_NEAR(value, -1.0f, 0.001f);
}
