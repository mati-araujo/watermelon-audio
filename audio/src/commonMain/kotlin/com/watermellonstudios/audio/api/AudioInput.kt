package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.input.InputMetering
import com.watermellonstudios.audio.domain.input.InputSource
import kotlinx.coroutines.flow.Flow

/**
 * El camino de entrada del motor: capturar, medir y monitorear.
 *
 * ## Por qué es una interfaz aparte y no métodos de [AudioEngine]
 *
 * Mismo criterio que [IEffectManager]: un subsistema con su propio ciclo de vida
 * —se abre, se mide, se suelta— y que la mayoría de los consumidores no usa. Un
 * sintetizador que sólo genera sonido no necesita nada de acá.
 *
 * ## Qué desbloquea
 *
 * Hasta WA-5.5 esta superficie existía entera en la C API y entera en el JNI, y
 * **no llegaba a `commonMain`**: iOS no tenía forma de tocar el camino de
 * entrada desde Kotlin, aunque `CoreAudioBackend` capturara desde la etapa 2 del
 * input path. Ese camino se escribió a ciegas y sigue sin validarse contra
 * audio real; esto es lo que hace posible validarlo.
 *
 * ## Uso
 *
 * ```kotlin
 * val input = AudioInputFactory.create()
 *
 * if (!input.start()) {
 *     // permiso denegado, o no hay device de entrada
 * }
 * input.monitoringEnabled = true
 *
 * input.meteringFlow().collect { m ->
 *     meterBar(m.peakLinear)
 *     if (m.isClipping) flashRed()
 * }
 *
 * input.release()
 * ```
 *
 * ## Threading
 *
 * Todo acá se llama desde threads de UI o de coroutine. El thread de audio
 * **jamás** entra a Kotlin (D6: el GC de Kotlin/Native no es RT-safe), así que
 * el medidor sale por polling y no por callback desde el render — por eso
 * [meteringFlow] es un poll y no un push.
 */
interface AudioInput {

    /**
     * Abre el stream de entrada. **No bloquea.**
     *
     * En el camino donde el backend carga la entrada (Apple, USB) hay que reabrir
     * el stream, y eso corre en un thread propio: reabrir puede tardar cientos de
     * ms y en iOS se midió colgándose adentro de `AVAudioSession`. Bloquear al que
     * llama —que en una app con UI es el main thread— es un freeze o un watchdog
     * kill.
     *
     * @return `false` **sólo si el pedido se rechazó de entrada**. `true` quiere
     *   decir "está viva o se está abriendo", que no es lo mismo:
     *
     *   - [isRunning] — está entregando frames
     *   - [isStarting] — todavía se está abriendo
     *
     * **El caso que más importa sigue siendo el permiso de micrófono denegado**, y
     * ahora se ve así: [isStarting] pasa a false y [isRunning] se queda en false.
     * Sin excepción y sin crash. Un consumidor que concluya "denegado" con
     * [isStarting] todavía en true va a acusar al motor de algo que no pasó.
     */
    fun start(): Boolean

    /** Cierra el stream. El nodo de entrada sigue existiendo — ver [release]. */
    fun stop()

    val isRunning: Boolean

    /**
     * Si un [start] todavía está abriendo el stream.
     *
     * Es la mitad que falta para distinguir "todavía no" de "no". Mientras sea
     * true, un [isRunning] en false no es una negativa.
     */
    val isStarting: Boolean

    /**
     * De dónde toma audio. **No todas las fuentes existen en todas las
     * plataformas** — ver [InputSource].
     */
    var source: InputSource

    /** Ganancia de entrada, en dB. */
    var gainDb: Float

    /** Si la entrada se escucha por la salida. Ojo con el feedback sin auriculares. */
    var monitoringEnabled: Boolean

    /** Volumen del monitoreo, 0..1. */
    var monitoringVolume: Float

    var noiseGateEnabled: Boolean

    /**
     * Umbral de la compuerta de ruido, en dB.
     *
     * **Es una función y no una propiedad porque no se puede leer**: no hay
     * getter en ninguna capa —ni en la C API, ni en el JNI—, así que un `var`
     * tendría que inventar el valor de vuelta o cachear el último escrito y
     * mentir en cuanto algo más lo cambie. Un setter suelto dice la verdad:
     * esto se escribe, no se consulta.
     */
    fun setNoiseGateThresholdDb(thresholdDb: Float)

    /** Latencia de entrada que reporta el backend, en ms. */
    val latencyMs: Float

    /**
     * Una lectura completa del medidor.
     *
     * @return `null` si no hay nodo de entrada. **No es lo mismo que silencio**:
     *   dibujar ceros en ese caso es mostrar una medición que nadie tomó.
     */
    fun metering(): InputMetering?

    /**
     * El medidor como flujo, muestreado cada [intervalMs].
     *
     * No emite mientras [metering] devuelva `null`, por la misma razón de
     * arriba: es mejor un medidor que no se mueve que uno que miente.
     *
     * @param intervalMs período de muestreo. El default (33 ms ≈ 30 Hz) alcanza
     *   para que un medidor se vea fluido; bajarlo no mejora la percepción y sí
     *   suma cruces de frontera.
     */
    fun meteringFlow(intervalMs: Long = DEFAULT_METERING_INTERVAL_MS): Flow<InputMetering>

    /**
     * Suelta el nodo de entrada entero.
     *
     * Distinto de [stop]: eso cierra el stream y deja el nodo listo para volver
     * a arrancar; esto libera sus buffers.
     */
    fun release()

    companion object {
        const val DEFAULT_METERING_INTERVAL_MS: Long = 33
    }
}
