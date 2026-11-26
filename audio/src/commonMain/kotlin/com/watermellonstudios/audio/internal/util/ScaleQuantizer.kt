package com.watermellonstudios.audio.internal.util

import com.watermellonstudios.audio.api.PitchQuantizer
import com.watermellonstudios.audio.callback.AudioLogger
import com.watermellonstudios.audio.domain.scale.ScaleMode
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Moved to `audio.api` with [PitchQuantizer] (REQ-055 S15); the alias keeps the consumers that
 * import it from here compiling.
 */
typealias QuantizationResult = com.watermellonstudios.audio.api.QuantizationResult

/**
 * The live pad's process-wide quantizer: a thin delegate over one [PitchQuantizer], keeping the
 * whole public API (and the [currentMidiNoteFlow] push stream) it always had. The algorithm, the
 * hysteresis and the numbers live in [PitchQuantizer]; an offline job builds its own instance and
 * never touches this one.
 */
object ScaleQuantizer {

    private val default = PitchQuantizer()

    /** Trace sink for note-change events; see [PitchQuantizer.logger]. */
    var logger: AudioLogger
        get() = default.logger
        set(value) {
            default.logger = value
        }

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
    val isQuarterTone: Boolean
        get() = default.isQuarterTone

    var equalNoteSpacing: Boolean
        get() = default.equalNoteSpacing
        set(value) {
            default.equalNoteSpacing = value
        }

    /** Set the current scale mode ID. Used to detect special modes like quarter-tone. */
    fun setScaleModeId(scaleModeId: Int) = default.setScaleModeId(scaleModeId)

    /** Set the root note for scale transposition (0=C … 9=A … 11=B). */
    fun setRootNote(rootNoteId: Int) = default.setRootNote(rootNoteId)

    /** Set the octave range for frequency mapping (1-5). */
    fun setOctaveRange(octaveRange: Int) = default.setOctaveRange(octaveRange)

    /** Set the engine-based frequency range (Phase 10A). */
    fun setEngineFrequencyRange(minHz: Float, maxHz: Float) = default.setEngineFrequencyRange(minHz, maxHz)

    /** All MIDI notes in the active scale within the frequency range. */
    fun generateScaleNotes(intervals: List<Int>): List<Int> = default.generateScaleNotes(intervals)

    /** Stateless equal-spacing: frequency for a given X without side effects. */
    fun equalSpacingMapStateless(x: Float, intervals: List<Int>): Float =
        default.equalSpacingMapStateless(x, intervals)

    /** Maps X position (0-1) to frequency, optionally quantized to a scale. */
    fun quantizeFrequency(x: Float, intervals: List<Int>): Float =
        quantizeFrequencyWithResult(x, intervals).frequency

    /**
     * Stateless X→frequency mapping using the same rootNote-centered range.
     * Does NOT update hysteresis or currentMidiNote — safe for secondary touches.
     */
    fun quantizeFrequencyStateless(x: Float, intervals: List<Int>): Float =
        default.quantizeFrequencyStateless(x, intervals)

    /**
     * Maps X position (0-1) to frequency with change detection, so callers can skip JNI calls when
     * the quantized note hasn't changed. In free mode it always reports changed=true.
     */
    fun quantizeFrequencyWithResult(x: Float, intervals: List<Int>): QuantizationResult {
        val result = default.quantizeFrequencyWithResult(x, intervals)
        _currentMidiNoteFlow.value = default.currentMidiNote
        return result
    }

    /** Convenience overload for [ScaleMode]. */
    fun quantizeFrequency(x: Float, scaleMode: ScaleMode): Float =
        quantizeFrequencyWithResult(x, scaleMode).frequency

    /** Convenience overload returning [QuantizationResult] for change detection. */
    fun quantizeFrequencyWithResult(x: Float, scaleMode: ScaleMode): QuantizationResult =
        quantizeFrequencyWithResult(x, scaleMode.intervals)

    /** Resets hysteresis state. Call when switching scales or starting a new touch. */
    fun resetHysteresis() = default.resetHysteresis()

    /** The note name for a frequency. */
    fun getNoteName(frequency: Float): String = default.getNoteName(frequency)
}
