// Smoke + characterization tests for the header-only synth engines. First host
// coverage for engines/ (previously 0 tests). Every engine must: expose sane
// parameter metadata, and stay finite at both parameter extremes over time.
// Oscillator engines must generate sound; Karplus-Strong must ring and decay.

#include "../FMEngine.h"
#include "../SupersawEngine.h"
#include "../WavetableEngine.h"
#include "../GranularEngine.h"
#include "../KarplusStrongEngine.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {
constexpr int kSr = 48000;
constexpr int kBlk = 512;

// Universal safety battery for any SynthEngine.
void engineBattery(SynthEngine& e, const char* label) {
    e.prepare(kSr, kBlk);

    const int n = e.getParameterCount();
    EXPECT_GE(n, 0) << label;
    EXPECT_LE(n, MAX_ENGINE_PARAMS) << label;
    EXPECT_NE(e.getName(), nullptr) << label;
    for (int i = 0; i < n; ++i) {
        const EngineParameterDef d = e.getParameterDef(i);
        EXPECT_NE(d.name, nullptr) << label << " param " << i;
        EXPECT_LE(d.minValue, d.defaultValue) << label << " param " << i;
        EXPECT_LE(d.defaultValue, d.maxValue) << label << " param " << i;
    }

    // Drive params to both extremes; output must stay finite over time.
    std::vector<float> buf(static_cast<size_t>(kBlk) * 2, 0.0f);
    for (float frac : {0.0f, 1.0f}) {
        e.reset();
        for (int i = 0; i < n; ++i) {
            const EngineParameterDef d = e.getParameterDef(i);
            e.setParameter(i, d.minValue + frac * (d.maxValue - d.minValue));
        }
        for (int blk = 0; blk < 40; ++blk) {
            e.process(buf.data(), kBlk, 440.0f, 0.8f);
            for (float s : buf) ASSERT_TRUE(std::isfinite(s)) << label;
        }
    }
}

float blockEnergy(const std::vector<float>& b) {
    float e = 0.0f;
    for (float s : b) e += s * s;
    return e;
}
}  // namespace

TEST(EngineSmoke, FmFiniteAndSane)        { FMEngine e;            engineBattery(e, "FM"); }
TEST(EngineSmoke, SupersawFiniteAndSane)  { SupersawEngine e;      engineBattery(e, "Supersaw"); }
TEST(EngineSmoke, WavetableFiniteAndSane) { WavetableEngine e;     engineBattery(e, "Wavetable"); }
TEST(EngineSmoke, GranularFiniteAndSane)  { GranularEngine e;      engineBattery(e, "Granular"); }
TEST(EngineSmoke, KarplusFiniteAndSane)   { KarplusStrongEngine e; engineBattery(e, "Karplus"); }

// Continuous oscillator engines must actually generate sound.
TEST(EngineSmoke, OscillatorEnginesProduceSound) {
    std::vector<float> buf(static_cast<size_t>(kBlk) * 2, 0.0f);
    auto energyOf = [&](SynthEngine& e) {
        e.prepare(kSr, kBlk);
        e.reset();
        float total = 0.0f;
        for (int blk = 0; blk < 10; ++blk) {
            e.process(buf.data(), kBlk, 440.0f, 0.9f);
            total += blockEnergy(buf);
        }
        return total;
    };
    FMEngine fm;        EXPECT_GT(energyOf(fm), 0.0f);
    SupersawEngine ss;  EXPECT_GT(energyOf(ss), 0.0f);
    WavetableEngine wt; EXPECT_GT(energyOf(wt), 0.0f);
}

// Karplus-Strong is excited on reset(): energy should appear then decay.
TEST(EngineSmoke, KarplusRingsThenDecays) {
    KarplusStrongEngine e;
    e.prepare(kSr, kBlk);
    e.reset();
    std::vector<float> buf(static_cast<size_t>(kBlk) * 2, 0.0f);

    e.process(buf.data(), kBlk, 220.0f, 0.9f);
    const float early = blockEnergy(buf);
    for (int blk = 0; blk < 300; ++blk) e.process(buf.data(), kBlk, 220.0f, 0.9f);
    const float late = blockEnergy(buf);

    EXPECT_GT(early, 0.0f);
    EXPECT_LT(late, early);
}
