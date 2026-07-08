// Fase 5 — Host tests for the round-trip loopback analysis primitives.
//
// The detection/aggregation logic is pure (RoundTripAnalysis.h), so we exercise
// it with synthetic captures: template delayed by D, passed through a simple
// channel (1st-order LPF), scaled by an arbitrary loop gain, buried in white
// noise at a target SNR. The analyzer must recover D within ±1 sample at
// sane SNRs and NEVER report a confident-but-wrong value at low SNR.

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <vector>

#include "../RoundTripAnalysis.h"

using watermelon_audio::usb::ChirpSpec;
using watermelon_audio::usb::generateChirp;
using watermelon_audio::usb::crossCorrelate;
using watermelon_audio::usb::CorrelationResult;
using watermelon_audio::usb::aggregateWithOutlierRejection;
using watermelon_audio::usb::medianOf;

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChirpLen   = 480;                 // 10 ms
constexpr int kWindow     = 12000;               // 250 ms search window
constexpr int kGuard      = kSampleRate / 500;   // ±2 ms

ChirpSpec defaultSpec() {
    ChirpSpec s;
    s.lengthSamples = kChirpLen;
    s.sampleRate = kSampleRate;
    s.startHz = 500.0f;
    s.endHz = 6000.0f;
    s.amplitude = 0.25f;
    return s;
}

float rms(const float* x, int n) {
    double e = 0.0;
    for (int i = 0; i < n; ++i) e += double(x[i]) * x[i];
    return static_cast<float>(std::sqrt(e / std::max(1, n)));
}

// Build a capture window: template placed at delay D, run through a 1st-order
// low-pass (channel), scaled by loopGain, plus white noise at snrDb relative to
// the placed chirp's RMS.
std::vector<float> makeCapture(const std::vector<float>& tmpl, int D,
                               float snrDb, float loopGain, float lpfAlpha,
                               uint32_t seed) {
    std::vector<float> win(kWindow, 0.0f);
    const int L = static_cast<int>(tmpl.size());
    // Place chirp through 1st-order LPF: y[n] = a*x[n] + (1-a)*y[n-1].
    float y = 0.0f;
    for (int i = 0; i < L && (D + i) < kWindow; ++i) {
        y = lpfAlpha * tmpl[i] + (1.0f - lpfAlpha) * y;
        win[D + i] = loopGain * y;
    }
    const float sigRms = rms(tmpl.data(), L) * loopGain;
    const float noiseStd = sigRms / std::pow(10.0f, snrDb / 20.0f);
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0f, noiseStd);
    for (int i = 0; i < kWindow; ++i) win[i] += noise(rng);
    return win;
}

TEST(RoundTripChirp, IsBoundedAndWindowed) {
    std::vector<float> chirp(kChirpLen, 0.0f);
    generateChirp(chirp.data(), defaultSpec());
    for (float v : chirp) EXPECT_LE(std::abs(v), 0.25f + 1e-4f);
    // Hann window forces the endpoints to (near) zero.
    EXPECT_NEAR(chirp.front(), 0.0f, 1e-4f);
    EXPECT_NEAR(chirp.back(), 0.0f, 1e-3f);
    // There is real energy in the middle.
    EXPECT_GT(rms(chirp.data(), kChirpLen), 0.05f);
}

TEST(RoundTripCorrelate, RecoversDelayAtHighSnr) {
    std::vector<float> chirp(kChirpLen, 0.0f);
    generateChirp(chirp.data(), defaultSpec());
    const int D = 1234;
    auto win = makeCapture(chirp, D, /*snrDb=*/40.0f, /*gain=*/0.5f,
                           /*lpfAlpha=*/0.6f, /*seed=*/1);
    auto r = crossCorrelate(win.data(), kWindow, chirp.data(), kChirpLen, kGuard);
    ASSERT_TRUE(r.valid);
    EXPECT_NEAR(r.lagSamples, D, 1);
    EXPECT_GT(r.confidence, 3.0f);
}

TEST(RoundTripCorrelate, RecoversDelayAtModerateSnr) {
    std::vector<float> chirp(kChirpLen, 0.0f);
    generateChirp(chirp.data(), defaultSpec());
    const int D = 5000;
    auto win = makeCapture(chirp, D, /*snrDb=*/20.0f, /*gain=*/0.2f,
                           /*lpfAlpha=*/0.5f, /*seed=*/7);
    auto r = crossCorrelate(win.data(), kWindow, chirp.data(), kChirpLen, kGuard);
    ASSERT_TRUE(r.valid);
    EXPECT_NEAR(r.lagSamples, D, 1);
    EXPECT_GT(r.confidence, 3.0f);
}

TEST(RoundTripCorrelate, GainInvariance) {
    // Normalized correlation must return the same lag regardless of loop gain.
    std::vector<float> chirp(kChirpLen, 0.0f);
    generateChirp(chirp.data(), defaultSpec());
    const int D = 3333;
    for (float gain : {0.05f, 0.5f, 2.0f}) {
        auto win = makeCapture(chirp, D, 40.0f, gain, 0.7f, /*seed=*/3);
        auto r = crossCorrelate(win.data(), kWindow, chirp.data(), kChirpLen, kGuard);
        ASSERT_TRUE(r.valid);
        EXPECT_NEAR(r.lagSamples, D, 1) << "gain=" << gain;
    }
}

TEST(RoundTripCorrelate, LowSnrNeverConfidentlyWrong) {
    // At 10 dB SNR the detector may miss, but it must not report a lag far from
    // truth WITH high confidence — a false positive is the dangerous failure.
    std::vector<float> chirp(kChirpLen, 0.0f);
    generateChirp(chirp.data(), defaultSpec());
    const int D = 4096;
    int confidentWrong = 0;
    for (uint32_t seed = 100; seed < 130; ++seed) {
        auto win = makeCapture(chirp, D, /*snrDb=*/10.0f, 0.15f, 0.5f, seed);
        auto r = crossCorrelate(win.data(), kWindow, chirp.data(), kChirpLen, kGuard);
        const bool wrong = std::abs(r.lagSamples - D) > 2;
        if (wrong && r.confidence > 3.0f) ++confidentWrong;
    }
    EXPECT_EQ(confidentWrong, 0);
}

TEST(RoundTripAggregate, MedianRejectsOutliers) {
    // 8 bursts at D=10.0 ms, 2 wild outliers at D+... → median stays 10.0, the
    // two are excluded.
    std::vector<float> vals = {10.0f, 10.1f, 9.9f, 10.0f, 10.05f,
                               9.95f, 10.0f, 10.02f, 27.0f, 26.5f};
    auto a = aggregateWithOutlierRejection(vals);
    EXPECT_NEAR(a.median, 10.0f, 0.15f);
    EXPECT_EQ(a.count, 8);
    EXPECT_LT(a.maxV, 11.0f);
}

TEST(RoundTripAggregate, EmptyIsSafe) {
    auto a = aggregateWithOutlierRejection({});
    EXPECT_EQ(a.count, 0);
    EXPECT_EQ(a.median, 0.0f);
}

TEST(RoundTripAggregate, MedianBasics) {
    EXPECT_NEAR(medianOf({3.0f, 1.0f, 2.0f}), 2.0f, 1e-6f);
    EXPECT_NEAR(medianOf({4.0f, 1.0f, 2.0f, 3.0f}), 2.5f, 1e-6f);
}

}  // namespace
