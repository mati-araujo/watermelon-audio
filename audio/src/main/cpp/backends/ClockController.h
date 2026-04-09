/**
 * ClockController.h
 *
 * Asynchronous Clock Feedback Controller for USB Audio
 *
 * USB Audio devices (UAC 1.0/2.0) with Asynchronous mode use a feedback
 * endpoint to communicate their actual sample rate to the host. This allows
 * the host to adjust the number of samples sent per USB frame to match
 * the device's clock.
 *
 * Without proper clock synchronization:
 * - Buffer underruns (device starves, audio clicks)
 * - Buffer overruns (device overflows, audio glitches)
 * - Long-term drift (buffers grow/shrink over time)
 *
 * This controller implements:
 * - Feedback endpoint parsing (10.14 and 16.16 formats)
 * - PID controller for smooth rate adjustment
 * - Fractional sample accumulator for sub-sample precision
 * - Moving average filter for noise rejection
 *
 * Reference: USB Audio Class 2.0 Specification, Section 5.12
 */

#pragma once

#include <atomic>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace watermelon_audio {

// =============================================================================
// PID Controller
// =============================================================================

/**
 * PIDController
 *
 * Discrete PID controller for audio clock synchronization.
 *
 * Design considerations for audio:
 * - Conservative gains to prevent oscillation
 * - Anti-windup to handle large initial errors
 * - Smooth output to prevent audible artifacts
 *
 * Tuning guidelines:
 * - Kp: 0.01 - 0.1 (start low, increase if response is slow)
 * - Ki: 0.001 - 0.01 (eliminates steady-state error)
 * - Kd: 0.0001 - 0.001 (dampens oscillation, often set to 0)
 */
class PIDController {
public:
    /**
     * Construct a PID controller.
     *
     * @param kp  Proportional gain (immediate response to error)
     * @param ki  Integral gain (eliminates steady-state error)
     * @param kd  Derivative gain (dampens oscillation)
     */
    PIDController(float kp = 0.05f, float ki = 0.005f, float kd = 0.0005f)
        : mKp(kp)
        , mKi(ki)
        , mKd(kd)
    {
    }

    /**
     * Calculate control output.
     *
     * @param setpoint    Desired value (expected feedback rate)
     * @param measurement Actual measured value (received feedback)
     * @return            Control adjustment (frames to add/remove)
     */
    float calculate(float setpoint, float measurement) {
        float error = setpoint - measurement;

        // Proportional term
        float p = mKp * error;

        // Integral term with anti-windup
        mIntegral += error;
        mIntegral = std::clamp(mIntegral, -mIntegralLimit, mIntegralLimit);
        float i = mKi * mIntegral;

        // Derivative term (rate of change of error)
        float derivative = error - mLastError;
        float d = mKd * derivative;
        mLastError = error;

        // Combined output with output limiting
        float output = p + i + d;
        return std::clamp(output, mOutputMin, mOutputMax);
    }

    /**
     * Reset controller state.
     *
     * Call when:
     * - Starting a new stream
     * - After a large discontinuity
     * - When switching USB devices
     */
    void reset() {
        mIntegral = 0.0f;
        mLastError = 0.0f;
    }

    // Setters for tuning
    void setGains(float kp, float ki, float kd) {
        mKp = kp;
        mKi = ki;
        mKd = kd;
    }

    void setOutputLimits(float min, float max) {
        mOutputMin = min;
        mOutputMax = max;
    }

    void setIntegralLimit(float limit) {
        mIntegralLimit = limit;
    }

    // Getters for monitoring
    float getProportionalTerm() const { return mKp * mLastError; }
    float getIntegralTerm() const { return mKi * mIntegral; }
    float getDerivativeTerm() const { return mKd * (mLastError - mPrevError); }

private:
    float mKp, mKi, mKd;
    float mIntegral = 0.0f;
    float mLastError = 0.0f;
    float mPrevError = 0.0f;

    float mIntegralLimit = 100.0f;   // Prevent integral windup
    float mOutputMin = -10.0f;        // Max frames to remove per packet
    float mOutputMax = 10.0f;         // Max frames to add per packet
};

// =============================================================================
// USB Audio Class Version
// =============================================================================

enum class UacVersion {
    UNKNOWN = 0,
    UAC_1_0 = 1,    // USB Audio Class 1.0 (Full-Speed, 10.14 feedback)
    UAC_2_0 = 2     // USB Audio Class 2.0 (High-Speed, 16.16 feedback)
};

// =============================================================================
// Clock Controller
// =============================================================================

/**
 * ClockController
 *
 * Processes USB audio feedback endpoint data and provides
 * frame count adjustments to maintain clock synchronization.
 *
 * Thread Safety:
 * - processFeedback(): Called from USB event thread
 * - getAdjustedFrameCount(): Called from USB transfer thread
 * - Both can be called concurrently (uses atomics)
 */
class ClockController {
public:
    /**
     * Construct a clock controller.
     *
     * @param nominalSampleRate  Expected sample rate (e.g., 48000)
     */
    explicit ClockController(int nominalSampleRate = 48000)
        : mNominalSampleRate(nominalSampleRate)
        , mPid(0.05f, 0.005f, 0.0005f)  // Conservative tuning
    {
        // Limit output to ±8 frames per 1ms packet
        mPid.setOutputLimits(-8.0f, 8.0f);

        // Initialize feedback buffer with expected value
        float expectedRate = static_cast<float>(nominalSampleRate) / 8000.0f;
        mFeedbackBuffer.fill(expectedRate);

        mCurrentRate.store(static_cast<float>(nominalSampleRate),
                          std::memory_order_relaxed);
    }

    /**
     * Process feedback from the USB feedback endpoint.
     *
     * USB Audio Feedback Format:
     *
     * UAC 1.0 (Full-Speed): 10.14 fixed point, 3 bytes
     *   - Value represents samples per USB frame (1ms)
     *   - At 48kHz: 48.0 → raw value = 48 * 16384 = 786432
     *
     * UAC 2.0 (High-Speed): 16.16 fixed point, 4 bytes
     *   - Value represents samples per microframe (125µs)
     *   - At 48kHz: 6.0 → raw value = 6 * 65536 = 393216
     *   - Note: High-Speed has 8 microframes per 1ms frame
     *
     * @param data    Raw feedback data from endpoint
     * @param length  Number of bytes in data (3 for UAC1, 4 for UAC2)
     * @param version UAC version for correct parsing
     */
    void processFeedback(const uint8_t* data, int length, UacVersion version) {
        float feedbackRate = parseFeedbackValue(data, length, version);

        if (feedbackRate <= 0.0f) {
            return;  // Invalid feedback data
        }

        // Store in circular buffer for moving average
        mFeedbackBuffer[mFeedbackIndex] = feedbackRate;
        mFeedbackIndex = (mFeedbackIndex + 1) % FEEDBACK_BUFFER_SIZE;

        if (mSamplesReceived < FEEDBACK_BUFFER_SIZE) {
            mSamplesReceived++;
        }

        // Calculate moving average (noise reduction)
        float avgRate = 0.0f;
        int samples = std::min(mSamplesReceived, FEEDBACK_BUFFER_SIZE);
        for (int i = 0; i < samples; ++i) {
            avgRate += mFeedbackBuffer[i];
        }
        avgRate /= static_cast<float>(samples);

        // Calculate expected rate based on USB speed
        float expectedRate;
        if (version == UacVersion::UAC_2_0) {
            // High-Speed: samples per microframe
            expectedRate = static_cast<float>(mNominalSampleRate) / 8000.0f;
        } else {
            // Full-Speed: samples per frame
            expectedRate = static_cast<float>(mNominalSampleRate) / 1000.0f;
        }

        // PID adjustment
        float adjustment = mPid.calculate(expectedRate, avgRate);
        mFrameAdjustment.store(adjustment, std::memory_order_release);

        // Calculate actual sample rate for monitoring
        float actualRate;
        if (version == UacVersion::UAC_2_0) {
            actualRate = avgRate * 8000.0f;  // Convert from samples/microframe
        } else {
            actualRate = avgRate * 1000.0f;  // Convert from samples/frame
        }
        mCurrentRate.store(actualRate, std::memory_order_relaxed);

        // Update statistics
        float drift = actualRate - static_cast<float>(mNominalSampleRate);
        mDriftPpm.store((drift / mNominalSampleRate) * 1000000.0f,
                       std::memory_order_relaxed);
    }

    /**
     * Get adjusted frame count for the next USB packet.
     *
     * Call this before filling each output USB transfer.
     * The returned value accounts for clock drift via the PID controller.
     *
     * @param nominalFrames  Nominal frames per packet (e.g., 48 for 48kHz/1ms)
     * @return               Adjusted frame count (may be ±1-2 from nominal)
     */
    int getAdjustedFrameCount(int nominalFrames) {
        float adjustment = mFrameAdjustment.load(std::memory_order_acquire);

        // Accumulate fractional adjustments
        mFractionalAccumulator += adjustment;

        // Extract integer part when accumulator crosses ±1
        int intAdjustment = 0;
        if (mFractionalAccumulator >= 1.0f) {
            intAdjustment = static_cast<int>(mFractionalAccumulator);
            mFractionalAccumulator -= static_cast<float>(intAdjustment);
        } else if (mFractionalAccumulator <= -1.0f) {
            intAdjustment = static_cast<int>(mFractionalAccumulator);
            mFractionalAccumulator -= static_cast<float>(intAdjustment);
        }

        // Clamp to prevent extreme adjustments
        int result = nominalFrames + intAdjustment;
        return std::clamp(result, nominalFrames - 4, nominalFrames + 4);
    }

    /**
     * Reset the controller state.
     *
     * Call when starting a new stream or after device reconnect.
     */
    void reset() {
        mPid.reset();
        mFractionalAccumulator = 0.0f;
        mFeedbackIndex = 0;
        mSamplesReceived = 0;
        mFrameAdjustment.store(0.0f, std::memory_order_relaxed);

        float expectedRate = static_cast<float>(mNominalSampleRate) / 8000.0f;
        mFeedbackBuffer.fill(expectedRate);
    }

    // =========================================================================
    // Monitoring/Debug
    // =========================================================================

    /**
     * Get current measured sample rate.
     */
    float getCurrentSampleRate() const {
        return mCurrentRate.load(std::memory_order_relaxed);
    }

    /**
     * Get clock drift in parts per million (PPM).
     *
     * Positive = device is faster than nominal
     * Negative = device is slower than nominal
     * Normal range: ±50 PPM for quality devices
     */
    float getDriftPpm() const {
        return mDriftPpm.load(std::memory_order_relaxed);
    }

    /**
     * Get current frame adjustment value.
     */
    float getCurrentAdjustment() const {
        return mFrameAdjustment.load(std::memory_order_relaxed);
    }

    /**
     * Check if controller has received enough feedback data.
     */
    bool isStable() const {
        return mSamplesReceived >= FEEDBACK_BUFFER_SIZE;
    }

    /**
     * Set nominal sample rate.
     */
    void setNominalSampleRate(int sampleRate) {
        mNominalSampleRate = sampleRate;
        reset();
    }

private:
    /**
     * Parse raw feedback endpoint data.
     */
    float parseFeedbackValue(const uint8_t* data, int length, UacVersion version) {
        if (version == UacVersion::UAC_1_0 && length >= 3) {
            // 10.14 fixed point format (3 bytes, little-endian)
            // 10 bits integer, 14 bits fractional
            uint32_t raw = static_cast<uint32_t>(data[0]) |
                          (static_cast<uint32_t>(data[1]) << 8) |
                          (static_cast<uint32_t>(data[2]) << 16);
            return static_cast<float>(raw) / 16384.0f;  // 2^14
        }
        else if (version == UacVersion::UAC_2_0 && length >= 4) {
            // 16.16 fixed point format (4 bytes, little-endian)
            // 16 bits integer, 16 bits fractional
            uint32_t raw = static_cast<uint32_t>(data[0]) |
                          (static_cast<uint32_t>(data[1]) << 8) |
                          (static_cast<uint32_t>(data[2]) << 16) |
                          (static_cast<uint32_t>(data[3]) << 24);
            return static_cast<float>(raw) / 65536.0f;  // 2^16
        }

        return 0.0f;  // Invalid
    }

    static constexpr int FEEDBACK_BUFFER_SIZE = 16;

    int mNominalSampleRate;
    PIDController mPid;

    // Circular buffer for feedback averaging
    std::array<float, FEEDBACK_BUFFER_SIZE> mFeedbackBuffer{};
    int mFeedbackIndex = 0;
    int mSamplesReceived = 0;

    // Fractional accumulator for sub-sample precision
    float mFractionalAccumulator = 0.0f;

    // Atomic state for cross-thread access
    std::atomic<float> mFrameAdjustment{0.0f};
    std::atomic<float> mCurrentRate{48000.0f};
    std::atomic<float> mDriftPpm{0.0f};
};

// =============================================================================
// Clock Statistics
// =============================================================================

/**
 * ClockStatistics
 *
 * Extended statistics for monitoring clock synchronization health.
 */
struct ClockStatistics {
    float currentSampleRate = 0.0f;      // Actual measured rate (Hz)
    float nominalSampleRate = 0.0f;      // Expected rate (Hz)
    float driftPpm = 0.0f;               // Clock drift (parts per million)
    float adjustment = 0.0f;             // Current frame adjustment
    bool isStable = false;               // True if enough samples received

    // Buffer health
    int outputBufferFrames = 0;          // Frames in output ring buffer
    int inputBufferFrames = 0;           // Frames in input ring buffer
    float outputBufferLatencyMs = 0.0f;  // Output buffer latency
    float inputBufferLatencyMs = 0.0f;   // Input buffer latency

    // Underrun/overrun counters
    uint64_t underrunCount = 0;
    uint64_t overrunCount = 0;
};

} // namespace watermelon_audio
