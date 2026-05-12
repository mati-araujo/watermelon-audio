// Validates the offline limiter clamps the output peak below the threshold
// and is approximately transparent for low-amplitude content.
#include <gtest/gtest.h>
#include "Limiter.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

float bufferPeak(const std::vector<float>& buf) {
    float p = 0.0f;
    for (float s : buf) p = std::max(p, std::fabs(s));
    return p;
}

}  // namespace

TEST(Limiter, ClampsPeakBelowThreshold) {
    constexpr int SR = 48000;
    constexpr int FRAMES = 4800;  // 100 ms
    std::vector<float> buf(FRAMES * 2);
    // Two transients well above the threshold.
    for (int i = 0; i < FRAMES; ++i) {
        buf[i * 2] = (i == 1000 || i == 3000) ? 1.5f : 0.3f * std::sin(0.05f * i);
        buf[i * 2 + 1] = buf[i * 2];
    }

    wm::OfflineLimiter limiter;
    limiter.prepare(SR, /*lookahead=*/5.0f, /*attack=*/1.0f,
                    /*release=*/50.0f, /*thresh=*/0.891f);
    limiter.processStereo(buf.data(), FRAMES);

    // Peak must drop below threshold + small numerical slack.
    EXPECT_LT(bufferPeak(buf), 0.92f);
}

TEST(Limiter, TransparentBelowThreshold) {
    constexpr int SR = 48000;
    constexpr int FRAMES = 4800;
    std::vector<float> buf(FRAMES * 2);
    for (int i = 0; i < FRAMES; ++i) {
        const float s = 0.5f * std::sin(2.0f * 3.14159265f * 220.0f * i / SR);
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }
    const auto original = buf;  // copy

    wm::OfflineLimiter limiter;
    limiter.prepare(SR);
    limiter.processStereo(buf.data(), FRAMES);

    // Skip the first lookahead samples (envelope warm-up). Beyond that, gain
    // should be ~1.0 since signal is well below threshold.
    const int warm = limiter.lookaheadFrames();
    for (int i = warm + 100; i < FRAMES; ++i) {
        EXPECT_NEAR(buf[i * 2],     original[i * 2],     1e-3f);
        EXPECT_NEAR(buf[i * 2 + 1], original[i * 2 + 1], 1e-3f);
    }
}

TEST(Limiter, HandlesZeroLengthBuffer) {
    wm::OfflineLimiter limiter;
    limiter.prepare(48000);
    std::vector<float> empty;
    // Should not crash.
    limiter.processStereo(empty.data(), 0);
    SUCCEED();
}
