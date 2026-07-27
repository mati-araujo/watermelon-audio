package com.watermellonstudios.audio.api

/**
 * La sección 12 de `watermelon_audio.h` —el camino de entrada— como contrato
 * propio.
 *
 * ## Por qué está separada de [IAudioNativeBridge]
 *
 * Mismo motivo que [IEffectStateProvider] y [IEffectStateWriter], que ya estaban
 * partidas así: [IAudioNativeBridge] tiene más de cien métodos, y un consumidor
 * que sólo necesita la entrada no debería depender de todos. En particular, un
 * test que quiera verificar la lógica de entrada tendría que implementar los
 * cien para escribir un fake — que es exactamente la clase de fricción que hace
 * que la lógica se quede sin test.
 *
 * ## Historia
 *
 * Hasta WA-5.5 esto no existía en `commonMain` en ninguna forma: la superficie
 * estaba entera en la C API y entera en `AudioNativeBridge` (Android), y iOS no
 * tenía cómo tocarla desde Kotlin aunque `CoreAudioBackend` capturara desde la
 * etapa 2 del input path.
 *
 * Los nombres son los que ya usaba Android, para que del lado JNI el cambio
 * fuera agregar `override` y nada más.
 */
interface IInputBridge {

    /** @return false si no se pudo abrir el stream (permiso denegado, sin device). */
    fun startInputStreamSync(): Boolean
    fun stopInputStreamSync()
    fun isInputStreamRunning(): Boolean

    fun setInputSourceSync(source: Int)
    fun getInputSource(): Int

    /** Ganancia de entrada en dB. */
    fun setInputGain(gainDb: Float)
    fun getInputGain(): Float

    fun setNoiseGateEnabled(enabled: Boolean)
    fun isNoiseGateEnabled(): Boolean

    /** Sin getter: no lo hay en ninguna capa, tampoco en la C API. */
    fun setNoiseGateThreshold(thresholdDb: Float)
    fun isNoiseGateOpen(): Boolean

    /** Pico del canal en dBFS. */
    fun getInputLevel(channel: Int): Float

    /** Pico del canal, lineal 0..1. */
    fun getInputLevelLinear(channel: Int): Float

    fun isInputClipping(): Boolean
    fun getInputLatencyMs(): Float

    /**
     * Los siete valores del medidor de una sola vez.
     *
     * @return null si no hay nodo de entrada. **Null y "todo en cero" no son lo
     *   mismo**: la C API deja el buffer intacto cuando falla, justamente para
     *   que nadie lea ceros como si fueran una medición, y esta firma preserva
     *   esa distinción en vez de aplanarla.
     */
    fun getInputMeteringSnapshot(): FloatArray?

    fun setMonitoringEnabledSync(enabled: Boolean)
    fun isMonitoringEnabled(): Boolean
    fun setMonitoringVolume(volume: Float)
    fun getMonitoringVolume(): Float

    fun releaseInputNodeSync()
}
