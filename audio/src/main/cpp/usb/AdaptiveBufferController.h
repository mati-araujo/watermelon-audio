/**
 * AdaptiveBufferController.h
 *
 * Adaptive ring buffer sizing controller for USB audio streaming.
 *
 * This controller monitors audio streaming metrics (underruns, health score,
 * jitter) and recommends buffer size adjustments to balance latency vs. stability.
 *
 * Features:
 * - Hysteresis to prevent oscillation (different thresholds for increase/decrease)
 * - Cooldown period after changes to allow system to stabilize
 * - Step-based adjustments (gradual changes, not sudden jumps)
 * - Integration with UsbLatencyProfiler metrics
 *
 * Thread Safety:
 * - All counters and state use std::atomic
 * - Safe to call onUnderrun/onTransferComplete from USB thread
 * - Safe to call evaluate() from DSP thread
 * - Statistics retrieval safe from any thread
 *
 * Usage:
 *   AdaptiveBufferController controller;
 *   controller.configure(AdaptiveBufferController::Config{});
 *   controller.setCurrentBufferMs(100);
 *
 *   // On each transfer complete:
 *   controller.onTransferComplete();
 *
 *   // On underrun:
 *   controller.onUnderrun();
 *
 *   // Periodically (from DSP thread):
 *   controller.updateFromProfiler(profiler.getStatistics());
 *   auto recommendation = controller.evaluate();
 *   if (recommendation != Recommendation::NO_CHANGE) {
 *       int newSize = controller.getRecommendedBufferMs();
 *       // Apply buffer resize
 *   }
 */

#pragma once

#include <atomic>
#include <chrono>
#include <algorithm>
#include "../platform/Logger.h"
#include "UsbLatencyProfiler.h"

// Logging macros for this header
#ifndef ADAPTIVE_BUFFER_LOG_TAG
#define ADAPTIVE_BUFFER_LOG_TAG "AdaptiveBuffer"
#endif

#ifndef LOGI
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, ADAPTIVE_BUFFER_LOG_TAG, __VA_ARGS__)
#endif

#ifndef LOGD
#define LOGD(...) wma::logMessage(wma::LogLevel::DEBUG, ADAPTIVE_BUFFER_LOG_TAG, __VA_ARGS__)
#endif

namespace noisypad {
namespace usb {

class AdaptiveBufferController {
public:
    // =========================================================================
    // Configuration
    // =========================================================================

    struct Config {
        // Buffer size limits
        int minBufferMs = 50;           // Minimum buffer (safety floor)
        int maxBufferMs = 200;          // Maximum buffer (current default)
        int defaultBufferMs = 100;      // Starting buffer (reduced from 200)
        int stepSizeMs = 25;            // Increment/decrement step

        // Timing
        int evaluationPeriodMs = 5000;      // How often to evaluate (5 seconds)
        int cooldownAfterChangeMs = 10000;  // Wait after change (10 seconds)
        int minTransfersForEvaluation = 100; // Minimum transfers before first evaluation

        // Thresholds for INCREASING buffer (any condition triggers increase)
        float underrunRateIncreaseThreshold = 0.02f;    // 2% underrun rate
        float healthScoreIncreaseThreshold = 70.0f;     // Health score below 70
        float jitterIncreaseThresholdUs = 1500.0f;      // Average jitter > 1.5ms

        // Thresholds for DECREASING buffer (ALL conditions must be met)
        float underrunRateDecreaseThreshold = 0.001f;   // 0.1% underrun rate
        float healthScoreDecreaseThreshold = 90.0f;     // Health score above 90
        float jitterDecreaseThresholdUs = 750.0f;       // Average jitter < 0.75ms
    };

    // =========================================================================
    // Recommendation
    // =========================================================================

    enum class Recommendation {
        NO_CHANGE,
        INCREASE_BUFFER,
        DECREASE_BUFFER
    };

    // =========================================================================
    // Statistics
    // =========================================================================

    struct Statistics {
        int currentBufferMs = 0;
        int recommendedBufferMs = 0;
        int underrunCount = 0;
        int overrunCount = 0;
        int transferCount = 0;
        float underrunRate = 0.0f;
        double lastHealthScore = 100.0;
        double lastJitterUs = 0.0;
        int adjustmentCount = 0;
        int64_t lastEvaluationTimeMs = 0;
        bool isEnabled = false;
    };

    // =========================================================================
    // Construction
    // =========================================================================

    AdaptiveBufferController() {
        mLastEvaluationTime = Clock::now();
        mLastChangeTime = Clock::now() - std::chrono::seconds(30); // Allow immediate first change
    }

    // =========================================================================
    // Configuration
    // =========================================================================

    void configure(const Config& config) {
        mConfig = config;
        mCurrentBufferMs.store(config.defaultBufferMs, std::memory_order_relaxed);
        mRecommendedBufferMs.store(config.defaultBufferMs, std::memory_order_relaxed);
        LOGI("AdaptiveBufferController configured: default=%dms, range=[%d-%d]ms, step=%dms",
             config.defaultBufferMs, config.minBufferMs, config.maxBufferMs, config.stepSizeMs);
    }

    void setEnabled(bool enabled) {
        mEnabled.store(enabled, std::memory_order_relaxed);
        LOGI("AdaptiveBufferController %s", enabled ? "enabled" : "disabled");
    }

    bool isEnabled() const {
        return mEnabled.load(std::memory_order_relaxed);
    }

    // =========================================================================
    // Buffer Size Management
    // =========================================================================

    void setCurrentBufferMs(int bufferMs) {
        int clamped = std::clamp(bufferMs, mConfig.minBufferMs, mConfig.maxBufferMs);
        mCurrentBufferMs.store(clamped, std::memory_order_relaxed);
        mRecommendedBufferMs.store(clamped, std::memory_order_relaxed);
    }

    int getCurrentBufferMs() const {
        return mCurrentBufferMs.load(std::memory_order_relaxed);
    }

    int getRecommendedBufferMs() const {
        return mRecommendedBufferMs.load(std::memory_order_relaxed);
    }

    int getDefaultBufferMs() const {
        return mConfig.defaultBufferMs;
    }

    // =========================================================================
    // Event Tracking
    // =========================================================================

    /**
     * Call when an underrun is detected.
     * Thread-safe, can be called from USB event thread.
     */
    void onUnderrun() {
        mUnderrunCount.fetch_add(1, std::memory_order_relaxed);
        mTotalUnderrunCount.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * Call when an overrun is detected.
     * Thread-safe, can be called from USB event thread.
     */
    void onOverrun() {
        mOverrunCount.fetch_add(1, std::memory_order_relaxed);
        mTotalOverrunCount.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * Call when a transfer completes successfully.
     * Thread-safe, can be called from USB event thread.
     */
    void onTransferComplete() {
        mTransferCount.fetch_add(1, std::memory_order_relaxed);
        mTotalTransferCount.fetch_add(1, std::memory_order_relaxed);
    }

    // =========================================================================
    // Profiler Integration
    // =========================================================================

    /**
     * Update metrics from the latency profiler.
     * Call periodically (e.g., every 100 DSP callbacks).
     *
     * @param stats  Current profiling statistics
     */
    void updateFromProfiler(const UsbProfilingStats& stats) {
        mLastHealthScore.store(stats.healthScore, std::memory_order_relaxed);
        mLastJitterUs.store(stats.outputTransfers.avgJitterUs, std::memory_order_relaxed);
        mLastLatencyMs.store(stats.outputTransfers.totalEstimatedLatencyMs, std::memory_order_relaxed);
    }

    // =========================================================================
    // Evaluation
    // =========================================================================

    /**
     * Evaluate current metrics and determine if buffer resize is needed.
     *
     * Should be called periodically from DSP thread (e.g., every 100 callbacks).
     * Only evaluates if sufficient time has passed since last evaluation.
     *
     * @return Recommendation for buffer size change
     */
    Recommendation evaluate() {
        if (!mEnabled.load(std::memory_order_relaxed)) {
            return Recommendation::NO_CHANGE;
        }

        // Check if enough time has passed for evaluation
        if (!isEvaluationDue()) {
            return Recommendation::NO_CHANGE;
        }

        // Check if we're in cooldown after a recent change
        if (isCooldownActive()) {
            // Still reset evaluation timer to prevent buildup
            mLastEvaluationTime = Clock::now();
            resetCounters();
            return Recommendation::NO_CHANGE;
        }

        // Check if we have enough data to evaluate
        int transferCount = mTransferCount.load(std::memory_order_relaxed);
        if (transferCount < mConfig.minTransfersForEvaluation) {
            return Recommendation::NO_CHANGE;
        }

        // Calculate current metrics
        float underrunRate = calculateUnderrunRate();
        double healthScore = mLastHealthScore.load(std::memory_order_relaxed);
        double jitterUs = mLastJitterUs.load(std::memory_order_relaxed);
        int currentMs = mCurrentBufferMs.load(std::memory_order_relaxed);

        // Reset counters for next period
        mLastEvaluationTime = Clock::now();
        mLastEvaluationTimeMs.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                mLastEvaluationTime.time_since_epoch()
            ).count(),
            std::memory_order_relaxed
        );
        resetCounters();

        // Log evaluation metrics
        LOGD("AdaptiveBuffer evaluate: underrunRate=%.4f, healthScore=%.1f, jitterUs=%.1f, currentMs=%d",
             underrunRate, healthScore, jitterUs, currentMs);

        // Decision logic with hysteresis

        // Condition to INCREASE buffer (any condition triggers increase)
        bool shouldIncrease =
            (underrunRate > mConfig.underrunRateIncreaseThreshold) ||
            (healthScore < mConfig.healthScoreIncreaseThreshold) ||
            (jitterUs > mConfig.jitterIncreaseThresholdUs);

        if (shouldIncrease && currentMs < mConfig.maxBufferMs) {
            int newMs = std::min(currentMs + mConfig.stepSizeMs, mConfig.maxBufferMs);
            mRecommendedBufferMs.store(newMs, std::memory_order_relaxed);
            mAdjustmentCount.fetch_add(1, std::memory_order_relaxed);
            mLastChangeTime = Clock::now();

            LOGI("AdaptiveBuffer: INCREASE %d -> %d ms (underrun=%.2f%%, health=%.1f, jitter=%.0fus)",
                 currentMs, newMs, underrunRate * 100.0f, healthScore, jitterUs);

            return Recommendation::INCREASE_BUFFER;
        }

        // Condition to DECREASE buffer (ALL conditions must be met, much stricter)
        bool canDecrease =
            (underrunRate < mConfig.underrunRateDecreaseThreshold) &&
            (healthScore > mConfig.healthScoreDecreaseThreshold) &&
            (jitterUs < mConfig.jitterDecreaseThresholdUs);

        if (canDecrease && currentMs > mConfig.minBufferMs) {
            int newMs = std::max(currentMs - mConfig.stepSizeMs, mConfig.minBufferMs);
            mRecommendedBufferMs.store(newMs, std::memory_order_relaxed);
            mAdjustmentCount.fetch_add(1, std::memory_order_relaxed);
            mLastChangeTime = Clock::now();

            LOGI("AdaptiveBuffer: DECREASE %d -> %d ms (underrun=%.2f%%, health=%.1f, jitter=%.0fus)",
                 currentMs, newMs, underrunRate * 100.0f, healthScore, jitterUs);

            return Recommendation::DECREASE_BUFFER;
        }

        return Recommendation::NO_CHANGE;
    }

    // =========================================================================
    // Reset
    // =========================================================================

    /**
     * Reset all counters and state.
     * Call when stream restarts.
     */
    void reset() {
        mUnderrunCount.store(0, std::memory_order_relaxed);
        mOverrunCount.store(0, std::memory_order_relaxed);
        mTransferCount.store(0, std::memory_order_relaxed);
        mTotalUnderrunCount.store(0, std::memory_order_relaxed);
        mTotalOverrunCount.store(0, std::memory_order_relaxed);
        mTotalTransferCount.store(0, std::memory_order_relaxed);
        mAdjustmentCount.store(0, std::memory_order_relaxed);

        mLastHealthScore.store(100.0, std::memory_order_relaxed);
        mLastJitterUs.store(0.0, std::memory_order_relaxed);
        mLastLatencyMs.store(0.0, std::memory_order_relaxed);

        mLastEvaluationTime = Clock::now();
        mLastChangeTime = Clock::now() - std::chrono::seconds(30);
        mLastEvaluationTimeMs.store(0, std::memory_order_relaxed);

        int defaultMs = mConfig.defaultBufferMs;
        mCurrentBufferMs.store(defaultMs, std::memory_order_relaxed);
        mRecommendedBufferMs.store(defaultMs, std::memory_order_relaxed);

        LOGI("AdaptiveBufferController reset to default=%dms", defaultMs);
    }

    // =========================================================================
    // Statistics
    // =========================================================================

    /**
     * Get current statistics for JNI/monitoring.
     * Thread-safe, can be called from any thread.
     */
    Statistics getStatistics() const {
        Statistics stats;
        stats.currentBufferMs = mCurrentBufferMs.load(std::memory_order_relaxed);
        stats.recommendedBufferMs = mRecommendedBufferMs.load(std::memory_order_relaxed);
        stats.underrunCount = mTotalUnderrunCount.load(std::memory_order_relaxed);
        stats.overrunCount = mTotalOverrunCount.load(std::memory_order_relaxed);
        stats.transferCount = mTotalTransferCount.load(std::memory_order_relaxed);
        stats.underrunRate = calculateTotalUnderrunRate();
        stats.lastHealthScore = mLastHealthScore.load(std::memory_order_relaxed);
        stats.lastJitterUs = mLastJitterUs.load(std::memory_order_relaxed);
        stats.adjustmentCount = mAdjustmentCount.load(std::memory_order_relaxed);
        stats.lastEvaluationTimeMs = mLastEvaluationTimeMs.load(std::memory_order_relaxed);
        stats.isEnabled = mEnabled.load(std::memory_order_relaxed);
        return stats;
    }

private:
    using Clock = std::chrono::steady_clock;

    // =========================================================================
    // Helper Methods
    // =========================================================================

    bool isEvaluationDue() const {
        auto now = Clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - mLastEvaluationTime
        ).count();
        return elapsed >= mConfig.evaluationPeriodMs;
    }

    bool isCooldownActive() const {
        auto now = Clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - mLastChangeTime
        ).count();
        return elapsed < mConfig.cooldownAfterChangeMs;
    }

    float calculateUnderrunRate() const {
        int transfers = mTransferCount.load(std::memory_order_relaxed);
        if (transfers == 0) return 0.0f;

        int underruns = mUnderrunCount.load(std::memory_order_relaxed);
        return static_cast<float>(underruns) / static_cast<float>(transfers);
    }

    float calculateTotalUnderrunRate() const {
        int transfers = mTotalTransferCount.load(std::memory_order_relaxed);
        if (transfers == 0) return 0.0f;

        int underruns = mTotalUnderrunCount.load(std::memory_order_relaxed);
        return static_cast<float>(underruns) / static_cast<float>(transfers);
    }

    void resetCounters() {
        mUnderrunCount.store(0, std::memory_order_relaxed);
        mOverrunCount.store(0, std::memory_order_relaxed);
        mTransferCount.store(0, std::memory_order_relaxed);
    }

    // =========================================================================
    // Configuration
    // =========================================================================

    Config mConfig;
    std::atomic<bool> mEnabled{false};

    // =========================================================================
    // Buffer State
    // =========================================================================

    std::atomic<int> mCurrentBufferMs{100};
    std::atomic<int> mRecommendedBufferMs{100};

    // =========================================================================
    // Counters (reset on each evaluation)
    // =========================================================================

    std::atomic<int> mUnderrunCount{0};
    std::atomic<int> mOverrunCount{0};
    std::atomic<int> mTransferCount{0};

    // =========================================================================
    // Total Counters (cumulative for statistics)
    // =========================================================================

    std::atomic<int> mTotalUnderrunCount{0};
    std::atomic<int> mTotalOverrunCount{0};
    std::atomic<int> mTotalTransferCount{0};
    std::atomic<int> mAdjustmentCount{0};

    // =========================================================================
    // Last Profiler Snapshot
    // =========================================================================

    std::atomic<double> mLastHealthScore{100.0};
    std::atomic<double> mLastJitterUs{0.0};
    std::atomic<double> mLastLatencyMs{0.0};

    // =========================================================================
    // Timing
    // =========================================================================

    Clock::time_point mLastEvaluationTime;
    Clock::time_point mLastChangeTime;
    std::atomic<int64_t> mLastEvaluationTimeMs{0};
};

} // namespace usb
} // namespace noisypad
