#include <gtest/gtest.h>
#include "LFO.h"
#include <cmath>

TEST(LFO, SineOutputRange) {
    LFO lfo;
    lfo.setSampleRate(48000.0f);
    lfo.setRate(1.0f);  // 1 Hz
    lfo.setWaveform(LFO::Waveform::SINE);

    for (int i = 0; i < 48000; i++) {
        float value = lfo.process();
        EXPECT_GE(value, -1.01f) << "Below -1 at sample " << i;
        EXPECT_LE(value, 1.01f) << "Above 1 at sample " << i;
    }
}

TEST(LFO, FrequencyAccuracy) {
    LFO lfo;
    lfo.setSampleRate(48000.0f);
    lfo.setRate(10.0f);  // 10 Hz
    lfo.setWaveform(LFO::Waveform::SINE);

    // Count zero crossings in 1 second (should be ~20 for 10Hz sine)
    int zeroCrossings = 0;
    float prev = lfo.process();
    for (int i = 1; i < 48000; i++) {
        float current = lfo.process();
        if ((prev >= 0.0f && current < 0.0f) || (prev < 0.0f && current >= 0.0f)) {
            zeroCrossings++;
        }
        prev = current;
    }

    EXPECT_NEAR(zeroCrossings, 20, 2);
}

TEST(LFO, AllWaveformsInRange) {
    LFO lfo;
    lfo.setSampleRate(48000.0f);
    lfo.setRate(5.0f);

    LFO::Waveform waveforms[] = {
        LFO::Waveform::SINE,
        LFO::Waveform::TRIANGLE,
        LFO::Waveform::SQUARE,
        LFO::Waveform::SAWTOOTH
    };

    for (auto wf : waveforms) {
        lfo.setWaveform(wf);
        lfo.reset();

        for (int i = 0; i < 48000; i++) {
            float value = lfo.process();
            EXPECT_GE(value, -1.01f) << "Waveform " << static_cast<int>(wf) << " below range at " << i;
            EXPECT_LE(value, 1.01f) << "Waveform " << static_cast<int>(wf) << " above range at " << i;
        }
    }
}

TEST(LFO, ResetResetsPhase) {
    LFO lfo;
    lfo.setSampleRate(48000.0f);
    lfo.setRate(1.0f);
    lfo.setWaveform(LFO::Waveform::SINE);

    // Process some samples
    for (int i = 0; i < 1000; i++) lfo.process();

    lfo.reset();
    float afterReset = lfo.process();

    // After reset, sine should start near 0
    EXPECT_NEAR(afterReset, 0.0f, 0.02f);
}
