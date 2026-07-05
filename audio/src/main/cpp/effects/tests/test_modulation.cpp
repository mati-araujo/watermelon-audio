// Effects sweep — batch 2: modulation effects (chorus, phaser, auto-pan,
// complex tremolo, random resonance). Smoke battery + dry-null, plus a check
// that auto-pan actually creates time-varying stereo movement.

#include "../ChorusEffect.h"
#include "../PhaserEffect.h"
#include "../AutoPanEffect.h"
#include "../ComplexTremEffect.h"
#include "../RandomResoEffect.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {
constexpr int kSr = 48000;
constexpr int kFrames = 8192;
constexpr float kPi = 3.14159265358979323846f;

std::vector<float> stereoTone(int frames, float freq, float amp) {
    std::vector<float> b(static_cast<size_t>(frames) * 2, 0.0f);
    for (int i = 0; i < frames; ++i) {
        const float s = std::sin(2.0f * kPi * freq * static_cast<float>(i) / kSr) * amp;
        b[i * 2] = s;
        b[i * 2 + 1] = s;
    }
    return b;
}

float energy(const std::vector<float>& b) {
    float e = 0.0f;
    for (float s : b) e += s * s;
    return e;
}

float stereoDifferenceEnergy(const std::vector<float>& b) {
    float e = 0.0f;
    const int frames = static_cast<int>(b.size() / 2);
    for (int i = 0; i < frames; ++i) {
        const float d = b[i * 2] - b[i * 2 + 1];
        e += d * d;
    }
    return e;
}

void expectFinite(const std::vector<float>& b) {
    for (float s : b) ASSERT_TRUE(std::isfinite(s));
}

void smokeEffect(Effect& fx) {
    fx.setSampleRate(kSr);

    auto tone = stereoTone(kFrames, 440.0f, 0.5f);
    std::vector<float> out(tone.size(), 0.0f);
    fx.process(tone.data(), out.data(), kFrames);
    expectFinite(out);
    EXPECT_GT(energy(out), 0.0f);

    auto hot = stereoTone(kFrames, 220.0f, 4.0f);
    std::vector<float> hotOut(hot.size(), 0.0f);
    fx.process(hot.data(), hotOut.data(), kFrames);
    expectFinite(hotOut);

    std::vector<float> silence(static_cast<size_t>(kFrames) * 2, 0.0f);
    std::vector<float> silenceOut(silence.size(), 0.0f);
    fx.process(silence.data(), silenceOut.data(), kFrames);
    expectFinite(silenceOut);
}

// mix at its dry value must pass through (after the mix smoother settles).
void expectDryNull(Effect& fx, int mixParam, float dryValue) {
    fx.setSampleRate(kSr);
    fx.setParam(mixParam, dryValue);
    auto in = stereoTone(kFrames, 330.0f, 0.4f);
    std::vector<float> out(in.size(), 0.0f);
    fx.process(in.data(), out.data(), kFrames);   // warm-up
    fx.process(in.data(), out.data(), kFrames);   // settled
    for (size_t i = 0; i < in.size(); ++i) EXPECT_NEAR(out[i], in[i], 1e-5f);
}
}  // namespace

TEST(ModulationSmoke, ChorusStable)       { ChorusEffect fx;      smokeEffect(fx); }
TEST(ModulationSmoke, PhaserStable)       { PhaserEffect fx;      smokeEffect(fx); }
TEST(ModulationSmoke, AutoPanStable)      { AutoPanEffect fx;     smokeEffect(fx); }
TEST(ModulationSmoke, ComplexTremStable)  { ComplexTremEffect fx; smokeEffect(fx); }
TEST(ModulationSmoke, RandomResoStable)   { RandomResoEffect fx;  smokeEffect(fx); }

TEST(ModulationDryNull, ChorusMixZeroIsDry) {
    ChorusEffect fx;
    expectDryNull(fx, ChorusEffect::MIX, 0.0f);  // MIX is 0-100 -> 0 = dry
}
TEST(ModulationDryNull, PhaserMixZeroIsDry) {
    PhaserEffect fx;
    expectDryNull(fx, PhaserEffect::MIX, 0.0f);
}
TEST(ModulationDryNull, AutoPanMixZeroIsDry) {
    AutoPanEffect fx;
    expectDryNull(fx, AutoPanEffect::PARAM_MIX, 0.0f);
}
TEST(ModulationDryNull, ComplexTremMixZeroIsDry) {
    ComplexTremEffect fx;
    expectDryNull(fx, ComplexTremEffect::PARAM_MIX, 0.0f);
}
TEST(ModulationDryNull, RandomResoMixZeroIsDry) {
    RandomResoEffect fx;
    expectDryNull(fx, RandomResoEffect::PARAM_MIX, 0.0f);
}

// Auto-pan at full depth must create time-varying stereo movement from a mono
// input (the L/R envelopes diverge as the pan LFO sweeps).
TEST(ModulationCharacter, AutoPanCreatesStereoMovement) {
    AutoPanEffect fx;
    fx.setSampleRate(kSr);
    fx.setParam(AutoPanEffect::PARAM_MIX, 1.0f);
    fx.setParam(AutoPanEffect::PARAM_DEPTH, 1.0f);
    fx.setParam(AutoPanEffect::PARAM_RATE, 4.0f);

    auto in = stereoTone(kFrames, 440.0f, 0.5f);  // identical L/R
    std::vector<float> out(in.size(), 0.0f);
    fx.process(in.data(), out.data(), kFrames);

    expectFinite(out);
    EXPECT_GT(stereoDifferenceEnergy(out), 0.001f);
}
