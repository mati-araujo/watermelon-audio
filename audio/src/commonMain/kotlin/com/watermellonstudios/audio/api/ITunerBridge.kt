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
     * Los [com.watermellonstudios.audio.domain.tuner.TunerSnapshot.VALUE_COUNT]
     * valores del snapshot de una sola vez, todos del mismo tick.
     *
     * 🔴 El número va por la constante y no escrito acá: este KDoc decía "ocho"
     * mientras el snapshot ya tenía quince, y un consumidor diseñó contra esa cifra.
     *
     * 🔴 **`stop()` NO borra la última lectura, y eso es una garantía del motor.**
     * Esta función lee de un buffer **publicado**, no del hilo de análisis corriendo,
     * y `wma_tuner_stop` no libera ni el ring ni el snapshot —está dicho explícito
     * en `watermelon_audio.cpp`—. Así que después de [stopTunerSync] la última
     * medición sigue disponible, y un envoltorio **no tiene nada que cachear**: un
     * caché propio sobreviviría también a la destrucción del subsistema, y ahí
     * convertiría *"no sé"* en *"sigue igual"*.
     *
     * @return null en **tres** casos distintos, y conviene no colapsarlos:
     *   1. **no existe la costura de análisis** (`analysis seam`) — el subsistema no
     *      está construido. NO es lo mismo que "el afinador está parado": parar deja
     *      la costura en pie, ver arriba.
     *   2. **todavía no se publicó nada** — la costura existe y aún no hubo un tick.
     *   3. la copia salió desgarrada / el buffer de salida es nulo.
     *
     *   **Null y "todo en cero" no son lo mismo**: la C API deja el buffer intacto
     *   cuando falla, justamente para que nadie lea ceros como si fueran una
     *   medición. Ver [com.watermellonstudios.audio.domain.tuner.TunerSnapshot.fromNative].
     *
     * ⚠️ Este KDoc decía *"null si no hay análisis"* a secas, y esa frase se lee como
     * *"si el afinador está parado"*. La ambigüedad hizo que un criterio de aceptación
     * de REQ-010 S1 se escribiera **mal dos veces** —primero prohibiendo conservar la
     * última lectura, después exigiendo un caché que el motor ya hacía innecesario— y
     * el desempate salió de leer el C++, que es justo lo que un consumidor de esta
     * interfaz no debería tener que hacer. La fuente autoritativa (`watermelon_audio.h`)
     * siempre dijo **"analysis seam"**; la precisión se perdió al traducir. (MINI-003)
     */
    fun getTunerSnapshot(): FloatArray?

    // ---- Modo intonación (REQ-001 S9) --------------------------------------

    /**
     * Guarda la lectura actual del strobe en un slot: 0 = armónico del 12º
     * traste, 1 = nota pisada.
     *
     * Devuelve `false` si todavía no convergió. **No guarda una medida a medio
     * integrar ni siquiera marcada**: el único uso posible de un dato así es
     * restarlo por accidente.
     */
    fun captureIntonation(slot: Int): Boolean

    /** Descarta las dos medidas: la señal se fue, o se cambió de cuerda. */
    fun resetIntonation()

    /** 0 falta el armónico · 1 falta la pisada · 2 listo · 3 cuerdas distintas. */
    fun intonationState(): Int

    /**
     * Cuánto está la nota pisada respecto del armónico, en cents. Positivo = hay
     * que alargar la cuerda. `NaN` mientras no haya resultado — y no `0`, que
     * sería "intonación perfecta".
     */
    fun intonationDifferenceCents(): Float

    // ---- Modo rápido (REQ-001 S5) ------------------------------------------

    /**
     * Las cuerdas del instrumento en Hz, **en orden de cuerda**.
     *
     * Con candidatos puestos el motor **elige el objetivo solo** desde la
     * detección gruesa — que es lo que faltaba para que el afinador funcione sin
     * que el consumidor empuje un objetivo a mano. Lista vacía = vuelve al
     * objetivo manual de [setTunerTargetHz].
     */
    fun setTunerCandidates(hz: FloatArray): Boolean

    /** Engancha a mano a una cuerda por índice, como cuando el músico la elige. -1 suelta. */
    fun lockTunerString(index: Int): Boolean

    // ---- El puerto offline (REQ-015 S2) ------------------------------------

    /**
     * Analiza una **grabación**: sin micrófono, sin dispositivo, sin permiso y sin stream.
     *
     * 🔴 **Es la única función de esta interfaz que no habla del motor.** Todas las demás son
     * sobre el afinador que el músico está mirando; ésta es sobre un buffer. La llamada nativa
     * **no lleva handle de motor** en ningún tramo, y el análisis arma su propio ring, su propio
     * snapshot y su propio análisis por llamada: correrla con el afinador vivo andando **no le
     * mueve la aguja** (AC-015.4). El puente acá es el transporte, no el motor.
     *
     * Corre el **mismo** análisis que el camino de tiempo real. Un camino paralelo mediría otro
     * motor, y su verde no diría nada del producto.
     *
     * Es **determinista**: el mismo buffer da el mismo resultado siempre. No hay thread, no hay
     * reloj y no hay nada que esperar — que es exactamente lo que lo vuelve usable como test de
     * regresión, y la razón por la que existe.
     *
     * @param samples `frames · channels` floats. Mono, o estéreo intercalado (L R L R…).
     * @param channels 1 o 2. Cualquier otra cosa se **rechaza** en vez de adivinarse: si el
     *   consumidor tuviera que mezclar el estéreo a mono por su cuenta y lo hiciera distinto de
     *   `0,5·(L+R)`, rompería la coincidencia con el camino vivo **en silencio** — y esa
     *   coincidencia es lo que le da sentido al puerto.
     * @param sampleRate el rate **del material**. No se asume 48000: un buffer de 44,1 leído
     *   como si fuera de 48 da una lectura bien formada y equivocada, y este repo ya pagó esa
     *   constante cableada una vez.
     * @param targetHz contra qué medir, igual que [setTunerTargetHz]. `0` = sin objetivo, y
     *   entonces cents/ángulo/incertidumbre cruzan como `NaN`, tal cual en vivo.
     *
     * @return los [com.watermellonstudios.audio.domain.tuner.TunerSnapshot.VALUE_COUNT] valores,
     *   o `null` si los argumentos no describen audio analizable o el análisis nunca llegó a
     *   publicar. **`null` y "todo en cero" no son lo mismo**: la C API deja el buffer intacto
     *   cuando falla, justamente para que nadie lea ceros como una medición.
     */
    fun analyzeTunerBuffer(
        samples: FloatArray,
        channels: Int,
        sampleRate: Int,
        targetHz: Float,
    ): FloatArray?

    /**
     * Igual que [analyzeTunerBuffer], **declarando el instrumento** (REQ-029 S1).
     *
     * Sin candidatos el motor no puede preguntarse *"¿esta altura es alguna cuerda?"*, así que
     * una nota que no es el objetivo cae en la compuerta de ausencia y sale como `NO_SIGNAL`.
     * Medido sobre 2.15.0: de 36 combinaciones de objetivo × tono, las 30 de afuera de la
     * diagonal reportaban ausencia con la altura EXACTA y claridad 0,9999 — tocando fuerte.
     *
     * Con `candidatesHz` declarado ese caso **no se da**: el modo rápido reengancha el objetivo
     * a la cuerda que suena y el motor la mide.
     *
     * @param candidatesHz los objetivos en Hz, **en orden de cuerda**. Vacío es legal y
     *   equivale a [analyzeTunerBuffer]: sin instrumento declarado.
     */
    fun analyzeTunerBufferWithCandidates(
        samples: FloatArray,
        channels: Int,
        sampleRate: Int,
        targetHz: Float,
        candidatesHz: FloatArray,
    ): FloatArray?
}
