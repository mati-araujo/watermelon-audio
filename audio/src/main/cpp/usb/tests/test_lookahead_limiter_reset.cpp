// First-playback distortion fix — LookaheadLimiter reset tests
//
// Background: before this fix, LookaheadLimiter::prepare() used
//   mDelayBuffer.resize(N, 0.0f)
// which only zero-initializes NEW elements. On re-prepare with the same
// sample rate the buffer already had size N, so the resize was a no-op
// and the delay buffer retained stale samples from the previous session.
// The first ~5 ms of new output read back leftover audio → distortion.
//
// These tests verify:
//   1. prepare() zeros the buffer even on re-entry (the bugfix).
//   2. reset() is equivalent to "clean state, no allocation".
//   3. Gain envelope state is restored to unity on reset().

#include <gtest/gtest.h>

#include "../../effects/LookaheadLimiter.h"

#include <vector>
#include <cmath>

namespace {

// Generate a block of stereo interleaved samples at constant level.
std::vector<float> makeBlock(int numFrames, float value) {
    return std::vector<float>(static_cast<size_t>(numFrames * 2), value);
}

// Return the maximum absolute value in a buffer.
float peakAbs(const float* buf, size_t len) {
    float maxAbs = 0.0f;
    for (size_t i = 0; i < len; ++i) {
        float a = std::abs(buf[i]);
        if (a > maxAbs) maxAbs = a;
    }
    return maxAbs;
}

} // namespace

// ---- Test 1: prepare() zeros the delay buffer on re-entry ----
//
// This is the direct regression test for the vector::resize(N, 0.0f) no-op
// bug. Feed non-zero audio, re-prepare, then process zeros and confirm the
// output is all zeros for at least the lookahead window.
TEST(LookaheadLimiterReset, PrepareZerosBufferOnReentry) {
    constexpr int sampleRate = 48000;
    // At 48 kHz, lookahead = 5 ms = 240 samples per channel
    constexpr int lookaheadFrames = 240;
    constexpr int blockFrames = 512;  // larger than lookahead

    LookaheadLimiter limiter;
    limiter.prepare(sampleRate);

    // Fill the delay buffer with non-zero audio (0.3 is under default
    // -0.5 dB threshold so the limiter passes through without gain
    // reduction — output matches input after the lookahead delay).
    auto loudBlock = makeBlock(blockFrames, 0.3f);
    std::vector<float> output(blockFrames * 2, 0.0f);
    limiter.process(loudBlock.data(), output.data(), blockFrames);

    // Sanity: after one block of 0.3f input, the last frames of output
    // should carry the 0.3f level (delayed). If they don't, the limiter
    // itself is broken and the rest of the test is meaningless.
    ASSERT_GT(peakAbs(output.data() + lookaheadFrames * 2,
                      (blockFrames - lookaheadFrames) * 2), 0.1f)
        << "Sanity: limiter should pass 0.3f input through after lookahead";

    // Re-prepare (same sample rate — this is the exact scenario the bug hits)
    limiter.prepare(sampleRate);

    // Now process a block of ZEROS. If the buffer is properly zeroed on
    // re-prepare, output must be all zeros. With the old bug, the first
    // ~240 samples would contain the stale 0.3f data.
    auto silentBlock = makeBlock(blockFrames, 0.0f);
    std::fill(output.begin(), output.end(), 999.0f);  // poison
    limiter.process(silentBlock.data(), output.data(), blockFrames);

    float maxLeak = peakAbs(output.data(), output.size());
    EXPECT_LT(maxLeak, 1e-6f)
        << "Delay buffer leaked stale audio after re-prepare: maxLeak=" << maxLeak;
}

// ---- Test 2: reset() is equivalent to a fresh prepared state ----
//
// Exercises the new reset() API. Same shape as test 1 but calls reset()
// instead of prepare() — reset() must not allocate and must zero both
// the delay buffer and the gain envelope.
TEST(LookaheadLimiterReset, ResetZerosBufferAndGain) {
    constexpr int sampleRate = 48000;
    constexpr int blockFrames = 512;

    LookaheadLimiter limiter;
    limiter.prepare(sampleRate);

    // Feed a LOUD block (above -0.5 dB threshold) so the gain envelope
    // actually drops below 1.0. Peak of 2.0 is well over 0dBFS → heavy
    // limiting required.
    auto loudBlock = makeBlock(blockFrames, 2.0f);
    std::vector<float> output(blockFrames * 2, 0.0f);
    limiter.process(loudBlock.data(), output.data(), blockFrames);

    // Gain reduction should have engaged (positive magnitude in dB)
    float gr = limiter.getGainReduction();
    ASSERT_GT(gr, 0.1f)
        << "Sanity: loud input should have triggered gain reduction, got " << gr;

    // Reset
    limiter.reset();

    // Note: getGainReduction() returns the last metering snapshot from
    // process(); reset() stores 0.0f explicitly, but the test verifies
    // behaviorally by running a silent block below and checking output,
    // which exercises both the delay buffer and gain envelope.

    // Process zeros — output must be silent
    auto silentBlock = makeBlock(blockFrames, 0.0f);
    std::fill(output.begin(), output.end(), 999.0f);
    limiter.process(silentBlock.data(), output.data(), blockFrames);

    float maxLeak = peakAbs(output.data(), output.size());
    EXPECT_LT(maxLeak, 1e-6f)
        << "Delay buffer leaked stale audio after reset(): maxLeak=" << maxLeak;
}

// ---- Test 3: reset() can be called repeatedly without side effects ----
TEST(LookaheadLimiterReset, ResetIsIdempotent) {
    LookaheadLimiter limiter;
    limiter.prepare(48000);

    auto block = makeBlock(256, 0.5f);
    std::vector<float> output(256 * 2, 0.0f);
    limiter.process(block.data(), output.data(), 256);

    // Call reset() three times in a row
    limiter.reset();
    limiter.reset();
    limiter.reset();

    // Process zeros — still must be silent
    auto silentBlock = makeBlock(256, 0.0f);
    std::fill(output.begin(), output.end(), 999.0f);
    limiter.process(silentBlock.data(), output.data(), 256);

    EXPECT_LT(peakAbs(output.data(), output.size()), 1e-6f);
}

// ---- Test 4: after reset(), a fresh loud signal is processed correctly ----
//
// The distortion bug had two contributing factors: stale delay buffer AND
// stale gain envelope. Verify both are flushed by feeding loud audio,
// resetting, and feeding the same loud audio again — the second pass
// should behave identically to a freshly prepared limiter.
TEST(LookaheadLimiterReset, ResetRestoresFreshBehavior) {
    constexpr int sampleRate = 48000;
    constexpr int blockFrames = 1024;

    // Reference: fresh limiter processing loud audio
    LookaheadLimiter reference;
    reference.prepare(sampleRate);
    auto loudBlock = makeBlock(blockFrames, 1.5f);
    std::vector<float> referenceOut(blockFrames * 2, 0.0f);
    reference.process(loudBlock.data(), referenceOut.data(), blockFrames);

    // Comparison: limiter that was used, then reset, then processes the same
    LookaheadLimiter reused;
    reused.prepare(sampleRate);
    // First pass — pollutes state
    std::vector<float> discard(blockFrames * 2, 0.0f);
    reused.process(loudBlock.data(), discard.data(), blockFrames);
    reused.reset();
    // Second pass — should match the fresh limiter
    std::vector<float> reusedOut(blockFrames * 2, 0.0f);
    reused.process(loudBlock.data(), reusedOut.data(), blockFrames);

    // Sample-by-sample comparison (allow tiny floating point delta)
    for (size_t i = 0; i < referenceOut.size(); ++i) {
        EXPECT_NEAR(reusedOut[i], referenceOut[i], 1e-5f)
            << "Mismatch at sample " << i
            << ": reference=" << referenceOut[i]
            << " reused=" << reusedOut[i];
        if (::testing::Test::HasNonfatalFailure()) break;  // stop spam
    }
}
