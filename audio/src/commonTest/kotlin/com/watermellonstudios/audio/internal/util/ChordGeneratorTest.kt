package com.watermellonstudios.audio.internal.util

import com.watermellonstudios.audio.domain.chord.ChordType
import kotlin.math.abs
import kotlin.math.pow
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/**
 * Cobertura de unidad para [ChordGenerator] — los 8 chord types contra
 * varias escalas, asegurando:
 *  - Sin escala: intervalos cromáticos puros
 *  - Con escala: snap a grado más cercano dentro del cap (±2 por default)
 *  - Hz y MIDI devuelven el MISMO intervalo snapeado (consistencia entre paths)
 */
class ChordGeneratorTest {

    private val majorIntervals = listOf(0, 2, 4, 5, 7, 9, 11)        // Major (Ionian)
    private val minorIntervals = listOf(0, 2, 3, 5, 7, 8, 10)        // Natural minor
    private val pentaMajor   = listOf(0, 2, 4, 7, 9)                 // Penta mayor
    private val rootA = 9   // A
    private val rootC = 0   // C

    private val rootFreqA4 = 440f
    private val rootMidiA4 = 69

    private fun expectedFreqFromInterval(rootFreq: Float, semitones: Int): Float =
        rootFreq * 2f.pow(semitones / 12f)

    @Test
    fun `chromatico sin escala devuelve intervalos crudos en Hz`() {
        for (chord in ChordType.entries) {
            val freqs = ChordGenerator.generateChordFrequencies(
                rootFreq = rootFreqA4,
                chordIntervals = chord.intervals,
                scaleIntervals = emptyList()
            )
            assertEquals(chord.intervals.size, freqs.size, "size mismatch for $chord")
            chord.intervals.forEachIndexed { i, interval ->
                val expected = expectedFreqFromInterval(rootFreqA4, interval)
                assertTrue(
                    abs(freqs[i] - expected) < 0.01f,
                    "$chord interval=$interval expected=$expected actual=${freqs[i]}"
                )
            }
        }
    }

    @Test
    fun `chromatico sin escala devuelve intervalos crudos en MIDI`() {
        for (chord in ChordType.entries) {
            val midiNotes = ChordGenerator.generateChordMidiNotes(
                rootMidi = rootMidiA4,
                chordIntervals = chord.intervals,
                scaleIntervals = emptyList()
            )
            val expected = chord.intervals.map { rootMidiA4 + it }.toIntArray()
            assertEquals(expected.toList(), midiNotes.toList(), "chord=$chord")
        }
    }

    @Test
    fun `legacy TRIAD cromatico es acorde mayor`() {
        // Phase15 legacy contract: 15C can migrate this expectation when chord theory becomes mode-aware.
        val midiNotes = ChordGenerator.generateChordMidiNotes(
            rootMidi = 60,
            chordIntervals = ChordType.TRIAD.intervals,
            scaleIntervals = emptyList()
        )

        assertEquals(listOf(64, 67), midiNotes.toList())
    }

    @Test
    fun `triada mayor en A-major escala no se modifica`() {
        // A major: A C# E => raíz=69 (A), tercera=73 (C#), quinta=76 (E)
        // Intervalos de TRIAD [4, 7]: 4->73 (C#), 7->76 (E) — ambas están en la escala A-major
        val midi = ChordGenerator.generateChordMidiNotes(
            rootMidi = rootMidiA4,
            chordIntervals = ChordType.TRIAD.intervals,
            scaleIntervals = majorIntervals,
            rootNoteId = rootA
        )
        assertEquals(listOf(73, 76), midi.toList())
    }

    @Test
    fun `legacy triada mayor en A-minor snapea empate hacia arriba`() {
        // Phase15 legacy contract: A minor has C(72) and D(74) equally near C#(73).
        // ScaleSnapping checks upward before downward, so the third snaps to D.
        // 15C may replace this when chord theory becomes mode-aware.
        // A minor: A C E => raíz=69 (A), tercera menor=72 (C), quinta=76 (E)
        // TRIAD [4, 7] sobre A:
        //   4 -> 73 (C#) — no está en A-minor; vecinos: 74 (D) gana por empate legacy
        //   7 -> 76 (E)  — está en A-minor → 76
        val midi = ChordGenerator.generateChordMidiNotes(
            rootMidi = rootMidiA4,
            chordIntervals = ChordType.TRIAD.intervals,
            scaleIntervals = minorIntervals,
            rootNoteId = rootA
        )
        assertEquals(listOf(74, 76), midi.toList())
    }

    @Test
    fun `Hz y MIDI producen los mismos intervalos snapeados`() {
        // Garantía de consistencia entre los dos paths (oscillator vs soundfont)
        for (chord in ChordType.entries) {
            for (scale in listOf(majorIntervals, minorIntervals, pentaMajor)) {
                val freqs = ChordGenerator.generateChordFrequencies(
                    rootFreq = rootFreqA4,
                    chordIntervals = chord.intervals,
                    scaleIntervals = scale,
                    rootNoteId = rootA
                )
                val midi = ChordGenerator.generateChordMidiNotes(
                    rootMidi = rootMidiA4,
                    chordIntervals = chord.intervals,
                    scaleIntervals = scale,
                    rootNoteId = rootA
                )
                assertEquals(freqs.size, midi.size, "chord=$chord scale=$scale")
                freqs.forEachIndexed { i, hz ->
                    val expected = expectedFreqFromInterval(rootFreqA4, midi[i] - rootMidiA4)
                    assertTrue(
                        abs(hz - expected) < 0.5f,
                        "chord=$chord scale=$scale i=$i hz=$hz expected=$expected midi=${midi[i]}"
                    )
                }
            }
        }
    }

    @Test
    fun `lista vacia de intervalos devuelve arrays vacios`() {
        assertEquals(0, ChordGenerator.generateChordFrequencies(440f, emptyList()).size)
        assertEquals(0, ChordGenerator.generateChordMidiNotes(60, emptyList()).size)
    }

    @Test
    fun `power chord en C-major mantiene la quinta`() {
        // C major, POWER [7] sobre C(=60): 60+7=67 (G) está en C-major → no se modifica
        val midi = ChordGenerator.generateChordMidiNotes(
            rootMidi = 60,
            chordIntervals = ChordType.POWER.intervals,
            scaleIntervals = majorIntervals,
            rootNoteId = rootC
        )
        assertEquals(listOf(67), midi.toList())
    }

    @Test
    fun `octava se preserva al snapear`() {
        // OCTAVE [12] sobre A en A-minor: 69+12=81 (A una octava arriba), está en la escala
        val midi = ChordGenerator.generateChordMidiNotes(
            rootMidi = rootMidiA4,
            chordIntervals = ChordType.OCTAVE.intervals,
            scaleIntervals = minorIntervals,
            rootNoteId = rootA
        )
        assertEquals(listOf(81), midi.toList())
    }
}
