package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.tuner.TunerSnapshot
import com.watermellonstudios.audio.domain.tuning.StringTarget
import com.watermellonstudios.audio.domain.tuning.TuningConfiguration

/**
 * El afinador visto desde una app (REQ-001 S3 · 3.16).
 *
 * QUÉ JUNTA ESTA INTERFAZ, Y POR QUÉ SON DOS COSAS SEPARADAS
 * ----------------------------------------------------------
 * Un afinador es la unión de dos mitades que este proyecto mantiene deliberadamente
 * separadas:
 *
 *   - **Qué DEBERÍA sonar** — [TuningConfiguration]: instrumento, afinación, temperamento,
 *     referencia y capo. Es Kotlin puro, se testea en milisegundos y no toca audio.
 *   - **Qué ESTÁ sonando** — [TunerSnapshot]: lo que el motor midió. Vive en C++, cruza la
 *     frontera como [TunerSnapshot.VALUE_COUNT] floats coherentes entre sí.
 *
 * 🔴 EL NÚMERO NO SE ESCRIBE EN PROSA, Y NO ES ESTILO. Este KDoc decía "ocho floats" mientras
 * el snapshot ya tenía **quince**: creció con S4, S5, S7 y REQ-003.2, y la prosa se quedó donde
 * estaba. Un consumidor congeló esa forma y diseñó pedidos contra ella — **lo append-only
 * protege su CÓDIGO, no su modelo mental**. Referenciar la constante hace que no pueda volver
 * a desfasarse.
 *
 * `ITuner` es el punto donde se juntan, y **nada más**: no calcula temperamentos ni analiza
 * audio. Que la unión sea una capa delgada es lo que permite que las dos mitades evolucionen
 * sin arrastrarse.
 *
 * LO QUE UN IMPLEMENTADOR TIENE QUE HACER Y NO SE VE EN LAS FIRMAS
 * ----------------------------------------------------------------
 * El estimador del motor **afina alrededor de un objetivo, no lo busca**: su rango de captura
 * es de unos pocos cents en la zona aguda. Así que un implementador de esta interfaz tiene la
 * obligación de **empujar la frecuencia objetivo al motor** cada vez que cambia
 * [configuration] o [selectedString]. Una implementación que sólo lea y nunca escriba el
 * objetivo va a reportar "sin enganche" para siempre y parecerá un problema del DSP.
 */
interface ITuner {

    /**
     * Qué debería sonar. Cambiarla recalcula [targets] y **obliga a re-empujar el objetivo
     * al motor** (ver la nota del encabezado).
     */
    var configuration: TuningConfiguration

    /**
     * Los objetivos de todas las cuerdas, **en orden de cuerda**.
     *
     * No está ordenado por frecuencia: en ukelele high-G y en banjo, la cuerda más aguda no es
     * la primera. Ver AC-001.15.
     */
    val targets: List<StringTarget>

    /**
     * Contra qué cuerda se mide, numerada desde 1. `null` = todavía no se eligió.
     *
     * No tiene default automático a propósito: elegir la cuerda por proximidad es detección
     * gruesa, y ésa es otra capa. Una implementación que la agregue lo hace explícito.
     */
    var selectedString: Int?

    /** Arranca el análisis. `false` si no se pudo — sin motor o sin entrada de audio. */
    fun start(): Boolean

    /** Para de analizar. La última lectura sigue siendo legible. */
    fun stop()

    val isRunning: Boolean

    /**
     * La lectura actual, o `null` si todavía no hay ninguna.
     *
     * `null` **no es** "afinado" ni "cero": es ausencia de dato, y hay que dibujarla distinto.
     */
    fun reading(): TunerReading?
}

/**
 * Una lectura: lo que el motor midió, junto a lo que debería sonar.
 *
 * Los dos lados van juntos y sin aplanar a propósito. Una UI necesita las dos cosas —"E2,
 * 82,41 Hz" y "+3,2 cents"— y derivarlas por separado en dos lugares es cómo se
 * desincronizan.
 */
data class TunerReading(
    /** Contra qué objetivo se midió. `null` si no hay cuerda seleccionada. */
    val target: StringTarget?,
    /** Lo que midió el motor. Sus campos de afinación son `Float?`: `null` = sin dato. */
    val snapshot: TunerSnapshot,
) {
    /**
     * Desviación contra el objetivo, en cents. **Positivo = sostenido**, la convención del
     * afinador y la misma del estimador de fase del motor.
     *
     * `null` cuando el motor todavía no tiene una medición — que **no** es lo mismo que 0,0:
     * cero cents es "afinado exacto", un valor plausible que una UI dibujaría como lectura.
     */
    val cents: Float? get() = snapshot.cents

    /** `true` sólo cuando hay objetivo, hay medición y el motor la declara convergida. */
    val isConverged: Boolean
        get() = target != null && snapshot.cents != null &&
            snapshot.state == com.watermellonstudios.audio.domain.tuner.TunerState.CONVERGED

    /**
     * `true` cuando esta lectura **no convergió porque la entrada llegó rota**, y no
     * porque todavía no haya integrado lo suficiente (REQ-009).
     *
     * Va al lado de [isConverged] porque es la pregunta que sigue: cuando aquélla da
     * `false`, ésta dice **qué hacer**. `false` en las dos = esperar; `true` acá =
     * revisar el cable, o cerrar lo que le está comiendo la CPU al teléfono.
     *
     * Nunca es `true` junto con [isConverged]: el motor no publica convergido sobre una
     * integración con hueco — eso es AC-009.1, y es el REQ entero.
     */
    val isInputBroken: Boolean get() = snapshot.inputDiscontinuity && !isConverged
}
