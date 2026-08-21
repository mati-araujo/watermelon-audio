package com.watermellonstudios.audio.domain.tuner

/**
 * Una lectura completa del afinador, tomada de una sola vez (REQ-001 S1).
 *
 * **Por qué un snapshot y no getters.** Igual que [com.watermellonstudios.audio.domain.input.InputMetering],
 * por costo de frontera — pero acá hay una razón más fuerte, y es de correctitud:
 * el motor publica estos valores con un seqlock, así que los ocho salen del
 * **mismo** tick. Leídos de a uno podrían mezclarse dos mediciones distintas, y
 * dibujar los cents de un tick con el ángulo de fase del siguiente hace saltar
 * el disco del strobe.
 *
 * **NaN no llega hasta acá.** El motor publica NaN —y no 0— en los campos que
 * todavía no tienen estimador detrás, porque `0.0` cents es un valor plausible
 * (afinado exacto) y se dibujaría como una medición. En Kotlin eso se representa
 * con `null`, que el compilador obliga a considerar. Un consumidor no puede
 * mostrar una ausencia por descuido.
 *
 * @property captureSampleRate Rate al que se CAPTURÓ, medido y no asumido. 0 si
 *   todavía no hay stream de entrada. Un consumidor que asuma 48000 se equivoca
 *   hasta por 1902 cents en un stream de 16 kHz.
 * @property levelRms  RMS lineal de lo analizado en el tick.
 * @property framesAnalyzed  Frames analizados desde el arranque.
 * @property droppedFrames   Frames que el análisis nunca vio porque el ring los
 *   pisó. Si sube sostenidamente, el análisis no llega.
 * @property state    Ver [TunerState].
 * @property cents        Desviación contra el objetivo, o null si no hay dato.
 * @property phaseAngle   Ángulo de fase acumulado en radianes, envuelto a ±π, o
 *   null. Lo publica el motor ya integrado a propósito: si la app tuviera que
 *   integrarlo, un frame perdido correría la fase para siempre.
 * @property uncertainty  Incertidumbre de la medición, o null si no hay dato.
 */
data class TunerSnapshot(
    val captureSampleRate: Int,
    val levelRms: Float,
    val framesAnalyzed: Long,
    val droppedFrames: Long,
    val state: TunerState,
    val cents: Float?,
    val phaseAngle: Float?,
    val uncertainty: Float?,
    /**
     * Altura detectada **sin objetivo**, en Hz, o `null` si no se encontró nota.
     *
     * Es lo que le permite a la app saber qué cuerda está sonando para después empujar el
     * objetivo. No confundir con [cents]: esto dice *qué nota es*, con error de decenas de
     * cents; aquello dice *cuán desafinada está*, con error de milicents — y sólo existe si
     * alguien puso un objetivo.
     */
    val detectedHz: Float?,
    /** Confianza de [detectedHz], 0..1. Por debajo de ~0,5 no hay nota creíble. */
    val detectionClarity: Float,
    /**
     * Coeficiente de inarmonicidad **B** de la cuerda que suena, de
     * `f_n = n·f₀·√(1 + B·n²)`, o `null` si no se pudo medir.
     *
     * Sale gratis del strobe: cuatro parciales que discrepan **son** la rigidez de
     * la cuerda. Valores típicos: ~10⁻⁵ en una prima de guitarra, ~10⁻⁴ en una
     * bordona. `null` no es lo mismo que `0f` — cero es una cuerda ideal, que es
     * un valor plausible.
     */
    val inharmonicityB: Float?,
    /**
     * Índice de la **cuerda** enganchada, o `null` si no hay enganche.
     *
     * Es índice de cuerda, no de frecuencia: en un ukelele high-G la cuerda 1 es
     * más aguda que la 3, y devolver "la más cercana en Hz" como número de cuerda
     * es el bug exacto que AC-001.15 existe para evitar.
     */
    val lockedString: Int?,
    /** 0 sin señal · 1 buscando · 2 enganchado · 3 sin enganche (cromático). */
    val fastModeState: Int,
    /**
     * Hasta dónde vale [cents], en **cents** contra el objetivo, o `null` si no
     * hay objetivo (REQ-003).
     *
     * Es la mitad que vuelve explicable la ausencia de [cents]: sin esto la app
     * ve desaparecer la aguja y no tiene con qué decir por qué ni hasta dónde.
     * Con esto puede dibujar el tramo en el que el número de al lado es
     * confiable — y advertir en el resto en vez de quedarse muda.
     *
     * **En cents y no en Hz** porque la ventana del estimador es fija en Hz, así
     * que su alcance en cents cambia con el registro: ~±119 en un E2 grave
     * contra ~±23 en A4. Y **depende del sample rate**, así que se lee por
     * snapshot y no se hornea en una constante: un device que negocia 44,1 kHz
     * da un rango más angosto que uno a 48.
     */
    val usableRangeCents: Float?,
) {
    companion object {
        /**
         * Cantidad de floats del snapshot nativo. Espeja `WMA_TUNER_SNAPSHOT_VALUES`.
         *
         * **Append-only**: crece, y lo nuevo va al final. Por eso [fromNative]
         * exige `size >= VALUE_COUNT` y no `==`: un motor MÁS NUEVO que esta
         * librería manda un array más largo y se lee el prefijo conocido, que es
         * exactamente la compatibilidad que el orden append-only compra.
         */
        const val VALUE_COUNT: Int = 15

        /**
         * Arma el snapshot desde los floats nativos, en el orden que documenta
         * `wma_tuner_get_snapshot`.
         *
         * @return null si el array viene corto — un contrato roto con la capa
         *   nativa, donde devolver datos a medias sería peor que no devolver nada.
         */
        fun fromNative(values: FloatArray): TunerSnapshot? {
            if (values.size < VALUE_COUNT) return null
            return TunerSnapshot(
                captureSampleRate = values[0].toInt(),
                levelRms = values[1],
                framesAnalyzed = values[2].toLong(),
                droppedFrames = values[3].toLong(),
                state = TunerState.fromNative(values[4]),
                cents = values[5].takeIf { !it.isNaN() },
                phaseAngle = values[6].takeIf { !it.isNaN() },
                uncertainty = values[7].takeIf { !it.isNaN() },
                detectedHz = values[8].takeIf { it > 0f && !it.isNaN() },
                detectionClarity = values[9],
                // El índice 11 es la marca de "medido": sin ella, un B de 0 no se
                // distingue de "no hubo medición" (AC-001.11).
                inharmonicityB = values[10].takeIf { values[11] >= 0.5f && !it.isNaN() },
                lockedString = values[12].toInt().takeIf { it >= 0 },
                fastModeState = values[13].toInt(),
                usableRangeCents = values[14].takeIf { !it.isNaN() },
            )
        }
    }
}

/**
 * En qué punto está la medición. Espeja `wma::analysis::SnapshotState`.
 *
 * Los cuatro estados son distintos para el usuario: "no llega señal" pide
 * revisar el cable, "no engancha" pide tocar más fuerte o más limpio, y
 * "midiendo" es un spinner, no un error.
 */
enum class TunerState {
    /** No llega nada por encima del piso de ruido. */
    NO_SIGNAL,

    /** Hay señal, pero el estimador no enganchó una altura. */
    NO_LOCK,

    /** Midiendo, todavía sin converger. */
    MEASURING,

    /** La incertidumbre bajó del umbral declarado. */
    CONVERGED,

    /**
     * El motor publicó un estado que esta versión de la librería no conoce.
     *
     * No se colapsa a `NO_SIGNAL`: un valor desconocido no es la ausencia de
     * señal, y taparlo con el estado más benigno haría que una versión nueva del
     * motor se vea como un afinador roto sin decir por qué.
     */
    UNKNOWN;

    companion object {
        fun fromNative(value: Float): TunerState = when (value.toInt()) {
            0 -> NO_SIGNAL
            1 -> NO_LOCK
            2 -> MEASURING
            3 -> CONVERGED
            else -> UNKNOWN
        }
    }
}
