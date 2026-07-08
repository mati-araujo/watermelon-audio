#pragma once

/**
 * RoundTripAnalysis.h
 *
 * Pure, allocation-tolerant analysis primitives for the physical loopback
 * round-trip latency measurer (Fase 5). Deliberately free of any RT-thread,
 * IAudioCallback, or backend dependency: everything here runs on the analysis
 * worker thread AFTER a capture completes, so std::vector / std::sort are fine.
 *
 * Kept header-only and dependency-free so the host test suite
 * (usb/tests/test_roundtrip_analyzer.cpp) can exercise the whole detection /
 * aggregation chain with synthetic signals — no device, no libusb.
 *
 *  - generateChirp():    Hann-windowed linear chirp stimulus (also the template)
 *  - crossCorrelate():   normalized cross-correlation → lag + confidence
 *  - aggregate...():      median / MAD with 3×MAD outlier rejection
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace watermelon_audio {
namespace usb {

struct ChirpSpec {
    int   lengthSamples = 480;    // 10 ms @ 48 kHz
    int   sampleRate    = 48000;
    float startHz       = 500.0f;
    float endHz         = 6000.0f;
    float amplitude     = 0.25f;  // ≈ −12 dBFS peak
};

/**
 * Write a Hann-windowed linear chirp (startHz→endHz) into out[0..lengthSamples).
 * Same routine generates the emitted stimulus AND the correlation template, so
 * the template is exactly what a perfect loop returns (gain aside).
 */
inline void generateChirp(float* out, const ChirpSpec& s) {
    const int N = s.lengthSamples;
    if (N <= 0) return;
    const double sr = (s.sampleRate > 0) ? s.sampleRate : 48000.0;
    const double dur = N / sr;
    const double sweep = (s.endHz - s.startHz) / dur;  // Hz per second
    for (int n = 0; n < N; ++n) {
        const double t = n / sr;
        const double phase = 2.0 * M_PI * (s.startHz * t + 0.5 * sweep * t * t);
        // Hann window; guard N==1 to avoid divide-by-zero.
        const double w = (N > 1)
            ? 0.5 * (1.0 - std::cos(2.0 * M_PI * n / (N - 1)))
            : 1.0;
        out[n] = static_cast<float>(s.amplitude * w * std::sin(phase));
    }
}

struct CorrelationResult {
    int   lagSamples = 0;    // argmax within [0, signalLen-templateLen]
    float peak       = 0.0f; // normalized peak correlation (immune to loop gain)
    float confidence = 0.0f; // peak / max sidelobe outside ±guard of the peak
    bool  valid      = false;
};

/**
 * Normalized cross-correlation of a template (length L) slid across a search
 * window (length W ≥ L). Normalization by the per-lag signal energy makes the
 * result immune to loop gain / AGC — only affects confidence, never the lag.
 *
 * @param guardSamples  samples around the peak excluded when measuring the
 *                      sidelobe floor for the confidence ratio (≈ ±2 ms).
 * @return lag of best match (0 = template starts at the window's first sample).
 */
inline CorrelationResult crossCorrelate(const float* signal, int W,
                                        const float* tmpl, int L,
                                        int guardSamples) {
    CorrelationResult r;
    if (!signal || !tmpl || L <= 0 || W < L) return r;

    double tmplEnergy = 0.0;
    for (int i = 0; i < L; ++i) tmplEnergy += static_cast<double>(tmpl[i]) * tmpl[i];
    const double tmplNorm = std::sqrt(tmplEnergy);
    if (tmplNorm <= 0.0) return r;

    const int lags = W - L + 1;
    std::vector<float> corr(static_cast<size_t>(lags), 0.0f);
    for (int d = 0; d < lags; ++d) {
        double dot = 0.0, sigEnergy = 0.0;
        for (int i = 0; i < L; ++i) {
            const double s = signal[d + i];
            dot += s * tmpl[i];
            sigEnergy += s * s;
        }
        const double denom = std::sqrt(sigEnergy) * tmplNorm;
        corr[static_cast<size_t>(d)] = denom > 0.0
            ? static_cast<float>(dot / denom) : 0.0f;
    }

    int best = 0;
    float peak = corr[0];
    for (int d = 1; d < lags; ++d) {
        if (corr[static_cast<size_t>(d)] > peak) {
            peak = corr[static_cast<size_t>(d)];
            best = d;
        }
    }

    float sidelobe = 0.0f;
    for (int d = 0; d < lags; ++d) {
        if (std::abs(d - best) <= guardSamples) continue;
        const float a = std::abs(corr[static_cast<size_t>(d)]);
        if (a > sidelobe) sidelobe = a;
    }

    r.lagSamples = best;
    r.peak = peak;
    r.confidence = (sidelobe > 1e-6f)
        ? peak / sidelobe
        : (peak > 0.0f ? 1.0e6f : 0.0f);  // no sidelobe = perfectly clean
    r.valid = true;
    return r;
}

/** Median of a copy of @p v (v is taken by value so the caller's order is kept). */
inline float medianOf(std::vector<float> v) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return (n % 2 == 1) ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

struct Aggregate {
    float median = 0.0f;
    float mad    = 0.0f;   // median absolute deviation (robust jitter)
    float minV   = 0.0f;
    float maxV   = 0.0f;
    int   count  = 0;      // samples kept after outlier rejection
};

/**
 * Median + MAD with a single 3×MAD outlier-rejection pass (spec 5.2). Values
 * beyond 3×MAD of the median are dropped and the statistics recomputed on the
 * survivors. A near-zero MAD keeps everything (no spurious rejection).
 */
inline Aggregate aggregateWithOutlierRejection(const std::vector<float>& vals) {
    Aggregate a;
    if (vals.empty()) return a;

    const float med = medianOf(vals);
    std::vector<float> dev;
    dev.reserve(vals.size());
    for (float v : vals) dev.push_back(std::abs(v - med));
    const float mad = medianOf(dev);

    std::vector<float> kept;
    kept.reserve(vals.size());
    const float thresh = 3.0f * mad;
    for (float v : vals) {
        if (mad <= 1e-6f || std::abs(v - med) <= thresh) kept.push_back(v);
    }
    if (kept.empty()) kept = vals;

    const float med2 = medianOf(kept);
    std::vector<float> dev2;
    dev2.reserve(kept.size());
    for (float v : kept) dev2.push_back(std::abs(v - med2));

    a.median = med2;
    a.mad    = medianOf(dev2);
    a.minV   = *std::min_element(kept.begin(), kept.end());
    a.maxV   = *std::max_element(kept.begin(), kept.end());
    a.count  = static_cast<int>(kept.size());
    return a;
}

}  // namespace usb
}  // namespace watermelon_audio
