#include "VoiceManager.h"
#include "../platform/Logger.h"
#include <cstring>
#include <algorithm>
#include <cassert>

#define LOG_TAG "VoiceManager"
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) wma::logMessage(wma::LogLevel::DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) wma::logMessage(wma::LogLevel::WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)

namespace voice {

// ==================== CONSTRUCTOR ====================

VoiceManager::VoiceManager() {
    // Create voice pool with default configuration
    VoiceAllocationConfig config;
    config.maxVoices = 8;
    config.reservedForTouch = 4;
    config.enableStealing = true;
    config.stealingStrategy = StealingStrategy::OLDEST;
    config.attackTimeMs = 5.0f;
    config.releaseTimeMs = 50.0f;
    config.stealReleaseTimeMs = 40.0f;

    mVoicePool = std::make_unique<VoicePool>(config);

    // Initialize empty source snapshot
    mActiveSourceSnapshot.store(&mSourceSnapshot1, std::memory_order_release);

    LOGI("VoiceManager created");
}

// ==================== LIFECYCLE ====================

void VoiceManager::prepare(int sampleRate, int maxBlockSize) {
    mSampleRate = sampleRate;
    mMaxBlockSize = maxBlockSize;

    // Prepare voice pool
    if (mVoicePool) {
        mVoicePool->prepare(sampleRate, maxBlockSize);
    }

    // Pre-allocate output buffer
    mOutputBuffer.resize(maxBlockSize * 2, 0.0f);

    // Request note map clear (consumed by audio thread)
    mNoteMapClearRequested.store(true, std::memory_order_release);

    LOGI("VoiceManager prepared: sampleRate=%d, maxBlockSize=%d", sampleRate, maxBlockSize);
}

void VoiceManager::reset() {
    if (mVoicePool) {
        mVoicePool->reset();
    }

    // Clear events from all sources (safe: sources are stable via snapshot)
    {
        std::lock_guard<std::mutex> lock(mSourceMutex);
        for (auto& source : mSourcesOwned) {
            if (source) {
                source->clearEvents();
            }
        }
    }

    // Request note map clear (consumed by audio thread)
    mNoteMapClearRequested.store(true, std::memory_order_release);

    // Reset sample time
    mSampleTime.store(0, std::memory_order_release);

    LOGI("VoiceManager reset");
}

// ==================== SOURCE REGISTRATION ====================

void VoiceManager::registerSource(std::unique_ptr<VoiceTriggerSource> source) {
    if (!source) {
        LOGW("Attempted to register null source");
        return;
    }

    std::lock_guard<std::mutex> lock(mSourceMutex);

    int sourceId = source->getSourceId();
    int priority = source->getPriority();

    // Check if source with same ID already exists
    for (const auto& existing : mSourcesOwned) {
        if (existing && existing->getSourceId() == sourceId) {
            LOGW("Source with ID %d already registered", sourceId);
            return;
        }
    }

    if (static_cast<int>(mSourcesOwned.size()) >= MAX_SOURCES) {
        LOGW("Cannot register source: max %d sources reached", MAX_SOURCES);
        return;
    }

    // Set source priority in voice pool
    if (mVoicePool) {
        mVoicePool->setSourcePriority(sourceId, priority);
    }

    mSourcesOwned.push_back(std::move(source));

    // Atomically update snapshot for audio thread
    updateSourceSnapshot();

    LOGI("Registered source ID %d with priority %d", sourceId, priority);
}

void VoiceManager::unregisterSource(int sourceId) {
    std::lock_guard<std::mutex> lock(mSourceMutex);

    for (auto it = mSourcesOwned.begin(); it != mSourcesOwned.end(); ++it) {
        if (*it && (*it)->getSourceId() == sourceId) {
            // Release all voices from this source
            if (mVoicePool) {
                mVoicePool->releaseBySourceId(sourceId);
            }

            mSourcesOwned.erase(it);

            // Atomically update snapshot for audio thread
            updateSourceSnapshot();

            LOGI("Unregistered source ID %d", sourceId);
            return;
        }
    }
    LOGW("Source ID %d not found for unregistration", sourceId);
}

VoiceTriggerSource* VoiceManager::getSource(int sourceId) {
    std::lock_guard<std::mutex> lock(mSourceMutex);
    for (auto& source : mSourcesOwned) {
        if (source && source->getSourceId() == sourceId) {
            return source.get();
        }
    }
    return nullptr;
}

TouchTriggerSource* VoiceManager::getTouchSource() {
    VoiceTriggerSource* source = getSource(TouchTriggerSource::SOURCE_ID);
    if (source && source->getType() == TriggerSourceType::TOUCH) {
        return static_cast<TouchTriggerSource*>(source);
    }
    return nullptr;
}

// ==================== SOURCE SNAPSHOT ====================

void VoiceManager::updateSourceSnapshot() {
    // PRECONDITION: mSourceMutex must be held by caller

    // Determine which snapshot is inactive (not being read by audio thread)
    bool usingSnapshot1 = mUsingSnapshot1.load(std::memory_order_acquire);
    SourceSnapshot* inactiveSnapshot = usingSnapshot1 ? &mSourceSnapshot2 : &mSourceSnapshot1;

    // Build new snapshot
    inactiveSnapshot->count = 0;
    for (const auto& source : mSourcesOwned) {
        if (source && inactiveSnapshot->count < MAX_SOURCES) {
            inactiveSnapshot->sources[inactiveSnapshot->count] = source.get();
            inactiveSnapshot->count++;
        }
    }
    // Clear remaining slots
    for (int i = inactiveSnapshot->count; i < MAX_SOURCES; ++i) {
        inactiveSnapshot->sources[i] = nullptr;
    }

    // Atomic swap: audio thread will pick up new snapshot on next callback
    mActiveSourceSnapshot.store(inactiveSnapshot, std::memory_order_release);
    mUsingSnapshot1.store(!usingSnapshot1, std::memory_order_release);
}

// ==================== NOTE MAP (audio-thread-only) ====================

int VoiceManager::noteMapFind(int noteId) const {
    for (int i = 0; i < mNoteMapSize; ++i) {
        if (mNoteMap[i].noteId == noteId) {
            return i;
        }
    }
    return -1;
}

void VoiceManager::noteMapInsert(int noteId, int voiceIndex) {
    // Check if already exists (update)
    int idx = noteMapFind(noteId);
    if (idx >= 0) {
        mNoteMap[idx].voiceIndex = voiceIndex;
        return;
    }
    // Insert new entry
    if (mNoteMapSize < MAX_VOICE_SLOTS) {
        mNoteMap[mNoteMapSize] = {noteId, voiceIndex};
        mNoteMapSize++;
    }
}

void VoiceManager::noteMapErase(int noteId) {
    int idx = noteMapFind(noteId);
    if (idx >= 0) {
        // Swap with last element for O(1) removal
        mNoteMapSize--;
        if (idx < mNoteMapSize) {
            mNoteMap[idx] = mNoteMap[mNoteMapSize];
        }
        mNoteMap[mNoteMapSize] = {-1, -1};
    }
}

void VoiceManager::noteMapClear() {
    for (int i = 0; i < mNoteMapSize; ++i) {
        mNoteMap[i] = {-1, -1};
    }
    mNoteMapSize = 0;
}

int VoiceManager::noteMapSize() const {
    return mNoteMapSize;
}

// ==================== MAIN PROCESSING ====================

void VoiceManager::process(float* output, int numFrames) {
    if (!mVoicePool) {
        std::memset(output, 0, numFrames * 2 * sizeof(float));
        return;
    }

    // Check if UI thread requested a note map clear
    if (mNoteMapClearRequested.exchange(false, std::memory_order_acquire)) {
        noteMapClear();
    }

    // Get current sample time
    uint64_t sampleTime = mSampleTime.load(std::memory_order_acquire);

    // Read source snapshot atomically (lock-free)
    SourceSnapshot* snapshot = mActiveSourceSnapshot.load(std::memory_order_acquire);

    // Process tick for all sources (for time-based sources like arpeggiator)
    if (snapshot) {
        for (int i = 0; i < snapshot->count; ++i) {
            if (snapshot->sources[i]) {
                snapshot->sources[i]->processTick(sampleTime, numFrames);
            }
        }
    }

    // Process events from all sources
    processSourceEvents();

    // Render all voices
    mVoicePool->renderAll(numFrames);

    // Mix to output
    mVoicePool->mixToOutput(output, numFrames);

    // Update sample time
    mSampleTime.store(sampleTime + numFrames, std::memory_order_release);
}

// ==================== CONFIGURATION ====================

void VoiceManager::setMaxVoices(int max) {
    max = std::max(1, std::min(max, 16));
    LOGI("setMaxVoices: %d (note: requires recreation of VoicePool to take effect)", max);
}

void VoiceManager::setStealingStrategy(StealingStrategy strategy) {
    if (mVoicePool) {
        VoiceAllocationConfig config = mVoicePool->getConfig();
        config.stealingStrategy = strategy;
        mVoicePool->setConfig(config);
        LOGI("Set stealing strategy to %d", static_cast<int>(strategy));
    }
}

void VoiceManager::setVoiceConfig(const VoiceAllocationConfig& config) {
    if (mVoicePool) {
        mVoicePool->setConfig(config);
        LOGI("Updated voice configuration");
    }
}

// ==================== QUERIES ====================

int VoiceManager::getActiveVoiceCount() const {
    return mVoicePool ? mVoicePool->getActiveVoiceCount() : 0;
}

int VoiceManager::getAvailableVoiceCount() const {
    return mVoicePool ? mVoicePool->getAvailableVoiceCount() : 0;
}

// ==================== EVENT PROCESSING ====================

void VoiceManager::processSourceEvents() {
    uint64_t sampleTime = mSampleTime.load(std::memory_order_acquire);

    // Read source snapshot atomically (lock-free)
    SourceSnapshot* snapshot = mActiveSourceSnapshot.load(std::memory_order_acquire);
    if (!snapshot) return;

    for (int i = 0; i < snapshot->count; ++i) {
        auto* source = snapshot->sources[i];
        if (!source) continue;

        int sourceId = source->getSourceId();
        int priority = source->getPriority();

        while (source->hasEvents()) {
            VoiceTriggerEvent event = source->popEvent();

            // Update timestamp if not set
            if (event.timestamp == 0) {
                event.timestamp = sampleTime;
            }

            switch (event.type) {
                case TriggerEventType::NOTE_ON:
                    handleNoteOn(event, sourceId, priority);
                    break;

                case TriggerEventType::NOTE_OFF:
                    handleNoteOff(event);
                    break;

                case TriggerEventType::PARAM_CHANGE:
                    handleParamChange(event);
                    break;
            }
        }
    }
}

void VoiceManager::handleNoteOn(const VoiceTriggerEvent& event, int sourceId, int priority) {
    if (!mVoicePool) return;

    // Create voice parameters
    VoiceParams params;
    params.frequency = event.frequency;
    params.amplitude = event.amplitude;
    params.pan = event.pan;
    params.pressure = event.pressure;
    params.oscillatorType = event.oscillatorType;
    params.sourceId = sourceId;
    params.noteId = event.noteId;

    // Allocate voice
    int voiceIndex = mVoicePool->allocateVoice(params, event.timestamp);

    if (voiceIndex >= 0) {
        // Store mapping for later lookup (audio-thread-only, no lock needed)
        noteMapInsert(event.noteId, voiceIndex);
    } else {
        mVoiceAllocFailures.bump();  // WD-1.1 — era un LOGW en el thread de audio
    }
}

void VoiceManager::handleNoteOff(const VoiceTriggerEvent& event) {
    if (!mVoicePool) return;

    // Find voice by noteId (audio-thread-only linear scan)
    int idx = noteMapFind(event.noteId);
    if (idx >= 0) {
        int voiceIndex = mNoteMap[idx].voiceIndex;
        mVoicePool->releaseVoice(voiceIndex);
        noteMapErase(event.noteId);
    } else {
        // Fallback: try to release by noteId through pool
        mVoicePool->releaseByNoteId(event.noteId);
    }
}

void VoiceManager::handleParamChange(const VoiceTriggerEvent& event) {
    if (!mVoicePool) return;

    // Find voice by noteId (audio-thread-only linear scan)
    int idx = noteMapFind(event.noteId);
    if (idx >= 0) {
        int voiceIndex = mNoteMap[idx].voiceIndex;
        Voice* voice = mVoicePool->getVoice(voiceIndex);
        if (voice) {
            voice->setFrequency(event.frequency);
            voice->setAmplitude(event.amplitude);
            voice->setPan(event.pan);
            voice->setPressure(event.pressure);
        } else {
            mParamChangeMisses.bump();  // WD-1.1
        }
    } else {
        // Fallback: find voice by noteId in pool
        int voiceIndex = mVoicePool->findVoiceByNoteId(event.noteId);
        if (voiceIndex >= 0) {
            Voice* voice = mVoicePool->getVoice(voiceIndex);
            if (voice) {
                voice->setFrequency(event.frequency);
                voice->setAmplitude(event.amplitude);
                voice->setPan(event.pan);
                voice->setPressure(event.pressure);
            }
            // Update mapping (audio-thread-only)
            noteMapInsert(event.noteId, voiceIndex);
        } else {
            mParamChangeMisses.bump();  // WD-1.1
        }
    }
}

} // namespace voice
