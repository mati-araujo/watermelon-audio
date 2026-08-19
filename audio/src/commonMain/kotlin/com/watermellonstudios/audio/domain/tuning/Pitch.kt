package com.watermellonstudios.audio.domain.tuning

import kotlin.jvm.JvmInline
import kotlin.math.log2
import kotlin.math.pow

/**
 * Las dos unidades del dominio, como tipos y no como `Double` (REQ-001 S3 · 3.12).
 *
 * POR QUÉ NO ALCANZA CON `Double`
 * -------------------------------
 * Un `Double` no dice si son hercios o cents, y las dos cantidades aparecen juntas en cada
 * firma de este dominio: `objetivo(nota) -> Hz`, `desviación -> cents`, `capo -> semitonos`.
 * Mezclarlas no es un error hipotético: es **el** error de este dominio, y el compilador no
 * puede ayudar mientras las tres sean el mismo tipo.
 *
 * Son `value class`, así que en tiempo de ejecución siguen siendo un `Double` — el tipo
 * cuesta cero y sólo existe para el compilador.
 */

/** Frecuencia en hercios. Siempre > 0 para una nota real. */
@JvmInline
value class Frequency(val hz: Double) : Comparable<Frequency> {
    init {
        require(hz > 0.0 && hz.isFinite()) { "Frequency inválida: $hz Hz" }
    }

    override fun compareTo(other: Frequency): Int = hz.compareTo(other.hz)

    /**
     * Cuántos cents hay de `this` a [reference].
     *
     * **Positivo = `this` está POR ENCIMA**, que es la convención del afinador y la misma
     * que usa el estimador de fase del motor. Invertirla acá haría que la aguja de la app
     * y el disco del strobe giren en sentidos opuestos.
     */
    fun centsAbove(reference: Frequency): Cents = Cents(1200.0 * log2(hz / reference.hz))

    /** La frecuencia que está a [cents] de ésta, con el mismo signo. */
    fun shiftedBy(cents: Cents): Frequency = Frequency(hz * 2.0.pow(cents.value / 1200.0))

    override fun toString(): String = "${hz}Hz"
}

/**
 * Desviación o intervalo, en cents. 1200 cents = una octava; 100 = un semitono temperado.
 *
 * Puede ser negativo (bemol) y puede exceder 1200 (intervalos de más de una octava).
 */
@JvmInline
value class Cents(val value: Double) : Comparable<Cents> {
    override fun compareTo(other: Cents): Int = value.compareTo(other.value)

    operator fun plus(other: Cents): Cents = Cents(value + other.value)
    operator fun minus(other: Cents): Cents = Cents(value - other.value)
    operator fun unaryMinus(): Cents = Cents(-value)

    override fun toString(): String = "${value}¢"

    companion object {
        val ZERO = Cents(0.0)
        val OCTAVE = Cents(1200.0)
        val SEMITONE_EQUAL = Cents(100.0)
    }
}

/**
 * Semitonos de transposición — capo, afinaciones "medio tono abajo", octavación.
 *
 * Entero a propósito: un capo no puede estar en medio semitono, y permitir un `Double` acá
 * invitaría a usarlo para desafinar, que es lo que expresa [Cents].
 */
@JvmInline
value class Semitones(val count: Int) {
    operator fun plus(other: Semitones) = Semitones(count + other.count)
    operator fun unaryMinus() = Semitones(-count)

    /** El intervalo que representa en temperamento IGUAL. Otros temperamentos no son lineales. */
    val equalTemperedCents: Cents get() = Cents(count * 100.0)
}
