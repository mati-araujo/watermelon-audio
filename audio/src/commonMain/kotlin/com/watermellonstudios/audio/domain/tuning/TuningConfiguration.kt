package com.watermellonstudios.audio.domain.tuning

import kotlin.math.pow

/**
 * El objetivo de una cuerda: qué nota debería sonar y en qué frecuencia exacta.
 *
 * Lleva la nota Y la frecuencia porque la UI necesita las dos: el nombre para mostrar y los
 * hercios para comparar. Derivarlas por separado en dos lugares es cómo se desincronizan.
 */
data class StringTarget(
    /** Numerada desde 1, como la numera el músico. Ver el invariante de `Instrument.kt`. */
    val stringIndex: Int,
    val note: Note,
    val frequency: Frequency,
)

/**
 * Todo lo que hace falta para saber qué frecuencia debería sonar (REQ-001 S3).
 *
 * Es un **modelo puro**: no toca una sola muestra de audio, no conoce el motor, y se testea
 * entero en JVM en milisegundos. Esa separación es deliberada — la mitad musical de un
 * afinador profesional no necesita DSP para ser correcta, y mezclarlas haría que testear un
 * temperamento requiera un stream de audio.
 *
 * LO QUE ESTE MODELO NO HACE, Y NO ES OLVIDO
 * ------------------------------------------
 * Entrega el objetivo **teórico**. La corrección por **inarmonicidad** —que en una cuerda real
 * mueve el objetivo perceptual, sobre todo en pianos y en bordonas— es de otra etapa y se
 * aplica encima. Separarlas permite testear el temperamento sin un modelo físico de cuerda de
 * por medio, y permite que la corrección física evolucione sin tocar la teoría musical.
 */
data class TuningConfiguration(
    val tuning: Tuning,
    val temperament: Temperament = Temperament.STANDARD,
    val reference: TuningReference = TuningReference.STANDARD,
    val capo: Semitones = Semitones(0),
) {
    init {
        require(capo.count in -12..12) {
            "Capo fuera del rango de AC-001.14 (−12..+12): ${capo.count}"
        }
    }

    /**
     * Los objetivos de todas las cuerdas, **en orden de cuerda**.
     *
     * No se ordena por frecuencia: ver AC-001.15 y el invariante de `Instrument.kt`.
     */
    fun targets(): List<StringTarget> =
        tuning.notes.mapIndexed { i, note ->
            val transposed = transpose(note)
            StringTarget(i + 1, transposed, frequencyOf(transposed))
        }

    /**
     * El objetivo de una cuerda concreta, numerada desde 1. Null si no existe.
     *
     * **Delega en [targets] a propósito, aunque cueste una lista de cinco elementos.** Cuando
     * las dos entradas construían el objetivo por su cuenta, un mutante que ordenaba
     * [targets] por altura —el bug exacto de AC-001.15— SOBREVIVÍA: los tests de afinación
     * reentrante entraban por acá y no lo veían. Una sola construcción hace imposible esa
     * divergencia, que es mejor que un test que la vigile.
     */
    fun targetForString(stringIndex: Int): StringTarget? =
        targets().getOrNull(stringIndex - 1)

    /**
     * La frecuencia de una nota bajo esta referencia y este temperamento.
     *
     * Dos pasos, y el orden importa: primero el temperamento igual desde la referencia, y
     * encima la desviación del temperamento elegido. Hacerlo al revés obligaría a re-anclar
     * la referencia en cada temperamento.
     */
    fun frequencyOf(note: Note): Frequency {
        val equal = reference.a4.hz * 2.0.pow((note.midi - Note.A4.midi) / 12.0)
        val deviation = temperament.deviations()[note.pitchClass]
        return Frequency(equal * 2.0.pow(deviation / 1200.0))
    }

    /**
     * Aplica el capo, **saturando** en los extremos del rango MIDI en vez de lanzar.
     *
     * Saturar y no lanzar es deliberado: el capo lo mueve un usuario con un control, y un
     * bajo de 5 cuerdas con capo −12 se sale del rango por abajo. Que la app explote porque
     * alguien arrastró un slider sería peor que devolver la nota más grave posible.
     */
    private fun transpose(note: Note): Note =
        Note((note.midi + capo.count).coerceIn(0, 127))
}
