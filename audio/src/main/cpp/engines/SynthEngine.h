#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include "../dsp/ParameterSmoother.h"

/**
 * @brief Maximum number of parameters per synth engine
 */
static constexpr int MAX_ENGINE_PARAMS = 6;

/**
 * @brief Metadata for a single engine parameter
 *
 * Used by UI to auto-generate controls for each engine.
 */
struct EngineParameterDef {
    const char* name;       ///< Display name (e.g., "Brightness")
    const char* shortName;  ///< Short label for compact UI (e.g., "BRIGHT")
    float minValue;         ///< Minimum value
    float maxValue;         ///< Maximum value
    float defaultValue;     ///< Default value
};

/**
 * @brief Engine type identifiers
 *
 * Must match EngineType.kt enum IDs in core-domain.
 * CLASSIC (0) uses the legacy AudioSource oscillators, not SynthEngine.
 */
enum class EngineTypeId : int {
    CLASSIC = 0,
    KARPLUS_STRONG = 1,
    FM_SYNTH = 2,
    WAVETABLE = 3,
    GRANULAR = 4,
    SUPERSAW = 5,
    SOUNDFONT = 6
};

/**
 * @class SynthEngine
 * @brief Abstract base class for synthesis engines with parameters
 *
 * Each engine implements a distinct synthesis technique (Karplus-Strong,
 * FM, Wavetable, Granular, Supersaw) with up to MAX_ENGINE_PARAMS
 * controllable parameters.
 *
 * RT-Safety contract:
 * - process() must be 100% lock-free: no mutex, no new/malloc, no syscalls
 * - Parameters are updated via std::atomic (UI thread → audio thread)
 * - Buffers must be pre-allocated in prepare(), never in process()
 *
 * Usage:
 *   engine->prepare(sampleRate, maxBlockSize);
 *   engine->setParameter(0, 0.7f);
 *   engine->process(buffer, numFrames, 440.0f, 0.8f);
 */
class SynthEngine {
public:
    virtual ~SynthEngine() = default;

    // ========== Lifecycle ==========

    /**
     * @brief Prepare engine for processing
     * @param sampleRate Sample rate in Hz (typically 48000)
     * @param maxBlockSize Maximum frames per process() call
     *
     * Called once before processing starts. Pre-allocate all buffers here.
     * NOT called from audio thread — allocations are safe.
     */
    virtual void prepare(int32_t sampleRate, int32_t maxBlockSize) {
        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        // Configure parameter smoothers: 5ms smoothing time
        for (auto& smoother : mParamSmoothers) {
            smoother.setSmoothingTime(5.0f, static_cast<float>(sampleRate));
        }
    }

    /**
     * @brief Reset engine state
     *
     * Called on voice retrigger or note-on. Engines should reset
     * their internal state (delay lines, phases, envelopes, etc.)
     * but NOT deallocate buffers.
     *
     * May be called from audio thread — must be RT-safe.
     */
    virtual void reset() = 0;

    // ========== Processing ==========

    /**
     * @brief Generate audio output
     * @param buffer Stereo interleaved output buffer (numFrames * 2 floats)
     *               Engine should WRITE (not accumulate) to this buffer
     * @param numFrames Number of frames to generate
     * @param frequency Target frequency in Hz (from XY mapper / voice system)
     * @param amplitude Target amplitude 0.0-1.0 (from XY mapper / voice system)
     *
     * RT-SAFE: Must not allocate, lock, or syscall.
     * Buffer format: [L0, R0, L1, R1, ...] interleaved stereo
     */
    virtual void process(float* buffer, int32_t numFrames,
                         float frequency, float amplitude) = 0;

    // ========== Parameters (lock-free) ==========

    /**
     * @brief Set an engine-specific parameter
     * @param paramId Parameter index (0 to getParameterCount()-1)
     * @param value Parameter value (typically 0.0-1.0 normalized)
     *
     * Thread-safe: Can be called from UI thread while audio is processing.
     */
    void setParameter(int paramId, float value) {
        if (paramId >= 0 && paramId < MAX_ENGINE_PARAMS) {
            mParams[paramId].store(value, std::memory_order_release);
        }
    }

    /**
     * @brief Get current value of an engine parameter
     * @param paramId Parameter index
     * @return Current value, or 0.0f if paramId is out of range
     */
    float getParameter(int paramId) const {
        if (paramId >= 0 && paramId < MAX_ENGINE_PARAMS) {
            return mParams[paramId].load(std::memory_order_acquire);
        }
        return 0.0f;
    }

    // ========== Metadata ==========

    /**
     * @brief Get engine display name
     * @return Human-readable name (e.g., "Karplus-Strong")
     */
    virtual const char* getName() const = 0;

    /**
     * @brief Get number of controllable parameters
     * @return Parameter count (0 to MAX_ENGINE_PARAMS)
     */
    virtual int getParameterCount() const = 0;

    /**
     * @brief Get metadata for a specific parameter
     * @param paramId Parameter index
     * @return Parameter definition with name, range, and default
     */
    virtual EngineParameterDef getParameterDef(int paramId) const = 0;

    // ========== Tempo sync ==========

    /**
     * @brief Set global BPM for tempo-aware engines
     * @param bpm Beats per minute (20-300)
     */
    virtual void setBpm(float bpm) {
        mBpm.store(bpm, std::memory_order_release);
    }

protected:
    /// Engine parameters (lock-free, UI → audio thread)
    std::array<std::atomic<float>, MAX_ENGINE_PARAMS> mParams{};

    /// Parameter smoothers (one per param, configured in prepare())
    std::array<ParameterSmoother, MAX_ENGINE_PARAMS> mParamSmoothers;

    /**
     * @brief Read a parameter with smoothing applied
     * @param paramId Parameter index
     * @return Smoothed value (call once per sample for best results, or once per block)
     *
     * RT-safe. Use this instead of direct mParams[].load() for per-block reads.
     */
    float smoothParam(int paramId) {
        float target = mParams[paramId].load(std::memory_order_relaxed);
        return mParamSmoothers[paramId].process(target);
    }

    /// Global BPM for tempo sync
    std::atomic<float> mBpm{120.0f};

    /// Sample rate (set in prepare())
    int32_t mSampleRate = 48000;

    /// Max block size (set in prepare())
    int32_t mMaxBlockSize = 0;
};
