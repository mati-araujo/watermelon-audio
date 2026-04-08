#pragma once

#include <vector>
#include <memory>
#include <atomic>
#include "AudioSource.h"
#include "../modulators/SignalModulator.h"
#include "../modulators/BurstModulator.h"
#include "../modulators/AMModulator.h"
#include "../modulators/FMModulator.h"
#include "../modulators/PWMModulator.h"
#include "../modulators/EnvelopeModulator.h"
#include "../modulators/RingModulator.h"
#include "../modulators/GateModulator.h"
#include "../oscillators/Oscillators.h"

/**
 * @class OscillatorBank
 * @brief Owns classic oscillators (primary + dual-touch) and signal modulators.
 *
 * Extracted from AudioEngine (Phase 1E) to reduce class size and clarify
 * ownership boundaries.  All methods are lock-free / RT-safe where noted.
 */
class OscillatorBank {
public:
    OscillatorBank();
    ~OscillatorBank() = default;

    // Non-copyable, non-movable (owned by AudioEngine)
    OscillatorBank(const OscillatorBank&) = delete;
    OscillatorBank& operator=(const OscillatorBank&) = delete;
    OscillatorBank(OscillatorBank&&) = delete;
    OscillatorBank& operator=(OscillatorBank&&) = delete;

    // ========== Lifecycle ==========

    /**
     * @brief Configure all oscillators and modulators with the stream sample rate.
     * @param sampleRate Sample rate in Hz
     *
     * NOT RT-safe — call from start() / configureComponents.
     */
    void prepare(int sampleRate);

    // ========== Oscillator control ==========

    /**
     * @brief Set the active oscillator type.
     * @param typeId 0=Sine, 1=Square, 2=Saw, 3=Triangle, 4=Noise
     * Lock-free.
     */
    void setOscillatorType(int typeId);

    /** @return current oscillator index */
    int getOscillatorType() const {
        return mCurrentOscillatorIndex.load(std::memory_order_acquire);
    }

    /** @return number of oscillator types */
    int getOscillatorCount() const {
        return static_cast<int>(mOscillators.size());
    }

    // ========== Modulator control ==========

    /**
     * @brief Set the active modulator type.
     * @param typeId 0=NONE, 1=BURST, 2=AM, 3=FM, 4=PWM, 5=ENV, 6=RING, 7=GATE
     * Lock-free.
     */
    void setModulatorType(int typeId);

    /** @return current modulator index */
    int getModulatorType() const {
        return mCurrentModulatorIndex.load(std::memory_order_acquire);
    }

    /** @return true when a modulator is active (typeId > 0) */
    bool hasActiveModulator() const {
        return mHasActiveModulator.load(std::memory_order_acquire);
    }

    /**
     * @brief Set a parameter on the current modulator.
     * Lock-free.
     */
    void setModulatorParameter(int paramId, float value);

    // ========== Rendering (RT-safe) ==========

    /**
     * @brief Render the current primary oscillator into an interleaved stereo buffer.
     * @param output  Pre-allocated interleaved stereo buffer (numFrames * 2 floats)
     * @param numFrames Number of stereo frames
     */
    void renderPrimary(float* output, int numFrames);

    /**
     * @brief Render the current dual-touch (secondary) oscillator.
     */
    void renderSecondary(float* output, int numFrames);

    /**
     * @brief Apply the current modulator to an interleaved stereo buffer.
     * No-op when modulator is NONE.
     */
    void applyModulation(float* buffer, int numFrames);

    // ========== Direct access (for AudioGraph sync / legacy code) ==========

    /** Get primary oscillator by index (may be nullptr for NONE) */
    AudioSource* getPrimaryOscillator(int index) const {
        if (index >= 0 && index < static_cast<int>(mOscillators.size()))
            return mOscillators[index].get();
        return nullptr;
    }

    /** Get secondary (dual-touch) oscillator by index */
    AudioSource* getSecondaryOscillator(int index) const {
        if (index >= 0 && index < static_cast<int>(mDualTouchOscillators.size()))
            return mDualTouchOscillators[index].get();
        return nullptr;
    }

    /** Get modulator by index (index 0 is nullptr = NONE) */
    SignalModulator* getModulator(int index) const {
        if (index >= 0 && index < static_cast<int>(mModulators.size()))
            return mModulators[index].get();
        return nullptr;
    }

    /**
     * @brief Set frequency and amplitude on the current primary oscillator.
     * Lock-free.
     */
    void setFrequencyAndAmplitude(float freq, float amp);

    /**
     * @brief Set frequency and amplitude on the current secondary oscillator.
     * Lock-free.
     */
    void setSecondaryFrequencyAndAmplitude(float freq, float amp);

    /**
     * @brief Set frequency and amplitude on ALL primary oscillators.
     * Used by updateXY / setFrequencyAndAmplitude on AudioEngine.
     * Lock-free.
     */
    void setAllPrimaryParams(float freq, float amp);

private:
    // Primary oscillators — order defines type ID (0=Sine, 1=Square, 2=Saw, 3=Tri, 4=Noise)
    std::vector<std::unique_ptr<AudioSource>> mOscillators;

    // Dual-touch oscillators — independent phase/smoothers per touch
    std::vector<std::unique_ptr<AudioSource>> mDualTouchOscillators;

    // Current oscillator type (atomic for UI<->Audio safety)
    std::atomic<int> mCurrentOscillatorIndex{0};

    // Modulators — index 0 = NONE (nullptr), 1-7 = active modulators
    std::vector<std::unique_ptr<SignalModulator>> mModulators;

    // Current modulator type
    std::atomic<int> mCurrentModulatorIndex{0};

    // Early-exit flag for audio callback
    std::atomic<bool> mHasActiveModulator{false};
};
