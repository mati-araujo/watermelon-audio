// Fase 5 — End-to-end host test for the RoundTripMeasurer state machine.
//
// Drives the measurer as an IAudioCallback through a synthetic analog loop: the
// input handed to each callback is the output emitted D samples earlier, scaled
// by a loop gain and buried in a little noise. Exercises the real calibration →
// measuring → analyzing → complete path and the worker thread — no device.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <random>
#include <thread>
#include <vector>

#include "../RoundTripMeasurer.h"

using watermelon_audio::usb::RoundTripMeasurer;
using Phase = watermelon_audio::usb::RoundTripMeasurer::Phase;
using Error = watermelon_audio::usb::RoundTripMeasurer::Error;

namespace {

constexpr int kSr = 48000;
constexpr int kBlock = 256;
constexpr int kChannels = 2;

// A synthetic loopback: feeds each callback the output emitted `delay` frames
// ago × gain (+ optional noise). `feedSignal=false` forces silent input.
struct LoopbackHarness {
    RoundTripMeasurer& m;
    int delay;
    float gain;
    float noiseStd;
    bool feedSignal;

    std::vector<float> history;   // mono output history (ch0)
    int64_t processed = 0;
    std::mt19937 rng{42};

    LoopbackHarness(RoundTripMeasurer& measurer, int delaySamples, float loopGain,
                    float noise, bool feed)
        : m(measurer), delay(delaySamples), gain(loopGain), noiseStd(noise),
          feedSignal(feed) {}

    void step() {
        std::vector<float> out(static_cast<size_t>(kBlock * kChannels), 0.0f);
        std::vector<float> in(static_cast<size_t>(kBlock * kChannels), 0.0f);
        // stddev must be > 0 even if never sampled (libstdc++ asserts in the
        // ctor); the > 0 gate below keeps no-noise runs noise-free.
        std::normal_distribution<float> noise(0.0f, noiseStd > 0.0f ? noiseStd : 1.0f);
        for (int i = 0; i < kBlock; ++i) {
            const int64_t src = processed + i - delay;
            float s = 0.0f;
            if (feedSignal && src >= 0 && src < static_cast<int64_t>(history.size())) {
                s = history[static_cast<size_t>(src)] * gain;
            }
            if (feedSignal && noiseStd > 0.0f) s += noise(rng);
            for (int c = 0; c < kChannels; ++c) in[static_cast<size_t>(i * kChannels + c)] = s;
        }
        m.onAudioReady(out.data(), in.data(), kBlock);
        for (int i = 0; i < kBlock; ++i) {
            history.push_back(out[static_cast<size_t>(i * kChannels)]);
        }
        processed += kBlock;
    }
};

RoundTripMeasurer::StartParams params(int burst = 8, int interval = 150,
                                      int search = 120) {
    RoundTripMeasurer::StartParams p;
    p.sampleRate = kSr;
    p.outChannels = kChannels;
    p.inChannels = kChannels;
    p.config.burstCount = burst;
    p.config.burstIntervalMs = interval;
    p.config.amplitude = 0.25f;
    p.config.searchWindowMs = search;
    return p;
}

// Drive the harness until the measurer reaches a terminal phase or we hit a cap.
Phase driveToTerminal(LoopbackHarness& h, int maxBlocks = 20000) {
    for (int i = 0; i < maxBlocks; ++i) {
        Phase p = h.m.poll().phase;
        if (p == Phase::COMPLETE || p == Phase::ERROR) return p;
        h.step();
        // Give the worker thread a chance once we've stopped feeding.
        if (h.m.poll().phase == Phase::ANALYZING) {
            // ESTIMULO: simula el intervalo real entre transferencias USB.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    // Final settle for the worker.
    for (int i = 0; i < 200; ++i) {
        Phase p = h.m.poll().phase;
        if (p == Phase::COMPLETE || p == Phase::ERROR) return p;
        // WAIT-OK: polling — bucle con techo por iteraciones (200), con salida
        //          temprana. El lint solo reconoce los techos por deadline.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return h.m.poll().phase;
}

TEST(RoundTripMeasurer, RecoversSyntheticLatency) {
    RoundTripMeasurer m;
    ASSERT_TRUE(m.start(params()));
    const int delay = 2000;  // 41.67 ms
    LoopbackHarness h(m, delay, /*gain=*/0.5f, /*noiseStd=*/0.0005f, /*feed=*/true);

    Phase term = driveToTerminal(h);
    auto snap = m.poll();
    ASSERT_EQ(term, Phase::COMPLETE) << "error=" << static_cast<int>(snap.result.error);
    const float expectedMs = 1000.0f * delay / kSr;
    EXPECT_NEAR(snap.result.medianMs, expectedMs, 0.3f);
    EXPECT_GE(snap.result.validBursts, 7);
    EXPECT_LT(snap.result.madMs, 0.5f);
    m.cancel();
}

TEST(RoundTripMeasurer, GainInvariantRecovery) {
    RoundTripMeasurer m;
    ASSERT_TRUE(m.start(params()));
    const int delay = 900;
    LoopbackHarness h(m, delay, /*gain=*/0.05f, /*noiseStd=*/0.0002f, /*feed=*/true);
    ASSERT_EQ(driveToTerminal(h), Phase::COMPLETE);
    EXPECT_NEAR(m.poll().result.medianMs, 1000.0f * delay / kSr, 0.3f);
    m.cancel();
}

TEST(RoundTripMeasurer, NoSignalErrors) {
    RoundTripMeasurer m;
    ASSERT_TRUE(m.start(params()));
    LoopbackHarness h(m, 2000, 0.5f, 0.0f, /*feedSignal=*/false);  // silent input
    ASSERT_EQ(driveToTerminal(h), Phase::ERROR);
    EXPECT_EQ(m.poll().result.error, Error::NO_SIGNAL);
    m.cancel();
}

TEST(RoundTripMeasurer, CancelReturnsToIdle) {
    RoundTripMeasurer m;
    ASSERT_TRUE(m.start(params()));
    LoopbackHarness h(m, 2000, 0.5f, 0.0f, true);
    for (int i = 0; i < 50; ++i) h.step();
    m.cancel();
    EXPECT_EQ(m.poll().phase, Phase::IDLE);
    // Can start again after a cancel.
    EXPECT_TRUE(m.start(params()));
    m.cancel();
}

TEST(RoundTripMeasurer, RejectsDoubleStart) {
    RoundTripMeasurer m;
    ASSERT_TRUE(m.start(params()));
    EXPECT_FALSE(m.start(params()));  // already active
    m.cancel();
}

TEST(RoundTripMeasurer, SoftwareLatencyBreakdown) {
    RoundTripMeasurer m;
    ASSERT_TRUE(m.start(params()));
    const int delay = 1500;
    LoopbackHarness h(m, delay, 0.5f, 0.0003f, true);
    // Feed a plausible software-latency reading each "poll" while measuring.
    for (int i = 0; i < 20000; ++i) {
        Phase p = m.poll().phase;
        if (p == Phase::COMPLETE || p == Phase::ERROR) break;
        if (p == Phase::MEASURING) m.noteSoftwareLatency(15.0f, 12.0f);
        h.step();
    }
    for (int i = 0; i < 200 && m.poll().phase == Phase::ANALYZING; ++i)
        // WAIT-OK: polling — bucle con techo por iteraciones (200), con salida
        //          temprana. El lint solo reconoce los techos por deadline.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto r = m.poll().result;
    ASSERT_EQ(m.poll().phase, Phase::COMPLETE);
    EXPECT_NEAR(r.softwareOutputMs, 15.0f, 0.01f);
    EXPECT_NEAR(r.softwareInputMs, 12.0f, 0.01f);
    // residual = median − (out+in); median ≈ 31.25 ms, out+in = 27 → ~4.25.
    EXPECT_NEAR(r.residualMs, r.medianMs - 27.0f, 0.01f);
    m.cancel();
}

}  // namespace
