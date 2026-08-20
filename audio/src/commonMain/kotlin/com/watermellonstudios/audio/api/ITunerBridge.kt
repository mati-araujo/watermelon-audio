package com.watermellonstudios.audio.api

/**
 * El camino del afinador a través de la frontera nativa (REQ-001 S1).
 *
 * Vive aparte de [IInputBridge] a propósito: **el motor no tiene por qué saber
 * que existe un afinador**, y esta interfaz es dónde se ve esa separación del
 * lado de Kotlin. El afinador consume la captura, no la configura.
 *
 * Las cuatro funciones son síncronas y no devuelven `Result`: ninguna hace
 * trabajo que pueda tardar ni fallar de una forma que amerite un mensaje.
 * Arrancar devuelve `Boolean` porque sí puede no lograrse —sin nodo de entrada
 * no hay nada que analizar— y aplanar eso a `Unit` haría un fallo
 * indistinguible de un éxito, que es exactamente lo que esta librería ya shippeó
 * una vez en iOS con las `wma_*` que devolvían `void`.
 */
interface ITunerBridge {

    /**
     * Arranca el análisis: la captura empieza a alimentar el ring y un thread
     * propio lo drena y publica snapshots.
     *
     * Idempotente. **No enciende el monitoreo**: afinar no es escucharse.
     *
     * @return false si no se pudo — sin motor o sin nodo de entrada.
     */
    fun startTunerSync(): Boolean

    /**
     * Para de analizar. El último snapshot **sigue siendo legible**, para que la
     * UI no parpadee a vacío cuando el usuario suelta la cuerda.
     */
    fun stopTunerSync()

    fun isTunerRunning(): Boolean

    /**
     * La frecuencia contra la que se mide, en Hz. `0` la borra.
     *
     * **El objetivo lo pone el consumidor, y no es provisorio.** El estimador afina
     * *alrededor* de un objetivo, no lo busca: su rango de captura es de unos pocos cents en
     * la zona aguda. Sin objetivo, el snapshot sigue devolviendo `null` en cents y el estado
     * es "sin enganche" — el motor **no adivina**.
     *
     * Cambiarlo **reinicia la integración**, así que no llamarlo por frame con el mismo valor.
     *
     * @return false si no hay motor o no se pudo crear el camino de análisis.
     */
    fun setTunerTargetHz(hz: Float): Boolean

    /** El objetivo actual en Hz, o 0 si no hay ninguno. */
    fun getTunerTargetHz(): Float

    /**
     * Los ocho valores del snapshot de una sola vez, todos del mismo tick.
     *
     * @return null si no hay análisis o si todavía no se publicó nada. **Null y
     *   "todo en cero" no son lo mismo**: la C API deja el buffer intacto cuando
     *   falla, justamente para que nadie lea ceros como si fueran una medición.
     *   Ver [com.watermellonstudios.audio.domain.tuner.TunerSnapshot.fromNative].
     */
    fun getTunerSnapshot(): FloatArray?
}
