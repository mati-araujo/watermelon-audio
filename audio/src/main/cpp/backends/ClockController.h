/**
 * ClockController.h
 *
 * Asynchronous Clock Feedback Controller for USB Audio
 *
 * USB Audio devices (UAC 1.0/2.0) with Asynchronous mode use a feedback
 * endpoint to communicate their actual sample rate to the host. This allows
 * the host to adjust the number of samples sent per USB packet to match
 * the device's clock.
 *
 * Without proper clock synchronization:
 * - Buffer underruns (device starves, audio clicks)
 * - Buffer overruns (device overflows, audio glitches)
 * - Long-term drift (buffers grow/shrink over time)
 *
 * Design (post-audit, hallazgos C1/C3/M2/M3):
 *   The feedback value Ff is NOT an error to be "corrected" against the
 *   nominal rate by a PID. Ff *is* the setpoint: it is exactly the number of
 *   audio frames the device wants per service interval. This controller
 *   therefore tracks Ff directly:
 *     - parse Ff (10.14 UAC1 / 16.16 UAC2)
 *     - convert it to frames-per-DATA-PACKET using the real packetsPerSecond
 *       cadence (NOT an assumed 1000/8000 split by UAC version)
 *     - EMA-smooth it
 *     - a pure fractional accumulator turns the smoothed fractional target
 *       into integer per-packet frame counts (the 44-44-44-45 pattern at
 *       44.1 kHz falls out naturally, even with no feedback at all)
 *
 * Thread Safety:
 * - processFeedback()/setMeasuredFramesPerPacket(): USB event thread
 * - getAdjustedFrameCount(): USB transfer/fill thread
 * - Cross-thread state held in atomics; the fractional accumulator is owned
 *   exclusively by the transfer thread.
 *
 * Reference: USB Audio Class 1.0/2.0 Specification, Section 5.12.
 */

#pragma once

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "../usb/UsbConstants.h"

namespace watermelon_audio {

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

class ClockController {
public:
    /**
     * Construct a clock controller.
     *
     * The default cadence (1000 packets/s) matches full-speed / 1 ms service
     * intervals. Real streams MUST call configure() with the cadence derived
     * from USB speed + bInterval; the default exists so unit tests and early
     * construction have sane fractional nominals.
     *
     * @param nominalSampleRate  Expected sample rate (e.g., 48000)
     */
    explicit ClockController(int nominalSampleRate = 48000) {
        configure(nominalSampleRate, kDefaultPacketsPerSecond);
    }

    /**
     * Configure the nominal operating point.
     *
     * @param sampleRateHz       Nominal sample rate (Hz)
     * @param packetsPerSecond   Real data-EP service cadence (from
     *                           TransferConfig: speed + bInterval). NOT
     *                           assumed from the UAC version.
     */
    void configure(int sampleRateHz, int packetsPerSecond) {
        mSampleRate = sampleRateHz;
        mPacketsPerSecond = std::max(1, packetsPerSecond);
        mNominalFramesPerPacket =
            double(sampleRateHz) / double(mPacketsPerSecond);
        reset();
    }

    /**
     * Process feedback from the explicit USB feedback endpoint.
     *
     * UAC 1.0 (Full-Speed): 10.14 fixed point, 3 bytes — samples per 1 ms frame
     * UAC 2.0 (High-Speed): 16.16 fixed point, 4 bytes — samples per 125 µs
     *                       microframe
     *
     * Ff is converted to frames-per-DATA-PACKET via the real cadence:
     *
     *   unitsPerSecond = 1000 (UAC1)  |  8000 (UAC2)
     *   target = Ff * (unitsPerSecond / packetsPerSecond)
     *
     * @param data    Raw feedback bytes
     * @param length  Byte count (3 for UAC1, 4 for UAC2)
     * @param version UAC version for correct parsing
     */
    void processFeedback(const uint8_t* data, int length, UacVersion version) {
        const double ff = parseFeedbackValue(data, length, version);
        if (ff <= 0.0) {
            return;  // malformed / too-short payload
        }
        const double unitsPerSecond =
            (version == UacVersion::UAC_2_0) ? 8000.0 : 1000.0;
        const double target = ff * (unitsPerSecond / double(mPacketsPerSecond));
        applyTarget(target, kFeedbackEmaAlpha);
    }

    /**
     * Feed an externally measured frames-per-output-packet value (implicit
     * feedback, 0.2). Same validation + EMA path as explicit feedback, but a
     * gentler alpha because the measurement window is long and already clean.
     */
    void setMeasuredFramesPerPacket(double framesPerPacket) {
        applyTarget(framesPerPacket, kMeasuredEmaAlpha);
    }

    /**
     * Get the integer frame count to use for every packet of the next output
     * transfer fill.
     *
     * The smoothed fractional target is integrated by a pure accumulator. The
     * caller applies the returned value uniformly to all `packetCount` packets
     * of the transfer, so the accumulator is advanced by `packetCount * target`
     * and the integer is distributed across the packets (the per-packet
     * remainder is carried back into the accumulator — no rounding bias).
     *
     * @param nominalFrames  Nominal integer frames per packet (e.g. 48)
     * @param packetCount    Packets in this transfer fill (>= 1)
     * @return               Per-packet frame count, clamped to
     *                       nominalFrames ± kClockAdjustFramesMax.
     */
    int getAdjustedFrameCount(int nominalFrames, int packetCount) {
        if (packetCount < 1) packetCount = 1;

        const double target = mHasMeasurement.load(std::memory_order_acquire)
            ? mTargetFramesPerPacket.load(std::memory_order_acquire)
            : mNominalFramesPerPacket;

        mAccum += target * double(packetCount);

        // floor(mAccum) — mAccum stays positive in steady state, but guard
        // with std::floor so a transient negative residue still truncates
        // toward -inf consistently.
        double totalF = std::floor(mAccum);
        mAccum -= totalF;
        long long total = static_cast<long long>(totalF);

        long long perPacket = total / packetCount;
        long long remainder = total - perPacket * packetCount;
        // Carry the undistributable remainder back so it lands next fill.
        mAccum += double(remainder);

        const long long lo = nominalFrames - usb::kClockAdjustFramesMax;
        const long long hi = nominalFrames + usb::kClockAdjustFramesMax;
        int frames = static_cast<int>(perPacket);
        if (perPacket < lo) {
            mAccum += double(perPacket - lo) * packetCount;
            frames = static_cast<int>(lo);
        } else if (perPacket > hi) {
            mAccum += double(perPacket - hi) * packetCount;
            frames = static_cast<int>(hi);
        }

        // Anti-windup: keep the catch-up residue bounded so a transient can't
        // produce a long burst of clamped packets after it clears.
        mAccum = std::clamp(mAccum,
                            -2.0 * usb::kClockAdjustFramesMax,
                            +2.0 * usb::kClockAdjustFramesMax);
        return frames;
    }

    /**
     * Reset to nominal (no measurement). Call on stream start / device change.
     */
    void reset() {
        mAccum = 0.0;
        mHasMeasurement.store(false, std::memory_order_release);
        mTargetFramesPerPacket.store(mNominalFramesPerPacket,
                                     std::memory_order_release);
        mCurrentRate.store(static_cast<float>(mSampleRate),
                           std::memory_order_relaxed);
        mDriftPpm.store(0.0f, std::memory_order_relaxed);
        mFrameAdjustment.store(0.0f, std::memory_order_relaxed);
    }

    // =========================================================================
    // Monitoring / Debug (same signatures as before for stats consumers)
    // =========================================================================

    /** Current measured sample rate (Hz), derived from the EMA target. */
    float getCurrentSampleRate() const {
        return mCurrentRate.load(std::memory_order_relaxed);
    }

    /** Clock drift in PPM (positive = device faster than nominal). */
    float getDriftPpm() const {
        return mDriftPpm.load(std::memory_order_relaxed);
    }

    /** Current fractional frame adjustment (target − nominal), for stats. */
    float getCurrentAdjustment() const {
        return mFrameAdjustment.load(std::memory_order_relaxed);
    }

    /** True once at least one valid measurement has been integrated. */
    bool isStable() const {
        return mHasMeasurement.load(std::memory_order_acquire);
    }

    /** Count of feedback/measurement values rejected by the ±10 % gate. */
    uint32_t getFeedbackRejectedCount() const {
        return mFeedbackRejected.load(std::memory_order_relaxed);
    }

    /** Adjust nominal sample rate, keeping the current cadence. */
    void setNominalSampleRate(int sampleRate) {
        configure(sampleRate, mPacketsPerSecond);
    }

    /**
     * Set the UAC version of the connected device. Resets state on an actual
     * change so a stale convergence from a previous device can't bleed in.
     */
    void setUacVersion(UacVersion version) {
        if (mUacVersion != version) {
            mUacVersion = version;
            reset();
        }
    }

    UacVersion getUacVersion() const { return mUacVersion; }

    /** Nominal frames-per-packet (fractional), for tests / diagnostics. */
    double getNominalFramesPerPacket() const { return mNominalFramesPerPacket; }

private:
    static constexpr int kDefaultPacketsPerSecond = 1000;
    static constexpr double kFeedbackEmaAlpha = 0.10;
    static constexpr double kMeasuredEmaAlpha = 0.05;
    static constexpr double kMaxRelativeDeviation = 0.10;  // ±10 %

    /**
     * Validate `target` (frames/packet) against nominal, EMA-smooth it, and
     * publish derived stats. Rejected values are counted and leave the target
     * untouched.
     */
    void applyTarget(double target, double alpha) {
        if (target <= 0.0 || mNominalFramesPerPacket <= 0.0) {
            mFeedbackRejected.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const double rel =
            std::abs(target / mNominalFramesPerPacket - 1.0);
        if (rel > kMaxRelativeDeviation) {
            mFeedbackRejected.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const double prev = mHasMeasurement.load(std::memory_order_acquire)
            ? mTargetFramesPerPacket.load(std::memory_order_acquire)
            : mNominalFramesPerPacket;
        const double ema = prev + alpha * (target - prev);

        mTargetFramesPerPacket.store(ema, std::memory_order_release);
        mHasMeasurement.store(true, std::memory_order_release);

        const double rate = ema * double(mPacketsPerSecond);
        mCurrentRate.store(static_cast<float>(rate), std::memory_order_relaxed);
        mDriftPpm.store(
            static_cast<float>((rate - mSampleRate) / mSampleRate * 1.0e6),
            std::memory_order_relaxed);
        mFrameAdjustment.store(
            static_cast<float>(ema - mNominalFramesPerPacket),
            std::memory_order_relaxed);
    }

    /**
     * Parse raw feedback endpoint data into the device's reported samples per
     * service interval (samples/ms-frame for UAC1, samples/microframe UAC2).
     */
    double parseFeedbackValue(const uint8_t* data, int length,
                              UacVersion version) const {
        if (version == UacVersion::UAC_1_0 && length >= 3) {
            // 10.14 fixed point (3 bytes, little-endian)
            uint32_t raw = static_cast<uint32_t>(data[0]) |
                          (static_cast<uint32_t>(data[1]) << 8) |
                          (static_cast<uint32_t>(data[2]) << 16);
            return static_cast<double>(raw) / 16384.0;  // 2^14
        }
        if (version == UacVersion::UAC_2_0 && length >= 4) {
            // 16.16 fixed point (4 bytes, little-endian)
            uint32_t raw = static_cast<uint32_t>(data[0]) |
                          (static_cast<uint32_t>(data[1]) << 8) |
                          (static_cast<uint32_t>(data[2]) << 16) |
                          (static_cast<uint32_t>(data[3]) << 24);
            return static_cast<double>(raw) / 65536.0;  // 2^16
        }
        return 0.0;  // invalid
    }

    int mSampleRate = 48000;
    int mPacketsPerSecond = kDefaultPacketsPerSecond;
    double mNominalFramesPerPacket = 48.0;
    UacVersion mUacVersion = UacVersion::UNKNOWN;

    // Fractional accumulator — owned by the transfer thread only.
    double mAccum = 0.0;

    // Cross-thread state.
    std::atomic<double> mTargetFramesPerPacket{48.0};
    std::atomic<bool> mHasMeasurement{false};
    std::atomic<float> mCurrentRate{48000.0f};
    std::atomic<float> mDriftPpm{0.0f};
    std::atomic<float> mFrameAdjustment{0.0f};
    std::atomic<uint32_t> mFeedbackRejected{0};
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
