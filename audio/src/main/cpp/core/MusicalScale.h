#ifndef MUSICAL_SCALE_H
#define MUSICAL_SCALE_H

#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <atomic>

/**
 * @file MusicalScale.h
 * @brief Musical scale quantization for XY controller
 *
 * PHASE 5: Pro Features - Musical Scales
 *
 * Converts continuous frequency values to quantized musical notes.
 * Supports multiple scale types and root note configuration.
 *
 * Features:
 * - 10 common musical scales
 * - Configurable root note (A-G#)
 * - Configurable octave range
 * - Smooth quantization with optional glide
 * - Direct X position to note mapping
 */
class MusicalScale {
public:
    /**
     * @brief Available musical scales
     */
    enum class Scale {
        CHROMATIC = 0,      ///< All 12 semitones (no quantization)
        MAJOR,              ///< Major scale (Ionian mode)
        MINOR,              ///< Natural minor (Aeolian mode)
        PENTATONIC_MAJOR,   ///< Major pentatonic (5 notes)
        PENTATONIC_MINOR,   ///< Minor pentatonic (5 notes)
        BLUES,              ///< Blues scale (6 notes)
        DORIAN,             ///< Dorian mode
        MIXOLYDIAN,         ///< Mixolydian mode
        HARMONIC_MINOR,     ///< Harmonic minor scale
        WHOLE_TONE,         ///< Whole tone scale (6 notes)
        NUM_SCALES
    };

    /**
     * @brief Note names for root selection
     */
    enum class RootNote {
        C = 0, Cs, D, Ds, E, F, Fs, G, Gs, A, As, B
    };

    /**
     * @brief Constructor
     */
    MusicalScale();

    /**
     * @brief Set the current scale
     * @param scale Scale type to use
     */
    void setScale(Scale scale);

    /**
     * @brief Set the root note
     * @param root Root note (C, C#, D, etc.)
     */
    void setRoot(RootNote root);

    /**
     * @brief Set root frequency directly (for custom tuning)
     * @param frequency Root frequency in Hz (default A4 = 440Hz)
     */
    void setRootFrequency(float frequency);

    /**
     * @brief Set octave range
     * @param numOctaves Number of octaves to span (1-5)
     */
    void setOctaveRange(int numOctaves);

    /**
     * @brief Quantize a frequency to the nearest note in scale
     * @param inputFreq Input frequency in Hz
     * @return Quantized frequency in Hz
     *
     * Finds the closest note in the current scale to the input frequency.
     */
    float quantize(float inputFreq) const;

    /**
     * @brief Convert X position (0-1) to frequency in scale
     * @param x Normalized X position (0.0 to 1.0)
     * @return Frequency in Hz corresponding to a note in the scale
     *
     * Maps X position linearly across all available notes in the scale
     * within the configured octave range.
     */
    float xToFrequency(float x) const;

    /**
     * @brief Get the number of notes in the current scale per octave
     */
    int getNotesPerOctave() const;

    /**
     * @brief Get the total number of notes available
     */
    int getTotalNotes() const;

    /**
     * @brief Get scale name as string
     * @param scale Scale type
     * @return Human-readable scale name
     */
    static const char* getScaleName(Scale scale);

    /**
     * @brief Get note name as string
     * @param root Root note
     * @return Note name (e.g., "C", "C#", "D")
     */
    static const char* getNoteName(RootNote root);

    /**
     * @brief Check if scale quantization is enabled
     */
    bool isEnabled() const { return mEnabled.load(std::memory_order_relaxed); }

    /**
     * @brief Enable or disable scale quantization
     * @param enabled true to enable, false to pass-through
     */
    void setEnabled(bool enabled) { mEnabled.store(enabled, std::memory_order_relaxed); }

private:
    // Current configuration
    std::atomic<int> mCurrentScale{static_cast<int>(Scale::CHROMATIC)};
    std::atomic<int> mRootNote{static_cast<int>(RootNote::A)};
    std::atomic<float> mRootFrequency{440.0f};  // A4 = 440Hz
    std::atomic<int> mOctaveRange{3};
    std::atomic<bool> mEnabled{false};

    // Cached scale data
    std::vector<int> mScaleIntervals;  // Semitone intervals in scale
    std::vector<float> mScaleNotes;    // Precomputed frequencies

    // Scale interval definitions (semitones from root)
    static constexpr std::array<std::array<int, 12>, static_cast<int>(Scale::NUM_SCALES)> SCALE_INTERVALS = {{
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},  // CHROMATIC
        {0, 2, 4, 5, 7, 9, 11, -1, -1, -1, -1, -1},  // MAJOR (7 notes)
        {0, 2, 3, 5, 7, 8, 10, -1, -1, -1, -1, -1},  // MINOR (7 notes)
        {0, 2, 4, 7, 9, -1, -1, -1, -1, -1, -1, -1},  // PENTATONIC_MAJOR (5 notes)
        {0, 3, 5, 7, 10, -1, -1, -1, -1, -1, -1, -1}, // PENTATONIC_MINOR (5 notes)
        {0, 3, 5, 6, 7, 10, -1, -1, -1, -1, -1, -1},  // BLUES (6 notes)
        {0, 2, 3, 5, 7, 9, 10, -1, -1, -1, -1, -1},   // DORIAN (7 notes)
        {0, 2, 4, 5, 7, 9, 10, -1, -1, -1, -1, -1},   // MIXOLYDIAN (7 notes)
        {0, 2, 3, 5, 7, 8, 11, -1, -1, -1, -1, -1},   // HARMONIC_MINOR (7 notes)
        {0, 2, 4, 6, 8, 10, -1, -1, -1, -1, -1, -1}   // WHOLE_TONE (6 notes)
    }};

    // Scale names
    static constexpr const char* SCALE_NAMES[] = {
        "Chromatic",
        "Major",
        "Minor",
        "Pentatonic Major",
        "Pentatonic Minor",
        "Blues",
        "Dorian",
        "Mixolydian",
        "Harmonic Minor",
        "Whole Tone"
    };

    // Note names
    static constexpr const char* NOTE_NAMES[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };

    /**
     * @brief Rebuild the scale note frequencies
     *
     * Called when scale, root, or octave range changes.
     */
    void rebuildScale();

    /**
     * @brief Convert semitones relative to A4 to frequency
     */
    static float semitoneToFrequency(float semitone, float rootFreq = 440.0f) {
        return rootFreq * std::pow(2.0f, semitone / 12.0f);
    }

    /**
     * @brief Convert frequency to semitones relative to A4
     */
    static float frequencyToSemitone(float freq, float rootFreq = 440.0f) {
        return 12.0f * std::log2(freq / rootFreq);
    }
};

#endif // MUSICAL_SCALE_H
