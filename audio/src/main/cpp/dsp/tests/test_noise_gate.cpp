#include <gtest/gtest.h>
#include <cmath>
#include <vector>

#include "../NoiseGate.h"

namespace {

constexpr int kSampleRate = 48000;

// Feed a constant-amplitude stereo block and return the gate state after it.
void feedLevel(NoiseGate& gate, float amplitude, int frames) {
    std::vector<float> buf(static_cast<size_t>(frames) * 2, amplitude);
    gate.process(buf.data(), frames);
}

float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }

}  // namespace

TEST(NoiseGate, OpensAboveThreshold) {
    NoiseGate gate;
    gate.prepare(kSampleRate);
    gate.setThreshold(-60.0f);

    feedLevel(gate, dbToLin(-40.0f), 4800);  // well above threshold
    EXPECT_TRUE(gate.isOpen());
    EXPECT_GT(gate.getGain(), 0.9f);
}

TEST(NoiseGate, StaysOpenWithinHysteresisBand) {
    NoiseGate gate;
    gate.prepare(kSampleRate);
    gate.setThreshold(-60.0f);
    gate.setHysteresis(6.0f);

    feedLevel(gate, dbToLin(-40.0f), 4800);
    ASSERT_TRUE(gate.isOpen());

    // -63 dB is below the open threshold but above close (-66 dB): no chatter.
    feedLevel(gate, dbToLin(-63.0f), 4800);
    EXPECT_TRUE(gate.isOpen());
}

// Regression: hysteresis was subtracted as a linear RATIO from a linear
// AMPLITUDE, making the close threshold negative — the gate could never
// re-close once open.
TEST(NoiseGate, ClosesWhenSignalDropsBelowHysteresisBand) {
    NoiseGate gate;
    gate.prepare(kSampleRate);
    gate.setThreshold(-60.0f);
    gate.setHysteresis(6.0f);

    feedLevel(gate, dbToLin(-40.0f), 4800);
    ASSERT_TRUE(gate.isOpen());

    // Far below close threshold (-66 dB); give the 50 ms release time to act.
    feedLevel(gate, dbToLin(-90.0f), kSampleRate / 2);
    EXPECT_FALSE(gate.isOpen());
    EXPECT_LT(gate.getGain(), 0.1f);
}

TEST(NoiseGate, ReopensAfterClosing) {
    NoiseGate gate;
    gate.prepare(kSampleRate);
    gate.setThreshold(-60.0f);

    feedLevel(gate, dbToLin(-40.0f), 4800);
    feedLevel(gate, dbToLin(-90.0f), kSampleRate / 2);
    ASSERT_FALSE(gate.isOpen());

    feedLevel(gate, dbToLin(-40.0f), 4800);
    EXPECT_TRUE(gate.isOpen());
    EXPECT_GT(gate.getGain(), 0.9f);
}

TEST(NoiseGate, NegativeHysteresisIsClamped) {
    NoiseGate gate;
    gate.prepare(kSampleRate);
    gate.setThreshold(-60.0f);
    gate.setHysteresis(-12.0f);  // would put close threshold above open

    feedLevel(gate, dbToLin(-40.0f), 4800);
    ASSERT_TRUE(gate.isOpen());

    // With clamped (0 dB) hysteresis the close threshold equals the open
    // threshold; a level just above it must keep the gate open.
    feedLevel(gate, dbToLin(-58.0f), 4800);
    EXPECT_TRUE(gate.isOpen());
}
