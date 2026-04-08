#include <gtest/gtest.h>
#include "DelayLine.h"
#include <cmath>

TEST(DelayLine, BasicDelay) {
    DelayLine delay;
    delay.setSampleRate(48000.0f);
    delay.setMaxDelay(1000.0f);  // 1000ms max

    // Write an impulse
    delay.write(1.0f);

    // Write 99 zeros
    for (int i = 1; i < 100; i++) {
        delay.write(0.0f);
    }

    // Read at 100 samples delay — should get the impulse
    float out = delay.read(100);
    EXPECT_NEAR(out, 1.0f, 0.001f);
}

TEST(DelayLine, ConsecutiveWriteRead) {
    DelayLine delay;
    delay.setSampleRate(48000.0f);
    delay.setMaxDelay(1000.0f);

    // Use the process() method which combines write + read
    // Feed 200 samples of a known signal, then check delayed output
    constexpr int DELAY_SAMPLES = 50;
    float result = 0.0f;

    for (int i = 0; i < 200; i++) {
        float input = (i == 0) ? 1.0f : 0.0f;  // Impulse at sample 0
        result = delay.process(input, DELAY_SAMPLES);
    }

    // After processing, the impulse should have passed through
    // Verify no NaN/Inf
    EXPECT_TRUE(std::isfinite(result));
}

TEST(DelayLine, InterpolatedReadIsFinite) {
    DelayLine delay;
    delay.setSampleRate(48000.0f);
    delay.setMaxDelay(100.0f);

    for (int i = 0; i < 500; i++) {
        delay.write(static_cast<float>(i) / 500.0f);
    }

    float fractionalRead = delay.readInterpolated(10.5f);
    EXPECT_TRUE(std::isfinite(fractionalRead));
}
