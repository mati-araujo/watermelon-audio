#include "../DelayEffect.h"
#include "../HpfDelayEffect.h"
#include "../TapeEchoEffect.h"
#include "../ReverbEffect.h"
#include "../HallReverbEffect.h"
#include "../RiserReverbEffect.h"
#include "../SpringReverbEffect.h"
#include "../PlateReverbEffect.h"
#include "../ShimmerReverbEffect.h"
#include "../EffectChain.h"
#include "../EffectTypes.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
constexpr int kSampleRate = 48000;

std::vector<float> makeStereoImpulse(int frames, float left = 1.0f, float right = 0.75f) {
    std::vector<float> buffer(frames * 2, 0.0f);
    buffer[0] = left;
    buffer[1] = right;
    return buffer;
}

std::vector<float> makeStereoTone(int frames) {
    std::vector<float> buffer(frames * 2, 0.0f);
    for (int i = 0; i < frames; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
        buffer[i * 2] = std::sin(2.0f * 3.14159265f * 220.0f * t) * 0.25f;
        buffer[i * 2 + 1] = std::sin(2.0f * 3.14159265f * 330.0f * t) * 0.2f;
    }
    return buffer;
}

float energy(const std::vector<float>& buffer, int startFrame = 0, int endFrame = -1) {
    if (endFrame < 0) endFrame = static_cast<int>(buffer.size() / 2);
    float e = 0.0f;
    for (int i = startFrame * 2; i < endFrame * 2; ++i) {
        e += buffer[i] * buffer[i];
    }
    return e;
}

float stereoDifferenceEnergy(const std::vector<float>& buffer) {
    float e = 0.0f;
    int frames = static_cast<int>(buffer.size() / 2);
    for (int i = 0; i < frames; ++i) {
        float d = buffer[i * 2] - buffer[i * 2 + 1];
        e += d * d;
    }
    return e;
}

void expectFinite(const std::vector<float>& buffer) {
    for (float sample : buffer) {
        EXPECT_TRUE(std::isfinite(sample));
    }
}
}

TEST(GuitarDelayReverb, DelayDefaultsMatchPublicKotlinDefaults) {
    DelayEffect delay;
    EXPECT_FLOAT_EQ(delay.getParam(0), 250.0f);
    EXPECT_FLOAT_EQ(delay.getParam(1), 0.4f);
    EXPECT_FLOAT_EQ(delay.getParam(2), 0.3f);
    EXPECT_FLOAT_EQ(delay.getParam(3), 120.0f);
    EXPECT_FLOAT_EQ(delay.getParam(4), 4.0f);
    EXPECT_FLOAT_EQ(delay.getParam(5), 0.0f);
}

TEST(GuitarDelayReverb, MixZeroIsDryNullForLegacyDelayAndReverb) {
    constexpr int frames = 512;
    auto input = makeStereoTone(frames);
    std::vector<float> output(frames * 2, 0.0f);

    DelayEffect delay;
    delay.setSampleRate(kSampleRate);
    delay.setParam(2, 0.0f);
    delay.process(input.data(), output.data(), frames);
    for (size_t i = 0; i < input.size(); ++i) {
        EXPECT_NEAR(output[i], input[i], 1e-6f);
    }

    std::fill(output.begin(), output.end(), 0.0f);
    ReverbEffect reverb;
    reverb.setSampleRate(kSampleRate);
    reverb.setParam(2, 0.0f);
    reverb.process(input.data(), output.data(), frames);
    for (size_t i = 0; i < input.size(); ++i) {
        EXPECT_NEAR(output[i], input[i], 1e-6f);
    }
}

TEST(GuitarDelayReverb, AbruptDelayTimeChangeRemainsFinite) {
    constexpr int frames = kSampleRate * 2;
    auto input = makeStereoTone(frames);
    std::vector<float> output(frames * 2, 0.0f);

    DelayEffect delay;
    delay.setSampleRate(kSampleRate);
    delay.setParam(2, 0.75f);
    delay.setParam(0, 50.0f);
    delay.process(input.data(), output.data(), frames / 2);
    delay.setParam(0, 1600.0f);
    delay.process(input.data() + frames, output.data() + frames, frames / 2);

    expectFinite(output);
}

TEST(GuitarDelayReverb, TempoSyncedDelayVariantsProduceEchoEnergy) {
    constexpr int frames = kSampleRate;
    auto input = makeStereoImpulse(frames);
    std::vector<float> output(frames * 2, 0.0f);

    HpfDelayEffect hpfDelay;
    hpfDelay.setSampleRate(kSampleRate);
    hpfDelay.setBpm(120.0f);
    hpfDelay.setParam(HpfDelayEffect::PARAM_SYNC, 1.0f);
    hpfDelay.setParam(HpfDelayEffect::PARAM_SUBDIVISION, 0.0f);
    hpfDelay.setParam(HpfDelayEffect::PARAM_FEEDBACK, 0.0f);
    hpfDelay.setParam(HpfDelayEffect::PARAM_MIX, 1.0f);
    hpfDelay.process(input.data(), output.data(), frames);
    expectFinite(output);
    EXPECT_GT(energy(output, kSampleRate / 3, frames), 0.0001f);

    std::fill(output.begin(), output.end(), 0.0f);
    TapeEchoEffect tapeEcho;
    tapeEcho.setSampleRate(kSampleRate);
    tapeEcho.setBpm(120.0f);
    tapeEcho.setParam(TapeEchoEffect::PARAM_SYNC, 1.0f);
    tapeEcho.setParam(TapeEchoEffect::PARAM_SUBDIVISION, 2.0f);
    tapeEcho.setParam(TapeEchoEffect::PARAM_FEEDBACK, 0.0f);
    tapeEcho.setParam(TapeEchoEffect::PARAM_MIX, 1.0f);
    tapeEcho.process(input.data(), output.data(), frames);
    expectFinite(output);
    EXPECT_GT(energy(output, kSampleRate / 4, frames), 0.0001f);
}

TEST(GuitarDelayReverb, HighFeedbackDelayVariantsRemainFinite) {
    constexpr int frames = kSampleRate * 3;
    auto input = makeStereoTone(frames);
    std::vector<float> output(frames * 2, 0.0f);

    HpfDelayEffect hpfDelay;
    hpfDelay.setSampleRate(kSampleRate);
    hpfDelay.setParam(HpfDelayEffect::PARAM_FEEDBACK, 0.95f);
    hpfDelay.setParam(HpfDelayEffect::PARAM_MIX, 0.8f);
    hpfDelay.process(input.data(), output.data(), frames);
    expectFinite(output);

    TapeEchoEffect tapeEcho;
    tapeEcho.setSampleRate(kSampleRate);
    tapeEcho.setParam(TapeEchoEffect::PARAM_FEEDBACK, 0.95f);
    tapeEcho.setParam(TapeEchoEffect::PARAM_MIX, 0.8f);
    tapeEcho.process(input.data(), output.data(), frames);
    expectFinite(output);
}

TEST(GuitarDelayReverb, RiserAttackTimeChangesEarlyTailEnergy) {
    constexpr int frames = kSampleRate;
    auto input = makeStereoImpulse(frames);
    std::vector<float> shortOut(frames * 2, 0.0f);
    std::vector<float> longOut(frames * 2, 0.0f);

    RiserReverbEffect shortAttack;
    shortAttack.setSampleRate(kSampleRate);
    shortAttack.setParam(0, 100.0f);
    shortAttack.setParam(5, 1.0f);
    shortAttack.process(input.data(), shortOut.data(), frames);

    RiserReverbEffect longAttack;
    longAttack.setSampleRate(kSampleRate);
    longAttack.setParam(0, 3000.0f);
    longAttack.setParam(5, 1.0f);
    longAttack.process(input.data(), longOut.data(), frames);

    EXPECT_GT(energy(shortOut, 0, kSampleRate / 3), energy(longOut, 0, kSampleRate / 3));
}

TEST(GuitarDelayReverb, HallAndNewReverbsPreserveStereoEnergy) {
    constexpr int frames = kSampleRate;
    auto input = makeStereoTone(frames);
    std::vector<float> output(frames * 2, 0.0f);

    HallReverbEffect hall;
    hall.setSampleRate(kSampleRate);
    hall.setParam(HallReverbEffect::PARAM_MIX, 0.7f);
    hall.process(input.data(), output.data(), frames);
    expectFinite(output);
    EXPECT_GT(stereoDifferenceEnergy(output), 0.001f);

    SpringReverbEffect spring;
    spring.setSampleRate(kSampleRate);
    spring.setParam(SpringReverbEffect::PARAM_MIX, 0.7f);
    spring.process(input.data(), output.data(), frames);
    expectFinite(output);
    EXPECT_GT(stereoDifferenceEnergy(output), 0.001f);

    PlateReverbEffect plate;
    plate.setSampleRate(kSampleRate);
    plate.setParam(PlateReverbEffect::PARAM_MIX, 0.7f);
    plate.process(input.data(), output.data(), frames);
    expectFinite(output);
    EXPECT_GT(stereoDifferenceEnergy(output), 0.001f);
}

TEST(GuitarDelayReverb, ShimmerReverbProducesPitchShiftedEnergy) {
    constexpr int frames = kSampleRate * 2;
    auto input = makeStereoTone(frames);
    std::vector<float> output(frames * 2, 0.0f);

    ShimmerReverbEffect shimmer;
    shimmer.setSampleRate(kSampleRate);
    shimmer.setParam(ShimmerReverbEffect::PARAM_PITCH_SEMITONES, 12.0f);
    shimmer.setParam(ShimmerReverbEffect::PARAM_SHIMMER_AMOUNT, 0.8f);
    shimmer.setParam(ShimmerReverbEffect::PARAM_MIX, 0.8f);
    shimmer.process(input.data(), output.data(), frames);

    expectFinite(output);
    EXPECT_GT(energy(output, kSampleRate / 2, frames), 0.001f);
}

TEST(GuitarDelayReverb, EffectChainGlobalBypassSettlesToDryOutput) {
    constexpr int frames = 32;
    EffectChain chain;
    chain.setSampleRate(1000);
    ASSERT_TRUE(chain.addEffect(EffectType::DELAY));
    chain.setParameter(0, 0, 20.0f);
    chain.setParameter(0, 1, 0.0f);
    chain.setParameter(0, 2, 1.0f);

    auto input = makeStereoTone(frames);
    std::vector<float> output(frames * 2, 0.0f);

    chain.setGlobalBypass(true);
    for (int i = 0; i < 200; ++i) {
        chain.process(input.data(), output.data(), frames);
    }

    EXPECT_TRUE(chain.getGlobalBypass());
    for (size_t i = 0; i < input.size(); ++i) {
        EXPECT_NEAR(output[i], input[i], 1.0e-5f);
    }
}

TEST(GuitarDelayReverb, EffectChainGlobalBypassDoesNotModifyPerEffectBypass) {
    EffectChain chain;
    chain.setSampleRate(kSampleRate);
    ASSERT_TRUE(chain.addEffect(EffectType::REVERB));

    chain.setBypass(0, true);
    chain.setGlobalBypass(true);
    EXPECT_TRUE(chain.getBypass(0));
    EXPECT_TRUE(chain.getGlobalBypass());

    chain.setGlobalBypass(false);
    EXPECT_TRUE(chain.getBypass(0));
    EXPECT_FALSE(chain.getGlobalBypass());
}
