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
 * Todas las operaciones son funciones puras.
 *
 * 🔴 **El array de retorno ES una asignación.** Este KDoc decía "sin allocations más allá del
 * array de retorno", que se lee como si no contara — y para un consumidor que llama esto **una
 * vez por frame** es justamente la asignación que paga. Quien esté en ese caso tiene que usar
 * [generateChordFrequenciesInto] / [generateChordMidiNotesInto], que escriben sobre un buffer
 * que trae el llamador.
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

        val result = FloatArray(chordIntervals.size)
        generateChordFrequenciesInto(
            out = result,
            rootFreq = rootFreq,
            chordIntervals = chordIntervals,
            scaleIntervals = scaleIntervals,
            rootNoteId = rootNoteId,
            maxSnapDistance = maxSnapDistance
        )
        return result
    }

    /**
     * Igual que [generateChordFrequencies], pero **escribe sobre [out]** en vez de pedir un array
     * nuevo, y devuelve **cuántas frecuencias escribió**.
     *
     * Existe para el consumidor que llama esto por frame: ahí el array de retorno de la otra
     * variante es una asignación por cuadro, y con la raíz moviéndose de forma continua no hay
     * nada que cachear.
     *
     * 🔴 **Devuelve el conteo y no `Unit` a propósito.** El caso de uso es reusar un buffer entre
     * acordes de distinto tamaño —una tríada escribe 2 posiciones, una séptima 3—, así que el
     * largo de [out] no dice cuántas valen. Sin el conteo, pasar de séptima a tríada dejaría en
     * la tercera posición la nota del acorde anterior: un valor perfectamente plausible, que
     * suena.
     *
     * [out] puede ser **más grande** que [chordIntervals]: se escriben las primeras `n` y el
     * resto queda como estaba. Más chico es un error y no un truncamiento, porque truncar en
     * silencio convierte un bug del llamador en un acorde incompleto que suena *casi* bien.
     *
     * Es la implementación real: [generateChordFrequencies] delega acá. Esa dirección es
     * deliberada — al revés, la variante "sin asignar" asignaría igual, y serían dos
     * implementaciones del mismo cálculo, libres de divergir.
     *
     * @return cuántas posiciones de [out] quedaron escritas
     * @throws IllegalArgumentException si [out] no entra el acorde entero
     */
    fun generateChordFrequenciesInto(
        out: FloatArray,
        rootFreq: Float,
        chordIntervals: List<Int>,
        scaleIntervals: List<Int> = emptyList(),
        rootNoteId: Int = 9,
        maxSnapDistance: Int = DEFAULT_MAX_SNAP_DISTANCE
    ): Int {
        require(out.size >= chordIntervals.size) {
            "El buffer entra ${out.size} y el acorde tiene ${chordIntervals.size} intervalos"
        }
        if (chordIntervals.isEmpty()) return 0

        val rootMidi = ScaleSnapping.frequencyToMidiNote(rootFreq)
        for (i in chordIntervals.indices) {
            val targetMidi = rootMidi + chordIntervals[i]
            val snappedMidi = if (scaleIntervals.isNotEmpty()) {
                ScaleSnapping.snapMidiToScale(targetMidi, scaleIntervals, rootNoteId, maxSnapDistance)
            } else {
                targetMidi
            }
            val snappedInterval = snappedMidi - rootMidi
            out[i] = rootFreq * 2f.pow(snappedInterval / 12f)
        }
        return chordIntervals.size
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
        generateChordMidiNotesInto(
            out = result,
            rootMidi = rootMidi,
            chordIntervals = chordIntervals,
            scaleIntervals = scaleIntervals,
            rootNoteId = rootNoteId,
            maxSnapDistance = maxSnapDistance
        )
        return result
    }

    /**
     * La variante con buffer de [generateChordMidiNotes]. Mismo contrato que
     * [generateChordFrequenciesInto], en el dominio MIDI: es el camino de SoundFont, y tiene el
     * mismo problema — se lo llama por frame mientras el dedo arrastra.
     *
     * @return cuántas posiciones de [out] quedaron escritas
     * @throws IllegalArgumentException si [out] no entra el acorde entero
     */
    fun generateChordMidiNotesInto(
        out: IntArray,
        rootMidi: Int,
        chordIntervals: List<Int>,
        scaleIntervals: List<Int> = emptyList(),
        rootNoteId: Int = 9,
        maxSnapDistance: Int = DEFAULT_MAX_SNAP_DISTANCE
    ): Int {
        require(out.size >= chordIntervals.size) {
            "El buffer entra ${out.size} y el acorde tiene ${chordIntervals.size} intervalos"
        }
        if (chordIntervals.isEmpty()) return 0

        for (i in chordIntervals.indices) {
            val targetMidi = rootMidi + chordIntervals[i]
            out[i] = if (scaleIntervals.isNotEmpty()) {
                ScaleSnapping.snapMidiToScale(targetMidi, scaleIntervals, rootNoteId, maxSnapDistance)
            } else {
                targetMidi
            }
        }
        return chordIntervals.size
    }
}
