// DistortionEffect noise gate — envelope-based gating regression suite.
//
// The old applyGate() decided per SAMPLE (|x| < threshold*0.1 -> 0): at
// amplitudes near the threshold it zeroed the near-zero-crossing span of
// every waveform cycle, chopping the signal into buzzy fragments. The
// envelope-based gate must pass supra-threshold signals CLEANLY (no chop)
// and mute sub-threshold signals entirely.
//
// The gate lives in the WET path only (dry mix bypasses it), so all tests
// run at MIX=1.

#include "../DistortionEffect.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
constexpr int kSr = 48000;
constexpr int kFrames = 4096;
constexpr float kPi = 3.14159265358979323846f;

// threshold param 0.14 -> open threshold 0.014 linear (the preset value the
// GHW capture was chopped by).
constexpr float kGateParam = 0.14f;
constexpr float kOpenThreshold = kGateParam * 0.1f;

std::vector<float> stereoTone(int frames, float freq, float amp) {
    std::vector<float> b(static_cast<size_t>(frames) * 2, 0.0f);
    for (int i = 0; i < frames; ++i) {
        const float s = std::sin(2.0f * kPi * freq * static_cast<float>(i) / kSr) * amp;
        b[i * 2] = s;
        b[i * 2 + 1] = s;
    }
    return b;
}

float rms(const std::vector<float>& b, size_t from = 0) {
    if (from >= b.size()) return 0.0f;
    double e = 0.0;
    for (size_t i = from; i < b.size(); ++i) e += double(b[i]) * b[i];
    return static_cast<float>(std::sqrt(e / double(b.size() - from)));
}

void configureGated(DistortionEffect& fx, float gateParam) {
    fx.setSampleRate(kSr);
    fx.setParam(DistortionEffect::MIX, 1.0f);  // gate lives in the wet path
    fx.setParam(DistortionEffect::GATE_THRESHOLD, gateParam);
}

// Process in RT-sized blocks (long buffers exceed MAX_BUFFER_SIZE).
void processChunked(DistortionEffect& fx, std::vector<float>& in,
                    std::vector<float>& out) {
    constexpr int kBlock = 1024;
    const int totalFrames = static_cast<int>(in.size() / 2);
    for (int off = 0; off < totalFrames; off += kBlock) {
        const int n = std::min(kBlock, totalFrames - off);
        fx.process(in.data() + size_t(off) * 2, out.data() + size_t(off) * 2, n);
    }
}
}  // namespace

// THE regression for the audible bug: a signal at 1.5x the open threshold
// must pass with the same energy as with the gate off. The per-sample gate
// zeroed ~46% of each cycle at this amplitude (|sin| < 1/1.5), losing a
// large chunk of energy and buzzing.
TEST(DistortionGate, NearThresholdSignalPassesCleanly) {
    DistortionEffect gated;
    configureGated(gated, kGateParam);
    auto tone = stereoTone(kFrames * 2, 440.0f, kOpenThreshold * 1.5f);
    std::vector<float> out(tone.size(), 0.0f);
    processChunked(gated, tone, out);

    DistortionEffect ungated;
    configureGated(ungated, 0.0f);
    auto tone2 = stereoTone(kFrames * 2, 440.0f, kOpenThreshold * 1.5f);
    std::vector<float> outUngated(tone2.size(), 0.0f);
    processChunked(ungated, tone2, outUngated);

    // Skip the first 100 ms (gate attack + filter settling).
    const size_t tail = static_cast<size_t>(kSr / 10) * 2;
    const float gatedRms = rms(out, tail);
    const float openRms = rms(outUngated, tail);
    ASSERT_GT(openRms, 0.0f);
    EXPECT_GT(gatedRms, openRms * 0.9f);
    EXPECT_LT(gatedRms, openRms * 1.1f);
}

// A signal that never reaches the open threshold must stay muted (the gate
// decides on the envelope, so nothing leaks through cycle peaks).
TEST(DistortionGate, BelowThresholdSignalStaysMuted) {
    DistortionEffect fx;
    configureGated(fx, kGateParam);
    auto tone = stereoTone(kFrames * 4, 440.0f, kOpenThreshold * 0.7f);
    std::vector<float> out(tone.size(), 0.0f);
    processChunked(fx, tone, out);

    DistortionEffect ungated;
    configureGated(ungated, 0.0f);
    auto tone2 = stereoTone(kFrames * 4, 440.0f, kOpenThreshold * 0.7f);
    std::vector<float> outUngated(tone2.size(), 0.0f);
    processChunked(ungated, tone2, outUngated);

    const size_t tail = static_cast<size_t>(kSr / 5) * 2;
    const float openRms = rms(outUngated, tail);
    ASSERT_GT(openRms, 0.0f);
    // Muted = at least ~26 dB below the ungated level.
    EXPECT_LT(rms(out, tail), openRms * 0.05f);
}

// After a loud phase opens the gate, a sub-close-threshold tail must fade to
// silence (release), not stay open forever nor chatter.
TEST(DistortionGate, DecayClosesAfterLoudPhase) {
    DistortionEffect fx;
    configureGated(fx, kGateParam);

    auto loud = stereoTone(kFrames * 2, 440.0f, kOpenThreshold * 20.0f);
    std::vector<float> outLoud(loud.size(), 0.0f);
    processChunked(fx, loud, outLoud);
    const size_t tailLoud = static_cast<size_t>(kSr / 10) * 2;  // skip 100 ms
    ASSERT_GT(rms(outLoud, tailLoud), 0.0f);

    auto quiet = stereoTone(kFrames * 4, 440.0f, kOpenThreshold * 0.3f);
    std::vector<float> outQuiet(quiet.size(), 0.0f);
    processChunked(fx, quiet, outQuiet);

    // Last 100 ms: gate closed, output faded out.
    const size_t tail = outQuiet.size() - static_cast<size_t>(kSr / 10) * 2;
    DistortionEffect ungated;
    configureGated(ungated, 0.0f);
    auto quiet2 = stereoTone(kFrames * 4, 440.0f, kOpenThreshold * 0.3f);
    std::vector<float> outUngated(quiet2.size(), 0.0f);
    processChunked(ungated, quiet2, outUngated);
    const float openRms = rms(outUngated, tail);
    ASSERT_GT(openRms, 0.0f);
    EXPECT_LT(rms(outQuiet, tail), openRms * 0.05f);
}

TEST(DistortionGate, ZeroThresholdBypassesGate) {
    DistortionEffect fx;
    configureGated(fx, 0.0f);
    auto tone = stereoTone(kFrames, 440.0f, kOpenThreshold * 0.5f);  // very quiet
    std::vector<float> out(tone.size(), 0.0f);
    processChunked(fx, tone, out);

    const size_t tail = static_cast<size_t>(kSr / 20) * 2;
    EXPECT_GT(rms(out, tail), 0.0f);
}
