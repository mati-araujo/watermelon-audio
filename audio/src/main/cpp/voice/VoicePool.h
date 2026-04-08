#pragma once

#include "Voice.h"
#include "VoiceTypes.h"
#include <vector>
#include <memory>
#include <atomic>
#include <array>

namespace voice {

// Maximum number of distinct source types for priority tracking
static constexpr int MAX_SOURCE_TYPES = 8;

/**
 * @struct SourcePriorityEntry
 * @brief Fixed-size entry for source priority lookup (lock-free)
 */
struct SourcePriorityEntry {
    std::atomic<int> sourceId{-1};
    std::atomic<int> priority{0};
};

/**
 * @class VoicePool
 * @brief Manages a pool of polyphonic voices with allocation and stealing
 *
 * Thread Safety:
 * - setConfig(), setSourcePriority() called from UI thread
 * - allocateVoice(), releaseVoice() called from audio thread
 * - renderAll(), mixToOutput() called from audio thread (RT-safe)
 * - Config fields use atomics for cross-thread reads
 * - Source priorities use fixed-size atomic array (no locks)
 */
class VoicePool {
public:
    /**
     * @brief Constructor
     * @param config Voice allocation configuration
     */
    explicit VoicePool(const VoiceAllocationConfig& config);
    ~VoicePool() = default;

    // Prevent copy
    VoicePool(const VoicePool&) = delete;
    VoicePool& operator=(const VoicePool&) = delete;

    // ==================== LIFECYCLE ====================

    void prepare(int sampleRate, int maxBlockSize);
    void reset();

    // ==================== VOICE ALLOCATION ====================

    int allocateVoice(const VoiceParams& params, uint64_t startTime);
    void releaseVoice(int voiceIndex);
    void releaseBySourceId(int sourceId);
    void releaseByNoteId(int noteId);
    void releaseAll();

    // ==================== RENDERING ====================

    void renderAll(int numFrames);
    void mixToOutput(float* output, int numFrames);

    // ==================== QUERIES ====================

    int getActiveVoiceCount() const;
    int getAvailableVoiceCount() const;
    int getTotalVoiceCount() const { return static_cast<int>(mVoices.size()); }
    Voice* getVoice(int index);
    const Voice* getVoice(int index) const;
    int findVoiceByNoteId(int noteId) const;

    // ==================== CONFIGURATION ====================

    /**
     * @brief Update configuration (UI thread)
     * Thread-safe: uses atomic stores for fields read by audio thread.
     */
    void setConfig(const VoiceAllocationConfig& config);

    /**
     * @brief Get current configuration
     * @return Configuration snapshot
     */
    VoiceAllocationConfig getConfig() const;

    /**
     * @brief Set source priority for voice stealing (UI thread)
     * Thread-safe: uses atomic fixed-size array.
     */
    void setSourcePriority(int sourceId, int priority);

private:
    // ==================== VOICE SELECTION STRATEGIES ====================

    int findAvailableVoice(const VoiceParams& params) const;
    int selectVoiceToSteal(const VoiceParams& params) const;
    int findOldestVoice() const;
    int findQuietestVoice() const;
    int findSameNoteVoice(int noteId) const;
    int findLowestPriorityVoice(int requestingSourceId) const;

    /** @brief Look up priority for a sourceId from atomic array */
    int getSourcePriority(int sourceId) const;

    // ==================== MEMBERS ====================

    std::vector<std::unique_ptr<Voice>> mVoices;

    // Config fields accessed by audio thread — use atomics
    std::atomic<int> mCfgReservedForTouch{4};
    std::atomic<bool> mCfgEnableStealing{true};
    std::atomic<int> mCfgStealingStrategy{static_cast<int>(StealingStrategy::OLDEST)};
    std::atomic<float> mCfgAttackTimeMs{5.0f};
    std::atomic<float> mCfgReleaseTimeMs{50.0f};
    std::atomic<float> mCfgStealReleaseTimeMs{10.0f};
    int mCfgMaxVoices = 8;  // Set once in constructor, never changed

    int mSampleRate = 48000;
    int mMaxBlockSize = 4096;

    // Pre-allocated mix buffer (stereo)
    std::vector<float> mMixBuffer;

    // Per-voice render buffers
    std::vector<std::vector<float>> mVoiceBuffers;

    // Fixed-size atomic priority array (replaces unordered_map)
    std::array<SourcePriorityEntry, MAX_SOURCE_TYPES> mSourcePriorities;

    // Default priorities for built-in sources
    static constexpr int PRIORITY_TOUCH = 100;
    static constexpr int PRIORITY_CHORD = 75;
    static constexpr int PRIORITY_ARPEGGIO = 50;
    static constexpr int PRIORITY_MIDI = 50;
    static constexpr int PRIORITY_SEQUENCER = 25;
};

} // namespace voice
