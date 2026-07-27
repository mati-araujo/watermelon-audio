#include "MusicalScale.h"
#include <limits>

// Define static constexpr members
constexpr std::array<std::array<int, 12>, static_cast<int>(MusicalScale::Scale::NUM_SCALES)>
    MusicalScale::SCALE_INTERVALS;
constexpr const char* MusicalScale::SCALE_NAMES[];
constexpr const char* MusicalScale::NOTE_NAMES[];

MusicalScale::MusicalScale() {
    // Initialize with chromatic scale
    rebuildScale();
}

void MusicalScale::setScale(Scale scale) {
    int scaleInt = static_cast<int>(scale);
    if (scaleInt >= 0 && scaleInt < static_cast<int>(Scale::NUM_SCALES)) {
        mCurrentScale.store(scaleInt, std::memory_order_relaxed);
        rebuildScale();
    }
}

void MusicalScale::setRoot(RootNote root) {
    int rootInt = static_cast<int>(root);
    if (rootInt >= 0 && rootInt < 12) {
        mRootNote.store(rootInt, std::memory_order_relaxed);

        // Recalculate root frequency based on A4 = 440Hz
        // A is note 9 (0=C, 9=A)
        float semitonesFromA = static_cast<float>(rootInt - 9);
        float rootFreq = 440.0f * std::pow(2.0f, semitonesFromA / 12.0f);
        mRootFrequency.store(rootFreq, std::memory_order_relaxed);

        rebuildScale();
    }
}

void MusicalScale::setRootFrequency(float frequency) {
    frequency = std::clamp(frequency, 20.0f, 20000.0f);
    mRootFrequency.store(frequency, std::memory_order_relaxed);
    rebuildScale();
}

void MusicalScale::setOctaveRange(int numOctaves) {
    numOctaves = std::clamp(numOctaves, 1, 5);
    mOctaveRange.store(numOctaves, std::memory_order_relaxed);
    rebuildScale();
}

float MusicalScale::quantize(float inputFreq) const {
    if (!mEnabled.load(std::memory_order_relaxed) || mScaleNotes.empty()) {
        return inputFreq;
    }

    // Find closest note in scale
    float minDist = std::numeric_limits<float>::max();
    float closestFreq = inputFreq;

    // Use logarithmic distance (semitones) for better musical matching
    float inputSemitone = frequencyToSemitone(inputFreq, mRootFrequency.load(std::memory_order_relaxed));

    for (float noteFreq : mScaleNotes) {
        float noteSemitone = frequencyToSemitone(noteFreq, mRootFrequency.load(std::memory_order_relaxed));
        float dist = std::abs(inputSemitone - noteSemitone);

        if (dist < minDist) {
            minDist = dist;
            closestFreq = noteFreq;
        }
    }

    return closestFreq;
}

float MusicalScale::xToFrequency(float x) const {
    x = std::clamp(x, 0.0f, 1.0f);

    if (mScaleNotes.empty()) {
        // Fallback: linear frequency mapping
        return 110.0f + x * 1650.0f;  // A2 to ~A5
    }

    // Map X position to note index
    int totalNotes = static_cast<int>(mScaleNotes.size());
    int noteIndex = static_cast<int>(x * (totalNotes - 1) + 0.5f);
    noteIndex = std::clamp(noteIndex, 0, totalNotes - 1);

    return mScaleNotes[noteIndex];
}

int MusicalScale::getNotesPerOctave() const {
    return static_cast<int>(mScaleIntervals.size());
}

int MusicalScale::getTotalNotes() const {
    return static_cast<int>(mScaleNotes.size());
}

const char* MusicalScale::getScaleName(Scale scale) {
    int idx = static_cast<int>(scale);
    if (idx >= 0 && idx < static_cast<int>(Scale::NUM_SCALES)) {
        return SCALE_NAMES[idx];
    }
    return "Unknown";
}

const char* MusicalScale::getNoteName(RootNote root) {
    int idx = static_cast<int>(root);
    if (idx >= 0 && idx < 12) {
        return NOTE_NAMES[idx];
    }
    return "?";
}

void MusicalScale::rebuildScale() {
    // Get current scale intervals
    int scaleIdx = mCurrentScale.load(std::memory_order_relaxed);
    const auto& intervals = SCALE_INTERVALS[scaleIdx];

    // Extract valid intervals (stop at -1)
    mScaleIntervals.clear();
    for (int interval : intervals) {
        if (interval >= 0) {
            mScaleIntervals.push_back(interval);
        } else {
            break;
        }
    }

    // Get parameters. La tabla se construye desde la FRECUENCIA de la raiz;
    // `mRootNote` no entra en esta cuenta y se cargaba en una local que nadie
    // leia.
    float rootFreq = mRootFrequency.load(std::memory_order_relaxed);
    int octaveRange = mOctaveRange.load(std::memory_order_relaxed);

    // Build frequency table
    // Center octaves around the root (e.g., 3 octaves = -1 to +1 from root octave)
    mScaleNotes.clear();

    int halfRange = octaveRange / 2;
    int startOctave = -halfRange;
    int endOctave = octaveRange - halfRange - 1;

    for (int oct = startOctave; oct <= endOctave; ++oct) {
        for (int interval : mScaleIntervals) {
            // Calculate semitones from root
            float semitones = static_cast<float>(oct * 12 + interval);

            // Convert to frequency
            float freq = rootFreq * std::pow(2.0f, semitones / 12.0f);

            // Only include audible frequencies (20Hz - 20kHz)
            if (freq >= 20.0f && freq <= 20000.0f) {
                mScaleNotes.push_back(freq);
            }
        }
    }

    // Sort by frequency (should already be sorted, but ensure)
    std::sort(mScaleNotes.begin(), mScaleNotes.end());
}
