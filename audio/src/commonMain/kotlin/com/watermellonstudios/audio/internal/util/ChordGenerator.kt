package com.watermellonstudios.audio.internal.util

import kotlin.math.pow
import kotlin.math.roundToInt

/**
 * Generates chord frequencies from a root frequency and chord intervals.
 *
 * When scale-aware mode is active, chord intervals are snapped to the
 * nearest note in the active scale, producing diatonic chords
 * (e.g., minor triad on a minor root, major on a major root).
 *
 * All operations are pure functions — no allocations beyond the result list.
 */
object ChordGenerator {

    /**
     * Generate chord frequencies from a root frequency.
     *
     * @param rootFreq Root frequency in Hz
     * @param chordIntervals Semitone intervals from root (e.g., [4, 7] for major triad)
     * @param scaleIntervals Scale intervals (semitones from scale root, e.g., [0,2,4,5,7,9,11] for major).
     *                       Empty = chromatic (no snapping).
     * @param rootNoteId Root note of the scale (0=C, 9=A, etc.)
     * @return List of harmony frequencies (does NOT include the root itself)
     */
    fun generateChordFrequencies(
        rootFreq: Float,
        chordIntervals: List<Int>,
        scaleIntervals: List<Int> = emptyList(),
        rootNoteId: Int = 9
    ): FloatArray {
        if (chordIntervals.isEmpty()) return FloatArray(0)

        val result = FloatArray(chordIntervals.size)

        for (i in chordIntervals.indices) {
            val interval = chordIntervals[i]
            val snappedInterval = if (scaleIntervals.isNotEmpty()) {
                snapToScale(interval, scaleIntervals, rootNoteId, rootFreq)
            } else {
                interval
            }
            result[i] = rootFreq * 2f.pow(snappedInterval / 12f)
        }

        return result
    }

    /**
     * Snap a chromatic interval to the nearest note in the active scale.
     *
     * Converts the interval to an absolute chromatic note, finds the closest
     * scale degree, and returns the adjusted interval.
     */
    private fun snapToScale(
        interval: Int,
        scaleIntervals: List<Int>,
        rootNoteId: Int,
        rootFreq: Float
    ): Int {
        // Convert root frequency to MIDI-ish note to find its chromatic position
        val rootMidi = frequencyToMidiNote(rootFreq)
        val targetMidi = rootMidi + interval
        val targetChromatic = ((targetMidi % 12) + 12) % 12

        // Build absolute chromatic positions of scale notes
        val scaleNotes = scaleIntervals.map { (it + rootNoteId) % 12 }

        // Find closest scale note to the target
        var closestDist = 12
        var closestNote = targetChromatic
        for (scaleNote in scaleNotes) {
            val dist = minOf(
                ((targetChromatic - scaleNote) + 12) % 12,
                ((scaleNote - targetChromatic) + 12) % 12
            )
            if (dist < closestDist) {
                closestDist = dist
                closestNote = scaleNote
            }
        }

        // Calculate the snapped interval: difference from root's chromatic position
        val rootChromatic = ((rootMidi % 12) + 12) % 12
        var snappedSemitones = ((closestNote - rootChromatic) + 12) % 12

        // Preserve octave of original interval
        val octaves = interval / 12
        snappedSemitones += octaves * 12

        // If original interval was >= 12 and snapped to 0, keep the octave
        if (interval > 0 && snappedSemitones == 0) {
            snappedSemitones = 12
        }

        return snappedSemitones
    }

    private fun frequencyToMidiNote(freq: Float): Int {
        return (69 + 12 * kotlin.math.log2(freq / 440f)).roundToInt()
    }
}
