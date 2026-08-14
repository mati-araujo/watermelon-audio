#include "VoicePool.h"
#include "../platform/Logger.h"
#include <cstring>
#include <algorithm>
#include <limits>

#define LOG_TAG "VoicePool"
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) wma::logMessage(wma::LogLevel::WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)

namespace voice {

// ==================== CONSTRUCTOR ====================

VoicePool::VoicePool(const VoiceAllocationConfig& config) {
    // Store config in atomics
    mCfgMaxVoices = std::max(1, std::min(config.maxVoices, 16));
    mCfgReservedForTouch.store(config.reservedForTouch, std::memory_order_relaxed);
    mCfgEnableStealing.store(config.enableStealing, std::memory_order_relaxed);
    mCfgStealingStrategy.store(static_cast<int>(config.stealingStrategy), std::memory_order_relaxed);
    mCfgAttackTimeMs.store(config.attackTimeMs, std::memory_order_relaxed);
    mCfgReleaseTimeMs.store(config.releaseTimeMs, std::memory_order_relaxed);
    mCfgStealReleaseTimeMs.store(config.stealReleaseTimeMs, std::memory_order_relaxed);

    // Create voices
    mVoices.reserve(mCfgMaxVoices);
    for (int i = 0; i < mCfgMaxVoices; ++i) {
        mVoices.push_back(std::make_unique<Voice>());
    }

    // Pre-allocate voice buffers
    mVoiceBuffers.resize(mCfgMaxVoices);

    // Set up default source priorities (atomic writes)
    setSourcePriority(1000, PRIORITY_TOUCH);    // TouchTriggerSource::SOURCE_ID
    setSourcePriority(2000, PRIORITY_ARPEGGIO); // ArpeggiatorTriggerSource::SOURCE_ID (future)
    setSourcePriority(3000, PRIORITY_CHORD);    // ChordEngineTriggerSource::SOURCE_ID (future)

    LOGI("VoicePool created with %d voices", mCfgMaxVoices);
}

// ==================== LIFECYCLE ====================

void VoicePool::prepare(int sampleRate, int maxBlockSize) {
    mSampleRate = sampleRate;
    mMaxBlockSize = maxBlockSize;

    float attackMs = mCfgAttackTimeMs.load(std::memory_order_relaxed);
    float releaseMs = mCfgReleaseTimeMs.load(std::memory_order_relaxed);
    float stealReleaseMs = mCfgStealReleaseTimeMs.load(std::memory_order_relaxed);

    // Prepare all voices
    for (auto& voice : mVoices) {
        voice->prepare(sampleRate, maxBlockSize);
        voice->setAttackTime(attackMs);
        voice->setReleaseTime(releaseMs);
        voice->setStealReleaseTime(stealReleaseMs);
    }

    // Pre-allocate mix buffer (stereo)
    mMixBuffer.resize(maxBlockSize * 2, 0.0f);

    // Pre-allocate voice buffers
    for (auto& buffer : mVoiceBuffers) {
        buffer.resize(maxBlockSize * 2, 0.0f);
    }

    LOGI("VoicePool prepared: sampleRate=%d, maxBlockSize=%d", sampleRate, maxBlockSize);
}

void VoicePool::reset() {
    for (auto& voice : mVoices) {
        voice->reset();
    }
    LOGI("VoicePool reset");
}

// ==================== VOICE ALLOCATION ====================

int VoicePool::allocateVoice(const VoiceParams& params, uint64_t startTime) {

    // First, try to find an available voice
    int voiceIndex = findAvailableVoice(params);

    // If no available voice and stealing is enabled, steal one
    bool stealingEnabled = mCfgEnableStealing.load(std::memory_order_relaxed);
    if (voiceIndex < 0 && stealingEnabled) {
        voiceIndex = selectVoiceToSteal(params);

        if (voiceIndex >= 0) {
            // Steal the voice (trigger fast release)
            mVoices[voiceIndex]->steal();
        }
    }

    // If we found a voice, trigger noteOn
    if (voiceIndex >= 0 && voiceIndex < static_cast<int>(mVoices.size())) {
        mVoices[voiceIndex]->noteOn(params, startTime);
    } else {
    }

    return voiceIndex;
}

void VoicePool::releaseVoice(int voiceIndex) {
    if (voiceIndex >= 0 && voiceIndex < static_cast<int>(mVoices.size())) {
        mVoices[voiceIndex]->noteOff();
    }
}

void VoicePool::releaseBySourceId(int sourceId) {
    for (auto& voice : mVoices) {
        if (voice->getSourceId() == sourceId && voice->isActive()) {
            voice->noteOff();
        }
    }
}

void VoicePool::releaseByNoteId(int noteId) {
    for (auto& voice : mVoices) {
        if (voice->getNoteId() == noteId && voice->isActive()) {
            voice->noteOff();
        }
    }
}

void VoicePool::releaseAll() {
    for (auto& voice : mVoices) {
        if (voice->isActive()) {
            voice->noteOff();
        }
    }
}

// ==================== RENDERING ====================

void VoicePool::renderAll(int numFrames) {
    // Render each voice to its buffer
    for (size_t i = 0; i < mVoices.size(); ++i) {
        if (mVoices[i]->isActive()) {
            mVoices[i]->render(mVoiceBuffers[i].data(), numFrames);
        } else {
            // Clear buffer for inactive voices
            std::memset(mVoiceBuffers[i].data(), 0, numFrames * 2 * sizeof(float));
        }
    }
}

void VoicePool::mixToOutput(float* output, int numFrames) {
    // Clear output buffer
    std::memset(output, 0, numFrames * 2 * sizeof(float));

    // Sum all voice buffers
    for (size_t i = 0; i < mVoices.size(); ++i) {
        if (mVoices[i]->isActive()) {
            const float* voiceBuffer = mVoiceBuffers[i].data();
            for (int j = 0; j < numFrames * 2; ++j) {
                output[j] += voiceBuffer[j];
            }
        }
    }

    // Apply headroom reduction to prevent clipping (divide by sqrt(maxVoices))
    int activeCount = getActiveVoiceCount();
    if (activeCount > 1) {
        float gain = 1.0f / sqrtf(static_cast<float>(activeCount));
        for (int j = 0; j < numFrames * 2; ++j) {
            output[j] *= gain;
        }
    }
}

// ==================== QUERIES ====================

int VoicePool::getActiveVoiceCount() const {
    int count = 0;
    for (const auto& voice : mVoices) {
        if (voice->isActive()) {
            ++count;
        }
    }
    return count;
}

int VoicePool::getAvailableVoiceCount() const {
    int count = 0;
    for (const auto& voice : mVoices) {
        if (voice->isAvailable()) {
            ++count;
        }
    }
    return count;
}

Voice* VoicePool::getVoice(int index) {
    if (index >= 0 && index < static_cast<int>(mVoices.size())) {
        return mVoices[index].get();
    }
    return nullptr;
}

const Voice* VoicePool::getVoice(int index) const {
    if (index >= 0 && index < static_cast<int>(mVoices.size())) {
        return mVoices[index].get();
    }
    return nullptr;
}

int VoicePool::findVoiceByNoteId(int noteId) const {
    for (size_t i = 0; i < mVoices.size(); ++i) {
        if (mVoices[i]->getNoteId() == noteId && mVoices[i]->isActive()) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ==================== CONFIGURATION ====================

void VoicePool::setConfig(const VoiceAllocationConfig& config) {
    // Atomically update config fields (maxVoices cannot change after construction)
    mCfgReservedForTouch.store(config.reservedForTouch, std::memory_order_relaxed);
    mCfgEnableStealing.store(config.enableStealing, std::memory_order_relaxed);
    mCfgStealingStrategy.store(static_cast<int>(config.stealingStrategy), std::memory_order_relaxed);
    mCfgAttackTimeMs.store(config.attackTimeMs, std::memory_order_relaxed);
    mCfgReleaseTimeMs.store(config.releaseTimeMs, std::memory_order_relaxed);
    mCfgStealReleaseTimeMs.store(config.stealReleaseTimeMs, std::memory_order_relaxed);

    // Update envelope times for all voices
    for (auto& voice : mVoices) {
        voice->setAttackTime(config.attackTimeMs);
        voice->setReleaseTime(config.releaseTimeMs);
        voice->setStealReleaseTime(config.stealReleaseTimeMs);
    }
}

VoiceAllocationConfig VoicePool::getConfig() const {
    VoiceAllocationConfig config;
    config.maxVoices = mCfgMaxVoices;
    config.reservedForTouch = mCfgReservedForTouch.load(std::memory_order_relaxed);
    config.enableStealing = mCfgEnableStealing.load(std::memory_order_relaxed);
    config.stealingStrategy = static_cast<StealingStrategy>(
        mCfgStealingStrategy.load(std::memory_order_relaxed));
    config.attackTimeMs = mCfgAttackTimeMs.load(std::memory_order_relaxed);
    config.releaseTimeMs = mCfgReleaseTimeMs.load(std::memory_order_relaxed);
    config.stealReleaseTimeMs = mCfgStealReleaseTimeMs.load(std::memory_order_relaxed);
    return config;
}

void VoicePool::setSourcePriority(int sourceId, int priority) {
    // Find existing entry or empty slot
    for (auto& entry : mSourcePriorities) {
        int existingId = entry.sourceId.load(std::memory_order_relaxed);
        if (existingId == sourceId) {
            entry.priority.store(priority, std::memory_order_relaxed);
            return;
        }
    }
    // Find empty slot
    for (auto& entry : mSourcePriorities) {
        int existingId = entry.sourceId.load(std::memory_order_relaxed);
        if (existingId == -1) {
            entry.priority.store(priority, std::memory_order_relaxed);
            entry.sourceId.store(sourceId, std::memory_order_release);
            return;
        }
    }
    LOGW("setSourcePriority: no free slots for sourceId %d", sourceId);
}

int VoicePool::getSourcePriority(int sourceId) const {
    for (const auto& entry : mSourcePriorities) {
        if (entry.sourceId.load(std::memory_order_acquire) == sourceId) {
            return entry.priority.load(std::memory_order_relaxed);
        }
    }
    return PRIORITY_TOUCH;  // Default
}

// ==================== VOICE SELECTION STRATEGIES ====================

int VoicePool::findAvailableVoice(const VoiceParams& params) const {
    // First, check for same-note retrigger if strategy is SAME_NOTE
    auto strategy = static_cast<StealingStrategy>(
        mCfgStealingStrategy.load(std::memory_order_relaxed));
    if (strategy == StealingStrategy::SAME_NOTE && params.noteId >= 0) {
        int sameNote = findSameNoteVoice(params.noteId);
        if (sameNote >= 0) {
            return sameNote;
        }
    }

    // Find first IDLE voice
    for (size_t i = 0; i < mVoices.size(); ++i) {
        if (mVoices[i]->isAvailable()) {
            return static_cast<int>(i);
        }
    }

    return -1;  // No available voice
}

int VoicePool::selectVoiceToSteal(const VoiceParams& params) const {
    auto strategy = static_cast<StealingStrategy>(
        mCfgStealingStrategy.load(std::memory_order_relaxed));

    switch (strategy) {
        case StealingStrategy::OLDEST:
            return findOldestVoice();

        case StealingStrategy::QUIETEST:
            return findQuietestVoice();

        case StealingStrategy::SAME_NOTE:
            // Try same note first, then fall back to oldest
            {
                int sameNote = findSameNoteVoice(params.noteId);
                if (sameNote >= 0) return sameNote;
                return findOldestVoice();
            }

        case StealingStrategy::LOWEST_PRIORITY:
            return findLowestPriorityVoice(params.sourceId);

        default:
            return findOldestVoice();
    }
}

int VoicePool::findOldestVoice() const {
    uint64_t oldestTime = std::numeric_limits<uint64_t>::max();
    int oldestIndex = -1;

    for (size_t i = 0; i < mVoices.size(); ++i) {
        if (mVoices[i]->isActive()) {
            uint64_t startTime = mVoices[i]->getStartTime();
            if (startTime < oldestTime) {
                oldestTime = startTime;
                oldestIndex = static_cast<int>(i);
            }
        }
    }

    return oldestIndex;
}

int VoicePool::findQuietestVoice() const {
    float quietestLevel = std::numeric_limits<float>::max();
    int quietestIndex = -1;

    for (size_t i = 0; i < mVoices.size(); ++i) {
        if (mVoices[i]->isActive()) {
            float level = mVoices[i]->getCurrentEnvelopeLevel();
            if (level < quietestLevel) {
                quietestLevel = level;
                quietestIndex = static_cast<int>(i);
            }
        }
    }

    return quietestIndex;
}

int VoicePool::findSameNoteVoice(int noteId) const {
    if (noteId < 0) return -1;

    for (size_t i = 0; i < mVoices.size(); ++i) {
        if (mVoices[i]->getNoteId() == noteId) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

int VoicePool::findLowestPriorityVoice(int requestingSourceId) const {
    int requestingPriority = getSourcePriority(requestingSourceId);

    int lowestPriority = std::numeric_limits<int>::max();
    int lowestIndex = -1;

    for (size_t i = 0; i < mVoices.size(); ++i) {
        if (mVoices[i]->isActive()) {
            int voiceSourceId = mVoices[i]->getSourceId();
            int voicePriority = getSourcePriority(voiceSourceId);

            // Only steal voices with lower priority than the requesting source
            if (voicePriority < lowestPriority && voicePriority < requestingPriority) {
                lowestPriority = voicePriority;
                lowestIndex = static_cast<int>(i);
            }
        }
    }

    // If no lower priority voice found, fall back to oldest
    if (lowestIndex < 0) {
        return findOldestVoice();
    }

    return lowestIndex;
}

} // namespace voice
