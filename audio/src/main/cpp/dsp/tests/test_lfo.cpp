#include <gtest/gtest.h>
#include "LFO.h"
#include <cmath>
#include <algorithm>
#include <vector>

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

// REQ-055 S2 — las formas aleatorias se pueden REPRODUCIR. Hasta S2 salian de un
// `thread_local std::mt19937` sembrado con `std::random_device`: dos LFOs iguales
// sorteaban distinto y un render offline con un LFO RANDOM no se podia repetir.
namespace {
std::vector<float> randomRun(LFO& lfo, LFO::Waveform w, int n) {
    lfo.setSampleRate(48000.0f);
    lfo.setRate(7.0f);
    lfo.setWaveform(w);
    std::vector<float> out(static_cast<size_t>(n));
    for (auto& v : out) v = lfo.process();
    return out;
}
}  // namespace

TEST(LFO, RandomWaveformsAreReproducibleAcrossInstancesAndAfterReset) {
    for (auto w : {LFO::Waveform::RANDOM, LFO::Waveform::RANDOM_SMOOTH}) {
        LFO a, b;
        const auto ra = randomRun(a, w, 48000);
        const auto rb = randomRun(b, w, 48000);
        EXPECT_EQ(ra, rb) << "dos LFOs recien construidos sortean distinto";

        // Y no es una constante: el sorteo se mueve (si no, el test no ve nada).
        float lo = 1.0f, hi = -1.0f;
        for (float v : ra) { lo = std::min(lo, v); hi = std::max(hi, v); }
        EXPECT_GT(hi - lo, 0.5f) << "la forma aleatoria no se mueve";

        a.reset();
        b.reset();
        LFO fresh;
        fresh.reset();
        EXPECT_EQ(randomRun(a, w, 48000), randomRun(fresh, w, 48000))
            << "reset() no vuelve a la secuencia del principio";
        for (float v : ra) {
            ASSERT_GE(v, -1.0f);
            ASSERT_LE(v, 1.0f);
        }
    }
}
