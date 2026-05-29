package com.watermellonstudios.audio.internal.util

import kotlin.math.log2
import kotlin.math.roundToInt

/**
 * Utilidades de cuantización musical compartidas entre los paths
 * de generación de acordes (Hz y MIDI).
 *
 * Pure functions, zero allocations beyond return values.
 */
object ScaleSnapping {

    /** Convierte una frecuencia en Hz al número MIDI equivalente (A4 = 69). */
    fun frequencyToMidiNote(freq: Float): Int {
        return (69 + 12 * log2(freq / 440f)).roundToInt()
    }

    /**
     * Devuelve el MIDI note más cercano que pertenece a la escala activa,
     * buscando hasta `maxDistance` semitonos en cada dirección.
     *
     * Si no encuentra match dentro del rango, retorna el MIDI note original
     * (preserva el carácter del acorde antes que forzar un grado lejano).
     *
     * @param midiNote MIDI note a cuantizar
     * @param scaleIntervals Intervalos de la escala (semitonos desde root, ej [0,2,4,5,7,9,11])
     * @param rootNoteId Nota raíz de la escala (0=C, 9=A, etc.)
     * @param maxDistance Máxima distancia en semitonos a buscar (default 2)
     * @return MIDI note cuantizado a la escala, o el original si está fuera de rango
     */
    fun snapMidiToScale(
        midiNote: Int,
        scaleIntervals: List<Int>,
        rootNoteId: Int,
        maxDistance: Int = 2
    ): Int {
        if (scaleIntervals.isEmpty()) return midiNote

        val noteInOctave = ((midiNote % 12) + 12) % 12
        val scaleNotes = scaleIntervals.map { ((it + rootNoteId) % 12 + 12) % 12 }.toSet()

        if (scaleNotes.contains(noteInOctave)) return midiNote

        for (offset in 1..maxDistance) {
            if (scaleNotes.contains(((noteInOctave + offset) % 12))) return midiNote + offset
            if (scaleNotes.contains(((noteInOctave - offset + 12) % 12))) return midiNote - offset
        }
        return midiNote
    }
}
