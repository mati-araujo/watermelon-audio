// First host coverage for voice/: TouchTriggerSource maps multitouch state to
// NOTE_ON / NOTE_OFF VoiceTriggerEvents through a lock-free queue. Events are
// produced synchronously in updateTouches() (processTick is a no-op for touch).

#include "../TouchTriggerSource.h"
#include "../VoiceTypes.h"

#include <gtest/gtest.h>

#include <vector>

using namespace voice;

namespace {
TouchData makeTouch(int pointerId, float x, float freq, float amp) {
    TouchData t;
    t.active = true;
    t.pointerId = pointerId;
    t.x = x;
    t.y = 0.5f;
    t.frequency = freq;
    t.amplitude = amp;
    t.pressure = 1.0f;
    return t;
}

// Drain the queue, counting events by type and returning them in order.
std::vector<VoiceTriggerEvent> drain(TouchTriggerSource& s) {
    std::vector<VoiceTriggerEvent> out;
    while (s.hasEvents()) out.push_back(s.popEvent());
    return out;
}
}  // namespace

TEST(TouchTriggerSourceTest, IdentityConstants) {
    TouchTriggerSource s;
    EXPECT_EQ(s.getType(), TriggerSourceType::TOUCH);
    EXPECT_EQ(s.getSourceId(), TouchTriggerSource::SOURCE_ID);
    EXPECT_EQ(s.getPriority(), TouchTriggerSource::PRIORITY);
}

TEST(TouchTriggerSourceTest, NoTouchesNoEvents) {
    TouchTriggerSource s;
    s.processTick(0, 128);
    EXPECT_FALSE(s.hasEvents());
    EXPECT_EQ(s.getActiveTouchCount(), 0);
}

TEST(TouchTriggerSourceTest, NewTouchEmitsNoteOnCarryingTouchData) {
    TouchTriggerSource s;
    const TouchData t = makeTouch(/*ptr=*/7, /*x=*/0.25f, /*freq=*/330.0f, /*amp=*/0.7f);
    s.updateTouches(&t, 1);

    auto events = drain(s);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, TriggerEventType::NOTE_ON);
    EXPECT_FLOAT_EQ(events[0].frequency, 330.0f);
    EXPECT_FLOAT_EQ(events[0].amplitude, 0.7f);
    EXPECT_FLOAT_EQ(events[0].pan, 0.25f);         // pan == clamped X
    EXPECT_GE(events[0].noteId, TouchTriggerSource::SOURCE_ID);
    EXPECT_EQ(s.getActiveTouchCount(), 1);
}

TEST(TouchTriggerSourceTest, SilentTouchProducesNoNoteOn) {
    TouchTriggerSource s;
    const TouchData t = makeTouch(1, 0.5f, 440.0f, /*amp=*/0.0005f);  // <= 0.001
    s.updateTouches(&t, 1);
    EXPECT_FALSE(s.hasEvents());
    EXPECT_EQ(s.getActiveTouchCount(), 0);
}

TEST(TouchTriggerSourceTest, EndedTouchEmitsMatchingNoteOff) {
    TouchTriggerSource s;
    const TouchData t = makeTouch(3, 0.5f, 440.0f, 0.8f);
    s.updateTouches(&t, 1);
    auto onEvents = drain(s);
    ASSERT_EQ(onEvents.size(), 1u);
    const int noteId = onEvents[0].noteId;

    s.updateTouches(nullptr, 0);  // all touches lifted
    auto offEvents = drain(s);
    ASSERT_EQ(offEvents.size(), 1u);
    EXPECT_EQ(offEvents[0].type, TriggerEventType::NOTE_OFF);
    EXPECT_EQ(offEvents[0].noteId, noteId);  // same note instance
    EXPECT_EQ(s.getActiveTouchCount(), 0);
}

TEST(TouchTriggerSourceTest, PanClampsExtremeX) {
    auto panForX = [](float x) {
        TouchTriggerSource s;
        const TouchData t = makeTouch(1, x, 440.0f, 0.8f);
        s.updateTouches(&t, 1);
        return s.popEvent().pan;
    };
    EXPECT_FLOAT_EQ(panForX(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(panForX(1.0f), 1.0f);
    EXPECT_FLOAT_EQ(panForX(1.5f), 1.0f);   // clamped
    EXPECT_FLOAT_EQ(panForX(-0.3f), 0.0f);  // clamped
}

TEST(TouchTriggerSourceTest, ConcurrentTouchesGetDistinctNoteIds) {
    TouchTriggerSource s;
    const TouchData both[2] = {
        makeTouch(1, 0.2f, 220.0f, 0.6f),
        makeTouch(2, 0.8f, 440.0f, 0.6f),
    };
    s.updateTouches(both, 2);

    auto events = drain(s);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_NE(events[0].noteId, events[1].noteId);
    EXPECT_EQ(s.getActiveTouchCount(), 2);
}

TEST(TouchTriggerSourceTest, CountIsCappedAtMaxTouchPoints) {
    TouchTriggerSource s;
    std::vector<TouchData> many;
    for (int i = 0; i < 8; ++i) many.push_back(makeTouch(i + 1, 0.5f, 440.0f, 0.6f));
    s.updateTouches(many.data(), static_cast<int>(many.size()));

    auto events = drain(s);
    EXPECT_EQ(events.size(), static_cast<size_t>(TouchTriggerSource::MAX_TOUCH_POINTS));
    EXPECT_EQ(s.getActiveTouchCount(), TouchTriggerSource::MAX_TOUCH_POINTS);
}
