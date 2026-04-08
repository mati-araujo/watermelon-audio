#include <gtest/gtest.h>
#include "BiquadFilter.h"
#include <cmath>
#include <vector>

class BiquadFilterTest : public ::testing::Test {
protected:
    static constexpr int SAMPLE_RATE = 48000;
    static constexpr int BLOCK_SIZE = 512;
};

TEST_F(BiquadFilterTest, LowPassAttenuatesHighFrequencies) {
    BiquadFilter filter;
    filter.setSampleRate(SAMPLE_RATE);
    filter.setLowpass(1000.0f);

    // Generate 10kHz sine (should be attenuated by LPF at 1kHz)
    std::vector<float> signal(BLOCK_SIZE);
    for (int i = 0; i < BLOCK_SIZE; i++) {
        signal[i] = std::sin(2.0f * M_PI * 10000.0f * i / SAMPLE_RATE);
    }

    float inputPeak = 0.0f;
    for (float s : signal) inputPeak = std::max(inputPeak, std::abs(s));

    for (int i = 0; i < BLOCK_SIZE; i++) {
        signal[i] = filter.process(signal[i]);
    }

    float outputPeak = 0.0f;
    for (float s : signal) outputPeak = std::max(outputPeak, std::abs(s));

    // 10kHz should be heavily attenuated by a 1kHz LPF
    EXPECT_LT(outputPeak, inputPeak * 0.1f);
}

TEST_F(BiquadFilterTest, LowPassPassesLowFrequencies) {
    BiquadFilter filter;
    filter.setSampleRate(SAMPLE_RATE);
    filter.setLowpass(10000.0f);

    // Generate 100Hz sine — should pass through
    std::vector<float> signal(BLOCK_SIZE);

    // Let filter settle
    for (int i = 0; i < BLOCK_SIZE; i++) {
        float s = std::sin(2.0f * M_PI * 100.0f * i / SAMPLE_RATE);
        filter.process(s);
    }

    // Measure settled output
    for (int i = 0; i < BLOCK_SIZE; i++) {
        signal[i] = std::sin(2.0f * M_PI * 100.0f * (i + BLOCK_SIZE) / SAMPLE_RATE);
        signal[i] = filter.process(signal[i]);
    }

    float outputPeak = 0.0f;
    for (float s : signal) outputPeak = std::max(outputPeak, std::abs(s));

    EXPECT_GT(outputPeak, 0.85f);
}

TEST_F(BiquadFilterTest, StabilityWithExtremeInput) {
    BiquadFilter filter;
    filter.setSampleRate(SAMPLE_RATE);
    filter.setLowpass(1000.0f, 10.0f);  // High Q

    float result = 0.0f;
    for (int i = 0; i < 10000; i++) {
        float input = (i % 2 == 0) ? 1000.0f : -1000.0f;
        result = filter.process(input);
        EXPECT_TRUE(std::isfinite(result)) << "NaN/Inf at sample " << i;
    }
}

TEST_F(BiquadFilterTest, HighPassAttenuatesLowFrequencies) {
    BiquadFilter filter;
    filter.setSampleRate(SAMPLE_RATE);
    filter.setHighpass(5000.0f);

    // Generate 100Hz sine (should be attenuated by HPF at 5kHz)
    std::vector<float> signal(BLOCK_SIZE);
    for (int i = 0; i < BLOCK_SIZE; i++) {
        signal[i] = std::sin(2.0f * M_PI * 100.0f * i / SAMPLE_RATE);
    }

    for (int i = 0; i < BLOCK_SIZE; i++) {
        signal[i] = filter.process(signal[i]);
    }

    float outputPeak = 0.0f;
    for (float s : signal) outputPeak = std::max(outputPeak, std::abs(s));

    EXPECT_LT(outputPeak, 0.1f);
}
