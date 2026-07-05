// Characterization + regression tests for effects touched during the
// warning-gate cleanup. Doubles as the first coverage for FilterEffect and
// CompressorEffect (previously untested on host), and guards the Tier-1 fixes:
//   - FilterEffect: the uninitialized `a0` (division by garbage for an
//     out-of-range filter type).
//   - CompressorEffect / HallReverb / RiserReverb: dead process()-local reads
//     were removed on the claim the params are applied elsewhere — these tests
//     prove the params still change the output.

#include "../FilterEffect.h"
#include "../CompressorEffect.h"
#include "../HallReverbEffect.h"
#include "../RiserReverbEffect.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
constexpr int kSr = 48000;
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

std::vector<float> stereoImpulse(int frames) {
    std::vector<float> b(static_cast<size_t>(frames) * 2, 0.0f);
    b[0] = 1.0f;
    b[1] = 1.0f;
    return b;
}

float energy(const std::vector<float>& b, int startFrame = 0, int endFrame = -1) {
    if (endFrame < 0) endFrame = static_cast<int>(b.size() / 2);
    float e = 0.0f;
    for (int i = startFrame * 2; i < endFrame * 2; ++i) e += b[i] * b[i];
    return e;
}

float peak(const std::vector<float>& b) {
    float p = 0.0f;
    for (float s : b) p = std::max(p, std::fabs(s));
    return p;
}

void expectFinite(const std::vector<float>& b) {
    for (float s : b) ASSERT_TRUE(std::isfinite(s));
}
}  // namespace

// ===========================================================================
// FilterEffect
// ===========================================================================

TEST(FilterEffectTest, AllTypesProduceFiniteOutput) {
    constexpr int frames = 4096;
    auto in = stereoTone(frames, 1000.0f, 0.5f);
    std::vector<float> out(in.size(), 0.0f);
    for (float typeVal : {0.0f, 1.0f, 2.0f}) {  // LPF, HPF, BPF
        FilterEffect f;
        f.setSampleRate(kSr);
        f.setParam(0, 1000.0f);
        f.setParam(1, 4.0f);
        f.setParam(2, typeVal);
        f.process(in.data(), out.data(), frames);
        expectFinite(out);
    }
}

// Regression guard for the uninitialized-a0 fix: setType() does not clamp, so
// an out-of-range type value hits the branch that previously left `a0`
// uninitialized and divided the coefficients by garbage -> NaN/Inf.
TEST(FilterEffectTest, OutOfRangeTypeStaysFinite) {
    constexpr int frames = 2048;
    auto in = stereoTone(frames, 1000.0f, 0.5f);
    std::vector<float> out(in.size(), 0.0f);

    FilterEffect f;
    f.setSampleRate(kSr);
    f.setParam(2, 9.0f);  // not LPF/HPF/BPF
    f.process(in.data(), out.data(), frames);
    expectFinite(out);
}

TEST(FilterEffectTest, LowpassAttenuatesHighsMoreThanLows) {
    constexpr int frames = 8192;
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);

    auto runLpf = [&](float toneHz) {
        auto in = stereoTone(frames, toneHz, 0.5f);
        FilterEffect f;
        f.setSampleRate(kSr);
        f.setParam(2, 0.0f);       // LPF
        f.setParam(1, 0.707f);     // flat Q
        f.setParam(0, 500.0f);     // cutoff 500 Hz
        f.process(in.data(), out.data(), frames);
        // Ignore the first block while the cutoff smoother settles.
        return energy(out, frames / 4, frames);
    };

    const float lowEnergy = runLpf(120.0f);    // well below cutoff -> passes
    const float highEnergy = runLpf(10000.0f); // well above cutoff -> attenuated
    EXPECT_GT(lowEnergy, highEnergy * 4.0f);
}

TEST(FilterEffectTest, AbruptCutoffSweepStaysFinite) {
    constexpr int frames = kSr;  // 1 s
    auto in = stereoTone(frames, 440.0f, 0.5f);
    std::vector<float> out(in.size(), 0.0f);

    FilterEffect f;
    f.setSampleRate(kSr);
    f.setParam(2, 0.0f);
    f.setParam(1, 20.0f);  // high Q
    for (int i = 0; i < frames; i += 480) {
        f.setParam(0, (i % 2 == 0) ? 200.0f : 8000.0f);
        f.process(in.data() + i * 2, out.data() + i * 2, 480);
    }
    expectFinite(out);
}

// ===========================================================================
// CompressorEffect
// ===========================================================================

TEST(CompressorEffectTest, LoudSignalIsCompressed) {
    constexpr int frames = kSr / 2;  // 0.5 s to settle the envelope
    auto in = stereoTone(frames, 220.0f, 0.9f);  // ~-0.9 dB, above -20 dB thr
    std::vector<float> out(in.size(), 0.0f);

    CompressorEffect c;
    c.setSampleRate(kSr);
    c.process(in.data(), out.data(), frames);

    expectFinite(out);
    EXPECT_LT(c.getGainReduction(), -1.0f);          // metered reduction (dB)
    EXPECT_LT(peak(out), peak(in));                  // peaks pulled down
}

TEST(CompressorEffectTest, QuietSignalPassesWithoutReduction) {
    constexpr int frames = kSr / 2;
    auto in = stereoTone(frames, 220.0f, 0.01f);  // ~-40 dB, below threshold
    std::vector<float> out(in.size(), 0.0f);

    CompressorEffect c;
    c.setSampleRate(kSr);
    c.process(in.data(), out.data(), frames);

    expectFinite(out);
    EXPECT_GT(c.getGainReduction(), -1.0f);  // essentially no compression
}

// Guards the removal of the dead `ratio` local: proves RATIO still reaches
// computeGain via the member. Higher ratio must reduce gain more.
TEST(CompressorEffectTest, HigherRatioReducesGainMore) {
    constexpr int frames = kSr / 2;
    auto in = stereoTone(frames, 220.0f, 0.9f);
    std::vector<float> out(in.size(), 0.0f);

    auto reductionForRatio = [&](float ratio) {
        CompressorEffect c;
        c.setSampleRate(kSr);
        c.setParam(CompressorEffect::THRESHOLD, -30.0f);
        c.setParam(CompressorEffect::RATIO, ratio);
        c.process(in.data(), out.data(), frames);
        return c.getGainReduction();  // more negative = more reduction
    };

    EXPECT_LT(reductionForRatio(20.0f), reductionForRatio(2.0f));
}

// ===========================================================================
// Reverb param guards (dead process()-local reads removed in Tier 1)
// ===========================================================================

// Proves HallReverb still applies decay time (removed dead `decayTime` local).
TEST(HallReverbEffectTest, DecayTimeChangesTailEnergy) {
    constexpr int frames = kSr;  // 1 s
    auto in = stereoImpulse(frames);
    std::vector<float> shortOut(in.size(), 0.0f);
    std::vector<float> longOut(in.size(), 0.0f);

    HallReverbEffect shortDecay;
    shortDecay.setSampleRate(kSr);
    shortDecay.setParam(HallReverbEffect::PARAM_MIX, 1.0f);
    shortDecay.setParam(HallReverbEffect::PARAM_DECAY_TIME, 0.5f);
    shortDecay.process(in.data(), shortOut.data(), frames);

    HallReverbEffect longDecay;
    longDecay.setSampleRate(kSr);
    longDecay.setParam(HallReverbEffect::PARAM_MIX, 1.0f);
    longDecay.setParam(HallReverbEffect::PARAM_DECAY_TIME, 15.0f);
    longDecay.process(in.data(), longOut.data(), frames);

    expectFinite(shortOut);
    expectFinite(longOut);
    // Late tail (last half second) must hold more energy with a longer decay.
    EXPECT_GT(energy(longOut, kSr / 2, frames), energy(shortOut, kSr / 2, frames));
}

// Proves RiserReverb still applies damping (removed dead `damping` local):
// the two damping extremes must yield materially different output.
TEST(RiserReverbEffectTest, DampingChangesOutput) {
    constexpr int frames = kSr;
    auto in = stereoTone(frames, 4000.0f, 0.5f);  // high tone: damping bites
    std::vector<float> noDamp(in.size(), 0.0f);
    std::vector<float> fullDamp(in.size(), 0.0f);

    RiserReverbEffect a;
    a.setSampleRate(kSr);
    a.setParam(RiserReverbEffect::PARAM_MIX, 1.0f);
    a.setParam(RiserReverbEffect::PARAM_DAMPING, 0.0f);
    a.process(in.data(), noDamp.data(), frames);

    RiserReverbEffect b;
    b.setSampleRate(kSr);
    b.setParam(RiserReverbEffect::PARAM_MIX, 1.0f);
    b.setParam(RiserReverbEffect::PARAM_DAMPING, 1.0f);
    b.process(in.data(), fullDamp.data(), frames);

    expectFinite(noDamp);
    expectFinite(fullDamp);
    const float eNo = energy(noDamp);
    const float eFull = energy(fullDamp);
    // Material difference proves damping is wired; a disconnected param would
    // give ~0. The observed delta is ~2-3% of total energy, so 1% is a robust
    // floor that still catches a regression that stops applying damping.
    EXPECT_GT(std::fabs(eNo - eFull), 0.01f * std::max(eNo, eFull));
}
