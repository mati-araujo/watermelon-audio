package com.watermellonstudios.audio.domain.tuning

import kotlin.math.log2
import kotlin.math.pow

/**
 * Los cuatro temperamentos de AC-001.13, cada uno con su tónica (REQ-001 S3 · 3.13).
 *
 * QUÉ ES UN TEMPERAMENTO, EN UNA LÍNEA
 * ------------------------------------
 * Cómo se reparten las doce alturas dentro de la octava. El **igual** las reparte parejas
 * —100 cents cada una— y por eso suena igual en todas las tonalidades y ninguna tercera es
 * pura. Los históricos sacrifican esa uniformidad para ganar intervalos puros en algunas
 * tonalidades, y ése es exactamente el motivo por el que alguien afina un clave, un órgano o
 * una guitarra fretless con ellos.
 *
 * LA CONVENCIÓN DE ANCLAJE, QUE HAY QUE DECLARAR O NO SIGNIFICA NADA
 * ------------------------------------------------------------------
 * Un temperamento define **intervalos relativos a su tónica**, no frecuencias absolutas. Para
 * llegar a hercios hace falta anclar algo, y acá se ancla así:
 *
 *   **La tónica conserva su frecuencia de temperamento igual; todo lo demás se desvía.**
 *
 * Consecuencia visible, y es la correcta: en justo en Do, el LA **no** queda exactamente en la
 * referencia — queda a −15,6 cents de ella. No es un error: es lo que significa afinar en
 * justo en Do. Si se anclara en La, esa desviación se la comería el Do y la tercera pura
 * dejaría de caer donde el músico la espera.
 *
 * En temperamento igual la desviación es cero en las doce alturas, así que **la tónica no
 * cambia nada** — y hay un test que exige justamente esa asimetría (3.6).
 *
 * POR QUÉ LOS NO-IGUALES SE GENERAN Y NO SE TABULAN
 * -------------------------------------------------
 * Pitagórico y mesotónico salen de **encadenar quintas** desde la tónica. Escribirlos como una
 * tabla de doce números invita a un typo que ningún test agarraría —cada entrada sería su
 * propia fuente de verdad— mientras que generarlos desde el intervalo generador deja UN solo
 * número por temperamento que puede estar mal, y ese número está en los tests.
 */
enum class TemperamentKind(val displayName: String) {
    EQUAL("Igual"),
    JUST("Justo"),
    PYTHAGOREAN("Pitagórico"),
    MEANTONE_QUARTER("Mesotónico 1/4"),
}

data class Temperament(
    val kind: TemperamentKind,
    /** Clase de altura de la tónica: 0 = C, 9 = A. Irrelevante para [TemperamentKind.EQUAL]. */
    val tonic: Int = 0,
) {
    init {
        require(tonic in 0..11) { "Tónica fuera de rango (0..11): $tonic" }
    }

    /**
     * Desviación de cada altura respecto del temperamento igual, en cents.
     * Índice = clase de altura **absoluta** (0 = C), no relativa a la tónica.
     */
    fun deviations(): DoubleArray = when (kind) {
        TemperamentKind.EQUAL -> DoubleArray(12)
        TemperamentKind.JUST -> fromRatios(JUST_RATIOS)
        TemperamentKind.PYTHAGOREAN -> fromFifthChain(PYTHAGOREAN_FIFTH_CENTS)
        TemperamentKind.MEANTONE_QUARTER -> fromFifthChain(MEANTONE_QUARTER_FIFTH_CENTS)
    }

    private fun fromRatios(ratios: DoubleArray): DoubleArray {
        val out = DoubleArray(12)
        for (i in 0 until 12) {
            out[(tonic + i) % 12] = ratioToCents(ratios[i]) - i * 100.0
        }
        return out
    }

    /**
     * Doce quintas encadenadas desde la tónica, de −3 a +8.
     *
     * Ese reparto es el estándar de doce notas y **no es un detalle de implementación**: deja
     * la "quinta del lobo" —la que absorbe toda la coma— entre G♯ y E♭, lejos de las
     * tonalidades usadas. Mover el rango mueve el lobo, que es una decisión de producto; por
     * eso está escrito acá y no enterrado.
     */
    private fun fromFifthChain(fifthCents: Double): DoubleArray {
        val out = DoubleArray(12)
        for (k in -3..8) {
            val relative = ((7 * k) % 12 + 12) % 12
            var cents = k * fifthCents
            while (cents < 0.0) cents += 1200.0
            while (cents >= 1200.0) cents -= 1200.0
            out[(tonic + relative) % 12] = cents - relative * 100.0
        }
        return out
    }

    companion object {
        val STANDARD = Temperament(TemperamentKind.EQUAL)

        /** Quintas puras, 3/2. Terceras anchas (407,8 contra 386,3): ése es su costo. */
        val PYTHAGOREAN_FIFTH_CENTS: Double = ratioToCents(3.0 / 2.0)

        /**
         * Quinta achatada en 1/4 de coma sintónica, para que la tercera mayor quede **pura**.
         *
         * El generador es la **raíz cuarta de 5**: cuatro de esas quintas apiladas dan
         * exactamente 5/1, o sea la tercera mayor pura dos octavas arriba. Por eso se escribe
         * como `5^(1/4)` y no como 696,578 — la definición ES la raíz, el decimal es su
         * consecuencia.
         *
         * La spec deja abierta la variante de 1/6 de coma; **acá es 1/4**, y declararlo es
         * parte del contrato.
         */
        val MEANTONE_QUARTER_FIFTH_CENTS: Double = ratioToCents(5.0.pow(0.25))

        /**
         * La coma pitagórica: lo que sobra al cerrar doce quintas puras contra siete octavas,
         * `(3/2)¹² / 2⁷` = 23,460 cents.
         *
         * Es la razón por la que NINGÚN temperamento de doce notas puede tener las doce
         * quintas puras — y por lo tanto la razón por la que existen todos los demás.
         */
        val PYTHAGOREAN_COMMA_CENTS: Double = 12 * PYTHAGOREAN_FIFTH_CENTS - 7 * 1200.0

        /** 1/1, 16/15, 9/8, 6/5, 5/4, 4/3, 45/32, 3/2, 8/5, 5/3, 9/5, 15/8 — 5 límite. */
        private val JUST_RATIOS = doubleArrayOf(
            1.0 / 1.0, 16.0 / 15.0, 9.0 / 8.0, 6.0 / 5.0, 5.0 / 4.0, 4.0 / 3.0,
            45.0 / 32.0, 3.0 / 2.0, 8.0 / 5.0, 5.0 / 3.0, 9.0 / 5.0, 15.0 / 8.0,
        )

        internal fun ratioToCents(ratio: Double): Double = 1200.0 * log2(ratio)
    }
}
