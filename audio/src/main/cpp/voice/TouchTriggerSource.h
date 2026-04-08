#pragma once

#include "VoiceTriggerSource.h"
#include "VoiceTypes.h"
#include <array>
#include <atomic>
#include <mutex>  // for mTouchMutex (UI-thread only, not on audio path)

namespace voice {

/**
 * @class TouchTriggerSource
 * @brief Trigger source for multitouch input (up to 4 simultaneous touches)
 *
 * Converts touch input from UI into NOTE_ON/NOTE_OFF/PARAM_CHANGE events.
 *
 * Thread Safety:
 * - updateTouches(): called from UI thread (via JNI), protected by mTouchMutex
 * - processTick(), hasEvents(), popEvent(): called from audio thread (lock-free)
 * - Event queue is lock-free SPSC (UI pushes, audio pops)
 */
class TouchTriggerSource : public VoiceTriggerSource {
public:
    static constexpr int MAX_TOUCH_POINTS = 4;
    static constexpr int SOURCE_ID = 1000;  // Base ID for touch source
    static constexpr int PRIORITY = 100;    // Highest priority

    TouchTriggerSource();
    ~TouchTriggerSource() override = default;

    // ==================== VoiceTriggerSource INTERFACE ====================

    TriggerSourceType getType() const override { return TriggerSourceType::TOUCH; }
    int getSourceId() const override { return SOURCE_ID; }
    int getPriority() const override { return PRIORITY; }

    void processTick(uint64_t sampleTime, int numFrames) override;
    bool hasEvents() const override;
    VoiceTriggerEvent popEvent() override;
    void clearEvents() override;

    // ==================== TOUCH INPUT ====================

    /**
     * @brief Update touch points from UI
     * @param touches Array of touch data
     * @param count Number of active touches (0-4)
     *
     * Thread-safe: Called from UI thread via JNI.
     * Generates NOTE_ON/NOTE_OFF events based on changes.
     */
    void updateTouches(const TouchData* touches, int count);

    // ==================== QUERIES ====================

    /**
     * @brief Get number of currently active touches
     * @return Active touch count (0-4)
     */
    int getActiveTouchCount() const;

    /**
     * @brief Get touch data for a specific index
     * @param index Touch index (0-3)
     * @return Touch data (may be inactive)
     */
    TouchData getTouchData(int index) const;

    // ==================== CONFIGURATION ====================

    /**
     * @brief Set default oscillator type for new notes
     * @param type Oscillator type (0-4)
     */
    void setOscillatorType(int type);

    /**
     * @brief Get current oscillator type
     * @return Oscillator type
     */
    int getOscillatorType() const {
        return mOscillatorType.load(std::memory_order_acquire);
    }

private:
    // ==================== INTERNAL STATE ====================

    struct TouchState {
        std::atomic<bool> active{false};
        std::atomic<int> noteId{-1};
        std::atomic<float> frequency{440.0f};
        std::atomic<float> amplitude{0.0f};
        std::atomic<float> pressure{1.0f};
        std::atomic<float> pan{0.5f};
        std::atomic<int> pointerId{-1};
    };

    std::array<TouchState, MAX_TOUCH_POINTS> mTouchStates;
    std::atomic<int> mOscillatorType{0};
    std::atomic<int> mNextNoteId{0};

    // Previous touch data for change detection
    std::array<TouchData, MAX_TOUCH_POINTS> mPrevTouchData;
    mutable std::mutex mTouchMutex;

    // ==================== HELPERS ====================

    int generateNoteId();
    float calculatePanFromX(float x) const;
    void processNewTouch(int index, const TouchData& touch, uint64_t timestamp);
    void processEndedTouch(int index, uint64_t timestamp);
    void processChangedTouch(int index, const TouchData& touch, uint64_t timestamp, bool forceUpdate = false);
};

} // namespace voice
