/**
 * LatencyBenchmark.h
 *
 * Precision latency measurement system for Oboe audio streams.
 *
 * Features:
 * - Output latency measurement via Oboe API
 * - Input latency measurement
 * - Round-trip latency via impulse detection
 * - Statistical analysis (min, max, avg, jitter)
 * - Callback timing measurement
 *
 * Usage:
 *   LatencyBenchmark benchmark;
 *   benchmark.startMeasurement();
 *   // ... run audio for a few seconds ...
 *   auto results = benchmark.getResults();
 */

#pragma once

#include <oboe/Oboe.h>
#include <atomic>
#include <vector>
#include <array>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <mutex>

namespace noisypad {

// =============================================================================
// Measurement Results
// =============================================================================

struct LatencyMeasurement {
    double outputLatencyMs = 0.0;      // DAC latency (frames to speaker)
    double inputLatencyMs = 0.0;       // ADC latency (mic to frames)
    double totalLatencyMs = 0.0;       // Combined output + input
    double roundTripMs = 0.0;          // Measured via impulse (most accurate)

    // Stream info
    int sampleRate = 0;
    int framesPerBurst = 0;
    int bufferCapacity = 0;

    // Calculated
    double burstLatencyMs = 0.0;       // framesPerBurst / sampleRate * 1000
    double bufferLatencyMs = 0.0;      // bufferCapacity / sampleRate * 1000

    bool isValid = false;
};

struct CallbackTiming {
    double avgDurationUs = 0.0;        // Average callback duration
    double maxDurationUs = 0.0;        // Maximum callback duration
    double minDurationUs = 0.0;        // Minimum callback duration
    double jitterUs = 0.0;             // Standard deviation
    double cpuLoadPercent = 0.0;       // Callback time / budget time * 100
    int callbackCount = 0;
    int underrunCount = 0;             // Times callback exceeded budget
};

struct BenchmarkResults {
    LatencyMeasurement latency;
    CallbackTiming timing;

    // Device info
    std::string deviceName;
    std::string apiName;               // "AAudio" or "OpenSL ES"
    bool isExclusiveMode = false;
    bool isLowLatencyMode = false;

    // Recommendations
    std::vector<std::string> recommendations;
};

// =============================================================================
// Callback Timing Tracker
// =============================================================================

/**
 * CallbackTimingTracker
 *
 * Measures the duration of audio callbacks to detect CPU overload.
 *
 * RT-Safe: Only uses atomic operations and pre-allocated storage.
 */
class CallbackTimingTracker {
public:
    static constexpr int MAX_SAMPLES = 1000;

    CallbackTimingTracker() {
        reset();
    }

    /**
     * Call at the START of each audio callback.
     */
    void onCallbackStart() {
        mCallbackStartTime = std::chrono::high_resolution_clock::now();
    }

    /**
     * Call at the END of each audio callback.
     *
     * @param budgetUs  The time budget for this callback in microseconds.
     *                  Typically: (framesPerBurst / sampleRate) * 1e6
     */
    void onCallbackEnd(double budgetUs) {
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            endTime - mCallbackStartTime
        ).count();

        double durationUs = duration / 1000.0;

        // Store in circular buffer (lock-free)
        int index = mSampleCount.fetch_add(1, std::memory_order_relaxed) % MAX_SAMPLES;
        mDurations[index] = durationUs;

        // Track max (atomic update)
        double currentMax = mMaxDurationUs.load(std::memory_order_relaxed);
        while (durationUs > currentMax) {
            if (mMaxDurationUs.compare_exchange_weak(currentMax, durationUs,
                std::memory_order_relaxed)) {
                break;
            }
        }

        // Track min
        double currentMin = mMinDurationUs.load(std::memory_order_relaxed);
        while (durationUs < currentMin) {
            if (mMinDurationUs.compare_exchange_weak(currentMin, durationUs,
                std::memory_order_relaxed)) {
                break;
            }
        }

        // Track underruns
        if (durationUs > budgetUs) {
            mUnderrunCount.fetch_add(1, std::memory_order_relaxed);
        }

        // Running average (exponential moving average)
        double alpha = 0.01;  // Smoothing factor
        double currentAvg = mAvgDurationUs.load(std::memory_order_relaxed);
        double newAvg = currentAvg + alpha * (durationUs - currentAvg);
        mAvgDurationUs.store(newAvg, std::memory_order_relaxed);
    }

    /**
     * Get timing statistics.
     *
     * NOT RT-safe: allocates and calculates.
     */
    CallbackTiming getStatistics(double budgetUs) const {
        CallbackTiming result;

        int count = std::min(mSampleCount.load(), MAX_SAMPLES);
        if (count == 0) {
            return result;
        }

        result.callbackCount = mSampleCount.load();
        result.avgDurationUs = mAvgDurationUs.load();
        result.maxDurationUs = mMaxDurationUs.load();
        result.minDurationUs = mMinDurationUs.load();
        result.underrunCount = mUnderrunCount.load();

        // Calculate jitter (standard deviation)
        double sumSq = 0.0;
        double avg = result.avgDurationUs;
        for (int i = 0; i < count; ++i) {
            double diff = mDurations[i] - avg;
            sumSq += diff * diff;
        }
        result.jitterUs = std::sqrt(sumSq / count);

        // CPU load percentage
        if (budgetUs > 0) {
            result.cpuLoadPercent = (result.avgDurationUs / budgetUs) * 100.0;
        }

        return result;
    }

    void reset() {
        mSampleCount.store(0, std::memory_order_relaxed);
        mAvgDurationUs.store(0.0, std::memory_order_relaxed);
        mMaxDurationUs.store(0.0, std::memory_order_relaxed);
        mMinDurationUs.store(1e9, std::memory_order_relaxed);  // Very high initial
        mUnderrunCount.store(0, std::memory_order_relaxed);
        std::fill(mDurations.begin(), mDurations.end(), 0.0);
    }

private:
    std::chrono::high_resolution_clock::time_point mCallbackStartTime;

    std::array<double, MAX_SAMPLES> mDurations{};
    std::atomic<int> mSampleCount{0};
    std::atomic<double> mAvgDurationUs{0.0};
    std::atomic<double> mMaxDurationUs{0.0};
    std::atomic<double> mMinDurationUs{1e9};
    std::atomic<int> mUnderrunCount{0};
};

// =============================================================================
// Round-Trip Latency Detector
// =============================================================================

/**
 * RoundTripDetector
 *
 * Measures actual round-trip latency by:
 * 1. Generating an impulse (click) on output
 * 2. Detecting the impulse on input
 * 3. Measuring the time between generation and detection
 *
 * This gives the TRUE end-to-end latency including:
 * - Software buffers
 * - DAC latency
 * - Analog path (cable/speaker/mic)
 * - ADC latency
 * - Software input buffers
 *
 * Usage requires physical loopback (output connected to input).
 */
class RoundTripDetector {
public:
    static constexpr int IMPULSE_SAMPLES = 64;        // Impulse length
    static constexpr float IMPULSE_AMPLITUDE = 0.5f;  // -6dB
    static constexpr float DETECTION_THRESHOLD = 0.1f; // Detection level
    static constexpr int MAX_LATENCY_SAMPLES = 48000; // Max 1 second

    enum class State {
        IDLE,
        WAITING_TO_SEND,
        IMPULSE_SENT,
        DETECTED,
        TIMEOUT
    };

    RoundTripDetector() {
        reset();
    }

    /**
     * Start a round-trip measurement.
     *
     * Call this, then process audio until isComplete() returns true.
     */
    void startMeasurement() {
        mState.store(State::WAITING_TO_SEND, std::memory_order_release);
        mImpulseSentSample = 0;
        mImpulseDetectedSample = 0;
        mSampleCounter = 0;
        mMeasuredLatencySamples = -1;
    }

    /**
     * Process output buffer - generates impulse when ready.
     *
     * RT-Safe: Call from audio callback.
     *
     * @param outputBuffer  Stereo interleaved output buffer
     * @param numFrames     Number of frames
     * @param sampleRate    Current sample rate (for timeout calculation)
     */
    void processOutput(float* outputBuffer, int numFrames, int sampleRate) {
        State state = mState.load(std::memory_order_acquire);

        if (state == State::WAITING_TO_SEND) {
            // Wait a bit before sending to let things stabilize
            if (mSampleCounter > sampleRate / 10) {  // 100ms delay
                // Generate impulse at start of buffer
                generateImpulse(outputBuffer, numFrames);
                mImpulseSentSample = mSampleCounter;
                mState.store(State::IMPULSE_SENT, std::memory_order_release);
            }
        }

        mSampleCounter += numFrames;

        // Timeout check
        if (state == State::IMPULSE_SENT) {
            if (mSampleCounter - mImpulseSentSample > MAX_LATENCY_SAMPLES) {
                mState.store(State::TIMEOUT, std::memory_order_release);
            }
        }
    }

    /**
     * Process input buffer - detects impulse.
     *
     * RT-Safe: Call from audio callback.
     *
     * @param inputBuffer   Stereo interleaved input buffer
     * @param numFrames     Number of frames
     */
    void processInput(const float* inputBuffer, int numFrames) {
        State state = mState.load(std::memory_order_acquire);

        if (state != State::IMPULSE_SENT) {
            return;
        }

        // Look for impulse in input
        for (int i = 0; i < numFrames; ++i) {
            float left = inputBuffer[i * 2];
            float right = inputBuffer[i * 2 + 1];
            float magnitude = std::max(std::abs(left), std::abs(right));

            if (magnitude > DETECTION_THRESHOLD) {
                mImpulseDetectedSample = mSampleCounter + i;
                mMeasuredLatencySamples = mImpulseDetectedSample - mImpulseSentSample;
                mState.store(State::DETECTED, std::memory_order_release);
                return;
            }
        }
    }

    /**
     * Check if measurement is complete.
     */
    bool isComplete() const {
        State state = mState.load(std::memory_order_acquire);
        return state == State::DETECTED || state == State::TIMEOUT;
    }

    /**
     * Check if measurement succeeded.
     */
    bool wasSuccessful() const {
        return mState.load(std::memory_order_acquire) == State::DETECTED;
    }

    /**
     * Get measured latency in samples.
     *
     * @return Latency in samples, or -1 if not measured.
     */
    int getLatencySamples() const {
        return mMeasuredLatencySamples;
    }

    /**
     * Get measured latency in milliseconds.
     *
     * @param sampleRate  Sample rate for conversion.
     * @return Latency in ms, or -1 if not measured.
     */
    double getLatencyMs(int sampleRate) const {
        if (mMeasuredLatencySamples < 0 || sampleRate <= 0) {
            return -1.0;
        }
        return (static_cast<double>(mMeasuredLatencySamples) / sampleRate) * 1000.0;
    }

    State getState() const {
        return mState.load(std::memory_order_acquire);
    }

    void reset() {
        mState.store(State::IDLE, std::memory_order_release);
        mSampleCounter = 0;
        mImpulseSentSample = 0;
        mImpulseDetectedSample = 0;
        mMeasuredLatencySamples = -1;
    }

private:
    void generateImpulse(float* buffer, int numFrames) {
        // Generate a short impulse (raised cosine for smooth onset)
        int impulseSamples = std::min(IMPULSE_SAMPLES, numFrames);

        for (int i = 0; i < impulseSamples; ++i) {
            // Raised cosine envelope
            float envelope = 0.5f * (1.0f - std::cos(2.0f * M_PI * i / impulseSamples));
            float sample = IMPULSE_AMPLITUDE * envelope;

            // Write to both channels
            buffer[i * 2] = sample;
            buffer[i * 2 + 1] = sample;
        }
    }

    std::atomic<State> mState{State::IDLE};
    int mSampleCounter = 0;
    int mImpulseSentSample = 0;
    int mImpulseDetectedSample = 0;
    int mMeasuredLatencySamples = -1;
};

// =============================================================================
// Main Benchmark Class
// =============================================================================

/**
 * LatencyBenchmark
 *
 * Comprehensive latency measurement for Oboe streams.
 */
class LatencyBenchmark {
public:
    LatencyBenchmark() = default;

    /**
     * Measure latency from an Oboe output stream.
     */
    LatencyMeasurement measureOutputLatency(oboe::AudioStream* stream) {
        LatencyMeasurement result;

        if (!stream) {
            return result;
        }

        result.sampleRate = stream->getSampleRate();
        result.framesPerBurst = stream->getFramesPerBurst();
        result.bufferCapacity = stream->getBufferCapacityInFrames();

        // Calculate theoretical latencies
        if (result.sampleRate > 0) {
            result.burstLatencyMs =
                (static_cast<double>(result.framesPerBurst) / result.sampleRate) * 1000.0;
            result.bufferLatencyMs =
                (static_cast<double>(result.bufferCapacity) / result.sampleRate) * 1000.0;
        }

        // Get Oboe's calculated latency
        auto latencyResult = stream->calculateLatencyMillis();
        if (latencyResult) {
            result.outputLatencyMs = latencyResult.value();
            result.isValid = true;
        }

        return result;
    }

    /**
     * Measure latency from an Oboe input stream.
     */
    LatencyMeasurement measureInputLatency(oboe::AudioStream* stream) {
        LatencyMeasurement result;

        if (!stream) {
            return result;
        }

        result.sampleRate = stream->getSampleRate();
        result.framesPerBurst = stream->getFramesPerBurst();
        result.bufferCapacity = stream->getBufferCapacityInFrames();

        if (result.sampleRate > 0) {
            result.burstLatencyMs =
                (static_cast<double>(result.framesPerBurst) / result.sampleRate) * 1000.0;
            result.bufferLatencyMs =
                (static_cast<double>(result.bufferCapacity) / result.sampleRate) * 1000.0;
        }

        auto latencyResult = stream->calculateLatencyMillis();
        if (latencyResult) {
            result.inputLatencyMs = latencyResult.value();
            result.isValid = true;
        }

        return result;
    }

    /**
     * Combine input and output measurements.
     */
    LatencyMeasurement measureFullDuplex(
        oboe::AudioStream* outputStream,
        oboe::AudioStream* inputStream
    ) {
        auto output = measureOutputLatency(outputStream);
        auto input = measureInputLatency(inputStream);

        LatencyMeasurement result = output;
        result.inputLatencyMs = input.inputLatencyMs;
        result.totalLatencyMs = output.outputLatencyMs + input.inputLatencyMs;
        result.isValid = output.isValid && input.isValid;

        return result;
    }

    /**
     * Get the callback timing tracker.
     *
     * Use this in your audio callback:
     *   benchmark.getTimingTracker().onCallbackStart();
     *   // ... process audio ...
     *   benchmark.getTimingTracker().onCallbackEnd(budgetUs);
     */
    CallbackTimingTracker& getTimingTracker() {
        return mTimingTracker;
    }

    /**
     * Get the round-trip detector.
     *
     * Use this for physical loopback testing.
     */
    RoundTripDetector& getRoundTripDetector() {
        return mRoundTripDetector;
    }

    /**
     * Generate full benchmark results with recommendations.
     */
    BenchmarkResults generateReport(
        oboe::AudioStream* outputStream,
        oboe::AudioStream* inputStream = nullptr
    ) {
        BenchmarkResults results;

        // Measure latency
        if (inputStream) {
            results.latency = measureFullDuplex(outputStream, inputStream);
        } else {
            results.latency = measureOutputLatency(outputStream);
        }

        // Add round-trip if measured
        if (mRoundTripDetector.wasSuccessful()) {
            results.latency.roundTripMs =
                mRoundTripDetector.getLatencyMs(results.latency.sampleRate);
        }

        // Get timing statistics
        double budgetUs = results.latency.burstLatencyMs * 1000.0;
        results.timing = mTimingTracker.getStatistics(budgetUs);

        // Get stream info
        if (outputStream) {
            results.apiName = (outputStream->getAudioApi() == oboe::AudioApi::AAudio)
                ? "AAudio" : "OpenSL ES";
            results.isExclusiveMode =
                (outputStream->getSharingMode() == oboe::SharingMode::Exclusive);
            results.isLowLatencyMode =
                (outputStream->getPerformanceMode() == oboe::PerformanceMode::LowLatency);
        }

        // Generate recommendations
        generateRecommendations(results);

        return results;
    }

    void reset() {
        mTimingTracker.reset();
        mRoundTripDetector.reset();
    }

private:
    void generateRecommendations(BenchmarkResults& results) {
        // Check if using AAudio
        if (results.apiName != "AAudio") {
            results.recommendations.push_back(
                "Consider upgrading to Android 8.1+ for AAudio support"
            );
        }

        // Check exclusive mode
        if (!results.isExclusiveMode) {
            results.recommendations.push_back(
                "Try Exclusive mode for lower latency (may not work on all devices)"
            );
        }

        // Check buffer size
        if (results.latency.bufferLatencyMs > 20.0) {
            results.recommendations.push_back(
                "Buffer capacity is high - try reducing buffer size"
            );
        }

        // Check CPU load
        if (results.timing.cpuLoadPercent > 70.0) {
            results.recommendations.push_back(
                "CPU load is high - consider simplifying DSP chain"
            );
        }

        // Check underruns
        if (results.timing.underrunCount > 0) {
            float underrunRate =
                static_cast<float>(results.timing.underrunCount) /
                static_cast<float>(results.timing.callbackCount) * 100.0f;

            if (underrunRate > 1.0f) {
                results.recommendations.push_back(
                    "Significant underruns detected - increase buffer size or reduce CPU load"
                );
            }
        }

        // Check jitter
        if (results.timing.jitterUs > 500.0) {
            results.recommendations.push_back(
                "High callback timing jitter - may indicate system load issues"
            );
        }

        // Overall assessment
        if (results.latency.outputLatencyMs < 10.0 &&
            results.timing.cpuLoadPercent < 50.0 &&
            results.timing.underrunCount == 0) {
            results.recommendations.push_back(
                "Audio configuration is well optimized!"
            );
        }
    }

    CallbackTimingTracker mTimingTracker;
    RoundTripDetector mRoundTripDetector;
};

} // namespace noisypad
