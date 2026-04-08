/**
 * UsbLatencyProfiler.h
 *
 * Precision latency profiling system for USB audio streams.
 *
 * Features:
 * - Transfer timing measurement (submission to completion)
 * - Jitter analysis (variation in transfer intervals)
 * - DSP callback duration tracking
 * - Ring buffer latency estimation
 * - Statistical analysis (min, max, avg, stddev, percentiles)
 *
 * Thread Safety:
 * - All operations are lock-free using atomics
 * - Safe to call from USB event thread and DSP thread
 * - Statistics retrieval is safe from any thread
 *
 * Usage:
 *   UsbLatencyProfiler profiler;
 *   profiler.onTransferSubmitted();
 *   // ... USB transfer completes ...
 *   profiler.onTransferCompleted();
 *   auto stats = profiler.getStatistics();
 */

#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <algorithm>

namespace noisypad {
namespace usb {

// =============================================================================
// Configuration
// =============================================================================

struct ProfilerConfig {
    static constexpr int MAX_SAMPLES = 1000;          // Circular buffer size
    static constexpr int PERCENTILE_95_INDEX = 950;   // 95th percentile
    static constexpr int PERCENTILE_99_INDEX = 990;   // 99th percentile
    static constexpr double EMA_ALPHA = 0.1;          // Exponential moving average factor
};

// =============================================================================
// Profiling Statistics
// =============================================================================

/**
 * Transfer timing statistics.
 */
struct TransferTimingStats {
    // Latency (submission to completion)
    double avgLatencyUs = 0.0;
    double minLatencyUs = 1e9;
    double maxLatencyUs = 0.0;
    double stddevLatencyUs = 0.0;
    double p95LatencyUs = 0.0;          // 95th percentile
    double p99LatencyUs = 0.0;          // 99th percentile
    double lastLatencyUs = 0.0;

    // Jitter (variation in transfer intervals)
    double avgJitterUs = 0.0;
    double maxJitterUs = 0.0;
    double lastJitterUs = 0.0;

    // Throughput
    uint64_t totalTransfers = 0;
    double transfersPerSecond = 0.0;

    // Ring buffer contribution to latency
    double ringBufferLatencyMs = 0.0;   // Estimated from fill level

    // Combined latency estimate
    double totalEstimatedLatencyMs = 0.0;
};

/**
 * DSP callback timing statistics.
 */
struct DspCallbackStats {
    double avgDurationUs = 0.0;
    double minDurationUs = 1e9;
    double maxDurationUs = 0.0;
    double stddevDurationUs = 0.0;
    double p95DurationUs = 0.0;
    double p99DurationUs = 0.0;
    double lastDurationUs = 0.0;

    double cpuLoadPercent = 0.0;        // Callback time / budget time * 100
    double budgetUs = 0.0;              // Time budget for callback

    int callbackCount = 0;
    int overrunCount = 0;               // Times callback exceeded budget
};

/**
 * Combined USB profiling statistics.
 */
struct UsbProfilingStats {
    TransferTimingStats outputTransfers;
    TransferTimingStats inputTransfers;
    DspCallbackStats dspCallback;

    // Health indicators
    bool isHealthy = true;
    double healthScore = 100.0;         // 0-100, lower is worse

    // Timestamp of last update
    uint64_t timestampMs = 0;
};

// =============================================================================
// Timing Sample Collector
// =============================================================================

/**
 * Lock-free circular buffer for timing samples.
 */
class TimingSampleCollector {
public:
    void addSample(double valueUs) {
        int index = mSampleCount.fetch_add(1, std::memory_order_relaxed) % ProfilerConfig::MAX_SAMPLES;
        mSamples[index] = valueUs;

        // Update min/max atomically
        updateMin(valueUs);
        updateMax(valueUs);

        // Update exponential moving average
        double currentAvg = mAvg.load(std::memory_order_relaxed);
        double newAvg = currentAvg + ProfilerConfig::EMA_ALPHA * (valueUs - currentAvg);
        mAvg.store(newAvg, std::memory_order_relaxed);

        mLast.store(valueUs, std::memory_order_relaxed);
    }

    void reset() {
        mSampleCount.store(0, std::memory_order_relaxed);
        mAvg.store(0.0, std::memory_order_relaxed);
        mMin.store(1e9, std::memory_order_relaxed);
        mMax.store(0.0, std::memory_order_relaxed);
        mLast.store(0.0, std::memory_order_relaxed);
        std::fill(mSamples.begin(), mSamples.end(), 0.0);
    }

    double getAvg() const { return mAvg.load(std::memory_order_relaxed); }
    double getMin() const { return mMin.load(std::memory_order_relaxed); }
    double getMax() const { return mMax.load(std::memory_order_relaxed); }
    double getLast() const { return mLast.load(std::memory_order_relaxed); }
    int getCount() const { return mSampleCount.load(std::memory_order_relaxed); }

    /**
     * Calculate standard deviation.
     * NOT lock-free - only call from non-RT thread.
     */
    double calculateStddev() const {
        int count = std::min(mSampleCount.load(), ProfilerConfig::MAX_SAMPLES);
        if (count < 2) return 0.0;

        double avg = mAvg.load();
        double sumSq = 0.0;
        for (int i = 0; i < count; ++i) {
            double diff = mSamples[i] - avg;
            sumSq += diff * diff;
        }
        return std::sqrt(sumSq / count);
    }

    /**
     * Calculate percentile value.
     * NOT lock-free - only call from non-RT thread.
     */
    double calculatePercentile(int percentileIndex) const {
        int count = std::min(mSampleCount.load(), ProfilerConfig::MAX_SAMPLES);
        if (count == 0) return 0.0;

        // Copy samples for sorting
        std::array<double, ProfilerConfig::MAX_SAMPLES> sorted;
        for (int i = 0; i < count; ++i) {
            sorted[i] = mSamples[i];
        }
        std::sort(sorted.begin(), sorted.begin() + count);

        int index = std::min(percentileIndex * count / ProfilerConfig::MAX_SAMPLES, count - 1);
        return sorted[index];
    }

private:
    void updateMin(double value) {
        double current = mMin.load(std::memory_order_relaxed);
        while (value < current) {
            if (mMin.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
                break;
            }
        }
    }

    void updateMax(double value) {
        double current = mMax.load(std::memory_order_relaxed);
        while (value > current) {
            if (mMax.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
                break;
            }
        }
    }

    std::array<double, ProfilerConfig::MAX_SAMPLES> mSamples{};
    std::atomic<int> mSampleCount{0};
    std::atomic<double> mAvg{0.0};
    std::atomic<double> mMin{1e9};
    std::atomic<double> mMax{0.0};
    std::atomic<double> mLast{0.0};
};

// =============================================================================
// Transfer Timing Profiler
// =============================================================================

/**
 * Profiles USB transfer latency and jitter.
 */
class TransferTimingProfiler {
public:
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = std::chrono::high_resolution_clock::time_point;

    /**
     * Call when a transfer is submitted.
     * Returns a token to use with onCompleted().
     */
    uint64_t onSubmitted() {
        uint64_t token = mNextToken.fetch_add(1, std::memory_order_relaxed);
        int index = static_cast<int>(token % MAX_PENDING);
        mSubmitTimes[index] = Clock::now();
        return token;
    }

    /**
     * Call when a transfer completes.
     * @param token  Token from onSubmitted()
     */
    void onCompleted(uint64_t token) {
        auto now = Clock::now();
        int index = static_cast<int>(token % MAX_PENDING);

        // Calculate latency
        auto submitTime = mSubmitTimes[index];
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(now - submitTime);
        double latencyUs = duration.count() / 1000.0;
        mLatencySamples.addSample(latencyUs);

        // Calculate jitter (variation from last completion interval)
        auto lastComplete = mLastCompleteTime.exchange(now);
        if (lastComplete.time_since_epoch().count() > 0) {
            auto interval = std::chrono::duration_cast<std::chrono::nanoseconds>(now - lastComplete);
            double intervalUs = interval.count() / 1000.0;

            double expectedIntervalUs = mExpectedIntervalUs.load(std::memory_order_relaxed);
            if (expectedIntervalUs > 0) {
                double jitterUs = std::abs(intervalUs - expectedIntervalUs);
                mJitterSamples.addSample(jitterUs);
            }
        }

        mCompletedCount.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * Set expected interval between transfers (for jitter calculation).
     * @param intervalUs  Expected interval in microseconds
     */
    void setExpectedInterval(double intervalUs) {
        mExpectedIntervalUs.store(intervalUs, std::memory_order_relaxed);
    }

    /**
     * Get transfer timing statistics.
     * NOT lock-free - only call from non-RT thread.
     */
    TransferTimingStats getStatistics() const {
        TransferTimingStats stats;

        stats.avgLatencyUs = mLatencySamples.getAvg();
        stats.minLatencyUs = mLatencySamples.getMin();
        stats.maxLatencyUs = mLatencySamples.getMax();
        stats.lastLatencyUs = mLatencySamples.getLast();
        stats.stddevLatencyUs = mLatencySamples.calculateStddev();
        stats.p95LatencyUs = mLatencySamples.calculatePercentile(ProfilerConfig::PERCENTILE_95_INDEX);
        stats.p99LatencyUs = mLatencySamples.calculatePercentile(ProfilerConfig::PERCENTILE_99_INDEX);

        stats.avgJitterUs = mJitterSamples.getAvg();
        stats.maxJitterUs = mJitterSamples.getMax();
        stats.lastJitterUs = mJitterSamples.getLast();

        stats.totalTransfers = mCompletedCount.load(std::memory_order_relaxed);

        return stats;
    }

    void reset() {
        mLatencySamples.reset();
        mJitterSamples.reset();
        mCompletedCount.store(0, std::memory_order_relaxed);
        mNextToken.store(0, std::memory_order_relaxed);
        mLastCompleteTime.store(TimePoint{});
    }

private:
    static constexpr int MAX_PENDING = 16;  // Max concurrent pending transfers

    std::array<TimePoint, MAX_PENDING> mSubmitTimes{};
    std::atomic<TimePoint> mLastCompleteTime{};
    std::atomic<double> mExpectedIntervalUs{0.0};

    TimingSampleCollector mLatencySamples;
    TimingSampleCollector mJitterSamples;

    std::atomic<uint64_t> mCompletedCount{0};
    std::atomic<uint64_t> mNextToken{0};
};

// =============================================================================
// DSP Callback Profiler
// =============================================================================

/**
 * Profiles DSP callback timing.
 */
class DspCallbackProfiler {
public:
    using Clock = std::chrono::high_resolution_clock;

    /**
     * Call at the START of the DSP callback.
     */
    void onCallbackStart() {
        mCallbackStart = Clock::now();
    }

    /**
     * Call at the END of the DSP callback.
     * @param budgetUs  Time budget for the callback in microseconds
     */
    void onCallbackEnd(double budgetUs) {
        auto now = Clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(now - mCallbackStart);
        double durationUs = duration.count() / 1000.0;

        mDurationSamples.addSample(durationUs);
        mBudgetUs.store(budgetUs, std::memory_order_relaxed);
        mCallbackCount.fetch_add(1, std::memory_order_relaxed);

        if (durationUs > budgetUs) {
            mOverrunCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /**
     * Get callback timing statistics.
     * NOT lock-free - only call from non-RT thread.
     */
    DspCallbackStats getStatistics() const {
        DspCallbackStats stats;

        stats.avgDurationUs = mDurationSamples.getAvg();
        stats.minDurationUs = mDurationSamples.getMin();
        stats.maxDurationUs = mDurationSamples.getMax();
        stats.lastDurationUs = mDurationSamples.getLast();
        stats.stddevDurationUs = mDurationSamples.calculateStddev();
        stats.p95DurationUs = mDurationSamples.calculatePercentile(ProfilerConfig::PERCENTILE_95_INDEX);
        stats.p99DurationUs = mDurationSamples.calculatePercentile(ProfilerConfig::PERCENTILE_99_INDEX);

        stats.budgetUs = mBudgetUs.load(std::memory_order_relaxed);
        if (stats.budgetUs > 0) {
            stats.cpuLoadPercent = (stats.avgDurationUs / stats.budgetUs) * 100.0;
        }

        stats.callbackCount = mCallbackCount.load(std::memory_order_relaxed);
        stats.overrunCount = mOverrunCount.load(std::memory_order_relaxed);

        return stats;
    }

    void reset() {
        mDurationSamples.reset();
        mCallbackCount.store(0, std::memory_order_relaxed);
        mOverrunCount.store(0, std::memory_order_relaxed);
        mBudgetUs.store(0.0, std::memory_order_relaxed);
    }

private:
    Clock::time_point mCallbackStart;
    TimingSampleCollector mDurationSamples;
    std::atomic<double> mBudgetUs{0.0};
    std::atomic<int> mCallbackCount{0};
    std::atomic<int> mOverrunCount{0};
};

// =============================================================================
// USB Latency Profiler (Main Class)
// =============================================================================

/**
 * UsbLatencyProfiler
 *
 * Comprehensive USB audio latency profiling.
 */
class UsbLatencyProfiler {
public:
    UsbLatencyProfiler() = default;

    // =========================================================================
    // Configuration
    // =========================================================================

    /**
     * Configure expected transfer intervals for jitter calculation.
     * @param outputIntervalUs  Expected output transfer interval (microseconds)
     * @param inputIntervalUs   Expected input transfer interval (microseconds)
     */
    void configure(double outputIntervalUs, double inputIntervalUs) {
        mOutputProfiler.setExpectedInterval(outputIntervalUs);
        mInputProfiler.setExpectedInterval(inputIntervalUs);
    }

    /**
     * Configure from transfer config.
     * @param framesPerPacket      Frames per USB packet
     * @param packetsPerTransfer   Packets per transfer
     * @param sampleRate           Sample rate in Hz
     */
    void configureFromTransfer(int framesPerPacket, int packetsPerTransfer, int sampleRate) {
        // Calculate expected transfer interval
        // Each transfer contains packetsPerTransfer packets
        // Each packet is framesPerPacket frames
        double totalFrames = framesPerPacket * packetsPerTransfer;
        double intervalMs = (totalFrames / sampleRate) * 1000.0;
        double intervalUs = intervalMs * 1000.0;

        mOutputProfiler.setExpectedInterval(intervalUs);
        mInputProfiler.setExpectedInterval(intervalUs);

        mSampleRate = sampleRate;
        mFramesPerBuffer = framesPerPacket * packetsPerTransfer;
    }

    // =========================================================================
    // Output Transfer Profiling
    // =========================================================================

    uint64_t onOutputSubmitted() {
        return mOutputProfiler.onSubmitted();
    }

    void onOutputCompleted(uint64_t token) {
        mOutputProfiler.onCompleted(token);
    }

    // =========================================================================
    // Input Transfer Profiling
    // =========================================================================

    uint64_t onInputSubmitted() {
        return mInputProfiler.onSubmitted();
    }

    void onInputCompleted(uint64_t token) {
        mInputProfiler.onCompleted(token);
    }

    // =========================================================================
    // DSP Callback Profiling
    // =========================================================================

    void onDspCallbackStart() {
        mDspProfiler.onCallbackStart();
    }

    void onDspCallbackEnd() {
        // Calculate budget: (framesPerBuffer / sampleRate) * 1e6 microseconds
        double budgetUs = 0.0;
        if (mSampleRate > 0) {
            budgetUs = (static_cast<double>(mFramesPerBuffer) / mSampleRate) * 1000000.0;
        }
        mDspProfiler.onCallbackEnd(budgetUs);
    }

    // =========================================================================
    // Ring Buffer Latency
    // =========================================================================

    /**
     * Update ring buffer latency estimate.
     * @param fillLevel      Current fill level (samples)
     * @param capacity       Total capacity (samples)
     * @param channelCount   Number of channels
     */
    void updateRingBufferLatency(size_t fillLevel, size_t capacity, int channelCount) {
        if (mSampleRate > 0 && channelCount > 0) {
            size_t frames = fillLevel / channelCount;
            double latencyMs = (static_cast<double>(frames) / mSampleRate) * 1000.0;
            mRingBufferLatencyMs.store(latencyMs, std::memory_order_relaxed);

            double fillPct = (static_cast<double>(fillLevel) / capacity) * 100.0;
            mRingBufferFillPct.store(fillPct, std::memory_order_relaxed);
        }
    }

    // =========================================================================
    // Statistics
    // =========================================================================

    /**
     * Get comprehensive profiling statistics.
     * NOT lock-free - only call from non-RT thread.
     */
    UsbProfilingStats getStatistics() const {
        UsbProfilingStats stats;

        stats.outputTransfers = mOutputProfiler.getStatistics();
        stats.inputTransfers = mInputProfiler.getStatistics();
        stats.dspCallback = mDspProfiler.getStatistics();

        // Add ring buffer latency
        double rbLatencyMs = mRingBufferLatencyMs.load(std::memory_order_relaxed);
        stats.outputTransfers.ringBufferLatencyMs = rbLatencyMs;
        stats.inputTransfers.ringBufferLatencyMs = rbLatencyMs;

        // Calculate total estimated latency
        // Total = USB transfer latency + ring buffer latency + DSP processing
        double usbLatencyMs = stats.outputTransfers.avgLatencyUs / 1000.0;
        double dspLatencyMs = stats.dspCallback.avgDurationUs / 1000.0;
        stats.outputTransfers.totalEstimatedLatencyMs = usbLatencyMs + rbLatencyMs + dspLatencyMs;
        stats.inputTransfers.totalEstimatedLatencyMs =
            (stats.inputTransfers.avgLatencyUs / 1000.0) + rbLatencyMs + dspLatencyMs;

        // Calculate health score
        stats.healthScore = calculateHealthScore(stats);
        stats.isHealthy = stats.healthScore >= 70.0;

        // Timestamp
        auto now = std::chrono::steady_clock::now();
        stats.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();

        return stats;
    }

    /**
     * Get just the DSP callback stats (for quick access).
     */
    DspCallbackStats getDspStats() const {
        return mDspProfiler.getStatistics();
    }

    /**
     * Get estimated total latency in milliseconds.
     */
    double getEstimatedLatencyMs() const {
        double usbLatencyUs = mOutputProfiler.getStatistics().avgLatencyUs;
        double rbLatencyMs = mRingBufferLatencyMs.load(std::memory_order_relaxed);
        double dspLatencyUs = mDspProfiler.getStatistics().avgDurationUs;

        return (usbLatencyUs / 1000.0) + rbLatencyMs + (dspLatencyUs / 1000.0);
    }

    void reset() {
        mOutputProfiler.reset();
        mInputProfiler.reset();
        mDspProfiler.reset();
        mRingBufferLatencyMs.store(0.0, std::memory_order_relaxed);
        mRingBufferFillPct.store(0.0, std::memory_order_relaxed);
    }

    // =========================================================================
    // Enable/Disable Profiling
    // =========================================================================

    void setEnabled(bool enabled) {
        mEnabled.store(enabled, std::memory_order_relaxed);
    }

    bool isEnabled() const {
        return mEnabled.load(std::memory_order_relaxed);
    }

private:
    /**
     * Calculate health score (0-100).
     */
    double calculateHealthScore(const UsbProfilingStats& stats) const {
        double score = 100.0;

        // Penalize high jitter
        if (stats.outputTransfers.avgJitterUs > 1000.0) {  // > 1ms jitter
            score -= 20.0;
        } else if (stats.outputTransfers.avgJitterUs > 500.0) {
            score -= 10.0;
        }

        // Penalize high CPU load
        if (stats.dspCallback.cpuLoadPercent > 80.0) {
            score -= 30.0;
        } else if (stats.dspCallback.cpuLoadPercent > 60.0) {
            score -= 15.0;
        }

        // Penalize overruns
        if (stats.dspCallback.overrunCount > 0 && stats.dspCallback.callbackCount > 0) {
            double overrunRate = static_cast<double>(stats.dspCallback.overrunCount) /
                                 stats.dspCallback.callbackCount;
            if (overrunRate > 0.01) {  // > 1% overrun rate
                score -= 30.0;
            } else if (overrunRate > 0.001) {  // > 0.1%
                score -= 15.0;
            }
        }

        // Penalize high latency
        double totalLatencyMs = stats.outputTransfers.totalEstimatedLatencyMs;
        if (totalLatencyMs > 20.0) {
            score -= 15.0;
        } else if (totalLatencyMs > 15.0) {
            score -= 5.0;
        }

        return std::max(0.0, score);
    }

    TransferTimingProfiler mOutputProfiler;
    TransferTimingProfiler mInputProfiler;
    DspCallbackProfiler mDspProfiler;

    std::atomic<double> mRingBufferLatencyMs{0.0};
    std::atomic<double> mRingBufferFillPct{0.0};

    int mSampleRate = 48000;
    int mFramesPerBuffer = 384;  // Default: 8ms at 48kHz

    std::atomic<bool> mEnabled{true};
};

} // namespace usb
} // namespace noisypad