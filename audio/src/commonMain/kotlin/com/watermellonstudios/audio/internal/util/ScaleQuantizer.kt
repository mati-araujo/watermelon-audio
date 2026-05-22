package com.watermellonstudios.audio.internal.util

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlin.math.abs
import kotlin.math.exp
import kotlin.math.ln
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.roundToInt

/**
 * Result of frequency quantization, indicating whether the note actually changed.
 * Callers can use [changed] to skip redundant JNI calls when the quantized
 * frequency is identical to the previous call.
 */
data class QuantizationResult(val frequency: Float, val changed: Boolean)

/**
 * Utility for quantizing frequencies to musical scales.
 *
 * This class is designed to work with any scale representation that provides
 * a list of semitone intervals. It does not depend on specific ScaleMode types.
 *
 * Includes hysteresis to prevent jitter at note boundaries: once quantized
 * to a note, the input must move beyond a threshold (0.7 semitones) before
 * switching to the next note.
 *
 * Supports root note transposition: intervals are shifted by [rootNoteId]
 * so that the scale starts on the chosen root note instead of C.
 *
 * Supports octave range: limits the X→frequency mapping to a specific
 * number of octaves centered around the root note.
 */
object ScaleQuantizer {

    // Default frequency range (used when octaveRange <= 0 or in free mode)
    private const val DEFAULT_MIN_FREQ = 20f
    private const val DEFAULT_MAX_FREQ = 2000f

    // Dynamic engine-based frequency range (Phase 10A)
    @Volatile private var engineMinFreq: Float = DEFAULT_MIN_FREQ
    @Volatile private var engineMaxFreq: Float = DEFAULT_MAX_FREQ

    // Reference: A4 = 440 Hz
    private const val A4_FREQ = 440f
    private const val A4_MIDI = 69

    // Hysteresis: distance in semitones before switching to a new note.
    // 0.7 means the input must move >70% of the way to the next scale note
    // before we actually switch. This prevents jitter at note boundaries.
    private const val HYSTERESIS_SEMITONES = 0.7f

    // Last quantized MIDI note (integer) for hysteresis tracking
    @Volatile private var lastQuantizedMidi: Int = -1

    // Backing StateFlow for currentMidiNote. StateFlow's equality-based emission
    // suppression means consumers only see real changes — no need for an explicit
    // distinctUntilChanged. See [currentMidiNoteFlow] for the public API.
    private val _currentMidiNoteFlow = MutableStateFlow(-1)

    /**
     * Push-based stream of the current quantized MIDI note (AUD-3).
     *
     * Emits whenever the quantized note changes. Frame-rate writers that pass
     * the same int (e.g. the XY drag staying inside a note's hysteresis zone,
     * or sub-cent jitter in free mode) do NOT cause emissions — `MutableStateFlow`
     * collapses identical values internally. Consumers can replace per-frame
     * `currentMidiNote` polling with a single collector.
     *
     * The value is `-1` until the first quantization, and is reset to `-1` when
     * the active engine returns to a free-frequency mode with no scale.
     */
    val currentMidiNoteFlow: StateFlow<Int> = _currentMidiNoteFlow.asStateFlow()

    /**
     * The current quantized MIDI note (or -1 if none). Read by NoteNameEffect
     * and other synchronous consumers. Backed by [currentMidiNoteFlow]; new
     * consumers should prefer the flow to avoid polling.
     */
    val currentMidiNote: Int
        get() = _currentMidiNoteFlow.value

    /** True if current note is a quarter-tone (between semitones). */
    @Volatile var isQuarterTone: Boolean = false
        private set

    // Current root note, octave range, and scale mode ID (set from AudioEngineStateManager)
    @Volatile private var currentRootNoteId: Int = 9  // A (default)
    @Volatile private var currentOctaveRange: Int = 3 // 3 octaves (default)
    @Volatile private var currentScaleModeId: Int = 0 // FREE (default)
    @Volatile var equalNoteSpacing: Boolean = false

    /** Scale mode ID for quarter-tone (24-TET). Must match ScaleMode.QUARTER_TONE.id */
    private const val QUARTER_TONE_ID = 12

    /**
     * Set the current scale mode ID. Used to detect special modes like quarter-tone.
     * @param scaleModeId ScaleMode.id value
     */
    fun setScaleModeId(scaleModeId: Int) {
        currentScaleModeId = scaleModeId
    }

    /**
     * Set the root note for scale transposition.
     * @param rootNoteId 0=C, 1=C#, 2=D, ..., 9=A, 10=A#, 11=B
     */
    fun setRootNote(rootNoteId: Int) {
        currentRootNoteId = rootNoteId.coerceIn(0, 11)
    }

    /**
     * Set the octave range for frequency mapping.
     * @param octaveRange Number of octaves (1-5)
     */
    fun setOctaveRange(octaveRange: Int) {
        currentOctaveRange = octaveRange.coerceIn(1, 5)
    }

    /**
     * Set the engine-based frequency range (Phase 10A).
     * This defines the base range; octave range and root note further refine it.
     */
    fun setEngineFrequencyRange(minHz: Float, maxHz: Float) {
        engineMinFreq = minHz.coerceIn(8f, 20000f)
        engineMaxFreq = maxHz.coerceIn(8f, 20000f)
    }

    /**
     * Calculate the min/max frequency range.
     *
     * Uses the engine-based range as outer bounds, then applies
     * root note + octave range centering within those bounds.
     */
    private fun getFrequencyRange(): Pair<Float, Float> {
        // Root note frequency in octave 4
        val rootFreq4 = A4_FREQ * 2f.pow((currentRootNoteId - 9) / 12f)

        val halfRange = currentOctaveRange / 2
        val minFreq = (rootFreq4 * 2f.pow(-halfRange.toFloat()))
            .coerceAtLeast(engineMinFreq)
        val maxFreq = (rootFreq4 * 2f.pow((currentOctaveRange - halfRange).toFloat()))
            .coerceAtMost(engineMaxFreq)

        return minFreq to maxFreq
    }

    /**
     * Generates all MIDI notes in the active scale within the frequency range.
     * Used for equal-spacing mode and by MusicalGuidesEffect.
     */
    fun generateScaleNotes(intervals: List<Int>): List<Int> {
        if (intervals.isEmpty()) return emptyList()
        val (minFreq, maxFreq) = getFrequencyRange()
        val minMidi = frequencyToMidi(minFreq).toInt() - 1
        val maxMidi = frequencyToMidi(maxFreq).toInt() + 1
        val shiftedIntervals = intervals.map { (it + currentRootNoteId) % 12 }.toSet()

        val notes = mutableListOf<Int>()
        for (midi in minMidi..maxMidi) {
            val noteClass = ((midi % 12) + 12) % 12
            if (noteClass in shiftedIntervals) {
                notes.add(midi)
            }
        }
        return notes
    }

    /**
     * Equal-spacing mode: maps X (0-1) directly to a scale note index.
     * Each note occupies the same fraction of the X axis.
     */
    private fun equalSpacingMap(x: Float, intervals: List<Int>): QuantizationResult {
        val scaleNotes = generateScaleNotes(intervals)
        if (scaleNotes.isEmpty()) {
            return QuantizationResult(440f, changed = false)
        }
        val index = (x.coerceIn(0f, 1f) * (scaleNotes.size - 1)).roundToInt()
            .coerceIn(0, scaleNotes.size - 1)
        val midiNote = scaleNotes[index]

        if (midiNote == lastQuantizedMidi) {
            return QuantizationResult(midiToFrequency(midiNote.toFloat()), changed = false)
        }

        val prevMidi = lastQuantizedMidi
        lastQuantizedMidi = midiNote
        _currentMidiNoteFlow.value = midiNote
        isQuarterTone = false
        android.util.Log.d("XY_TRACE", "[SQ-EQ] NOTE %d→%d idx=%d/%d".format(prevMidi, midiNote, index, scaleNotes.size))
        return QuantizationResult(midiToFrequency(midiNote.toFloat()), changed = true)
    }

    /**
     * Stateless equal-spacing: returns frequency for a given X without side effects.
     */
    fun equalSpacingMapStateless(x: Float, intervals: List<Int>): Float {
        val scaleNotes = generateScaleNotes(intervals)
        if (scaleNotes.isEmpty()) return 440f
        val index = (x.coerceIn(0f, 1f) * (scaleNotes.size - 1)).roundToInt()
            .coerceIn(0, scaleNotes.size - 1)
        return midiToFrequency(scaleNotes[index].toFloat())
    }

    /**
     * Maps X position (0-1) to frequency, optionally quantized to a scale.
     *
     * @param x Normalized X position (0.0 - 1.0)
     * @param intervals List of semitone intervals (empty for no quantization)
     * @return Frequency in Hz
     */
    fun quantizeFrequency(x: Float, intervals: List<Int>): Float {
        return quantizeFrequencyWithResult(x, intervals).frequency
    }

    /**
     * Stateless X→frequency mapping using the same rootNote-centered range.
     * Does NOT update hysteresis or currentMidiNote — safe for secondary touches.
     */
    fun quantizeFrequencyStateless(x: Float, intervals: List<Int>): Float {
        if (equalNoteSpacing && intervals.isNotEmpty() && currentScaleModeId != QUARTER_TONE_ID) {
            return equalSpacingMapStateless(x, intervals)
        }
        val (minFreq, maxFreq) = getFrequencyRange()
        val logMin = ln(minFreq)
        val logMax = ln(maxFreq)
        val rawFreq = exp(logMin + x.coerceIn(0f, 1f) * (logMax - logMin))
        if (intervals.isEmpty()) return rawFreq
        val midiNote = frequencyToMidi(rawFreq)
        val quantized = quantizeMidiToScale(midiNote, intervals)
        return midiToFrequency(quantized)
    }

    /**
     * Maps X position (0-1) to frequency with change detection.
     * Returns [QuantizationResult] so callers can skip JNI calls when
     * the quantized note hasn't changed (e.g. within hysteresis zone).
     *
     * In free mode (no intervals), always reports changed=true since
     * continuous frequency values are expected to differ each frame.
     */
    fun quantizeFrequencyWithResult(x: Float, intervals: List<Int>): QuantizationResult {
        // Equal note spacing: X maps linearly to scale degree index
        if (equalNoteSpacing && intervals.isNotEmpty() && currentScaleModeId != QUARTER_TONE_ID) {
            return equalSpacingMap(x, intervals)
        }

        val (minFreq, maxFreq) = getFrequencyRange()

        // Map X to frequency logarithmically within the octave range
        val logMin = ln(minFreq)
        val logMax = ln(maxFreq)
        val rawFreq = exp(logMin + x.coerceIn(0f, 1f) * (logMax - logMin))

        // Quarter-tone: 24-TET, quantize to nearest 50 cents (0.5 semitone)
        if (currentScaleModeId == QUARTER_TONE_ID) {
            return quantizeToQuarterToneWithResult(rawFreq)
        }

        // If no intervals, return raw frequency (free mode) — always "changed"
        if (intervals.isEmpty()) {
            lastQuantizedMidi = -1
            _currentMidiNoteFlow.value = (69f + 12f * (ln(rawFreq / 440f) / ln(2f))).roundToInt()
            isQuarterTone = false
            return QuantizationResult(rawFreq, changed = true)
        }

        // Quantize to scale with hysteresis, tracking whether note changed
        return quantizeToScaleWithResult(rawFreq, intervals)
    }

    /**
     * Convenience overload for scale modes with intervals property.
     * Works with any type that has an 'intervals' property.
     */
    inline fun <reified T> quantizeFrequency(x: Float, scaleMode: T): Float where T : Any {
        return quantizeFrequencyWithResult(x, scaleMode).frequency
    }

    /**
     * Convenience overload returning [QuantizationResult] for change detection.
     */
    inline fun <reified T> quantizeFrequencyWithResult(x: Float, scaleMode: T): QuantizationResult where T : Any {
        val intervals = try {
            scaleMode::class.java.getMethod("getIntervals").invoke(scaleMode) as? List<*>
        } catch (e: Exception) {
            null
        }
        @Suppress("UNCHECKED_CAST")
        return quantizeFrequencyWithResult(x, (intervals as? List<Int>) ?: emptyList())
    }

    /**
     * Resets hysteresis state. Call when switching scales or starting a new touch.
     */
    fun resetHysteresis() {
        lastQuantizedMidi = -1
    }

    /**
     * Quantizes to nearest quarter-tone (50 cents = 0.5 semitone).
     * Uses doubled MIDI values for integer hysteresis tracking:
     * quarterMidi = round(midiNote * 2) → each unit = 50 cents.
     */
    private fun quantizeToQuarterToneWithResult(frequency: Float): QuantizationResult {
        val midiNote = frequencyToMidi(frequency)
        // Double the MIDI space: each unit = 50 cents
        val quarterMidi = (midiNote * 2f).roundToInt()

        // Same quarter-tone step — no change
        if (quarterMidi == lastQuantizedMidi) {
            return QuantizationResult(midiToFrequency(lastQuantizedMidi / 2f), changed = false)
        }

        lastQuantizedMidi = quarterMidi
        _currentMidiNoteFlow.value = quarterMidi / 2  // floor: C+4 (qMidi=121) → midi 60 (C4)
        isQuarterTone = (quarterMidi % 2) != 0  // odd = between semitones
        val quantizedFreq = midiToFrequency(quarterMidi / 2f)
        return QuantizationResult(quantizedFreq, changed = true)
    }

    /**
     * Quantizes a frequency to the nearest note in a musical scale,
     * with hysteresis to prevent jitter at note boundaries.
     * Returns changed=false when hysteresis kept the previous note.
     */
    private fun quantizeToScaleWithResult(frequency: Float, intervals: List<Int>): QuantizationResult {
        if (intervals.isEmpty()) {
            return QuantizationResult(frequency, changed = true)
        }

        // Convert frequency to MIDI note number
        val midiNote = frequencyToMidi(frequency)

        // If we have a previous quantized note, check hysteresis
        if (lastQuantizedMidi >= 0) {
            val distanceFromLast = abs(midiNote - lastQuantizedMidi.toFloat())
            if (distanceFromLast < HYSTERESIS_SEMITONES) {
                // Still within hysteresis zone — keep the previous note
                return QuantizationResult(midiToFrequency(lastQuantizedMidi.toFloat()), changed = false)
            }
        }

        // Find the closest note in the scale (with root note transposition)
        val quantizedMidi = quantizeMidiToScale(midiNote, intervals)
        val newMidi = quantizedMidi.roundToInt()

        // If quantization lands on the same note, it's not a real change
        if (newMidi == lastQuantizedMidi) {
            return QuantizationResult(midiToFrequency(quantizedMidi), changed = false)
        }

        val prevMidi = lastQuantizedMidi
        lastQuantizedMidi = newMidi
        _currentMidiNoteFlow.value = newMidi
        isQuarterTone = false
        val (sqMin, sqMax) = getFrequencyRange()
        android.util.Log.d("XY_TRACE", "[SQ] NOTE %d→%d freq=%.1f rawMidi=%.1f range=[%.1f,%.1f] root=%d oct=%d".format(prevMidi, newMidi, midiToFrequency(quantizedMidi), midiNote, sqMin, sqMax, currentRootNoteId, currentOctaveRange))

        // Convert back to frequency
        return QuantizationResult(midiToFrequency(quantizedMidi), changed = true)
    }

    /**
     * Converts frequency to MIDI note number (floating point for precision).
     */
    private fun frequencyToMidi(frequency: Float): Float {
        return A4_MIDI + 12f * (ln(frequency / A4_FREQ) / ln(2f))
    }

    /**
     * Converts MIDI note number to frequency.
     */
    private fun midiToFrequency(midi: Float): Float {
        return A4_FREQ * 2f.pow((midi - A4_MIDI) / 12f)
    }

    /**
     * Quantizes a MIDI note to the nearest note in a scale,
     * transposed by the current root note.
     *
     * The intervals are defined relative to the root (e.g., Major = [0,2,4,5,7,9,11]).
     * We shift them by [currentRootNoteId] to get absolute chromatic positions,
     * then find the closest one to the input note.
     */
    private fun quantizeMidiToScale(midiNote: Float, intervals: List<Int>): Float {
        val noteInOctave = ((midiNote.roundToInt() % 12) + 12) % 12 // ensure positive
        val octave = midiNote.roundToInt() / 12

        // Shift intervals by root note to get absolute chromatic positions
        val shiftedIntervals = intervals.map { (it + currentRootNoteId) % 12 }

        // Find the closest shifted interval
        var closestInterval = shiftedIntervals[0]
        var minDistance = 12

        for (interval in shiftedIntervals) {
            val distance = abs(noteInOctave - interval)
            val wrappedDistance = min(distance, 12 - distance)

            if (wrappedDistance < minDistance) {
                minDistance = wrappedDistance
                closestInterval = interval
            }
        }

        // Handle octave wrapping
        val quantizedNote = if (closestInterval > noteInOctave + 6) {
            (octave - 1) * 12 + closestInterval
        } else if (closestInterval < noteInOctave - 6) {
            (octave + 1) * 12 + closestInterval
        } else {
            octave * 12 + closestInterval
        }

        return quantizedNote.toFloat()
    }

    /**
     * Gets the note name for a frequency.
     */
    fun getNoteName(frequency: Float): String {
        val midi = frequencyToMidi(frequency).roundToInt()
        val noteNames = arrayOf("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
        val noteName = noteNames[((midi % 12) + 12) % 12]
        val octave = (midi / 12) - 1
        return "$noteName$octave"
    }
}
