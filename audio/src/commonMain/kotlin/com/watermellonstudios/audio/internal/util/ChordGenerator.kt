package com.watermellonstudios.audio.internal.util

import kotlin.math.pow

/**
 * Genera frecuencias y MIDI notes de acordes a partir de una raíz e intervalos.
 *
 * En modo scale-aware (cuando se provee `scaleIntervals`), cada intervalo se
 * ajusta al grado más cercano de la escala — con un cap configurable de
 * `maxSnapDistance` semitonos. Si el grado más cercano está fuera del cap,
 * se mantiene el intervalo cromático original (preserva el carácter del acorde).
 *
 * Todas las operaciones son funciones puras — sin allocations más allá del array de retorno.
 */
object ChordGenerator {

    /** Distancia máxima por defecto al snapear intervalos a una escala. */
    const val DEFAULT_MAX_SNAP_DISTANCE = 2

    /**
     * Genera las frecuencias del acorde (en Hz) desde una frecuencia raíz.
     *
     * @param rootFreq Frecuencia raíz en Hz
     * @param chordIntervals Semitonos desde la raíz (ej [4, 7] para tríada mayor)
     * @param scaleIntervals Intervalos de la escala (semitonos desde scale root, ej [0,2,4,5,7,9,11] para mayor).
     *                       Vacío = cromático (sin snapping).
     * @param rootNoteId Nota raíz de la escala (0=C, 9=A, etc.)
     * @param maxSnapDistance Cap de semitonos al snapear cada intervalo
     * @return Frecuencias de armonía (NO incluye la raíz)
     */
    fun generateChordFrequencies(
        rootFreq: Float,
        chordIntervals: List<Int>,
        scaleIntervals: List<Int> = emptyList(),
        rootNoteId: Int = 9,
        maxSnapDistance: Int = DEFAULT_MAX_SNAP_DISTANCE
    ): FloatArray {
        if (chordIntervals.isEmpty()) return FloatArray(0)

        val rootMidi = ScaleSnapping.frequencyToMidiNote(rootFreq)
        val result = FloatArray(chordIntervals.size)

        for (i in chordIntervals.indices) {
            val targetMidi = rootMidi + chordIntervals[i]
            val snappedMidi = if (scaleIntervals.isNotEmpty()) {
                ScaleSnapping.snapMidiToScale(targetMidi, scaleIntervals, rootNoteId, maxSnapDistance)
            } else {
                targetMidi
            }
            val snappedInterval = snappedMidi - rootMidi
            result[i] = rootFreq * 2f.pow(snappedInterval / 12f)
        }

        return result
    }

    /**
     * Genera los MIDI notes del acorde desde un MIDI note raíz.
     *
     * Mismo contrato que [generateChordFrequencies] pero trabaja en el dominio MIDI
     * — útil para el path SoundFont donde las voces se disparan por MIDI note number.
     *
     * @return MIDI notes de la armonía (NO incluye la raíz)
     */
    fun generateChordMidiNotes(
        rootMidi: Int,
        chordIntervals: List<Int>,
        scaleIntervals: List<Int> = emptyList(),
        rootNoteId: Int = 9,
        maxSnapDistance: Int = DEFAULT_MAX_SNAP_DISTANCE
    ): IntArray {
        if (chordIntervals.isEmpty()) return IntArray(0)

        val result = IntArray(chordIntervals.size)
        for (i in chordIntervals.indices) {
            val targetMidi = rootMidi + chordIntervals[i]
            result[i] = if (scaleIntervals.isNotEmpty()) {
                ScaleSnapping.snapMidiToScale(targetMidi, scaleIntervals, rootNoteId, maxSnapDistance)
            } else {
                targetMidi
            }
        }
        return result
    }
}
