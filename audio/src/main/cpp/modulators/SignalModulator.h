#ifndef NOISYPAD_SIGNALMODULATOR_H
#define NOISYPAD_SIGNALMODULATOR_H

#include <cstdint>
#include <atomic>

/**
 * Base class for signal modulators in NoisyPad.
 *
 * Modulators process audio signals to create different presentation modes
 * beyond continuous waveforms (burst, AM, FM, PWM, envelope, etc.).
 *
 * This class follows the same real-time safe design as oscillators:
 * - Lock-free parameter updates using atomics
 * - Virtual process() method for polymorphism
 * - Stateful processing with sample-accurate timing
 */
class SignalModulator {
public:
    virtual ~SignalModulator() = default;

    /**
     * Process audio buffer applying modulation.
     *
     * @param buffer Stereo audio buffer (interleaved L/R)
     * @param numFrames Number of stereo frames to process
     *
     * This method is called from the audio thread and must be real-time safe:
     * - No memory allocations
     * - No locks
     * - No blocking operations
     */
    virtual void process(float* buffer, int32_t numFrames) = 0;

    /**
     * Set a modulator-specific parameter.
     *
     * @param paramId Parameter identifier (modulator-specific)
     * @param value Parameter value (typically 0.0 - 1.0)
     *
     * Real-time safe - can be called from any thread.
     */
    virtual void setParameter(int paramId, float value) = 0;

    /**
     * Reset internal state (phase, envelope, etc.).
     * Called when modulator is enabled or oscillator changes.
     *
     * Real-time safe.
     */
    virtual void reset() = 0;

    /**
     * Set sample rate for time-based calculations.
     *
     * @param sampleRate Sample rate in Hz (typically 48000)
     */
    void setSampleRate(int32_t sampleRate) {
        mSampleRate = sampleRate;
    }

    /**
     * Get current sample rate.
     */
    int32_t getSampleRate() const {
        return mSampleRate;
    }

protected:
    std::atomic<int32_t> mSampleRate{48000};
};

#endif // NOISYPAD_SIGNALMODULATOR_H
