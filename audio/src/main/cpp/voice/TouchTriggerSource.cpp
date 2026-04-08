#include "TouchTriggerSource.h"
#include "../platform/Logger.h"
#include <cmath>
#include <algorithm>

#define LOG_TAG "TouchTriggerSource"
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) wma::logMessage(wma::LogLevel::DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) wma::logMessage(wma::LogLevel::WARN, LOG_TAG, __VA_ARGS__)

namespace voice {

// ==================== CONSTRUCTOR ====================

TouchTriggerSource::TouchTriggerSource() {
    // Initialize previous touch data as inactive
    for (auto& touch : mPrevTouchData) {
        touch.active = false;
        touch.pointerId = -1;
    }
}

// ==================== VoiceTriggerSource INTERFACE ====================

void TouchTriggerSource::processTick(uint64_t sampleTime, int numFrames) {
    // TouchTriggerSource doesn't generate time-based events
    // (unlike Arpeggiator which would generate notes based on tempo)
    // Events are generated in updateTouches() when touch state changes
}

bool TouchTriggerSource::hasEvents() const {
    return mEventQueue.hasEvents();
}

VoiceTriggerEvent TouchTriggerSource::popEvent() {
    return popEventInternal();
}

void TouchTriggerSource::clearEvents() {
    clearEventsInternal();
}

// ==================== TOUCH INPUT ====================

void TouchTriggerSource::updateTouches(const TouchData* touches, int count) {
    std::lock_guard<std::mutex> lock(mTouchMutex);

    // Clamp count to valid range
    count = std::max(0, std::min(count, MAX_TOUCH_POINTS));

    // Get current timestamp (approximate, will be refined in audio callback)
    uint64_t timestamp = 0;  // VoiceManager will use its own sample time

    // BUG FIX: Track touches by pointerId, not by array slot index.
    // This fixes the issue where removing one touch causes remaining touches
    // to shift positions in the array, leading to wrong voice updates.

    // Step 1: Mark which existing slots are still active by checking pointerId match
    std::array<bool, MAX_TOUCH_POINTS> slotMatched = {false, false, false, false};
    std::array<int, MAX_TOUCH_POINTS> touchMatchedToSlot;  // Which slot each incoming touch matched to
    for (int i = 0; i < MAX_TOUCH_POINTS; ++i) {
        touchMatchedToSlot[i] = -1;  // -1 means no slot found yet
    }

    // Step 2: Match incoming touches to existing slots by pointerId
    for (int touchIdx = 0; touchIdx < count; ++touchIdx) {
        if (!touches[touchIdx].active || touches[touchIdx].amplitude <= 0.001f) {
            continue;
        }

        int incomingPointerId = touches[touchIdx].pointerId;

        // Look for an existing slot with the same pointerId
        for (int slotIdx = 0; slotIdx < MAX_TOUCH_POINTS; ++slotIdx) {
            if (mPrevTouchData[slotIdx].active &&
                mPrevTouchData[slotIdx].pointerId == incomingPointerId) {
                // Found a match!
                touchMatchedToSlot[touchIdx] = slotIdx;
                slotMatched[slotIdx] = true;
                break;
            }
        }
    }

    // Step 3: Process ended touches (slots that were active but didn't get matched)
    bool anyTouchEnded = false;
    for (int slotIdx = 0; slotIdx < MAX_TOUCH_POINTS; ++slotIdx) {
        if (mPrevTouchData[slotIdx].active && !slotMatched[slotIdx]) {
            processEndedTouch(slotIdx, timestamp);
            mPrevTouchData[slotIdx].active = false;
            mPrevTouchData[slotIdx].pointerId = -1;
            anyTouchEnded = true;
        }
    }

    // Step 4: Process matched touches (PARAM_CHANGE) and new touches (NOTE_ON)
    for (int touchIdx = 0; touchIdx < count; ++touchIdx) {
        if (!touches[touchIdx].active || touches[touchIdx].amplitude <= 0.001f) {
            continue;
        }

        int slotIdx = touchMatchedToSlot[touchIdx];

        if (slotIdx >= 0) {
            // Existing touch - check for parameter changes
            // FIX: When a sibling touch ended, force PARAM_CHANGE for remaining
            // touches to ensure their voices get refreshed (bypasses threshold check)
            processChangedTouch(slotIdx, touches[touchIdx], timestamp, anyTouchEnded);
            mPrevTouchData[slotIdx] = touches[touchIdx];
        } else {
            // New touch - find a free slot
            int freeSlot = -1;
            for (int s = 0; s < MAX_TOUCH_POINTS; ++s) {
                if (!mPrevTouchData[s].active) {
                    freeSlot = s;
                    break;
                }
            }

            if (freeSlot >= 0) {
                processNewTouch(freeSlot, touches[touchIdx], timestamp);
                mPrevTouchData[freeSlot] = touches[touchIdx];
            } else {
                LOGW("  Touch[%d] pointerId=%d: No free slot available!",
                     touchIdx, touches[touchIdx].pointerId);
            }
        }
    }
}

// ==================== QUERIES ====================

int TouchTriggerSource::getActiveTouchCount() const {
    int count = 0;
    for (const auto& state : mTouchStates) {
        if (state.active.load(std::memory_order_acquire)) {
            ++count;
        }
    }
    return count;
}

TouchData TouchTriggerSource::getTouchData(int index) const {
    if (index < 0 || index >= MAX_TOUCH_POINTS) {
        return TouchData{};
    }

    TouchData data;
    data.active = mTouchStates[index].active.load(std::memory_order_acquire);
    data.frequency = mTouchStates[index].frequency.load(std::memory_order_acquire);
    data.amplitude = mTouchStates[index].amplitude.load(std::memory_order_acquire);
    data.pressure = mTouchStates[index].pressure.load(std::memory_order_acquire);
    data.pointerId = mTouchStates[index].pointerId.load(std::memory_order_acquire);
    return data;
}

// ==================== CONFIGURATION ====================

void TouchTriggerSource::setOscillatorType(int type) {
    mOscillatorType.store(std::max(0, std::min(type, 4)), std::memory_order_release);
}

// ==================== HELPERS ====================

int TouchTriggerSource::generateNoteId() {
    // Generate unique note ID (wraps at INT_MAX)
    int id = mNextNoteId.fetch_add(1, std::memory_order_relaxed);
    // Combine with source ID for global uniqueness
    return SOURCE_ID + id;
}

float TouchTriggerSource::calculatePanFromX(float x) const {
    // X position (0-1) maps to pan (0=left, 0.5=center, 1=right)
    return std::max(0.0f, std::min(1.0f, x));
}

void TouchTriggerSource::processNewTouch(int index, const TouchData& touch, uint64_t timestamp) {
    int noteId = generateNoteId();
    float pan = calculatePanFromX(touch.x);

    // Update internal state
    mTouchStates[index].active.store(true, std::memory_order_release);
    mTouchStates[index].noteId.store(noteId, std::memory_order_release);
    mTouchStates[index].frequency.store(touch.frequency, std::memory_order_release);
    mTouchStates[index].amplitude.store(touch.amplitude, std::memory_order_release);
    mTouchStates[index].pressure.store(touch.pressure, std::memory_order_release);
    mTouchStates[index].pan.store(pan, std::memory_order_release);
    mTouchStates[index].pointerId.store(touch.pointerId, std::memory_order_release);

    // Create NOTE_ON event
    VoiceTriggerEvent event;
    event.type = TriggerEventType::NOTE_ON;
    event.noteId = noteId;
    event.frequency = touch.frequency;
    event.amplitude = touch.amplitude;
    event.pan = pan;
    event.pressure = touch.pressure;
    event.oscillatorType = mOscillatorType.load(std::memory_order_acquire);
    event.timestamp = timestamp;

    pushEvent(event);

#ifndef NDEBUG
    LOGD("Touch %d started: noteId=%d, freq=%.1f, amp=%.2f",
         index, noteId, touch.frequency, touch.amplitude);
#endif
}

void TouchTriggerSource::processEndedTouch(int index, uint64_t timestamp) {
    int noteId = mTouchStates[index].noteId.load(std::memory_order_acquire);

    // Update internal state
    mTouchStates[index].active.store(false, std::memory_order_release);

    // Create NOTE_OFF event
    VoiceTriggerEvent event;
    event.type = TriggerEventType::NOTE_OFF;
    event.noteId = noteId;
    event.timestamp = timestamp;

    pushEvent(event);

#ifndef NDEBUG
    LOGD("Touch %d ended: noteId=%d", index, noteId);
#endif

    // Clear state
    mTouchStates[index].noteId.store(-1, std::memory_order_release);
    mTouchStates[index].pointerId.store(-1, std::memory_order_release);
}

void TouchTriggerSource::processChangedTouch(int index, const TouchData& touch, uint64_t timestamp, bool forceUpdate) {
    // Check if parameters changed significantly
    float oldFreq = mTouchStates[index].frequency.load(std::memory_order_acquire);
    float oldAmp = mTouchStates[index].amplitude.load(std::memory_order_acquire);
    float oldPan = mTouchStates[index].pan.load(std::memory_order_acquire);

    float newPan = calculatePanFromX(touch.x);

    // Thresholds for change detection (reduced for smoother slow movements)
    constexpr float FREQ_THRESHOLD = 0.1f;  // 0.1 Hz — prevents stepping during slow drags
    constexpr float AMP_THRESHOLD = 0.005f; // 0.5%
    constexpr float PAN_THRESHOLD = 0.01f;  // 1%

    bool freqChanged = std::abs(touch.frequency - oldFreq) > FREQ_THRESHOLD;
    bool ampChanged = std::abs(touch.amplitude - oldAmp) > AMP_THRESHOLD;
    bool panChanged = std::abs(newPan - oldPan) > PAN_THRESHOLD;

    // FIX: When forceUpdate is true (a sibling touch ended), always send
    // PARAM_CHANGE to ensure the remaining voice stays properly tracked
    if (forceUpdate || freqChanged || ampChanged || panChanged) {
        int noteId = mTouchStates[index].noteId.load(std::memory_order_acquire);

        // Update internal state
        mTouchStates[index].frequency.store(touch.frequency, std::memory_order_release);
        mTouchStates[index].amplitude.store(touch.amplitude, std::memory_order_release);
        mTouchStates[index].pressure.store(touch.pressure, std::memory_order_release);
        mTouchStates[index].pan.store(newPan, std::memory_order_release);

        // Create PARAM_CHANGE event
        VoiceTriggerEvent event;
        event.type = TriggerEventType::PARAM_CHANGE;
        event.noteId = noteId;
        event.frequency = touch.frequency;
        event.amplitude = touch.amplitude;
        event.pan = newPan;
        event.pressure = touch.pressure;
        event.oscillatorType = mOscillatorType.load(std::memory_order_acquire);
        event.timestamp = timestamp;

        pushEvent(event);
    }
}

} // namespace voice
