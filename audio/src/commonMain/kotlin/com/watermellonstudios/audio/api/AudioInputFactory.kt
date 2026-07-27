package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.internal.bridge.getAudioBridge
import com.watermellonstudios.audio.internal.input.AudioInputImpl

/**
 * Factory de [AudioInput].
 *
 * Mismo patrón que [EffectManagerFactory]: la única puerta pública al
 * subsistema, sin exponer el bridge.
 *
 * ```kotlin
 * val input = AudioInputFactory.create()
 * ```
 *
 * @see AudioInput
 */
object AudioInputFactory {

    /**
     * Devuelve la vista del camino de entrada del motor.
     *
     * **No crea nada**: el nodo de entrada lo crea el motor la primera vez que
     * hace falta, y el bridge es un singleton. Llamar a esto dos veces da dos
     * vistas del mismo camino, no dos entradas — que es lo que corresponde,
     * porque el device de captura es uno solo.
     */
    // El motor es el implementador del puente, no un consumidor: las factories
    // publicas se construyen encima de el. Ver [InternalWatermelonApi].
    @OptIn(InternalWatermelonApi::class)
    fun create(): AudioInput = AudioInputImpl(getAudioBridge())
}
