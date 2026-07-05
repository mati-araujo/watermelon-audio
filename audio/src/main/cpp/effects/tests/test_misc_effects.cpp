// Effects sweep — batch 3 (final): the remaining processing effects that did
// not fit the earlier categories — granular, dynamics, EQ, vocoder.
// (BuiltInIRs / EffectDefaults are data/constant headers, not effects.)

#include "../BeatGrainEffect.h"
#include "../LookaheadLimiter.h"
#include "../ParametricEQ.h"
#include "../VocoderEffect.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
constexpr int kSr = 48000;
constexpr int kFrames = 8192;
constexpr int kBlock = 256;  // realistic DSP block; some effects (Vocoder) cap
                             // the max frames per process() call.
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

float peak(const std::vector<float>& b) {
    float p = 0.0f;
    for (float s : b) p = std::max(p, std::fabs(s));
    return p;
}

void expectFinite(const std::vector<float>& b) {
    for (float s : b) ASSERT_TRUE(std::isfinite(s));
}

// Process the whole buffer in realistic kBlock-sized chunks.
void processInBlocks(Effect& fx, std::vector<float>& in, std::vector<float>& out) {
    for (int off = 0; off < kFrames; off += kBlock) {
        const int n = std::min(kBlock, kFrames - off);
        fx.process(in.data() + off * 2, out.data() + off * 2, n);
    }
}

// finite on a tone, finite under a hot clipping input, finite on silence.
void smokeEffect(Effect& fx) {
    fx.setSampleRate(kSr);

    auto tone = stereoTone(kFrames, 440.0f, 0.5f);
    std::vector<float> out(tone.size(), 0.0f);
    processInBlocks(fx, tone, out);
    expectFinite(out);

    auto hot = stereoTone(kFrames, 220.0f, 4.0f);
    std::vector<float> hotOut(hot.size(), 0.0f);
    processInBlocks(fx, hot, hotOut);
    expectFinite(hotOut);

    std::vector<float> silence(static_cast<size_t>(kFrames) * 2, 0.0f);
    std::vector<float> silenceOut(silence.size(), 0.0f);
    processInBlocks(fx, silence, silenceOut);
    expectFinite(silenceOut);
}

void expectDryNull(Effect& fx, int mixParam, float dryValue) {
    fx.setSampleRate(kSr);
    fx.setParam(mixParam, dryValue);
    auto in = stereoTone(kFrames, 330.0f, 0.4f);
    std::vector<float> out(in.size(), 0.0f);
    processInBlocks(fx, in, out);  // warm-up
    processInBlocks(fx, in, out);  // settled
    for (size_t i = 0; i < in.size(); ++i) EXPECT_NEAR(out[i], in[i], 1e-5f);
}
}  // namespace

TEST(MiscEffectSmoke, BeatGrainStable)      { BeatGrainEffect fx;  smokeEffect(fx); }
TEST(MiscEffectSmoke, LookaheadLimiterStable) { LookaheadLimiter fx; smokeEffect(fx); }
TEST(MiscEffectSmoke, ParametricEqStable)   { ParametricEQ fx;     smokeEffect(fx); }
TEST(MiscEffectSmoke, VocoderStable)        { VocoderEffect fx;    smokeEffect(fx); }

TEST(MiscEffectDryNull, BeatGrainMixZeroIsDry) {
    BeatGrainEffect fx;
    expectDryNull(fx, BeatGrainEffect::PARAM_MIX, 0.0f);
}
// The Vocoder has no true dry-null: even at MIX=0 the signal is mono-summed
// and always passes the output LPF, so it is not a bit-identical bypass. Assert
// instead that the MIX parameter actually blends (mix=0 vs mix=1 differ).
TEST(MiscEffectCharacter, VocoderMixParameterHasEffect) {
    auto outAtMix = [](float mix) {
        VocoderEffect fx;
        fx.setSampleRate(kSr);
        fx.setParam(VocoderEffect::MIX, mix);
        auto in = stereoTone(kFrames, 330.0f, 0.4f);
        std::vector<float> out(in.size(), 0.0f);
        processInBlocks(fx, in, out);
        processInBlocks(fx, in, out);
        return out;
    };
    const auto dry = outAtMix(0.0f);
    const auto wet = outAtMix(1.0f);
    expectFinite(dry);
    expectFinite(wet);
    float diff = 0.0f;
    for (size_t i = 0; i < dry.size(); ++i) diff += std::fabs(dry[i] - wet[i]);
    EXPECT_GT(diff, 1.0f);
}

// A ParametricEQ left flat (defaults) must roughly preserve the signal, not
// zero it out or blow it up.
TEST(MiscEffectCharacter, ParametricEqDefaultPreservesEnergy) {
    ParametricEQ fx;
    fx.setSampleRate(kSr);
    auto in = stereoTone(kFrames, 440.0f, 0.5f);
    std::vector<float> out(in.size(), 0.0f);
    processInBlocks(fx, in, out);   // warm-up
    processInBlocks(fx, in, out);
    expectFinite(out);
    const float ei = energy(in), eo = energy(out);
    EXPECT_GT(eo, 0.25f * ei);
    EXPECT_LT(eo, 4.0f * ei);
}

// A look-ahead limiter must pull a hot signal's peaks below the input peak.
TEST(MiscEffectCharacter, LimiterReducesPeaks) {
    LookaheadLimiter fx;
    fx.setSampleRate(kSr);
    fx.setParam(0, -6.0f);  // threshold in dB

    auto in = stereoTone(kFrames, 220.0f, 2.0f);  // hot: peak 2.0
    std::vector<float> out(in.size(), 0.0f);
    processInBlocks(fx, in, out);   // prime lookahead/attack
    processInBlocks(fx, in, out);

    expectFinite(out);
    EXPECT_LT(peak(out), peak(in));   // limiting actually happened
}
