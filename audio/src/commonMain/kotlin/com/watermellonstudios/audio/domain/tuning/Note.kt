package com.watermellonstudios.audio.domain.tuning

/**
 * Una nota del sistema de 12 alturas, identificada por su número MIDI (REQ-001 S3).
 *
 * POR QUÉ MIDI Y NO (CLASE, OCTAVA)
 * ---------------------------------
 * Porque el número MIDI ya es la representación canónica de "cuál nota", es entero, es
 * comparable, y hace que transponer sea sumar. Guardar clase y octava por separado obliga a
 * re-derivar el entero en cada operación y abre la puerta al bug clásico de la octava:
 * B3 + 1 semitono = C4, no C3.
 *
 * La convención es la estándar: **A4 = 69**, C4 (do central) = 60, C-1 = 0.
 */
data class Note(val midi: Int) : Comparable<Note> {
    init {
        require(midi in 0..127) { "Nota MIDI fuera de rango: $midi" }
    }

    /** 0 = C, 1 = C#, … 9 = A, 11 = B. */
    val pitchClass: Int get() = midi % 12

    /** La octava en notación científica: A4 = 69 → 4. */
    val octave: Int get() = midi / 12 - 1

    val name: String get() = "${NAMES[pitchClass]}$octave"

    operator fun plus(semitones: Semitones): Note = Note(midi + semitones.count)
    operator fun minus(semitones: Semitones): Note = Note(midi - semitones.count)

    override fun compareTo(other: Note): Int = midi.compareTo(other.midi)

    override fun toString(): String = name

    companion object {
        private val NAMES = arrayOf("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")

        /** A4, la referencia. Su frecuencia la decide [TuningReference], no esta clase. */
        val A4 = Note(69)

        /**
         * Parsea "E2", "A#3", "Bb1". Devuelve null si no es una nota válida — **no lanza**:
         * los nombres suelen venir de datos o de la UI, y un null se compone.
         */
        fun parse(text: String): Note? {
            if (text.isEmpty()) return null
            val letter = text[0].uppercaseChar()
            val base = when (letter) {
                'C' -> 0; 'D' -> 2; 'E' -> 4; 'F' -> 5; 'G' -> 7; 'A' -> 9; 'B' -> 11
                else -> return null
            }
            var i = 1
            var accidental = 0
            while (i < text.length && (text[i] == '#' || text[i] == 'b')) {
                accidental += if (text[i] == '#') 1 else -1
                i++
            }
            val octave = text.substring(i).toIntOrNull() ?: return null
            val midi = (octave + 1) * 12 + base + accidental
            return if (midi in 0..127) Note(midi) else null
        }
    }
}

/**
 * La referencia de altura: qué frecuencia tiene A4.
 *
 * AC-001.12 pide aceptar **415 a 466 Hz** — de la afinación barroca (415) a las orquestas
 * que suben (466). No es un capricho: un afinador que sólo hace 440 no sirve para música
 * antigua ni para tocar con una orquesta europea.
 */
@kotlin.jvm.JvmInline
value class TuningReference(val a4: Frequency) {
    init {
        require(a4.hz in MIN_HZ..MAX_HZ) {
            "Referencia A4 fuera del rango soportado ($MIN_HZ–$MAX_HZ Hz): ${a4.hz}"
        }
    }

    companion object {
        const val MIN_HZ = 415.0
        const val MAX_HZ = 466.0

        val STANDARD = TuningReference(Frequency(440.0))
        val BAROQUE = TuningReference(Frequency(415.0))

        fun of(hz: Double) = TuningReference(Frequency(hz))
    }
}
