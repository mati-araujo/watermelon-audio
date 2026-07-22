#include <gtest/gtest.h>
#include "FDN.h"
#include <cmath>
#include <vector>
#include <algorithm>

namespace {

constexpr int kSampleRate = 48000;

struct Probe {
    std::vector<float> out;
    float medianDeltaBefore = 0.0f;
    float peakDeltaAfter = 0.0f;
    float contrast() const { return peakDeltaAfter / (medianDeltaBefore + 1e-12f); }
};

// Ring a tonal signal through the FDN and move `size` partway in, driving the
// parameter from a simulated UI thread at `updateHz`. Tonal material is what
// exposes tap discontinuities — a noise-like tail masks them, because splicing
// in a different chunk of noise looks statistically identical to the old one.
Probe runSizeMove(int updateHz, float from, float to, float gestureSeconds) {
    const int total = kSampleRate;
    const int move = kSampleRate / 2;
    const int period = kSampleRate / updateHz;

    FDN fdn;
    fdn.setSampleRate(kSampleRate);
    fdn.setDecayTime(4.0f);
    fdn.setDamping(0.5f, 0.2f);
    fdn.setModulation(0.0f);   // isolate size: no LFO tap movement
    fdn.setSize(from);

    Probe p;
    p.out.resize(total);
    for (int n = 0; n < total; ++n) {
        if (n % period == 0) {
            float t = static_cast<float>(n - move) / (gestureSeconds * kSampleRate);
            float size = t < 0.0f ? from : (t > 1.0f ? to : from + (to - from) * t);
            fdn.setSize(size);
        }
        float in = 0.2f * std::sin(2.0f * static_cast<float>(M_PI) * 330.0f
                                   * static_cast<float>(n) / kSampleRate);
        float l = 0.0f, r = 0.0f;
        fdn.process(in, in, l, r);
        p.out[n] = l;
    }

    // Baseline slew from the second before the gesture, versus the worst slew
    // during it. A tap that jumps shows up as a spike against that baseline.
    std::vector<float> deltas;
    for (int n = move - 4800; n < move; ++n)
        deltas.push_back(std::fabs(p.out[n] - p.out[n - 1]));
    std::sort(deltas.begin(), deltas.end());
    p.medianDeltaBefore = deltas[deltas.size() / 2];

    int end = move + static_cast<int>(gestureSeconds * kSampleRate) + 2400;
    for (int n = move + 1; n < end; ++n)
        p.peakDeltaAfter = std::max(p.peakDeltaAfter, std::fabs(p.out[n] - p.out[n - 1]));
    return p;
}

}  // namespace

// The regression this file exists for. `size` scales every delay tap, so before
// smoothing was wired up each UI update teleported all 8 read pointers and
// spliced unrelated tail into the output. The artifact scaled with how COARSE
// the UI updates were: at 15 Hz the worst single-sample jump reached ~34x the
// tail's own slew (~27% of full scale) — an audible click. Smoothing decouples
// the audio from the UI update rate, which is what these bounds assert.
TEST(FDN, SizeMoveArtifactIsIndependentOfUiUpdateRate) {
    float coarse = runSizeMove(15, 0.7f, 0.9f, 0.4f).contrast();
    float fine   = runSizeMove(375, 0.7f, 0.9f, 0.4f).contrast();

    // Unsmoothed, coarse updates were ~5x worse than fine ones. Smoothed, the
    // glide trajectory is the same regardless of how often the UI pushes.
    EXPECT_LT(coarse, fine * 1.5f)
        << "size artifact scales with UI update rate — smoothing is not applied "
           "(coarse=" << coarse << "x, fine=" << fine << "x)";
}

TEST(FDN, CoarseSizeMoveDoesNotSpliceTheTail) {
    Probe p = runSizeMove(15, 0.7f, 0.9f, 0.4f);
    // Pre-fix this was ~34x. The residual is the legitimate Doppler glide of
    // the taps, which is inherent to changing size and must not be "fixed".
    EXPECT_LT(p.contrast(), 12.0f)
        << "peak slew " << p.peakDeltaAfter << " vs median " << p.medianDeltaBefore;
}

TEST(FDN, SizeStillReachesItsTarget) {
    // Smoothing must not turn into a permanent offset: a settled FDN has to
    // sound like one that was configured at the target size all along.
    FDN glided, direct;
    for (FDN* f : {&glided, &direct}) {
        f->setSampleRate(kSampleRate);
        f->setDecayTime(4.0f);
        f->setDamping(0.5f, 0.2f);
        f->setModulation(0.0f);
    }
    glided.setSize(0.5f);
    direct.setSize(0.9f);

    // Let the glide finish (30 ms smoothing, so 500 ms is many time constants).
    glided.setSize(0.9f);
    for (int n = 0; n < kSampleRate / 2; ++n) {
        float l, r;
        glided.process(0.0f, 0.0f, l, r);
        direct.process(0.0f, 0.0f, l, r);
    }

    // Same excitation into both; outputs must now agree.
    double diff = 0.0, energy = 0.0;
    for (int n = 0; n < 4800; ++n) {
        float in = (n == 0) ? 1.0f : 0.0f;
        float gl = 0.0f, gr = 0.0f, dl = 0.0f, dr = 0.0f;
        glided.process(in, in, gl, gr);
        direct.process(in, in, dl, dr);
        diff += std::fabs(gl - dl);
        energy += std::fabs(dl);
    }
    EXPECT_LT(diff, energy * 0.01) << "glided size never converged on its target";
}

// Arrival time of the first echo is a direct readout of the shortest delay
// tap, and therefore of the size the audio thread is actually using. Note this
// cannot be tested by diffing against a fresh FDN: reset() deliberately only
// clears the delay lines, so biquad state and LFO phase survive it.
TEST(FDN, ResetSnapsSizeInsteadOfGliding) {
    FDN fdn;
    fdn.setSampleRate(kSampleRate);
    fdn.setModulation(0.0f);
    fdn.setDamping(0.0f, 0.0f);   // keep the impulse edge intact

    fdn.setSize(0.3f);
    fdn.setSize(0.9f);            // target moves; smoother still sits at 0.3
    fdn.reset();                  // no tail survives, so this must snap

    int firstEcho = -1;
    for (int n = 0; n < 4000 && firstEcho < 0; ++n) {
        float in = (n == 0) ? 1.0f : 0.0f;
        float l = 0.0f, r = 0.0f;
        fdn.process(in, in, l, r);
        if (std::fabs(l) > 1e-3f) firstEcho = n;
    }

    // Shortest tap is 29.7 ms; at size 0.9 that is 1283 samples. A smoother
    // left mid-glide starts the tap short and sweeps it out, landing ~1155.
    const int expected = static_cast<int>(29.7f * 0.9f * kSampleRate / 1000.0f);
    EXPECT_NEAR(firstEcho, expected, 8)
        << "first echo at " << firstEcho << ", expected ~" << expected
        << " — reset() left the size smoother mid-glide";
}

TEST(FDN, OutputStaysFiniteAcrossAnAggressiveSizeSweep) {
    FDN fdn;
    fdn.setSampleRate(kSampleRate);
    fdn.setDecayTime(8.0f);
    fdn.setDamping(0.2f, 0.2f);
    fdn.setModulation(1.0f);
    fdn.setSize(0.1f);

    for (int n = 0; n < kSampleRate * 2; ++n) {
        // Slam size between the extremes every 5 ms.
        if (n % 240 == 0) fdn.setSize((n / 240) % 2 == 0 ? 1.0f : 0.1f);
        float in = 0.3f * std::sin(2.0f * static_cast<float>(M_PI) * 220.0f
                                   * static_cast<float>(n) / kSampleRate);
        float l = 0.0f, r = 0.0f;
        fdn.process(in, in, l, r);
        ASSERT_TRUE(std::isfinite(l) && std::isfinite(r)) << "non-finite at " << n;
        ASSERT_LT(std::fabs(l), 100.0f) << "runaway feedback at " << n;
    }
}
