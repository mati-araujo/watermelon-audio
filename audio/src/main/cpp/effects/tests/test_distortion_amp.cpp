// Effects sweep — batch 1: distortion / amp-modeling effects (previously
// untested). Smoke battery (finite output, stability under hot input, silence
// stays finite) plus dry-null (mix=0 => output == input) and a check that
// DistortionEffect actually alters the signal.

#include "../DistortionEffect.h"
#include "../AmpSimulator.h"
#include "../CabinetSimulator.h"
#include "../DecimatorEffect.h"
#include "../DeciHpfEffect.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {
constexpr int kSr = 48000;
constexpr int kFrames = 4096;
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

void expectFinite(const std::vector<float>& b) {
    for (float s : b) ASSERT_TRUE(std::isfinite(s));
}

// finite on a tone (+ produces output), finite under a hot clipping input,
// finite on silence.
void smokeEffect(Effect& fx) {
    fx.setSampleRate(kSr);

    auto tone = stereoTone(kFrames, 440.0f, 0.5f);
    std::vector<float> out(tone.size(), 0.0f);
    fx.process(tone.data(), out.data(), kFrames);
    expectFinite(out);
    EXPECT_GT(energy(out), 0.0f);

    auto hot = stereoTone(kFrames, 220.0f, 4.0f);  // way over full-scale
    std::vector<float> hotOut(hot.size(), 0.0f);
    fx.process(hot.data(), hotOut.data(), kFrames);
    expectFinite(hotOut);

    std::vector<float> silence(static_cast<size_t>(kFrames) * 2, 0.0f);
    std::vector<float> silenceOut(silence.size(), 0.0f);
    fx.process(silence.data(), silenceOut.data(), kFrames);
    expectFinite(silenceOut);
}

// mix at its dry setting must pass the input through untouched. Some effects
// smooth the mix parameter, so a warm-up pass is needed to let the smoother
// reach the dry value before asserting a true pass-through.
void expectDryNull(Effect& fx, int mixParam, float dryValue) {
    fx.setSampleRate(kSr);
    fx.setParam(mixParam, dryValue);
    auto in = stereoTone(kFrames, 330.0f, 0.4f);
    std::vector<float> out(in.size(), 0.0f);
    // Tres pasadas de warm-up, no una. El smoother de mix es exponencial: con
    // target 0 se acerca a cero pero nunca llega, y lo que queda se multiplica
    // por la señal WET. Hasta WD-3.2 una pasada alcanzaba por un motivo que no
    // era el que parecia — el constructor de DistortionEffect no sembraba sus
    // ParameterSmoother, asi que el drive arrancaba en 0 y la rama wet estaba
    // practicamente en silencio durante todo el test. Con el drive en su valor
    // real desde el primer bloque, la misma cola de mix deja 7,6e-5 de residuo.
    //
    // Medido: con una pasada 7,6e-5, con tres 2,3e-23. No hay piso, es cola —
    // por eso se agregan pasadas en vez de aflojar la tolerancia.
    for (int warmUp = 0; warmUp < 3; ++warmUp) {
        fx.process(in.data(), out.data(), kFrames);
    }
    fx.process(in.data(), out.data(), kFrames);          // settled pass must be dry
    for (size_t i = 0; i < in.size(); ++i) EXPECT_NEAR(out[i], in[i], 1e-5f);
}
}  // namespace

TEST(DistortionAmpSmoke, DistortionEffectStable)  { DistortionEffect fx;  smokeEffect(fx); }
TEST(DistortionAmpSmoke, AmpSimulatorStable)      { AmpSimulator fx;      smokeEffect(fx); }
TEST(DistortionAmpSmoke, CabinetSimulatorStable)  { CabinetSimulator fx;  smokeEffect(fx); }
TEST(DistortionAmpSmoke, DecimatorEffectStable)   { DecimatorEffect fx;   smokeEffect(fx); }
TEST(DistortionAmpSmoke, DeciHpfEffectStable)     { DeciHpfEffect fx;     smokeEffect(fx); }

TEST(DistortionAmpDryNull, DistortionMixZeroIsDry) {
    DistortionEffect fx;
    expectDryNull(fx, DistortionEffect::MIX, 0.0f);
}
TEST(DistortionAmpDryNull, CabinetMixZeroIsDry) {
    CabinetSimulator fx;
    expectDryNull(fx, CabinetSimulator::MIX, 0.0f);  // MIX is 0-100 -> 0 = dry
}
TEST(DistortionAmpDryNull, DecimatorMixZeroIsDry) {
    DecimatorEffect fx;
    expectDryNull(fx, DecimatorEffect::PARAM_MIX, 0.0f);
}
TEST(DistortionAmpDryNull, DeciHpfMixZeroIsDry) {
    DeciHpfEffect fx;
    expectDryNull(fx, DeciHpfEffect::PARAM_MIX, 0.0f);
}

// Fully-wet distortion with drive must change the waveform (add harmonics /
// alter amplitude) — i.e. the nonlinear path is actually engaged.
TEST(DistortionAmpCharacter, DistortionAltersSignal) {
    DistortionEffect fx;
    fx.setSampleRate(kSr);
    fx.setParam(DistortionEffect::MIX, 1.0f);
    fx.setParam(DistortionEffect::DRIVE, 1.0f);
    fx.setParam(DistortionEffect::LEVEL, 1.0f);

    auto in = stereoTone(kFrames, 220.0f, 0.5f);
    std::vector<float> out(in.size(), 0.0f);
    fx.process(in.data(), out.data(), kFrames);

    expectFinite(out);
    float diff = 0.0f;
    for (size_t i = 0; i < in.size(); ++i) diff += std::fabs(out[i] - in[i]);
    EXPECT_GT(diff, 1.0f);  // output is materially different from the input
}
