#pragma once

#include "VoiceTypes.h"
#include "../core/AudioSource.h"
#include "../engines/SynthEngine.h"
#include "../dsp/ParameterSmoother.h"
#include "../dsp/StateVariableFilter.h"
#include <memory>
#include <vector>
#include <atomic>
#include <cstdint>

namespace voice {

/**
 * @class Voice
 * @brief Individual polyphonic voice with oscillator, envelope, and state machine
 *
 * Thread Safety:
 * - noteOn(), noteOff(), steal(), setters can be called from UI thread
 * - render() is called from audio thread (RT-safe, lock-free)
 * - Uses atomic operations for cross-thread communication
 *
 * Envelope: Simple Attack/Release (not full ADSR)
 * - Attack: Linear ramp from 0 to 1
 * - Release: Linear ramp from current level to 0
 * - Steal: Fast release for voice stealing
 */
class Voice {
public:
    Voice();
    ~Voice() = default;

    // Prevent copy (owns unique oscillators)
    Voice(const Voice&) = delete;
    Voice& operator=(const Voice&) = delete;

    // Atomics are not movable, so delete move operations
    // VoicePool uses unique_ptr<Voice> for storage instead
    Voice(Voice&&) = delete;
    Voice& operator=(Voice&&) = delete;

    // ==================== LIFECYCLE ====================

    /**
     * @brief Prepare the voice for playback
     * @param sampleRate Sample rate in Hz
     * @param maxBlockSize Maximum number of frames per render call
     *
     * Must be called before render(). Allocates buffers and configures oscillators.
     */
    void prepare(int sampleRate, int maxBlockSize);

    /**
     * @brief Reset voice to initial state (IDLE)
     */
    void reset();

    // ==================== NOTE CONTROL ====================

    /**
     * @brief Start playing a note (NOTE_ON)
     * @param params Voice parameters (frequency, amplitude, etc.)
     * @param startTime Sample time when note started
     *
     * Thread-safe: Can be called from UI thread.
     * Transitions: IDLE -> ATTACK, or re-triggers if already playing.
     */
    void noteOn(const VoiceParams& params, uint64_t startTime);

    /**
     * @brief Stop playing a note (NOTE_OFF)
     *
     * Thread-safe: Can be called from UI thread.
     * Transitions: SUSTAIN/ATTACK -> RELEASE
     */
    void noteOff();

    /**
     * @brief Steal this voice for a new note (fast release)
     *
     * Thread-safe: Can be called from UI thread.
     * Transitions: Any -> STEALING (then IDLE when envelope reaches 0)
     */
    void steal();

    // ==================== REAL-TIME PARAMETER UPDATES ====================

    /**
     * @brief Update frequency while voice is active
     * @param freq Frequency in Hz
     *
     * Thread-safe, lock-free. Safe to call from any thread.
     */
    void setFrequency(float freq);

    /**
     * @brief Update target amplitude while voice is active
     * @param amp Amplitude 0.0-1.0
     *
     * Thread-safe, lock-free. Safe to call from any thread.
     */
    void setAmplitude(float amp);

    /**
     * @brief Update pan position while voice is active
     * @param pan Pan 0.0 (left) to 1.0 (right), 0.5 = center
     *
     * Thread-safe, lock-free. Safe to call from any thread.
     */
    void setPan(float pan);

    /**
     * @brief Update pressure/velocity while voice is active
     * @param pressure Pressure 0.0-1.0
     *
     * Thread-safe, lock-free. Safe to call from any thread.
     */
    void setPressure(float pressure);

    /**
     * @brief Change oscillator type while voice is active
     * @param type Oscillator type (0-4)
     *
     * Thread-safe, lock-free. Safe to call from any thread.
     */
    void setOscillatorType(int type);

    /**
     * @brief Assign a SynthEngine instance for non-classic rendering (Phase 6)
     * @param engine Pointer to engine (non-owning, owned by AudioEngine pool)
     *              Pass nullptr to use classic oscillators
     *
     * Thread-safe via atomic pointer swap.
     */
    void setEngine(SynthEngine* engine);

    // ==================== RENDERING ====================

    /**
     * @brief Render audio output for this voice
     * @param buffer Output buffer (stereo interleaved)
     * @param numFrames Number of frames to render
     *
     * RT-SAFE: No locks, no allocations. Called from audio thread.
     * Returns silence if voice is IDLE.
     */
    void render(float* buffer, int numFrames);

    // ==================== QUERIES ====================

    /**
     * @brief Get current voice state
     * @return VoiceState enum value
     */
    VoiceState getState() const {
        return static_cast<VoiceState>(mState.load(std::memory_order_acquire));
    }

    /**
     * @brief Check if voice is available for allocation
     * @return true if state is IDLE
     */
    bool isAvailable() const {
        return getState() == VoiceState::IDLE;
    }

    /**
     * @brief Check if voice is currently producing sound
     * @return true if not IDLE
     */
    bool isActive() const {
        return getState() != VoiceState::IDLE;
    }

    /**
     * @brief Get the trigger source ID
     * @return Source ID or -1 if none
     */
    int getSourceId() const {
        return mSourceId.load(std::memory_order_acquire);
    }

    /**
     * @brief Get the note ID (for same-note stealing)
     * @return Note ID or -1 if none
     */
    int getNoteId() const {
        return mNoteId.load(std::memory_order_acquire);
    }

    /**
     * @brief Get the sample time when this voice started
     * @return Start time in samples
     */
    uint64_t getStartTime() const {
        return mStartTime.load(std::memory_order_acquire);
    }

    /**
     * @brief Get current envelope level (for quietest voice stealing)
     * @return Envelope level 0.0-1.0
     */
    float getCurrentEnvelopeLevel() const {
        return mEnvelopeLevel.load(std::memory_order_acquire);
    }

    // ==================== CONFIGURATION ====================

    /**
     * @brief Set attack time
     * @param ms Attack time in milliseconds
     */
    void setAttackTime(float ms);

    /**
     * @brief Set decay time
     * @param ms Decay time in milliseconds
     */
    void setDecayTime(float ms);

    /**
     * @brief Set sustain level
     * @param level Sustain level 0.0-1.0
     */
    void setSustainLevel(float level);

    /**
     * @brief Set release time
     * @param ms Release time in milliseconds
     */
    void setReleaseTime(float ms);

    /**
     * @brief Set steal release time (fast release)
     * @param ms Steal release time in milliseconds
     */
    void setStealReleaseTime(float ms);

    // ==================== VOICE FILTER (Phase 6) ====================

    /**
     * @brief Enable/disable per-voice filter
     */
    void setFilterEnabled(bool enabled);

    /**
     * @brief Set filter cutoff frequency
     * @param hz Cutoff in Hz (20-20000)
     */
    void setFilterCutoff(float hz);

    /**
     * @brief Set filter resonance
     * @param q Resonance 0.0-1.0
     */
    void setFilterResonance(float q);

    /**
     * @brief Set filter mode (0=LP, 1=HP, 2=BP)
     */
    void setFilterMode(int mode);

private:
    // ==================== INTERNAL METHODS ====================

    void createOscillators();
    void updateEnvelope(int numFrames);
    void applyPanning(float* buffer, int numFrames);
    void calculateEnvelopeRates();

    // ==================== OSCILLATORS ====================

    // One oscillator per type for seamless switching (Classic engine)
    std::vector<std::unique_ptr<AudioSource>> mOscillators;
    std::atomic<int> mCurrentOscType{0};

    // SynthEngine for non-classic rendering (Phase 6)
    // Non-owning pointer, owned by AudioEngine's engine pool
    std::atomic<SynthEngine*> mEngine{nullptr};

    // ==================== STATE MACHINE ====================

    std::atomic<int> mState{static_cast<int>(VoiceState::IDLE)};

    // ==================== PARAMETERS (ATOMIC FOR LOCK-FREE) ====================

    std::atomic<float> mFrequency{440.0f};
    std::atomic<float> mAmplitude{0.0f};
    std::atomic<float> mPan{0.5f};
    std::atomic<float> mPressure{1.0f};

    // ==================== VOICE IDENTITY ====================

    std::atomic<int> mSourceId{-1};
    std::atomic<int> mNoteId{-1};
    std::atomic<uint64_t> mStartTime{0};

    // ==================== ENVELOPE ====================

    std::atomic<float> mEnvelopeLevel{0.0f};
    float mEnvelopePhase = 0.0f;  // Internal, only used in audio thread

    // ADSR envelope rates (per-sample increment/decrement)
    std::atomic<float> mAttackRate{0.0f};
    std::atomic<float> mDecayRate{0.0f};
    std::atomic<float> mSustainLevel{0.8f};
    std::atomic<float> mReleaseRate{0.0f};
    std::atomic<float> mStealReleaseRate{0.0f};

    // ADSR envelope timing configuration
    float mAttackTimeMs = 5.0f;
    float mDecayTimeMs = 100.0f;
    float mSustainLevelConfig = 0.8f;
    float mReleaseTimeMs = 50.0f;
    float mStealReleaseTimeMs = 10.0f;

    // ==================== PARAMETER SMOOTHERS ====================

    ParameterSmoother mFreqSmoother;
    ParameterSmoother mAmpSmoother;
    ParameterSmoother mPanSmoother;
    ParameterSmoother mPressureSmoother;

    // ==================== CONFIGURATION ====================

    int mSampleRate = 48000;
    int mMaxBlockSize = 4096;

    // ==================== VOICE FILTER (Phase 6) ====================

    StateVariableFilter mFilter;
    std::atomic<bool> mFilterEnabled{false};
    std::atomic<float> mFilterCutoff{8000.0f};  // Hz
    std::atomic<float> mFilterResonance{0.0f};   // 0-1
    std::atomic<int> mFilterMode{0};             // 0=LP, 1=HP, 2=BP
    ParameterSmoother mCutoffSmoother;

    // ==================== PRE-ALLOCATED BUFFER ====================

    std::vector<float> mOscBuffer;  // Temporary buffer for oscillator output
};

} // namespace voice
