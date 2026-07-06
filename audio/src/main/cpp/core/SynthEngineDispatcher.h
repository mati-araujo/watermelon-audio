#pragma once

#include <memory>
#include <atomic>
#include <array>
#include <vector>
#include "../engines/SynthEngine.h"
#include "../engines/KarplusStrongEngine.h"
#include "../engines/FMEngine.h"
#include "../engines/SupersawEngine.h"
#include "../engines/WavetableEngine.h"
#include "../engines/GranularEngine.h"
#include "../engines/SoundFontEngine.h"
#include "../engines/SoundFontManager.h"

/**
 * @class SynthEngineDispatcher
 * @brief Owns and dispatches all synth engine instances (Phase 1E extraction)
 *
 * Manages:
 * - 5 primary engine instances (KS, FM, Supersaw, Wavetable, Granular)
 * - 5 secondary engine instances for dual touch
 * - 5 voice pools (MAX_VOICE_ENGINES each)
 * - SoundFont engine + manager
 * - Engine type selection with crossfade state
 * - Engine parameter propagation
 *
 * RT-Safety: All getters are lock-free. Engine switching uses atomics.
 * Crossfade state is only modified from the audio thread.
 */
class SynthEngineDispatcher {
public:
    static constexpr int MAX_VOICE_ENGINES = 16;
    static constexpr int ENGINE_CROSSFADE_SAMPLES = 240; // ~5ms at 48kHz

    SynthEngineDispatcher();
    ~SynthEngineDispatcher() = default;

    // Non-copyable, non-movable
    SynthEngineDispatcher(const SynthEngineDispatcher&) = delete;
    SynthEngineDispatcher& operator=(const SynthEngineDispatcher&) = delete;

    // ========== LIFECYCLE ==========

    /**
     * @brief Prepare all engines with the given sample rate and block size
     * @param sampleRate Sample rate in Hz
     * @param maxBlockSize Maximum expected block size in frames
     */
    void prepare(int sampleRate, int maxBlockSize);

    /**
     * @brief Reset all engines (clear state, stop all notes)
     */
    void reset();

    // ========== ENGINE TYPE ==========

    /**
     * @brief Set the active synthesis engine type
     * @param engineType Engine ID (0=CLASSIC, 1=KS, 2=FM, 3=WT, 4=GRAIN, 5=SUPER, 6=SF)
     * Lock-free: Safe to call from any thread.
     */
    void setEngineType(int engineType);

    /**
     * @brief Get the current engine type
     * @return Engine type ID
     */
    int getEngineType() const {
        return mCurrentEngineType.load(std::memory_order_acquire);
    }

    // ========== ENGINE DISPATCH ==========

    /**
     * @brief Get the primary SynthEngine for a given engine type
     * @return Pointer to engine, or nullptr if type is CLASSIC or invalid
     */
    SynthEngine* getEngine(int engineType) const;

    /**
     * @brief Get the secondary SynthEngine for dual touch (touch 2)
     * @return Pointer to engine, or nullptr if type is CLASSIC or invalid
     */
    SynthEngine* getDualTouchEngine(int engineType) const;

    /**
     * @brief Get engine instance from voice pool
     * @param engineType Engine type ID
     * @param voiceIndex Voice index (0 to MAX_VOICE_ENGINES-1)
     * @return Pointer to engine, or nullptr
     */
    SynthEngine* getVoiceEngine(int engineType, int voiceIndex) const;

    // ========== ENGINE PARAMETERS ==========

    /**
     * @brief Set a parameter on all instances of the current engine type
     * @param paramId Parameter index (0 to MAX_ENGINE_PARAMS-1)
     * @param value Parameter value (typically 0.0-1.0)
     *
     * Propagates to primary, secondary (dual touch), and all voice pool instances.
     * No-op for CLASSIC engine.
     * Lock-free: Safe to call from any thread.
     */
    void setEngineParameter(int paramId, float value);

    // ========== CROSSFADE STATE (audio thread only) ==========

    /**
     * @brief Detect engine type change and start crossfade ramp.
     * Call once per audio block from the audio thread.
     *
     * @return The cached engine type for this block
     */
    int detectCrossfadeAndGetType();

    /**
     * @brief Apply crossfade ramp to interleaved stereo buffer
     * @param buffer Interleaved stereo audio data
     * @param numFrames Number of frames
     *
     * If a crossfade is active, multiplies samples by the ramp gain.
     * No-op if no crossfade is in progress.
     */
    void applyCrossfade(float* buffer, int numFrames);

    /**
     * @brief Check if a crossfade is currently active
     */
    bool isCrossfading() const { return mEngineCrossfadeRemaining > 0; }

    // ========== VOICE SYSTEM INTEGRATION ==========

    /**
     * @brief Assign engine pointers to all voices based on current engine type
     * @param voiceManager Pointer to VoiceManager (forward-declared, passed from AudioEngine)
     *
     * NOTE: This method is defined in SynthEngineDispatcher.cpp and includes
     * the VoiceManager headers there to avoid header dependency.
     */
    void updateVoiceEngines(void* voiceManager);

    // ========== SOUNDFONT METHODS ==========

    bool loadSoundFont(const void* data, int size, int sampleRate);
    bool loadSoundFontFromPath(const char* path, int sampleRate);
    bool loadSoundFontFromFd(int fd, int64_t offset, int64_t length, int sampleRate);
    void unloadSoundFont();
    void setSoundFontPreset(int presetIndex);
    int getSoundFontPresetCount() const;
    const char* getSoundFontPresetName(int presetIndex) const;
    bool getSoundFontPresetKeyRange(int presetIndex, int& outMinKey, int& outMaxKey) const;
    bool getSoundFontPresetBankProgram(int presetIndex, int& outBank, int& outProgram) const;
    bool isSoundFontLoaded() const;
    void sfNoteOn(int touchId, int midiNote, float velocity);
    void sfNoteOff(int touchId);
    void sfNoteOffAll();
    void sfNoteOffAllExcept(int keepTouchId);

    /** @brief Direct access to SoundFontEngine (for processAudioBlock rendering) */
    SoundFontEngine* getSoundFontEngine() const { return mSoundFontEngine.get(); }

    /** @brief Direct access to SoundFontManager */
    SoundFontManager* getSoundFontManager() const { return mSoundFontManager.get(); }

    // ========== ENGINE BUFFER ==========

    /** @brief Get pre-allocated engine scratch buffer (for RT-safe rendering) */
    std::vector<float>& getEngineBuffer() { return mEngineBuffer; }

private:
    // Engine type state
    std::atomic<int> mCurrentEngineType{0};

    // Crossfade state (audio thread only — no atomics needed)
    int mPrevEngineType{0};
    float mEngineCrossfadeGain{1.0f};
    int mEngineCrossfadeRemaining{0};

    // Primary engine instances (touch 1 / single touch)
    std::unique_ptr<KarplusStrongEngine> mKarplusStrong;
    std::unique_ptr<FMEngine> mFMEngine;
    std::unique_ptr<SupersawEngine> mSupersawEngine;
    std::unique_ptr<WavetableEngine> mWavetableEngine;
    std::unique_ptr<GranularEngine> mGranularEngine;

    // Secondary engine instances (touch 2 / dual touch)
    std::unique_ptr<KarplusStrongEngine> mKarplusStrong2;
    std::unique_ptr<FMEngine> mFMEngine2;
    std::unique_ptr<SupersawEngine> mSupersawEngine2;
    std::unique_ptr<WavetableEngine> mWavetableEngine2;
    std::unique_ptr<GranularEngine> mGranularEngine2;

    // Voice engine pools (one instance per voice per engine type)
    std::array<std::unique_ptr<KarplusStrongEngine>, MAX_VOICE_ENGINES> mKSPool;
    std::array<std::unique_ptr<FMEngine>, MAX_VOICE_ENGINES> mFMPool;
    std::array<std::unique_ptr<SupersawEngine>, MAX_VOICE_ENGINES> mSupersawPool;
    std::array<std::unique_ptr<WavetableEngine>, MAX_VOICE_ENGINES> mWavetablePool;
    std::array<std::unique_ptr<GranularEngine>, MAX_VOICE_ENGINES> mGranularPool;

    // SoundFont engine (single instance handles all polyphony via tsf)
    std::unique_ptr<SoundFontManager> mSoundFontManager;
    std::unique_ptr<SoundFontEngine> mSoundFontEngine;

    // Pre-allocated buffer for engine output (RT-safe)
    std::vector<float> mEngineBuffer;
};
