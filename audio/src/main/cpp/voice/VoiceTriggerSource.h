#pragma once

#include "VoiceTypes.h"
#include "../dsp/LockFreeEventQueue.h"
#include <cstdint>

namespace voice {

/**
 * @class VoiceTriggerSource
 * @brief Abstract interface for voice trigger sources
 *
 * A trigger source generates NOTE_ON, NOTE_OFF, and PARAM_CHANGE events
 * that are processed by the VoiceManager to allocate and control voices.
 *
 * Built-in implementations:
 * - TouchTriggerSource: Multitouch input (up to 4 points)
 *
 * Future implementations:
 * - ArpeggiatorTriggerSource: Arpeggiator patterns
 * - ChordEngineTriggerSource: Auto-chord generation
 * - MidiTriggerSource: MIDI input
 * - SequencerTriggerSource: Step sequencer
 *
 * Thread Safety:
 * - updateTouches() or similar input methods: called from UI thread
 * - processTick(): called from audio thread
 * - Event queue is lock-free SPSC (UI pushes, audio pops)
 */
class VoiceTriggerSource {
public:
    virtual ~VoiceTriggerSource() = default;

    // ==================== IDENTITY ====================

    /**
     * @brief Get the type of this trigger source
     * @return TriggerSourceType enum value
     */
    virtual TriggerSourceType getType() const = 0;

    /**
     * @brief Get the unique source ID
     * @return Source ID (used for voice tracking)
     *
     * Convention:
     * - Touch: 1000
     * - Arpeggiator: 2000
     * - ChordEngine: 3000
     * - MIDI: 4000
     * - Sequencer: 5000
     */
    virtual int getSourceId() const = 0;

    /**
     * @brief Get the priority of this source
     * @return Priority (higher = more important for voice stealing)
     *
     * Convention:
     * - Touch: 100 (highest)
     * - ChordEngine: 75
     * - Arpeggiator: 50
     * - MIDI: 50
     * - Sequencer: 25
     */
    virtual int getPriority() const = 0;

    // ==================== EVENT PROCESSING ====================

    /**
     * @brief Process a tick of sample time
     * @param sampleTime Current sample time
     * @param numFrames Number of frames in this tick
     *
     * Called from audio thread every audio callback.
     * Source should generate events based on timing (e.g., arpeggiator).
     */
    virtual void processTick(uint64_t sampleTime, int numFrames) = 0;

    /**
     * @brief Check if there are pending events
     * @return true if events are available
     *
     * Lock-free: safe to call from audio thread.
     */
    virtual bool hasEvents() const = 0;

    /**
     * @brief Pop the next event from the queue
     * @return Next event (undefined if hasEvents() is false)
     *
     * Lock-free: safe to call from audio thread.
     */
    virtual VoiceTriggerEvent popEvent() = 0;

    /**
     * @brief Clear all pending events
     *
     * Lock-free: safe to call from any thread.
     */
    virtual void clearEvents() = 0;

protected:
    // Maximum events in flight — 4 touches × 3 event types (ON/OFF/CHANGE) = 12,
    // plus headroom for rapid touch sequences
    static constexpr size_t MAX_PENDING_EVENTS = 32;

    /**
     * @brief Helper to push an event to the queue (lock-free, UI thread)
     * @param event Event to push
     */
    void pushEvent(const VoiceTriggerEvent& event) {
        mEventQueue.push(event);  // drops if full — acceptable for touch events
    }

    /**
     * @brief Helper to check event count (lock-free)
     * @return Number of pending events
     */
    size_t getEventCount() const {
        return mEventQueue.size();
    }

    /**
     * @brief Helper to pop event (lock-free, audio thread)
     * @return Event or default event if queue empty
     */
    VoiceTriggerEvent popEventInternal() {
        VoiceTriggerEvent event{};
        mEventQueue.pop(event);
        return event;
    }

    /**
     * @brief Helper to clear events (lock-free)
     */
    void clearEventsInternal() {
        mEventQueue.clear();
    }

    LockFreeEventQueue<VoiceTriggerEvent, MAX_PENDING_EVENTS> mEventQueue;
};

} // namespace voice
