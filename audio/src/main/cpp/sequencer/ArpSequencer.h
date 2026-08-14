#pragma once

#include <atomic>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <array>
#include "../platform/Logger.h"

// ========== ARP LOGGING ==========
// Filter with: adb logcat -s ARP_SEQ
#define ARP_SEQ_TAG "ARP_SEQ"

#ifdef NDEBUG
    // Release: only errors
    #define ARP_LOGI(...) ((void)0)
    #define ARP_DIAG(...) ((void)0)
    #define ARP_LOGE(...) wma::logMessage(wma::LogLevel::ERROR, ARP_SEQ_TAG, __VA_ARGS__)
#else
    // Debug: full logging
    #define ARP_LOGI(...) wma::logMessage(wma::LogLevel::INFO, ARP_SEQ_TAG, __VA_ARGS__)
    #define ARP_DIAG(...) wma::logMessage(wma::LogLevel::DEBUG, ARP_SEQ_TAG, __VA_ARGS__)
    #define ARP_LOGE(...) wma::logMessage(wma::LogLevel::ERROR, ARP_SEQ_TAG, __VA_ARGS__)
#endif

/**
 * @class ArpSequencer
 * @brief Lock-free, sample-accurate arpeggiator for the audio thread
 *
 * Runs entirely in the audio callback. Receives base frequency from XY pad
 * (atomic, lock-free) and generates rhythmic note patterns synced to global BPM.
 *
 * Thread Safety:
 * - All setters are lock-free (std::atomic), safe from UI thread
 * - process() is RT-safe: no allocations, no locks, no syscalls
 * - Scale intervals use double-buffering for lock-free swap
 *
 * Diagnostic logging:
 * - Filter with: adb logcat -s ARP_SEQ
 * - Logs note triggers, gate state, step advancement (debug builds only)
 */
class ArpSequencer {
public:
    // Pattern IDs (must match ArpPattern.kt enum IDs)
    static constexpr int PATTERN_UP = 0;
    static constexpr int PATTERN_DOWN = 1;
    static constexpr int PATTERN_UP_DOWN = 2;
    static constexpr int PATTERN_DOWN_UP = 3;
    static constexpr int PATTERN_RANDOM = 4;
    static constexpr int PATTERN_CONVERGE = 5;
    static constexpr int PATTERN_DIVERGE = 6;
    static constexpr int PATTERN_ORDER = 7;
    static constexpr int PATTERN_STOCHASTIC = 8;
    static constexpr int PATTERN_WALK = 9;

    static constexpr int MAX_NOTES = 48;       // 4 octaves * 12 semitones max
    static constexpr int MAX_PATTERN_LEN = 96;  // Up-down doubles the note count

    /**
     * @brief Output from a single process() call
     *
     * The caller (AudioEngine) uses these values to override oscillator/engine
     * frequency and amplitude when the arp is active.
     */
    struct ArpOutput {
        float frequency = 440.0f;   ///< Target frequency in Hz
        float amplitude = 0.0f;     ///< Peak amplitude for this block (velocity * 1.0)
        float gateEnvStart = 0.0f;  ///< Gate envelope level at block START (for per-sample ramp)
        float gateEnvEnd = 0.0f;    ///< Gate envelope level at block END
        bool  gateOn = false;       ///< Gate state (true = note sounding)
        bool  trigger = false;      ///< Rising edge: true only on new note onset
        int   stepIndex = 0;        ///< Current step index (for UI feedback)
        int   totalSteps = 0;       ///< Total steps in current pattern
    };

    ArpSequencer() {
        // Initialize default chromatic scale
        for (int i = 0; i < 12; ++i) {
            mScaleIntervals[0][i] = i;
            mScaleIntervals[1][i] = i;
        }
        mScaleCount[0] = 12;
        mScaleCount[1] = 12;
    }

    // ==================== CONFIGURATION (UI thread, atomic) ====================

    void setEnabled(bool enabled) {
        bool wasEnabled = mEnabled.load(std::memory_order_acquire);
        mEnabled.store(enabled, std::memory_order_release);

        if (enabled && !wasEnabled) {
            // Reset sequencer on enable for clean start
            mNeedsReset.store(true, std::memory_order_release);
            ARP_LOGI("Arpeggiator ENABLED");
        } else if (!enabled && wasEnabled) {
            mGateOpen.store(false, std::memory_order_release);
            ARP_LOGI("Arpeggiator DISABLED");
        }
    }

    bool isEnabled() const {
        return mEnabled.load(std::memory_order_acquire);
    }

    void setPattern(int patternId) {
        // [[maybe_unused]]: sólo lo lee el ARP_LOGI de abajo, que es ((void)0) con NDEBUG.
        [[maybe_unused]] const int old = mPattern.load(std::memory_order_acquire);
        mPattern.store(patternId, std::memory_order_release);
        mNeedsPatternRebuild.store(true, std::memory_order_release);
        ARP_LOGI("Pattern changed: %d -> %d", old, patternId);
    }

    void setSubdivision(float beatsPerStep) {
        mBeatsPerStep.store(beatsPerStep, std::memory_order_release);
        ARP_LOGI("Subdivision changed: %.3f beats/step", beatsPerStep);
    }

    void setOctaveRange(int octaves) {
        octaves = std::clamp(octaves, 1, 4);
        mOctaveRange.store(octaves, std::memory_order_release);
        mNeedsPatternRebuild.store(true, std::memory_order_release);
        ARP_LOGI("Octave range: %d", octaves);
    }

    void setGateLength(float gate) {
        mGateLength.store(std::clamp(gate, 0.05f, 1.0f), std::memory_order_release);
    }

    void setSwing(float swing) {
        mSwing.store(std::clamp(swing, 0.5f, 0.75f), std::memory_order_release);
    }

    void setLatch(bool latch) {
        mLatch.store(latch, std::memory_order_release);
        ARP_LOGI("Latch: %s", latch ? "ON" : "OFF");
    }

    void setVelocity(float vel) {
        mVelocity.store(std::clamp(vel, 0.0f, 1.0f), std::memory_order_release);
    }

    void setVelocityVariation(float var) {
        mVelocityVariation.store(std::clamp(var, 0.0f, 0.5f), std::memory_order_release);
    }

    /** Ratchet: momentary double-time. Hold to activate, release to return. */
    void setRatchet(bool active) {
        mRatchet.store(active, std::memory_order_release);
    }

    /** Trigger pattern regeneration for Random/Stochastic/Walk patterns */
    void regeneratePattern() {
        mNeedsPatternRebuild.store(true, std::memory_order_release);
    }

    void setProbability(float prob) {
        mProbability.store(std::clamp(prob, 0.0f, 1.0f), std::memory_order_release);
    }

    // ==================== INPUT (from XY pad, lock-free) ====================

    /** Base frequency from XY touch position (scale-quantized or raw) */
    void setBaseFrequency(float freq) {
        mBaseFrequency.store(freq, std::memory_order_release);
    }

    /** Touch active state — drives gate when arp is enabled */
    void setTouchActive(bool active) {
        bool wasActive = mTouchActive.load(std::memory_order_acquire);
        mTouchActive.store(active, std::memory_order_release);

        if (active && !wasActive) {
            // Debounce: ignore rapid re-triggers within ~30ms
            // This prevents click-inducing sequencer resets from quick finger lifts
            int samplesSince = mSamplesSinceLastTouch.load(std::memory_order_relaxed);
            if (samplesSince < mTouchDebounceSamples) {
                ARP_DIAG("Touch debounced (samples since last: %d)", samplesSince);
                return;
            }
            // New touch: reset sequencer for immediate sync
            mNeedsReset.store(true, std::memory_order_release);
            mNeedsPatternRebuild.store(true, std::memory_order_release);
            mSamplesSinceLastTouch.store(0, std::memory_order_relaxed);
            ARP_LOGI("Touch START — reset sequencer");
        }
        if (!active && !mLatch.load(std::memory_order_acquire)) {
            mGateOpen.store(false, std::memory_order_release);
            ARP_LOGI("Touch END — gate closed (no latch)");
        }
    }

    /**
     * @brief Set scale intervals for note generation
     * @param intervals Array of semitone offsets (e.g., {0,2,4,5,7,9,11} for major)
     * @param count Number of intervals (1-12)
     *
     * Double-buffered: writes to inactive buffer, then swaps atomically.
     * Safe to call from UI thread while audio is processing.
     */
    void setScaleIntervals(const int* intervals, int count) {
        int writeIdx = 1 - mActiveScaleBuffer.load(std::memory_order_acquire);
        mScaleCount[writeIdx] = std::min(count, 12);
        for (int i = 0; i < mScaleCount[writeIdx]; ++i) {
            mScaleIntervals[writeIdx][i] = intervals[i];
        }
        mActiveScaleBuffer.store(writeIdx, std::memory_order_release);
        mNeedsPatternRebuild.store(true, std::memory_order_release);
        ARP_LOGI("Scale set: %d intervals", count);
    }

    /**
     * @brief Prepare the sequencer with the audio stream's sample rate
     * @param sampleRate Sample rate in Hz (e.g., 48000)
     *
     * Must be called before first process(). NOT called from audio thread.
     */
    void prepare(int sampleRate) {
        mSampleRate = sampleRate;

        // Gate envelope: 2ms attack, 5ms release (anti-click)
        mGateAttackRate = 1.0f / (0.002f * static_cast<float>(sampleRate));
        mGateReleaseRate = 1.0f / (0.005f * static_cast<float>(sampleRate));

        // Touch debounce: ~30ms at actual sample rate
        mTouchDebounceSamples = static_cast<int>(0.030f * static_cast<float>(sampleRate));

        ARP_LOGI("Prepared: sampleRate=%d, gateAttack=%.6f/sample, gateRelease=%.6f/sample, debounce=%d",
                 sampleRate, mGateAttackRate, mGateReleaseRate, mTouchDebounceSamples);
    }

    // ==================== PROCESS (audio thread, RT-safe) ====================

    /**
     * @brief Process one audio block and return arp state
     * @param numFrames Number of frames in this callback
     * @param bpm Current global BPM (from AudioEngine::mBpm)
     * @return ArpOutput with frequency, amplitude, gate, and step info
     *
     * RT-SAFE: No allocations, no locks, no syscalls.
     * Call once per audio callback, BEFORE oscillator/engine rendering.
     */
    ArpOutput process(int numFrames, float bpm) {
        ArpOutput out{};
        out.frequency = mBaseFrequency.load(std::memory_order_acquire);
        out.amplitude = 0.0f;
        out.gateOn = false;
        out.trigger = false;
        out.stepIndex = mCurrentStep;
        out.totalSteps = mPatternLength;

        // Track samples since last touch for debounce (relaxed: approximate is fine)
        mSamplesSinceLastTouch.fetch_add(numFrames, std::memory_order_relaxed);

        if (!mEnabled.load(std::memory_order_acquire)) {
            // When disabled, ensure gate envelope decays to zero
            updateGateEnvelope(false, numFrames);
            return out;
        }

        bool touchActive = mTouchActive.load(std::memory_order_acquire);
        bool latched = mLatch.load(std::memory_order_acquire);
        if (!touchActive && !latched) {
            updateGateEnvelope(false, numFrames);
            return out;
        }

        // Handle reset (new touch or enable)
        if (mNeedsReset.load(std::memory_order_acquire)) {
            mSampleCounter = 0;
            mCurrentStep = 0;
            mCurrentStepActive = true;
            mNeedsReset.store(false, std::memory_order_release);
        }

        // Rebuild note pattern if needed
        if (mNeedsPatternRebuild.load(std::memory_order_acquire)) {
            rebuildPattern();
            mNeedsPatternRebuild.store(false, std::memory_order_release);
        }

        if (mPatternLength == 0) {
            updateGateEnvelope(false, numFrames);
            return out;
        }

        // Calculate timing
        float beatsPerStep = mBeatsPerStep.load(std::memory_order_acquire);
        // Ratchet: halve the step duration for double-time
        if (mRatchet.load(std::memory_order_acquire)) {
            beatsPerStep *= 0.5f;
        }
        float samplesPerBeat = (60.0f / bpm) * static_cast<float>(mSampleRate);
        float baseSamplesPerStep = samplesPerBeat * beatsPerStep;

        // Apply swing: odd steps are delayed
        float swing = mSwing.load(std::memory_order_acquire);
        float currentStepSamples;
        if (mCurrentStep % 2 == 1) {
            currentStepSamples = baseSamplesPerStep * (swing * 2.0f);
        } else {
            currentStepSamples = baseSamplesPerStep * ((1.0f - swing) * 2.0f);
        }

        // Ensure minimum step size to prevent division issues
        if (currentStepSamples < 1.0f) currentStepSamples = 1.0f;

        // Gate length in samples
        float gateLength = mGateLength.load(std::memory_order_acquire);
        float gateSamples = currentStepSamples * gateLength;

        // Advance step if counter exceeds current step duration
        bool newStep = false;
        mSampleCounter += numFrames;
        while (mSampleCounter >= static_cast<int64_t>(currentStepSamples)) {
            mSampleCounter -= static_cast<int64_t>(currentStepSamples);
            // [[maybe_unused]]: sólo lo lee el ARP_DIAG de abajo, que es ((void)0) con NDEBUG.
            [[maybe_unused]] const int prevStep = mCurrentStep;
            mCurrentStep = (mCurrentStep + 1) % mPatternLength;
            newStep = true;

            // Recalculate step duration for new step (swing may differ)
            if (mCurrentStep % 2 == 1) {
                currentStepSamples = baseSamplesPerStep * (swing * 2.0f);
            } else {
                currentStepSamples = baseSamplesPerStep * ((1.0f - swing) * 2.0f);
            }
            if (currentStepSamples < 1.0f) currentStepSamples = 1.0f;
            gateSamples = currentStepSamples * gateLength;

            ARP_DIAG("Step %d -> %d (pattern len=%d)", prevStep, mCurrentStep, mPatternLength);
        }

        // Gate region: we're within the "on" portion of the step
        bool inGateRegion = mSampleCounter < static_cast<int64_t>(gateSamples);

        // Probability check (only on new step)
        if (newStep) {
            float prob = mProbability.load(std::memory_order_acquire);
            mCurrentStepActive = (prob >= 1.0f) || (xorshift32Norm() < prob);
        }

        bool gateOn = inGateRegion && mCurrentStepActive;
        bool trigger = newStep && mCurrentStepActive;

        // Calculate frequency from pattern (using pre-computed ratios, no pow() in hot path)
        float baseFreq = mBaseFrequency.load(std::memory_order_acquire);
        out.frequency = baseFreq * mFreqRatios[mCurrentStep];

        // Apply velocity variation on new step
        if (newStep) {
            float velVar = mVelocityVariation.load(std::memory_order_acquire);
            float baseVel = mVelocity.load(std::memory_order_acquire);
            if (velVar > 0.001f) {
                float r = (xorshift32Norm() - 0.5f) * 2.0f * velVar;
                mCurrentVelocity = std::clamp(baseVel + r, 0.0f, 1.0f);
            } else {
                mCurrentVelocity = baseVel;
            }
        }

        // Gate envelope: return start/end for per-sample interpolation by caller
        float envStart = mGateEnvelopeLevel;
        updateGateEnvelope(gateOn, numFrames);
        float envEnd = mGateEnvelopeLevel;

        out.gateOn = gateOn;
        out.trigger = trigger;
        out.amplitude = mCurrentVelocity;  // Peak amplitude (velocity only)
        out.gateEnvStart = envStart;
        out.gateEnvEnd = envEnd;
        out.stepIndex = mCurrentStep;
        out.totalSteps = mPatternLength;

        // Store for UI feedback
        mGateOpen.store(gateOn, std::memory_order_release);
        mStepForUI.store(mCurrentStep, std::memory_order_release);
        mPatternLengthForUI.store(mPatternLength, std::memory_order_release);
        mGateEnvForUI.store(mGateEnvelopeLevel, std::memory_order_release);

        // Diagnostic logging (throttled: every ~500ms at 48kHz/256 frames)
        if (trigger) {
            mDiagNoteCount++;
            ARP_DIAG("NOTE ON: step=%d, freq=%.1fHz (base=%.1f, +%dst), amp=%.2f",
                     mCurrentStep, out.frequency, baseFreq, mNotePattern[mCurrentStep], out.amplitude);
        }

        return out;
    }

    // ==================== UI FEEDBACK (read from UI thread) ====================

    /** Is the gate currently open? (for visual pulse indicator) */
    bool isGateOpen() const { return mGateOpen.load(std::memory_order_acquire); }

    /** Current step index (for step visualizer) */
    int getCurrentStep() const { return mStepForUI.load(std::memory_order_acquire); }

    /** Total steps in current pattern (for step visualizer) */
    int getTotalSteps() const { return mPatternLengthForUI.load(std::memory_order_acquire); }

    /** Current gate envelope level (for smooth visual feedback) */
    float getGateEnvelopeLevel() const { return mGateEnvForUI.load(std::memory_order_acquire); }

    /** Total notes triggered since last enable (for diagnostics) */
    int getDiagNoteCount() const { return mDiagNoteCount; }

private:
    // ==================== GATE ENVELOPE (anti-click) ====================

    /**
     * @brief Update gate envelope for anti-click smoothing
     * @param gateOn Target gate state
     * @param numFrames Frames in current block (for envelope calculation)
     *
     * Linear attack (2ms) and release (5ms) to prevent clicks on gate transitions.
     * Called once per block; the level is used as a multiplier on amplitude.
     */
    void updateGateEnvelope(bool gateOn, int numFrames) {
        if (gateOn) {
            // Attack: ramp up
            mGateEnvelopeLevel += mGateAttackRate * static_cast<float>(numFrames);
            if (mGateEnvelopeLevel > 1.0f) mGateEnvelopeLevel = 1.0f;
        } else {
            // Release: ramp down
            mGateEnvelopeLevel -= mGateReleaseRate * static_cast<float>(numFrames);
            if (mGateEnvelopeLevel < 0.0f) mGateEnvelopeLevel = 0.0f;
        }
    }

    // ==================== PATTERN GENERATION (audio thread, RT-safe) ====================

    /**
     * @brief Rebuild the note pattern from current scale + octave range + pattern type
     *
     * Called from audio thread when mNeedsPatternRebuild is set.
     * All arrays are pre-allocated (MAX_NOTES, MAX_PATTERN_LEN) — no allocations.
     */
    void rebuildPattern() {
        int scaleIdx = mActiveScaleBuffer.load(std::memory_order_acquire);
        int scaleCount = mScaleCount[scaleIdx];

        // Fallback to chromatic if empty
        if (scaleCount <= 0) {
            scaleCount = 12;
            for (int i = 0; i < 12; ++i) mScaleIntervals[scaleIdx][i] = i;
        }

        int octaves = mOctaveRange.load(std::memory_order_acquire);
        int pattern = mPattern.load(std::memory_order_acquire);

        // Build note list: scale intervals repeated across octave range
        int noteCount = 0;
        for (int oct = 0; oct < octaves && noteCount < MAX_NOTES; ++oct) {
            for (int i = 0; i < scaleCount && noteCount < MAX_NOTES; ++i) {
                mNoteList[noteCount++] = mScaleIntervals[scaleIdx][i] + (oct * 12);
            }
        }

        if (noteCount == 0) {
            mPatternLength = 0;
            // WD-1.1 — sin log: rebuildPattern() corre en el thread de audio.
            return;
        }

        // Apply pattern ordering
        switch (pattern) {
            case PATTERN_UP:          patternUp(noteCount); break;
            case PATTERN_DOWN:        patternDown(noteCount); break;
            case PATTERN_UP_DOWN:     patternUpDown(noteCount); break;
            case PATTERN_DOWN_UP:     patternDownUp(noteCount); break;
            case PATTERN_RANDOM:      patternRandom(noteCount); break;
            case PATTERN_CONVERGE:    patternConverge(noteCount); break;
            case PATTERN_DIVERGE:     patternDiverge(noteCount); break;
            case PATTERN_ORDER:       patternUp(noteCount); break;  // Same as Up for single touch
            case PATTERN_STOCHASTIC:  patternStochastic(noteCount); break;
            case PATTERN_WALK:        patternWalk(noteCount); break;
            default:                  patternUp(noteCount); break;
        }

        // Pre-compute frequency ratios (avoids pow() in audio hot path)
        for (int i = 0; i < mPatternLength; ++i) {
            mFreqRatios[i] = std::pow(2.0f, mNotePattern[i] / 12.0f);
        }

        // Clamp current step to avoid out-of-bounds
        if (mCurrentStep >= mPatternLength) {
            mCurrentStep = 0;
        }

        // WD-1.1 — sin log: rebuildPattern() corre en el thread de audio, y
        // ARP_LOGI solo se compilaba a nada en release. En debug logueaba en
        // cada cambio de acorde, que es exactamente cuando el motor tiene mas
        // trabajo por bloque.
    }

    // --- Pattern implementations ---

    void patternUp(int noteCount) {
        mPatternLength = noteCount;
        for (int i = 0; i < noteCount; ++i) {
            mNotePattern[i] = mNoteList[i];
        }
    }

    void patternDown(int noteCount) {
        mPatternLength = noteCount;
        for (int i = 0; i < noteCount; ++i) {
            mNotePattern[i] = mNoteList[noteCount - 1 - i];
        }
    }

    void patternUpDown(int noteCount) {
        if (noteCount < 2) { patternUp(noteCount); return; }
        mPatternLength = std::min((noteCount * 2) - 2, MAX_PATTERN_LEN);
        // Up portion
        for (int i = 0; i < noteCount; ++i) {
            mNotePattern[i] = mNoteList[i];
        }
        // Down portion (exclude endpoints to avoid repeated notes)
        int idx = noteCount;
        for (int i = noteCount - 2; i > 0 && idx < mPatternLength; --i) {
            mNotePattern[idx++] = mNoteList[i];
        }
        mPatternLength = idx;
    }

    void patternDownUp(int noteCount) {
        if (noteCount < 2) { patternDown(noteCount); return; }
        mPatternLength = std::min((noteCount * 2) - 2, MAX_PATTERN_LEN);
        // Down portion
        for (int i = 0; i < noteCount; ++i) {
            mNotePattern[i] = mNoteList[noteCount - 1 - i];
        }
        // Up portion (exclude endpoints)
        int idx = noteCount;
        for (int i = 1; i < noteCount - 1 && idx < mPatternLength; ++i) {
            mNotePattern[idx++] = mNoteList[i];
        }
        mPatternLength = idx;
    }

    void patternRandom(int noteCount) {
        // Fisher-Yates shuffle (RT-safe with xorshift PRNG)
        mPatternLength = noteCount;
        for (int i = 0; i < noteCount; ++i) mNotePattern[i] = mNoteList[i];
        for (int i = noteCount - 1; i > 0; --i) {
            int j = static_cast<int>(xorshift32Norm() * static_cast<float>(i + 1));
            j = std::clamp(j, 0, i);
            std::swap(mNotePattern[i], mNotePattern[j]);
        }
    }

    void patternConverge(int noteCount) {
        // Alternates between lowest and highest, converging to center
        mPatternLength = noteCount;
        int lo = 0, hi = noteCount - 1, idx = 0;
        while (lo <= hi) {
            if (idx < MAX_PATTERN_LEN) mNotePattern[idx++] = mNoteList[lo++];
            if (lo <= hi && idx < MAX_PATTERN_LEN) mNotePattern[idx++] = mNoteList[hi--];
        }
        mPatternLength = idx;
    }

    void patternDiverge(int noteCount) {
        // Starts from center, expands outward
        mPatternLength = noteCount;
        int mid = noteCount / 2;
        int lo, hi, idx = 0;

        if (noteCount % 2 == 1) {
            if (idx < MAX_PATTERN_LEN) mNotePattern[idx++] = mNoteList[mid];
            lo = mid - 1;
            hi = mid + 1;
        } else {
            lo = mid - 1;
            hi = mid;
        }

        while (idx < noteCount && idx < MAX_PATTERN_LEN) {
            if (lo >= 0) mNotePattern[idx++] = mNoteList[lo--];
            if (hi < noteCount && idx < noteCount && idx < MAX_PATTERN_LEN) {
                mNotePattern[idx++] = mNoteList[hi++];
            }
        }
        mPatternLength = idx;
    }

    void patternStochastic(int noteCount) {
        // Probabilistic: weighted random favoring neighbors
        mPatternLength = noteCount;
        int pos = noteCount / 2;
        for (int i = 0; i < noteCount && i < MAX_PATTERN_LEN; ++i) {
            float r = xorshift32Norm();
            if (r < 0.4f) {
                pos = std::clamp(pos + 1, 0, noteCount - 1);
            } else if (r < 0.8f) {
                pos = std::clamp(pos - 1, 0, noteCount - 1);
            } else {
                pos = static_cast<int>(xorshift32Norm() * static_cast<float>(noteCount));
                pos = std::clamp(pos, 0, noteCount - 1);
            }
            mNotePattern[i] = mNoteList[pos];
        }
    }

    void patternWalk(int noteCount) {
        // Random walk: +-1-2 steps per note
        mPatternLength = noteCount;
        int pos = 0;
        for (int i = 0; i < noteCount && i < MAX_PATTERN_LEN; ++i) {
            mNotePattern[i] = mNoteList[std::clamp(pos, 0, noteCount - 1)];
            float r = xorshift32Norm();
            int step;
            if (r < 0.25f) step = -2;
            else if (r < 0.5f) step = -1;
            else if (r < 0.75f) step = 1;
            else step = 2;
            pos = std::clamp(pos + step, 0, noteCount - 1);
        }
    }

    // ==================== RT-SAFE PRNG ====================

    /** xorshift32 returning normalized float [0, 1) — no syscalls */
    float xorshift32Norm() {
        mRngState ^= mRngState << 13;
        mRngState ^= mRngState >> 17;
        mRngState ^= mRngState << 5;
        return static_cast<float>(mRngState & 0x7FFFFFFF) / 2147483648.0f;
    }

    // ==================== ATOMIC PARAMETERS (UI → audio thread) ====================

    std::atomic<bool>  mEnabled{false};
    std::atomic<int>   mPattern{PATTERN_UP};
    std::atomic<float> mBeatsPerStep{0.5f};     // Default: 1/8 note
    std::atomic<int>   mOctaveRange{1};
    std::atomic<float> mGateLength{0.75f};
    std::atomic<float> mSwing{0.5f};            // 0.5 = straight
    std::atomic<bool>  mLatch{false};
    std::atomic<float> mVelocity{1.0f};
    std::atomic<float> mVelocityVariation{0.0f};
    std::atomic<float> mProbability{1.0f};
    std::atomic<bool>  mRatchet{false};       // Momentary double-time

    // Input from XY pad
    std::atomic<float> mBaseFrequency{440.0f};
    std::atomic<bool>  mTouchActive{false};

    // Scale intervals (double-buffered for lock-free swap)
    std::atomic<int> mActiveScaleBuffer{0};
    int mScaleIntervals[2][12]{};
    int mScaleCount[2]{12, 12};

    // Rebuild/reset flags (set from UI thread, consumed by audio thread)
    std::atomic<bool> mNeedsPatternRebuild{true};
    std::atomic<bool> mNeedsReset{false};

    // ==================== AUDIO THREAD STATE (NOT atomic) ====================
    // Only accessed from the audio callback — no synchronization needed

    int mNoteList[MAX_NOTES]{};              ///< Raw note list (semitone offsets)
    int mNotePattern[MAX_PATTERN_LEN]{};     ///< Ordered pattern (after applying direction)
    float mFreqRatios[MAX_PATTERN_LEN]{};    ///< Pre-computed freq ratios (pow avoidance)
    int mPatternLength = 0;                  ///< Current pattern length
    int mCurrentStep = 0;                    ///< Current step in pattern
    int64_t mSampleCounter = 0;              ///< Sample counter within current step
    bool mCurrentStepActive = true;          ///< Whether current step passed probability check
    float mCurrentVelocity = 1.0f;           ///< Current step velocity (after variation)
    uint32_t mRngState = 0x12345678;         ///< PRNG state (xorshift32)
    std::atomic<int> mSamplesSinceLastTouch{0}; ///< Debounce counter (atomic: written by audio, read by UI)
    int mTouchDebounceSamples = 1440;        ///< ~30ms (updated in prepare() for actual sample rate)

    // Gate envelope (anti-click smoothing)
    // Defaults computed for 48kHz so arp works even if prepare() hasn't been called yet
    float mGateEnvelopeLevel = 0.0f;         ///< Current gate envelope level (0-1)
    float mGateAttackRate = 1.0f / (0.002f * 48000.0f);   ///< Per-sample attack increment (~2ms)
    float mGateReleaseRate = 1.0f / (0.005f * 48000.0f);  ///< Per-sample release decrement (~5ms)

    // Configuration
    int mSampleRate = 48000;

    // Diagnostics
    int mDiagNoteCount = 0;                  ///< Total notes triggered since enable

    // ==================== UI FEEDBACK (atomic, read from UI thread) ====================

    std::atomic<bool> mGateOpen{false};
    std::atomic<int>  mStepForUI{0};
    std::atomic<int>  mPatternLengthForUI{0};
    std::atomic<float> mGateEnvForUI{0.0f};
};
