#pragma once

#include <cstdint>

namespace voice {

/**
 * @brief State machine states for a polyphonic voice
 */
enum class VoiceState : int {
    IDLE = 0,       // Available for allocation
    ATTACK = 1,     // Envelope rising (0 → 1)
    DECAY = 2,      // Envelope falling (1 → sustain level)
    SUSTAIN = 3,    // Holding at sustain level
    RELEASE = 4,    // Envelope falling (sustain → 0)
    STEALING = 5    // Being stolen - fast release
};

/**
 * @brief Parameters for triggering a voice
 */
struct VoiceParams {
    float frequency = 440.0f;       // Frequency in Hz
    float amplitude = 0.0f;         // Amplitude 0.0-1.0
    float pan = 0.5f;               // Pan position: 0=left, 0.5=center, 1=right
    float pressure = 1.0f;          // Touch pressure for expressiveness
    int oscillatorType = 0;         // 0=Sine, 1=Square, 2=Saw, 3=Triangle, 4=Noise
    int sourceId = -1;              // ID of trigger source (-1 = none)
    int noteId = -1;                // Unique note ID for voice stealing
};

/**
 * @brief Types of voice trigger sources
 */
enum class TriggerSourceType : int {
    TOUCH = 0,          // Multitouch input (up to 4 points)
    ARPEGGIATOR = 1,    // Arpeggiator (future)
    CHORD_ENGINE = 2,   // Chord automation (future)
    MIDI = 3,           // MIDI input (future)
    SEQUENCER = 4       // Step sequencer (future)
};

/**
 * @brief Types of trigger events
 */
enum class TriggerEventType : int {
    NOTE_ON = 0,        // Start playing a note
    NOTE_OFF = 1,       // Stop playing a note
    PARAM_CHANGE = 2    // Change parameters of active note
};

/**
 * @brief Event from a trigger source to the voice manager
 */
struct VoiceTriggerEvent {
    TriggerEventType type = TriggerEventType::NOTE_ON;
    int noteId = -1;                // Unique ID for this note instance
    float frequency = 440.0f;       // Frequency in Hz
    float amplitude = 0.0f;         // Target amplitude 0.0-1.0
    float pan = 0.5f;               // Pan position
    float pressure = 1.0f;          // Pressure/velocity
    int oscillatorType = 0;         // Oscillator type index
    uint64_t timestamp = 0;         // Sample time when event occurred
};

/**
 * @brief Voice stealing strategy
 */
enum class StealingStrategy : int {
    OLDEST = 0,         // Steal the voice that started earliest
    QUIETEST = 1,       // Steal the voice with lowest envelope level
    SAME_NOTE = 2,      // Prefer stealing same noteId (retrigger)
    LOWEST_PRIORITY = 3 // Steal based on source priority
};

/**
 * @brief Configuration for voice pool allocation
 */
struct VoiceAllocationConfig {
    int maxVoices = 8;              // Maximum simultaneous voices
    int reservedForTouch = 4;       // Voices reserved for touch input
    bool enableStealing = true;     // Allow voice stealing when pool exhausted
    StealingStrategy stealingStrategy = StealingStrategy::OLDEST;

    // ADSR Envelope timing
    float attackTimeMs = 5.0f;        // Time to reach peak (0→1)
    float decayTimeMs = 100.0f;       // Time to fall from peak to sustain (1→sustain)
    float sustainLevel = 0.8f;        // Sustain level (0.0-1.0)
    float releaseTimeMs = 50.0f;      // Time to fade to zero (sustain→0)
    float stealReleaseTimeMs = 10.0f; // Fast release when stealing
};

/**
 * @brief Touch point data from UI
 */
struct TouchData {
    float x = 0.0f;                 // Normalized X position (0-1)
    float y = 0.0f;                 // Normalized Y position (0-1)
    float frequency = 440.0f;       // Mapped frequency in Hz
    float amplitude = 0.0f;         // Mapped amplitude (0-1)
    float pressure = 1.0f;          // Touch pressure (0-1)
    int pointerId = -1;             // Android pointer ID
    bool active = false;            // Is this touch point active?
};

} // namespace voice
