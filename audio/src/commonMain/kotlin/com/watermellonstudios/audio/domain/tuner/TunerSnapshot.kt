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
 * @property cents        Desviación contra el objetivo, o null si no hay dato. Que haya número no
 *   dice que convergió: eso lo dice sólo [state]. Con [TunerState.MEASURING] el número es de una de
 *   dos clases, que el snapshot no distingue:
 *   - la **lectura temprana** (desde v2.26.0, R-PITCH-65): desde ~0,2 s de cada ataque, y otra vez
 *     cuando el veredicto se corta o espera a un segmento nuevo, la **altura instantánea** —no una
 *     predicción de la asentada (R-PITCH-67)—. Durante el glide del ataque puede estar a varios
 *     cents de donde la cuerda se asienta, y con 2 o más parciales su σ lo cubre. No se separa de la
 *     otra por el tiempo desde el ataque;
 *   - la lectura del **veredicto con σ > 0,1**, que ya existía antes de v2.26.0 y ahora publica
 *     max(su σ, la deriva temprana); esa σ todavía es optimista.
 *   Los dos huecos de σ —ésa, y la temprana con un solo parcial— están en
 *   `docs/tuner/accuracy_contract.md`.
 * @property phaseAngle   Ángulo de fase acumulado en radianes, envuelto a ±π, o
 *   null. Lo publica el motor ya integrado a propósito: si la app tuviera que
 *   integrarlo, un frame perdido correría la fase para siempre.
 * @property uncertainty  Incertidumbre (1σ, en cents) de [cents], o null si no hay dato. Sale de
 *   los residuos del ajuste entre parciales (con 3 o más; con menos, de la propagada) (R-PITCH-52);
 *   en la lectura temprana se le suma en cuadratura un término de deriva que crece con el glide del
 *   ataque, y una [TunerState.MEASURING] del veredicto publica max(su σ, esa deriva). La promesa sobre las
 *   [TunerState.MEASURING] es |error| ≤ 2σ en el 95 % de las lecturas: los estratos medidos, los
 *   PROVISIONAL y los huecos, en `docs/tuner/accuracy_contract.md`. Una lectura temprana puede
 *   tener σ ≤ 0,1 y seguir en [TunerState.MEASURING]: "convergió" lo dice [state], no este número.
 * @property inputDiscontinuity `true` si esta lectura arrastra un hueco en la
 *   entrada (REQ-009). Ver el KDoc del campo: es lo que distingue "todavía no"
 *   de "la entrada llegó rota".
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
     * **En cents y no en Hz** porque el paso de las rejillas del estimador (1024
     * frames) es fijo en Hz, así que su alcance en cents cambia con el registro:
     * ~±395 en un E2 grave contra ~±81 en A4 a 48 kHz (REQ-057: `1200·log2(1 +
     * 0,9·fs/(2·1024·f0))`; antes, con la ventana de 4096, ~±119 contra ~±23).
     * Y **depende del sample rate**, así que se lee por
     * snapshot y no se hornea en una constante: un device que negocia 44,1 kHz
     * da un rango más angosto que uno a 48.
     */
    val usableRangeCents: Float?,
    /**
     * `true` si la integración detrás de [cents] arrastra un **hueco en la
     * entrada** (REQ-009).
     *
     * Es la mitad que vuelve accionable a [TunerState.MEASURING]. Sin esto la app
     * muestra el mismo spinner en dos situaciones que le piden al usuario cosas
     * **opuestas**: *"todavía no convergí"* se arregla **esperando**, y *"la
     * entrada llegó rota"* se arregla **revisando el cable** —o cerrando lo que le
     * come la CPU al teléfono—. Esperar frente a un problema que no se arregla
     * solo es la peor de las dos salidas.
     *
     * **No es `droppedFrames > 0`.** Ese contador es acumulado y monótono: tras el
     * primer desborde queda arriba para siempre, así que una app que lo usara para
     * esto diría "revisá el cable" el resto de la sesión. Esta bandera describe la
     * lectura **viva**: se levanta con el hueco y se baja sola cuando el motor
     * vuelve a tener una medición propia.
     *
     * **`Boolean` y no `Boolean?`**: acá no hay ausencia que expresar. La pregunta
     * *"¿esta lectura vio un hueco?"* tiene respuesta siempre — incluso sin
     * objetivo y sin señal, donde la respuesta es `false`.
     */
    val inputDiscontinuity: Boolean,

    /**
     * Cuántas veces la entrada perdió continuidad desde que arrancó el análisis.
     * Acumulado y monótono (REQ-014).
     *
     * 🔴 **No es redundante con [inputDiscontinuity], y la diferencia es la razón
     * de que exista.** Esa bandera **se puede perder**: se baja sola, así que
     * verla depende de cuántas veces la app alcanzó a mirar entre una publicación
     * y la siguiente. Con el teléfono cargado son dos o tres, o sea que verla es
     * un volado.
     *
     * Con esto, *"¿pasó algo desde la última vez que miré?"* se contesta
     * comparando contra el valor anterior, **sin haber estado mirando en el
     * instante**. Es lo que le permite a una app decir "la entrada se cortó
     * mientras estabas afinando" en vez de no enterarse nunca.
     *
     * **No lo uses como condición para ocultar la aguja.** Es monótono: una
     * guarda apoyada acá se traba y no vuelve a mostrar nada. Para eso está
     * [inputDiscontinuity], que baja sola.
     *
     * Cuenta **eventos**, no ticks: un desborde sostenido suma uno.
     */
    val discontinuityCount: Long,

    /**
     * Si [detectedHz] tiene **soporte espectral** en la señal (REQ-031). Es la bandera
     * **compañera** de ese valor y lo califica **siempre**, no sólo cuando el estado no es
     * [TunerState.CONVERGED]:
     *
     * - `true`: hay energía en ese fundamental **o en su octava**.
     * - `false`: no la hay en ninguno de los dos. La altura es un **submúltiplo** que explica
     *   los mismos datos, no algo que se midió — *"vi esto y no le creo"*. [detectedHz] se
     *   sigue publicando igual, porque es más útil que el silencio.
     * - `null`: no hay altura que calificar ([detectedHz] es `null`).
     *
     * **Por qué existe.** El período de una señal puede ser ambiguo: si faltan los parciales
     * que lo desambigüan, un submúltiplo del período también es un período, y el detector
     * puede leer una nota un tercio o un quinto **abajo** de su altura real. Medido: una E4
     * con fundamental débil más H3 y H5 se lee como A2, y con el instrumento declarado el
     * motor la publicaba convergida a −1,955 cents — la coincidencia entre el 3.er armónico
     * de A2 y el f0 real de E4. [detectionClarity] **no** lo delata (0,9946 sobre el falso
     * contra 0,9995 sobre el verdadero): responde *"¿hay UNA altura clara?"*, y esa pregunta
     * tiene la misma respuesta en los dos casos.
     *
     * **Fundamental o octava, no fundamental solo.** Una bordona real puede tener el
     * fundamental 40 dB por debajo de su segundo parcial y tiene que seguir midiéndose. Lo
     * que separa las dos poblaciones es la **forma**, no el nivel: en el falso faltan el
     * fundamental *y* su octava.
     *
     * **`Boolean?` y no `Boolean`**: sin altura la pregunta no tiene respuesta, y `false` se
     * leería como *"vi una altura y no le creo"*. Es la misma convención que [cents] o
     * [usableRangeCents]: ausente, no falso.
     *
     * 🔴 **Mirá la bandera antes que el estado, no al revés.** `false` **no** implica
     * [TunerState.NO_LOCK]: sin instrumento declarado una altura ajena cae en
     * [TunerState.NO_SIGNAL] (es el contrato de la ausencia, R-PITCH-56) **y la bandera viaja
     * igual** — `NO_SIGNAL` con `detectedHz` = 109,87 y `spectralSupport = false`. `NO_LOCK` con
     * `false` sólo aparece **con** candidatos. Un consumidor que sólo mire la bandera cuando ve
     * `NO_LOCK` no la mira nunca sin candidatos. Lo midió el consumidor sobre 2.16.0.
     */
    val spectralSupport: Boolean?,
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
        const val VALUE_COUNT: Int = 18

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
                // 0/1, no NaN: ver el KDoc del campo. El `>= 0.5f` es la misma
                // defensa que el índice 11 — un float que viaja por la frontera
                // no se compara por igualdad exacta.
                inputDiscontinuity = values[15] >= 0.5f,
                discontinuityCount = values[16].toLong(),
                // NaN = no hay altura que calificar (REQ-031). El `>= 0.5f` es la misma
                // defensa que los índices 11 y 15.
                spectralSupport = values[17].takeIf { !it.isNaN() }?.let { it >= 0.5f },
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

    /**
     * Midiendo, todavía sin converger. Puede traer `cents` + σ: la lectura temprana de cada
     * ataque (la altura instantánea, R-PITCH-65/67) o la del veredicto que todavía no convergió;
     * ver `TunerSnapshot.cents`. También puede venir sin `cents`: es ausencia, no cero.
     */
    MEASURING,

    /**
     * La medición pasó el veredicto de tendencia (12 ventanas de fase, ~1 s; en un re-punteo de
     * la misma nota se acumulan entre segmentos) **y** su σ es ≤ 0,1 cents (R-PITCH-62). Nunca
     * antes del veredicto, aunque una lectura temprana tenga σ chica.
     */
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
