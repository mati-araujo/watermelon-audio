#pragma once

/**
 * RoundTripMeasurer.h — Fase 5 physical loopback latency measurer.
 *
 * A self-contained IAudioCallback that, when installed on a running FULL_DUPLEX
 * backend via LibusbBackend::swapCallback(), measures the real analog round-trip
 * latency (OUT→cable→IN) by emitting Hann-windowed chirps and cross-correlating
 * the captured input against the template.
 *
 * Two strictly separated planes (spec 5.2):
 *  - RT plane (onAudioReady): generates the stimulus into outputData and copies
 *    inputData into a PRE-ALLOCATED capture buffer with an absolute sample
 *    counter. Zero alloc, zero locks, zero analysis. Emits silence between
 *    bursts so a swap over live playback is glitchless.
 *  - Analysis plane (worker thread): once the capture completes, runs the
 *    correlation/statistics from RoundTripAnalysis.h and publishes the result.
 *
 * Backend-agnostic: depends only on IAudioCallback + StreamInfo, never on
 * LibusbBackend, so the same measurer can drive the Oboe path later. The backend
 * owns the callback swap (install/restore); this class only measures and reports
 * a phase that the JNI polls.
 */

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "../backends/IAudioBackend.h"  // IAudioCallback, BackendError, StreamInfo
#include "RoundTripAnalysis.h"

namespace watermelon_audio {
namespace usb {

class RoundTripMeasurer : public IAudioCallback {
public:
    enum class Phase : int {
        IDLE = 0, CALIBRATING = 1, MEASURING = 2, ANALYZING = 3,
        COMPLETE = 4, ERROR = 5,
    };
    enum class Error : int {
        NONE = 0, NO_SIGNAL = 1, CLIPPING = 2, UNRELIABLE = 3,
        REQUIRES_FULL_DUPLEX = 4, STREAM_LOST = 5, TIMEOUT = 6,
    };

    struct Config {
        int   burstCount      = 10;
        int   burstIntervalMs = 300;
        float amplitude       = 0.25f;
        int   searchWindowMs  = 250;
    };

    struct StartParams {
        int sampleRate    = 48000;
        int outChannels   = 2;
        int inChannels    = 2;
        int profile       = 0;   // context only (UsbLatencyProfile ordinal)
        int jitterBudgetMs = 0;  // context only
        Config config;
    };

    struct Result {
        float medianMs = 0.0f, madMs = 0.0f, minMs = 0.0f, maxMs = 0.0f;
        int   validBursts = 0, totalBursts = 0;
        float confidence = 0.0f;
        float softwareOutputMs = 0.0f, softwareInputMs = 0.0f, residualMs = 0.0f;
        int   sampleRate = 0, profile = 0, jitterBudgetMs = 0;
        Error error = Error::NONE;
    };

    struct Snapshot {
        Phase  phase = Phase::IDLE;
        float  progressPct = 0.0f;
        int    currentBurst = 0;
        int    totalBursts = 0;
        Result result;
    };

    RoundTripMeasurer() = default;
    ~RoundTripMeasurer() override;

    RoundTripMeasurer(const RoundTripMeasurer&) = delete;
    RoundTripMeasurer& operator=(const RoundTripMeasurer&) = delete;

    /**
     * Prepare and begin a measurement (non-RT: allocates the capture buffer and
     * spawns the analysis worker). Call BEFORE the backend swaps this in as its
     * callback. Returns false if already active.
     */
    bool start(const StartParams& params);

    /**
     * Stop and reset to IDLE (non-RT). Idempotent. Joins the worker. The caller
     * (JNI) is responsible for restoring the original backend callback first.
     */
    void cancel();

    /** Lock-free-ish snapshot for the JNI poll. */
    Snapshot poll() const;

    /**
     * Feed an averaged sample of the backend's reported software latency (L7),
     * called from the non-RT JNI poll. Single writer; accumulated for the result
     * breakdown (median − (out+in) = residual).
     */
    void noteSoftwareLatency(float outMs, float inMs);

    // ---- IAudioCallback ----
    IAudioCallback::Result onAudioReady(float* outputData, const float* inputData,
                                        int32_t numFrames) override;
    void onBackendError(BackendError error) override;
    void onStreamConfigChanged(const StreamInfo&) override {}

private:
    void analysisLoop();          // worker thread body
    void runAnalysis();           // one-shot: correlate captured bursts → result
    void setPhase(Phase p) { mPhase.store(static_cast<int>(p), std::memory_order_release); }
    Phase phase() const { return static_cast<Phase>(mPhase.load(std::memory_order_acquire)); }

    // ---- cross-thread state (atomics) ----
    std::atomic<int>   mPhase{static_cast<int>(Phase::IDLE)};
    std::atomic<int>   mErrorCode{static_cast<int>(Error::NONE)};
    std::atomic<float> mProgressPct{0.0f};
    std::atomic<int>   mCurrentBurst{0};
    std::atomic<bool>  mWorkerStop{false};
    std::atomic<bool>  mStreamLost{false};

    // Software-latency accumulation (JNI writer, worker reader).
    std::atomic<float> mSwOutSum{0.0f};
    std::atomic<float> mSwInSum{0.0f};
    std::atomic<int>   mSwCount{0};

    // ---- RT-owned state (touched only by onAudioReady between start/cancel) ----
    StartParams mParams;
    int   mChirpLen = 0;
    int   mIntervalSamples = 0;
    int   mSearchSamples = 0;
    int   mCalibrationSamples = 0;
    float mAmplitude = 0.25f;
    int   mCalibRetries = 0;
    int64_t mSampleCounter = 0;       // frames since MEASURING began
    int64_t mCalibCounter = 0;        // frames since CALIBRATING began
    int   mBurstIndex = 0;
    int64_t mNextBurstSample = 0;
    double mCalibSumSq = 0.0;         // input energy accumulator (calibration)
    float mCalibPeak = 0.0f;
    double mCalibPhase = 0.0;         // 1 kHz tone phase

    // ---- preallocated buffers ----
    std::vector<float> mTemplate;     // chirp template (mChirpLen)
    std::vector<float> mCapture;      // absolute-indexed mono input capture
    std::vector<int64_t> mEmitSample; // per-burst emit sample offset
    float mNoiseFloor = 0.0f;         // input RMS from calibration (validity gate)

    Result mResult;                   // published under phase == COMPLETE/ERROR
    std::thread mWorker;
};

}  // namespace usb
}  // namespace watermelon_audio
