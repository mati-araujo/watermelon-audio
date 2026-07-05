// ============================================================================
// bench_mix_into — micro-benchmark for TrackBuffer::mixInto (F0 metric).
//
// NOT a CI gate: a PR-time measurement to show the effect of QW-1 (unity-speed
// fast path) and QW-2 (per-block gain ramp). Runs host-side (x86), so the
// absolute numbers are a proxy, not the on-device budget — but the RELATIVE
// speed 1.0 (fast path) vs speed 1.5 (interpolating slow path) and the 8- vs
// 16-track scaling carry over.
//
// Build & run (from looper/tests):
//   cmake -S . -B build && cmake --build build --config Release
//   ./build/Release/bench_mix_into            (or build/bench_mix_into)
//
// Budget reference from the plan: 16 tracks should stay < 25% of a 10 ms
// callback on a mid-range core. We print the % of a 10 ms callback each
// configuration consumes for the chosen block size.
// ============================================================================
#include "TrackBuffer.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kLoopFrames = 96000;   // 2 s loop — realistic live take
constexpr int kBlock = 512;          // ~10.7 ms callback @ 48k
constexpr int kWarmup = 64;
constexpr int kIters = 20000;        // mixInto calls timed, per track

// Fill a track with a mildly complex signal so the interpolator has real work.
void prepareTrack(TrackBuffer& tb, int index) {
    tb.allocate(kLoopFrames, kSampleRate);
    for (int i = 0; i < kLoopFrames; ++i) {
        const float ph = 2.0f * static_cast<float>(M_PI) * i / kLoopFrames;
        const float s = 0.4f * std::sin(ph * (3 + index))
                      + 0.2f * std::sin(ph * (7 + index));
        tb.writeFrame(s, s * 0.9f);
    }
    tb.finalizeRecording();
    tb.setPlaying(true);
    tb.setVolume(0.8f);
    tb.setPan(-0.5f + 0.1f * index);   // spread pans so gains differ per track
    tb.setMuted(false);
}

double benchConfig(int numTracks, float speed) {
    std::vector<TrackBuffer> tracks(numTracks);
    for (int t = 0; t < numTracks; ++t) {
        prepareTrack(tracks[t], t);
        tracks[t].setSpeed(speed);
    }
    std::vector<float> mix(static_cast<size_t>(kBlock) * 2, 0.0f);

    for (int w = 0; w < kWarmup; ++w) {
        std::fill(mix.begin(), mix.end(), 0.0f);
        for (int t = 0; t < numTracks; ++t) tracks[t].mixInto(mix.data(), kBlock);
    }

    volatile float sink = 0.0f;
    const auto start = std::chrono::steady_clock::now();
    for (int it = 0; it < kIters; ++it) {
        std::fill(mix.begin(), mix.end(), 0.0f);
        for (int t = 0; t < numTracks; ++t) tracks[t].mixInto(mix.data(), kBlock);
        sink += mix[0];
    }
    const auto end = std::chrono::steady_clock::now();
    (void)sink;

    const double totalNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    return totalNs / kIters;   // ns per callback (all tracks mixed)
}

}  // namespace

int main() {
    std::printf("TrackBuffer::mixInto benchmark  (block=%d frames, loop=%d, host x86)\n",
                kBlock, kLoopFrames);
    std::printf("%-8s %-8s %14s %12s\n", "tracks", "speed", "ns/callback", "% of 10ms");
    std::printf("---------------------------------------------------------\n");

    const double callbackBudgetNs = 10.0e6;  // 10 ms
    const int trackCounts[] = {8, 16};
    const float speeds[] = {1.0f, 1.5f};
    for (int tc : trackCounts) {
        for (float sp : speeds) {
            const double nsPerCb = benchConfig(tc, sp);
            std::printf("%-8d %-8.2f %14.0f %11.1f%%\n",
                        tc, sp, nsPerCb, 100.0 * nsPerCb / callbackBudgetNs);
        }
    }
    std::printf("\nspeed 1.00 exercises the QW-1 unity fast path; 1.50 the "
                "Catmull-Rom slow path.\n");
    return 0;
}
