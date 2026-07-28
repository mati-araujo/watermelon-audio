package com.watermellonstudios.audio.api

/**
 * El arpegiador de la sección 9 de `watermelon_audio.h`, como contrato propio.
 *
 * ## Por qué es una interfaz aparte y no más miembros de [IAudioNativeBridge]
 *
 * El mismo motivo que ya partió [IInputBridge], [IEffectStateProvider] y
 * [IEffectStateWriter]: [IAudioNativeBridge] pasa los cien métodos, y un consumidor
 * que sólo secuencia notas no tiene por qué depender del ciclo de vida del motor ni
 * de la cadena de efectos. En particular un fake de test: quien quiera probar la
 * lógica de arpegio no debería tener que implementar cien métodos para escribirlo.
 *
 * ## Por qué existe ahora
 *
 * Hasta acá los 18 miembros vivían **sólo** en el `AudioNativeBridge` de Android.
 * La C API los tiene desde siempre (`wma_arp_*`, 19 funciones) y cinterop ya genera
 * sus bindings, así que el hueco era de fachada Kotlin: iOS no tenía cómo nombrarlos
 * aunque el motor los ejecutara. El consumidor que lo pide es NoisyPad, cuyo
 * `AudioEngineStateManager` los llama y no puede bajar a `commonMain` sin esto.
 *
 * Los nombres son **los que ya usaba Android**, para que del lado JNI el cambio sea
 * agregar `override` y nada más — la misma regla que siguió [IInputBridge].
 *
 * ## Por qué [isArpEnabled] entra aunque ningún consumidor de producción la llame
 *
 * Por la regla de opt-in del repo iba a quedar afuera. Entra igual, y **no** por
 * simetría con el setter: es la única lectura de esta interfaz que no depende del
 * thread de audio, o sea la única forma de demostrar que los bindings llegan al motor.
 *
 * Los otros tres getters no sirven para eso. [regenerateArpPattern] sólo levanta un
 * flag y el patrón se reconstruye dentro de `process()`, así que con el motor parado
 * [getArpTotalSteps], [getArpCurrentStep] e [isArpGateOpen] devuelven su valor de
 * reposo **pase lo que pase** — y una implementación rota devolvería lo mismo. Se
 * midió: tras `setArpScaleIntervals` + `setArpOctaveRange` + `regenerateArpPattern`,
 * `totalSteps` sigue en 0. `mEnabled`, en cambio, es un `std::atomic<bool>` que
 * `wma_arp_is_enabled` lee directo.
 *
 * Sin esto los 18 miembros compilarían y nadie podría probar que hacen algo. El
 * consumidor es el test — que es un consumidor.
 *
 * ## Regla RT
 *
 * Casi todo esto está marcado `RT-safe` en el header: del otro lado hay `std::atomic`
 * y las implementaciones **no toman locks**, igual que los parámetros de tiempo real
 * de [IAudioNativeBridge]. El costo de un mutex por gesto no se justifica.
 */
interface IArpeggiatorBridge {

    // ==================== ESTADO ====================

    /** Prende o apaga el secuenciador. `RT-safe`. */
    fun setArpEnabled(enabled: Boolean)

    /**
     * Si el secuenciador está prendido. Lee un `std::atomic<bool>` directo, **sin
     * pasar por el thread de audio** — ver el porqué de su existencia en el KDoc de
     * la interfaz.
     */
    fun isArpEnabled(): Boolean

    /** Suelta el latch y vuelve a generar el patrón desde la nota actual. */
    fun regenerateArpPattern()

    // ==================== PATRÓN ====================

    /** Id del patrón; corresponde a `ArpPattern.id` del dominio del consumidor. */
    fun setArpPattern(patternId: Int)

    /** Beats por paso — 0.5 = corchea, 0.25 = semicorchea. `RT-safe`. */
    fun setArpSubdivision(beatsPerStep: Float)

    /** Cuántas octavas recorre el patrón antes de repetirse. `RT-safe`. */
    fun setArpOctaveRange(octaves: Int)

    /**
     * Intervalos de la escala sobre la que se arpegia, en semitonos.
     *
     * **No es `RT-safe`** — a diferencia del resto de esta interfaz: copia el array
     * del lado nativo. Llamarlo por frame de gesto es un error de uso.
     */
    fun setArpScaleIntervals(intervals: IntArray)

    // ==================== EXPRESIÓN ====================

    /** Fracción del paso que la nota suena, 0..1. `RT-safe`. */
    fun setArpGateLength(gate: Float)

    /** Desplazamiento de los pasos pares, 0 = recto. `RT-safe`. */
    fun setArpSwing(swing: Float)

    /** Mantiene las notas sonando cuando se suelta el toque. `RT-safe`. */
    fun setArpLatch(latch: Boolean)

    /** Velocidad base de cada nota, 0..1. `RT-safe`. */
    fun setArpVelocity(velocity: Float)

    /** Cuánto varía la velocidad entre pasos, 0 = todas iguales. `RT-safe`. */
    fun setArpVelocityVariation(variation: Float)

    /** Probabilidad de que un paso suene, 0..1. 1 = suenan todos. `RT-safe`. */
    fun setArpProbability(probability: Float)

    /** Repite el paso actual en subdivisiones más rápidas mientras esté activo. `RT-safe`. */
    fun setArpRatchet(active: Boolean)

    // ==================== ENTRADA ====================

    /** Si hay un dedo apoyado: el arpegio corre mientras esto sea `true` (o con latch). `RT-safe`. */
    fun setArpTouchActive(active: Boolean)

    /** La frecuencia raíz sobre la que se construyen los pasos. `RT-safe`. */
    fun setArpBaseFrequency(frequency: Float)

    // ==================== LECTURA (para la UI) ====================

    /**
     * En qué paso va la secuencia. Se lee por polling desde la UI.
     *
     * Es polling y no callback **por la regla RT (D6)**: el thread de audio no entra
     * a Kotlin nunca, así que el paso actual se pregunta, no se avisa.
     */
    fun getArpCurrentStep(): Int

    /** Cuántos pasos tiene el patrón vigente. 0 si no hay patrón generado. */
    fun getArpTotalSteps(): Int

    /** Si el gate está abierto ahora mismo — para que la UI pueda pulsar al ritmo. */
    fun isArpGateOpen(): Boolean
}
