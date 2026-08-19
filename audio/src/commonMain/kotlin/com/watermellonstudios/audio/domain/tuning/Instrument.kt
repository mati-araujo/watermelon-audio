package com.watermellonstudios.audio.domain.tuning

/**
 * Instrumentos y afinaciones de AC-001.14 (REQ-001 S3 · 3.14).
 *
 * EL INVARIANTE QUE ORGANIZA TODO ESTE ARCHIVO
 * --------------------------------------------
 * **Una cuerda se identifica por su ÍNDICE, nunca por su altura.** La numeración es la del
 * músico: la cuerda 1 es la que el instrumento llama 1, y en guitarra ésa es la MÁS AGUDA.
 *
 * No es una convención cualquiera — es AC-001.15, y existe porque hay instrumentos
 * **reentrantes**, donde las cuerdas no vienen ordenadas por altura:
 *
 *   - **Ukelele high-G**: la cuerda 4 es un G4, MÁS AGUDA que la cuerda 3 (C4). Es lo que le
 *     da su sonido característico, y es la afinación por defecto del instrumento.
 *   - **Banjo de 5 cuerdas**: la cuerda 5 es un bordón G4 más agudo que las cuatro restantes,
 *     y ni siquiera recorre el mástil entero.
 *
 * Un afinador que asocie "la nota más grave que escucho" con "la cuerda más grave" le dice al
 * ukelelista que su cuerda 4 está una octava baja. Por eso [Tuning.notes] **conserva el orden
 * de las cuerdas** y este archivo nunca ordena por frecuencia.
 */
data class Instrument(
    val id: String,
    val displayName: String,
    val stringCount: Int,
) {
    init {
        require(stringCount > 0) { "Un instrumento sin cuerdas no es un instrumento: $id" }
    }

    companion object {
        val GUITAR = Instrument("guitar", "Guitarra", 6)
        val BASS_4 = Instrument("bass4", "Bajo 4 cuerdas", 4)
        val BASS_5 = Instrument("bass5", "Bajo 5 cuerdas", 5)
        val UKULELE = Instrument("ukulele", "Ukelele", 4)
        val VIOLIN = Instrument("violin", "Violín", 4)
        val MANDOLIN = Instrument("mandolin", "Mandolina", 4)
        val BANJO_5 = Instrument("banjo5", "Banjo 5 cuerdas", 5)

        val ALL = listOf(GUITAR, BASS_4, BASS_5, UKULELE, VIOLIN, MANDOLIN, BANJO_5)
    }
}

/**
 * Una afinación: qué nota le toca a cada cuerda, **en orden de cuerda**.
 *
 * `notes[0]` es la cuerda 1. Ver el invariante del encabezado: esta lista NO está ordenada por
 * altura y no hay que ordenarla.
 */
data class Tuning(
    val id: String,
    val displayName: String,
    val instrument: Instrument,
    val notes: List<Note>,
) {
    init {
        require(notes.size == instrument.stringCount) {
            "La afinación '$id' declara ${notes.size} notas para un instrumento de " +
                "${instrument.stringCount} cuerdas"
        }
    }

    /** La nota de la cuerda [stringIndex], numerada desde 1. Null si esa cuerda no existe. */
    fun noteForString(stringIndex: Int): Note? = notes.getOrNull(stringIndex - 1)

    /** True si alguna cuerda rompe el orden de alturas: ukelele high-G, banjo. */
    val isReentrant: Boolean
        get() = notes.zipWithNext().any { (a, b) -> b > a }

    companion object {
        private fun n(name: String): Note =
            requireNotNull(Note.parse(name)) { "Nota inválida en el catálogo: $name" }

        // --- guitarra: cuerda 1 = mi agudo -----------------------------------
        val GUITAR_STANDARD = Tuning(
            "guitar_standard", "Estándar (EADGBE)", Instrument.GUITAR,
            listOf(n("E4"), n("B3"), n("G3"), n("D3"), n("A2"), n("E2")),
        )
        val GUITAR_DROP_D = Tuning(
            "guitar_drop_d", "Drop D", Instrument.GUITAR,
            listOf(n("E4"), n("B3"), n("G3"), n("D3"), n("A2"), n("D2")),
        )
        val GUITAR_DADGAD = Tuning(
            "guitar_dadgad", "DADGAD", Instrument.GUITAR,
            listOf(n("D4"), n("A3"), n("G3"), n("D3"), n("A2"), n("D2")),
        )
        val GUITAR_OPEN_G = Tuning(
            "guitar_open_g", "Open G", Instrument.GUITAR,
            listOf(n("D4"), n("B3"), n("G3"), n("D3"), n("G2"), n("D2")),
        )
        val GUITAR_HALF_STEP_DOWN = Tuning(
            "guitar_half_down", "Medio tono abajo", Instrument.GUITAR,
            listOf(n("D#4"), n("A#3"), n("F#3"), n("C#3"), n("G#2"), n("D#2")),
        )

        // --- bajo ------------------------------------------------------------
        val BASS_4_STANDARD = Tuning(
            "bass4_standard", "Bajo 4 estándar", Instrument.BASS_4,
            listOf(n("G2"), n("D2"), n("A1"), n("E1")),
        )
        val BASS_5_STANDARD = Tuning(
            "bass5_standard", "Bajo 5 estándar (B grave)", Instrument.BASS_5,
            listOf(n("G2"), n("D2"), n("A1"), n("E1"), n("B0")),
        )

        // --- ukelele: el caso reentrante de manual ---------------------------
        /** La cuerda 4 (G4) es MÁS AGUDA que la 3 (C4). Es la afinación por defecto. */
        val UKULELE_HIGH_G = Tuning(
            "ukulele_high_g", "Ukelele high-G", Instrument.UKULELE,
            listOf(n("A4"), n("E4"), n("C4"), n("G4")),
        )
        val UKULELE_LOW_G = Tuning(
            "ukulele_low_g", "Ukelele low-G", Instrument.UKULELE,
            listOf(n("A4"), n("E4"), n("C4"), n("G3")),
        )

        // --- quintas ---------------------------------------------------------
        val VIOLIN_STANDARD = Tuning(
            "violin_standard", "Violín (GDAE)", Instrument.VIOLIN,
            listOf(n("E5"), n("A4"), n("D4"), n("G3")),
        )
        val MANDOLIN_STANDARD = Tuning(
            "mandolin_standard", "Mandolina (GDAE)", Instrument.MANDOLIN,
            listOf(n("E5"), n("A4"), n("D4"), n("G3")),
        )

        // --- banjo: bordón agudo en la cuerda 5 ------------------------------
        /** La cuerda 5 es el bordón G4, más agudo que las otras cuatro. */
        val BANJO_5_OPEN_G = Tuning(
            "banjo5_open_g", "Banjo Open G", Instrument.BANJO_5,
            listOf(n("D4"), n("B3"), n("G3"), n("D3"), n("G4")),
        )

        val ALL = listOf(
            GUITAR_STANDARD, GUITAR_DROP_D, GUITAR_DADGAD, GUITAR_OPEN_G, GUITAR_HALF_STEP_DOWN,
            BASS_4_STANDARD, BASS_5_STANDARD,
            UKULELE_HIGH_G, UKULELE_LOW_G,
            VIOLIN_STANDARD, MANDOLIN_STANDARD,
            BANJO_5_OPEN_G,
        )

        fun forInstrument(instrument: Instrument): List<Tuning> =
            ALL.filter { it.instrument == instrument }
    }
}
