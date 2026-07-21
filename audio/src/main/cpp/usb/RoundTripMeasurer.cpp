/**
 * RoundTripMeasurer.cpp — see RoundTripMeasurer.h.
 */

#include "RoundTripMeasurer.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "../platform/Logger.h"

#define LOG_TAG "RoundTripMeasurer"
#undef LOGI
#undef LOGW
#undef LOGE
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) wma::logMessage(wma::LogLevel::WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)

namespace watermelon_audio {
namespace usb {

namespace {
constexpr float kNoSignalRms   = 0.001f;   // −60 dBFS
constexpr float kClipPeak      = 0.891f;   // −1 dBFS
constexpr int   kMaxCalibRetry = 2;
constexpr float kMinConfidence = 3.0f;
constexpr float kMinPeak       = 0.30f;    // normalized correlation floor
constexpr int   kMinValidBursts = 7;
}  // namespace

RoundTripMeasurer::~RoundTripMeasurer() {
    cancel();
}

bool RoundTripMeasurer::start(const StartParams& params) {
    const Phase p = phase();
    if (p != Phase::IDLE) {
        LOGW("start() ignored: measurer already active (phase=%d)", static_cast<int>(p));
        return false;
    }
    // Join any lingering worker from a previous run before re-arming.
    mWorkerStop.store(true, std::memory_order_release);
    if (mWorker.joinable()) mWorker.join();
    mWorkerStop.store(false, std::memory_order_release);

    mParams = params;
    const int sr = std::max(8000, params.sampleRate);

    ChirpSpec spec;
    spec.sampleRate = sr;
    spec.lengthSamples = (sr / 100);  // 10 ms
    spec.amplitude = params.config.amplitude;
    mChirpLen = spec.lengthSamples;
    mTemplate.assign(static_cast<size_t>(mChirpLen), 0.0f);
    generateChirp(mTemplate.data(), spec);

    mIntervalSamples     = std::max(mChirpLen + sr / 20, params.config.burstIntervalMs * sr / 1000);
    mSearchSamples       = std::max(sr / 10, params.config.searchWindowMs * sr / 1000);
    mCalibrationSamples  = sr / 2;  // 500 ms
    mAmplitude           = params.config.amplitude;
    mCalibRetries        = 0;

    const int burstCount = std::max(1, params.config.burstCount);
    mEmitSample.assign(static_cast<size_t>(burstCount), 0);
    for (int k = 0; k < burstCount; ++k) {
        mEmitSample[static_cast<size_t>(k)] = static_cast<int64_t>(k) * mIntervalSamples;
    }
    // Capture spans all bursts + the last search window + 1 s of slack, so the
    // worker can read every window without wrap. Preallocated once (non-RT).
    const int64_t captureLen =
        mEmitSample.back() + mSearchSamples + mChirpLen + sr;
    mCapture.assign(static_cast<size_t>(captureLen), 0.0f);

    // Reset RT-owned counters.
    mSampleCounter = 0;
    mCalibCounter = 0;
    mBurstIndex = 0;
    mNextBurstSample = 0;
    mCalibSumSq = 0.0;
    mCalibPeak = 0.0f;
    mCalibPhase = 0.0;

    mResult = Result{};
    mResult.totalBursts = burstCount;
    mResult.sampleRate = sr;
    mResult.profile = params.profile;
    mResult.jitterBudgetMs = params.jitterBudgetMs;

    mErrorCode.store(static_cast<int>(Error::NONE), std::memory_order_relaxed);
    mProgressPct.store(0.0f, std::memory_order_relaxed);
    mCurrentBurst.store(0, std::memory_order_relaxed);
    mStreamLost.store(false, std::memory_order_relaxed);
    mSwOutSum.store(0.0f, std::memory_order_relaxed);
    mSwInSum.store(0.0f, std::memory_order_relaxed);
    mSwCount.store(0, std::memory_order_relaxed);

    setPhase(Phase::CALIBRATING);
    mWorker = std::thread(&RoundTripMeasurer::analysisLoop, this);
    LOGI("start: sr=%d out=%dch in=%dch bursts=%d interval=%d frames chirp=%d",
         sr, params.outChannels, params.inChannels, burstCount,
         mIntervalSamples, mChirpLen);
    return true;
}

void RoundTripMeasurer::cancel() {
    mWorkerStop.store(true, std::memory_order_release);
    if (mWorker.joinable()) mWorker.join();
    setPhase(Phase::IDLE);
    mProgressPct.store(0.0f, std::memory_order_relaxed);
    mCurrentBurst.store(0, std::memory_order_relaxed);
}

void RoundTripMeasurer::noteSoftwareLatency(float outMs, float inMs) {
    // Single non-RT writer (JNI poll); relaxed accumulation is sufficient.
    mSwOutSum.store(mSwOutSum.load(std::memory_order_relaxed) + outMs,
                    std::memory_order_relaxed);
    mSwInSum.store(mSwInSum.load(std::memory_order_relaxed) + inMs,
                   std::memory_order_relaxed);
    mSwCount.fetch_add(1, std::memory_order_relaxed);
}

RoundTripMeasurer::Snapshot RoundTripMeasurer::poll() const {
    Snapshot s;
    s.phase = phase();  // acquire
    s.progressPct = mProgressPct.load(std::memory_order_relaxed);
    s.currentBurst = mCurrentBurst.load(std::memory_order_relaxed);
    s.totalBursts = mResult.totalBursts;
    if (s.phase == Phase::COMPLETE || s.phase == Phase::ERROR) {
        s.result = mResult;  // published-before-release by the worker (or start())
        // Single source of truth for the terminal error: the atomic. mResult.error
        // is never written off the worker, so terminal paths that only flip the
        // phase (RT calibration failures, worker STREAM_LOST/TIMEOUT) report here.
        s.result.error = static_cast<Error>(mErrorCode.load(std::memory_order_acquire));
    }
    return s;
}

// ============================================================================
// RT plane — onAudioReady
// ============================================================================

IAudioCallback::Result RoundTripMeasurer::onAudioReady(
        float* outputData, const float* inputData, int32_t numFrames) {
    const int outCh = std::max(1, mParams.outChannels);
    const int inCh = std::max(1, mParams.inChannels);
    const Phase p = phase();

    // Any non-active phase emits silence and touches nothing else.
    if (p != Phase::CALIBRATING && p != Phase::MEASURING) {
        if (outputData) std::fill(outputData, outputData + numFrames * outCh, 0.0f);
        return IAudioCallback::Result::CONTINUE;
    }

    if (p == Phase::CALIBRATING) {
        // Emit a 1 kHz tone and accumulate the loop-back input level.
        const double w = 2.0 * M_PI * 1000.0 / mParams.sampleRate;
        for (int i = 0; i < numFrames; ++i) {
            const float s = static_cast<float>(mAmplitude * std::sin(mCalibPhase));
            mCalibPhase += w;
            if (mCalibPhase > 2.0 * M_PI) mCalibPhase -= 2.0 * M_PI;
            if (outputData) {
                for (int c = 0; c < outCh; ++c) outputData[i * outCh + c] = s;
            }
            if (inputData) {
                const float in = inputData[i * inCh];
                mCalibSumSq += static_cast<double>(in) * in;
                const float a = std::fabs(in);
                if (a > mCalibPeak) mCalibPeak = a;
            }
        }
        mCalibCounter += numFrames;
        mProgressPct.store(10.0f * static_cast<float>(mCalibCounter) /
                               std::max(1, mCalibrationSamples),
                           std::memory_order_relaxed);

        if (mCalibCounter >= mCalibrationSamples) {
            const float rmsIn = static_cast<float>(
                std::sqrt(mCalibSumSq / std::max<int64_t>(1, mCalibCounter)));
            if (rmsIn < kNoSignalRms) {
                mErrorCode.store(static_cast<int>(Error::NO_SIGNAL),
                                 std::memory_order_relaxed);
                setPhase(Phase::ERROR);  // error read from mErrorCode (see poll())
            } else if (mCalibPeak > kClipPeak && mCalibRetries < kMaxCalibRetry) {
                // Too hot — halve amplitude and re-run calibration.
                mAmplitude *= 0.5f;
                ++mCalibRetries;
                mCalibCounter = 0;
                mCalibSumSq = 0.0;
                mCalibPeak = 0.0f;
            } else if (mCalibPeak > kClipPeak) {
                mErrorCode.store(static_cast<int>(Error::CLIPPING),
                                 std::memory_order_relaxed);
                setPhase(Phase::ERROR);  // error read from mErrorCode (see poll())
            } else {
                mSampleCounter = 0;
                mBurstIndex = 0;
                setPhase(Phase::MEASURING);
            }
        }
        return IAudioCallback::Result::CONTINUE;
    }

    // ---- MEASURING ----
    const int burstCount = static_cast<int>(mEmitSample.size());
    const int64_t captureLen = static_cast<int64_t>(mCapture.size());
    for (int i = 0; i < numFrames; ++i) {
        const int64_t a = mSampleCounter + i;
        // Advance past finished bursts.
        while (mBurstIndex < burstCount &&
               a >= mEmitSample[static_cast<size_t>(mBurstIndex)] + mChirpLen) {
            ++mBurstIndex;
        }
        float s = 0.0f;
        if (mBurstIndex < burstCount) {
            const int64_t off = a - mEmitSample[static_cast<size_t>(mBurstIndex)];
            if (off >= 0 && off < mChirpLen) s = mTemplate[static_cast<size_t>(off)];
        }
        if (outputData) {
            for (int c = 0; c < outCh; ++c) outputData[i * outCh + c] = s;
        }
        if (a >= 0 && a < captureLen) {
            mCapture[static_cast<size_t>(a)] = inputData ? inputData[i * inCh] : 0.0f;
        }
    }
    mSampleCounter += numFrames;
    mCurrentBurst.store(std::min(mBurstIndex, burstCount), std::memory_order_relaxed);
    mProgressPct.store(10.0f + 80.0f * static_cast<float>(std::min(mBurstIndex, burstCount)) /
                           static_cast<float>(burstCount),
                       std::memory_order_relaxed);

    const int64_t lastEmit = mEmitSample.back();
    if (mSampleCounter >= lastEmit + mSearchSamples) {
        mProgressPct.store(90.0f, std::memory_order_relaxed);
        setPhase(Phase::ANALYZING);  // freezes the capture for the worker
    }
    return IAudioCallback::Result::CONTINUE;
}

void RoundTripMeasurer::onBackendError(BackendError error) {
    // Not RT. If the stream dies mid-test the worker restores a clean ERROR.
    LOGW("backend error during round-trip test: %d", static_cast<int>(error));
    mStreamLost.store(true, std::memory_order_release);
}

// ============================================================================
// Analysis plane — worker thread
// ============================================================================

void RoundTripMeasurer::analysisLoop() {
    const auto startTime = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::milliseconds(
        static_cast<int64_t>(mParams.config.burstCount) *
            mParams.config.burstIntervalMs + 5000);

    for (;;) {
        if (mWorkerStop.load(std::memory_order_acquire)) return;
        const Phase p = phase();

        if (mStreamLost.load(std::memory_order_acquire) &&
            p != Phase::COMPLETE && p != Phase::ERROR) {
            mErrorCode.store(static_cast<int>(Error::STREAM_LOST),
                             std::memory_order_relaxed);
            setPhase(Phase::ERROR);  // error read from mErrorCode (see poll())
            continue;
        }
        if (p == Phase::ANALYZING) {
            runAnalysis();
            continue;
        }
        if (p == Phase::COMPLETE || p == Phase::ERROR) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (std::chrono::steady_clock::now() - startTime > timeout) {
            LOGW("round-trip test timed out in phase %d", static_cast<int>(p));
            mErrorCode.store(static_cast<int>(Error::TIMEOUT),
                             std::memory_order_relaxed);
            setPhase(Phase::ERROR);  // error read from mErrorCode (see poll())
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void RoundTripMeasurer::runAnalysis() {
    const int sr = mParams.sampleRate;
    const int guard = std::max(1, sr / 500);  // ±2 ms
    const int burstCount = static_cast<int>(mEmitSample.size());
    const int64_t captureLen = static_cast<int64_t>(mCapture.size());

    std::vector<float> latencyMs;
    std::vector<float> confidences;
    latencyMs.reserve(static_cast<size_t>(burstCount));
    confidences.reserve(static_cast<size_t>(burstCount));

    for (int k = 0; k < burstCount; ++k) {
        const int64_t winStart = mEmitSample[static_cast<size_t>(k)];
        int64_t winLen = mSearchSamples + mChirpLen;
        if (winStart + winLen > captureLen) winLen = captureLen - winStart;
        if (winLen < mChirpLen) continue;

        const CorrelationResult r = crossCorrelate(
            &mCapture[static_cast<size_t>(winStart)], static_cast<int>(winLen),
            mTemplate.data(), mChirpLen, guard);
        // lag is relative to winStart == emit sample → it IS the loop latency.
        const bool valid = r.valid && r.confidence > kMinConfidence && r.peak > kMinPeak;
        if (valid) {
            latencyMs.push_back(static_cast<float>(r.lagSamples) * 1000.0f / sr);
            confidences.push_back(r.confidence);
        }
    }

    const Aggregate agg = aggregateWithOutlierRejection(latencyMs);
    Result res = mResult;  // keeps sampleRate/profile/jitterBudget/totalBursts
    res.validBursts = static_cast<int>(latencyMs.size());
    res.medianMs = agg.median;
    res.madMs = agg.mad;
    res.minMs = agg.minV;
    res.maxMs = agg.maxV;
    res.confidence = medianOf(confidences);

    const int c = mSwCount.load(std::memory_order_relaxed);
    if (c > 0) {
        res.softwareOutputMs = mSwOutSum.load(std::memory_order_relaxed) / c;
        res.softwareInputMs = mSwInSum.load(std::memory_order_relaxed) / c;
    }
    res.residualMs = res.medianMs - (res.softwareOutputMs + res.softwareInputMs);

    if (res.validBursts >= kMinValidBursts && agg.count >= 1) {
        res.error = Error::NONE;
        mResult = res;
        setPhase(Phase::COMPLETE);
        LOGI("round-trip COMPLETE: median=%.2f ms mad=%.2f valid=%d/%d residual=%.2f",
             res.medianMs, res.madMs, res.validBursts, res.totalBursts, res.residualMs);
    } else {
        res.error = Error::UNRELIABLE;
        mResult = res;
        mErrorCode.store(static_cast<int>(Error::UNRELIABLE), std::memory_order_relaxed);
        setPhase(Phase::ERROR);
        LOGW("round-trip UNRELIABLE: only %d/%d valid bursts",
             res.validBursts, res.totalBursts);
    }
}

}  // namespace usb
}  // namespace watermelon_audio
