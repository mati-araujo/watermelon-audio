package com.watermellonstudios.audio.internal.util

import com.watermellonstudios.audio.domain.chord.ChordType
import kotlin.math.abs
import kotlin.math.pow
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
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

/**
 * REQ-004 — generar el acorde **sobre un buffer del llamador**.
 *
 * El consumidor que motiva el requerimiento llama a esto una vez por frame mientras el dedo
 * arrastra, y ahí el array de retorno no es un detalle: es la asignación. Estos tests fijan el
 * contrato de la variante que no lo pide.
 */
class ChordGeneratorIntoTest {

    private val majorScale = listOf(0, 2, 4, 5, 7, 9, 11)
    private val rootA = 9
    private val rootC = 0
    private val rootFreqA4 = 440f
    private val rootMidiA4 = 69

    /** Una tríada (2 intervalos de armonía) y una séptima (3): distinto largo, mismo buffer. */
    private val triada = listOf(4, 7)
    private val septima = listOf(4, 7, 11)

    /**
     * 🔑 Un acorde cuyos intervalos **no** están en la escala mayor, para que el snapping tenga
     * algo que mover. Lo destapó el control negativo: con `triada` y `septima` —4, 7 y 11, que
     * son grados de la mayor— el snapping es un no-op, así que romperlo no hacía fallar el test
     * de equivalencia. Un caso que no ejercita lo que dice ejercitar es teatro.
     */
    private val menorConNovena = listOf(3, 6, 10)

    // ── AC-004.1 — escribe en el buffer y dice cuántas ────────────────────────

    @Test
    fun `AC-004_1 escribe en el buffer del llamador y devuelve cuantas escribio`() {
        val out = FloatArray(6)

        val n = ChordGenerator.generateChordFrequenciesInto(
            out = out,
            rootFreq = rootFreqA4,
            chordIntervals = triada,
            scaleIntervals = majorScale,
            rootNoteId = rootA
        )

        assertEquals(2, n)
        assertTrue(out[0] > 0f && out[1] > 0f, "las dos primeras posiciones tienen que estar escritas")
    }

    // ── AC-004.2 — no puede divergir de la que devuelve array ─────────────────

    @Test
    fun `AC-004_2 da exactamente lo mismo que la variante que devuelve array`() {
        val casos = listOf(
            Triple(triada, emptyList<Int>(), 0),
            Triple(triada, majorScale, rootA),
            Triple(septima, majorScale, rootA),
            Triple(septima, emptyList(), 0),
            // Los dos que de verdad ejercitan el snapping: 3, 6 y 10 no son grados de la mayor.
            Triple(menorConNovena, majorScale, rootA),
            Triple(menorConNovena, majorScale, rootC)
        )
        for ((intervals, scale, root) in casos) {
            val esperado = ChordGenerator.generateChordFrequencies(
                rootFreq = rootFreqA4,
                chordIntervals = intervals,
                scaleIntervals = scale,
                rootNoteId = root
            )
            val out = FloatArray(esperado.size)
            val n = ChordGenerator.generateChordFrequenciesInto(
                out = out,
                rootFreq = rootFreqA4,
                chordIntervals = intervals,
                scaleIntervals = scale,
                rootNoteId = root
            )

            assertEquals(esperado.size, n, "count distinto para $intervals / escala=${scale.size}")
            esperado.forEachIndexed { i, v ->
                assertEquals(v, out[i], "valor distinto en $i para $intervals / escala=${scale.size}")
            }
        }
    }

    @Test
    fun `AC-004_2 y AC-004_5 lo mismo para las notas MIDI`() {
        val esperado = ChordGenerator.generateChordMidiNotes(
            rootMidi = rootMidiA4,
            chordIntervals = menorConNovena,
            scaleIntervals = majorScale,
            rootNoteId = rootA
        )
        val out = IntArray(esperado.size)
        val n = ChordGenerator.generateChordMidiNotesInto(
            out = out,
            rootMidi = rootMidiA4,
            chordIntervals = menorConNovena,
            scaleIntervals = majorScale,
            rootNoteId = rootA
        )

        assertEquals(esperado.size, n)
        esperado.forEachIndexed { i, v -> assertEquals(v, out[i], "nota distinta en $i") }
    }

    // ── AC-004.3 — un buffer corto es un error, no un truncamiento ────────────

    /**
     * Truncar en silencio convertiría un bug del llamador en un acorde incompleto que suena
     * *casi* bien — la peor forma de fallar en audio.
     */
    @Test
    fun `AC-004_3 un buffer mas corto que el acorde falla en vez de truncar`() {
        val corto = FloatArray(septima.size - 1)

        assertFailsWith<IllegalArgumentException> {
            ChordGenerator.generateChordFrequenciesInto(
                out = corto,
                rootFreq = rootFreqA4,
                chordIntervals = septima
            )
        }
    }

    @Test
    fun `AC-004_3 y AC-004_5 el buffer corto tambien falla en el camino MIDI`() {
        val corto = IntArray(septima.size - 1)

        assertFailsWith<IllegalArgumentException> {
            ChordGenerator.generateChordMidiNotesInto(
                out = corto,
                rootMidi = rootMidiA4,
                chordIntervals = septima
            )
        }
    }

    @Test
    fun `AC-004_3 un buffer mas grande es valido y deja el resto intacto`() {
        val out = FloatArray(6) { -1f }

        val n = ChordGenerator.generateChordFrequenciesInto(
            out = out,
            rootFreq = rootFreqA4,
            chordIntervals = triada
        )

        assertEquals(2, n)
        for (i in n until out.size) {
            assertEquals(-1f, out[i], "la posición $i está fuera del resultado y no se toca")
        }
    }

    // ── AC-004.4 — el reuso entre acordes de distinto largo ───────────────────

    /**
     * El motivo por el que la función devuelve `Int` y no `Unit`. Si el llamador reusa el buffer
     * y confía en su tamaño, después de pasar de séptima a tríada leería una nota de más — la
     * del acorde anterior, que quedó ahí y es un valor perfectamente plausible.
     */
    @Test
    fun `AC-004_4 al reusar el buffer el count es el de la llamada actual`() {
        val out = FloatArray(6)

        val nSeptima = ChordGenerator.generateChordFrequenciesInto(
            out = out, rootFreq = rootFreqA4, chordIntervals = septima
        )
        val restoDeLaSeptima = out[2]
        val nTriada = ChordGenerator.generateChordFrequenciesInto(
            out = out, rootFreq = rootFreqA4, chordIntervals = triada
        )

        assertEquals(3, nSeptima)
        assertEquals(2, nTriada, "la tríada tiene que reportar 2, no el largo del buffer ni el anterior")
        assertTrue(restoDeLaSeptima > 0f, "control: la séptima sí había escrito la tercera posición")
        assertEquals(restoDeLaSeptima, out[2], "y esa posición queda intacta — por eso el count importa")
    }

    @Test
    fun `AC-004_1 un acorde sin intervalos escribe cero`() {
        val out = FloatArray(4) { -1f }

        val n = ChordGenerator.generateChordFrequenciesInto(
            out = out, rootFreq = rootFreqA4, chordIntervals = emptyList()
        )

        assertEquals(0, n)
        assertEquals(-1f, out[0], "sin intervalos no se escribe nada")
    }
}
