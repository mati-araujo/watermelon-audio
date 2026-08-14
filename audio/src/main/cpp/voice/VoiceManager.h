#pragma once

#include "VoicePool.h"
#include "VoiceTriggerSource.h"
#include "TouchTriggerSource.h"
#include <vector>
#include <memory>
#include <atomic>
#include <array>
#include <mutex>
#include "../platform/RtCounter.h"

namespace voice {

// Maximum number of simultaneous voices (for fixed-size note map)
static constexpr int MAX_VOICE_SLOTS = 16;

// Maximum number of trigger sources
static constexpr int MAX_SOURCES = 8;

/**
 * @struct SourceSnapshot
 * @brief Immutable snapshot of registered sources for RT-safe iteration
 *
 * Similar to EffectSnapshot in EffectChain — built on UI thread under mutex,
 * atomically swapped for audio thread consumption.
 */
struct SourceSnapshot {
    std::array<VoiceTriggerSource*, MAX_SOURCES> sources{};
    int count = 0;
};

/**
 * @class VoiceManager
 * @brief Central coordinator for the polyphonic voice system
 *
 * Connects trigger sources to the voice pool:
 * - Registers and manages trigger sources (touch, arpeggiator, etc.)
 * - Processes events from sources and allocates/releases voices
 * - Renders all voices and mixes to output
 *
 * Thread Safety:
 * - registerSource(), unregisterSource(), prepare(), reset() from UI thread
 * - process() is called from audio thread (RT-safe, lock-free)
 * - Sources use atomic snapshot-swap (like EffectChain)
 * - Note map is audio-thread-only (cleared via atomic flag)
 */
class VoiceManager {
public:
    VoiceManager();
    ~VoiceManager() = default;

    // Prevent copy
    VoiceManager(const VoiceManager&) = delete;
    VoiceManager& operator=(const VoiceManager&) = delete;

    // ==================== LIFECYCLE ====================

    /**
     * @brief Prepare the voice manager for playback
     * @param sampleRate Sample rate in Hz
     * @param maxBlockSize Maximum frames per audio callback
     */
    void prepare(int sampleRate, int maxBlockSize);

    /**
     * @brief Reset all voices and sources to initial state
     */
    void reset();

    // ==================== SOURCE REGISTRATION ====================

    /**
     * @brief Register a trigger source
     * @param source Unique pointer to source (ownership transferred)
     */
    void registerSource(std::unique_ptr<VoiceTriggerSource> source);

    /**
     * @brief Unregister a trigger source by ID
     * @param sourceId Source ID to unregister
     */
    void unregisterSource(int sourceId);

    /**
     * @brief Get a trigger source by ID
     * @param sourceId Source ID
     * @return Pointer to source or nullptr if not found
     */
    VoiceTriggerSource* getSource(int sourceId);

    /**
     * @brief Get the TouchTriggerSource (convenience method)
     * @return Pointer to touch source or nullptr if not registered
     */
    TouchTriggerSource* getTouchSource();

    // ==================== MAIN PROCESSING ====================

    /**
     * @brief Process audio for this block
     * @param output Output buffer (stereo interleaved)
     * @param numFrames Number of frames to render
     *
     * RT-SAFE: Called from audio thread. Lock-free.
     */
    void process(float* output, int numFrames);

    // ==================== CONFIGURATION ====================

    /**
     * @brief Set maximum number of voices
     * @param max Maximum voices (1-16)
     */
    void setMaxVoices(int max);

    /**
     * @brief Set voice stealing strategy
     * @param strategy Stealing strategy enum value
     */
    void setStealingStrategy(StealingStrategy strategy);

    /**
     * @brief Set voice allocation configuration
     * @param config Configuration struct
     */
    void setVoiceConfig(const VoiceAllocationConfig& config);

    // ==================== QUERIES ====================

    /**
     * @brief Get number of currently active voices
     * @return Active voice count
     */
    int getActiveVoiceCount() const;

    /**
     * @brief Get number of available voices
     * @return Available voice count
     */
    int getAvailableVoiceCount() const;

    /**
     * @brief Get the voice pool (for advanced configuration)
     * @return Pointer to voice pool
     */
    VoicePool* getVoicePool() { return mVoicePool.get(); }

    /**
     * @brief Get current sample time
     * @return Sample time counter
     */
    uint64_t getCurrentSampleTime() const {
        return mSampleTime.load(std::memory_order_acquire);
    }

private:
    // ==================== EVENT PROCESSING ====================

    void processSourceEvents();
    void handleNoteOn(const VoiceTriggerEvent& event, int sourceId, int priority);
    void handleNoteOff(const VoiceTriggerEvent& event);
    void handleParamChange(const VoiceTriggerEvent& event);

    // ==================== SOURCE SNAPSHOT ====================

    /**
     * @brief Update the active source snapshot after registration changes
     * Must be called with mSourceMutex held.
     */
    void updateSourceSnapshot();

    // ==================== NOTE MAP (audio-thread-only) ====================

    /**
     * @brief Fixed-size note-to-voice map entry
     * noteId == -1 means slot is empty
     */
    struct NoteMapEntry {
        int noteId = -1;
        int voiceIndex = -1;
    };

    /** @brief Find entry by noteId, returns index or -1 */
    int noteMapFind(int noteId) const;

    /** @brief Insert or update mapping */
    void noteMapInsert(int noteId, int voiceIndex);

    /** @brief Erase entry by noteId */
    void noteMapErase(int noteId);

    /** @brief Clear all entries */
    void noteMapClear();

    /** @brief Get current size */
    int noteMapSize() const;

    // ==================== MEMBERS ====================

    std::unique_ptr<VoicePool> mVoicePool;

    // Source ownership (UI thread, protected by mutex)
    std::vector<std::unique_ptr<VoiceTriggerSource>> mSourcesOwned;
    mutable std::mutex mSourceMutex;  // Only for structural changes (register/unregister)

    // Source snapshot for RT thread (atomic pointer swap, like EffectChain)
    std::atomic<SourceSnapshot*> mActiveSourceSnapshot{nullptr};
    SourceSnapshot mSourceSnapshot1;
    SourceSnapshot mSourceSnapshot2;
    std::atomic<bool> mUsingSnapshot1{true};

    // Fixed-size note-to-voice map (audio-thread-only, no locks needed)
    // Cleared via atomic flag set by UI thread, consumed by audio thread
    std::array<NoteMapEntry, MAX_VOICE_SLOTS> mNoteMap{};
    int mNoteMapSize = 0;  // Audio-thread-only counter
    std::atomic<bool> mNoteMapClearRequested{false};

    int mSampleRate = 48000;
    int mMaxBlockSize = 4096;

    std::atomic<uint64_t> mSampleTime{0};

    // WD-1.1 — reemplazan a los LOGW de handleNoteOn/handleParamChange, que
    // corren en el thread de audio (los despacha la cola lock-free de eventos).
    wma::RtCounter mVoiceAllocFailures;   ///< no habia voz libre para un note-on
    wma::RtCounter mParamChangeMisses;    ///< param change para un noteId desconocido

public:
    uint64_t getVoiceAllocFailures() const { return mVoiceAllocFailures.get(); }
    uint64_t getParamChangeMisses() const { return mParamChangeMisses.get(); }

    // Pre-allocated output buffer
    std::vector<float> mOutputBuffer;
};

} // namespace voice
