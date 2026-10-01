package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.domain.error.NativeBridgeException
import kotlinx.coroutines.delay
import kotlinx.coroutines.withTimeoutOrNull

/**
 * REQ-050 S2 (D8, D11) — **parar con la variante `suspend` devuelve éxito recién con el motor
 * nativo en Stopped.**
 *
 * `AudioEngine::stopWithFade` arma el fade y vuelve; un worker para el motor un fade + ~60 ms
 * después. El `success` que devolvía el bridge significaba entonces "el motor aceptó", y quien
 * reconfiguraba enseguida —`setUseBackendManager(true)` para pasar a USB— lo encontraba todavía
 * corriendo: el nativo lo rechazaba con un LOGE y nada más (MINI-041 #4).
 *
 * Lo usan los bridges de Android y de iOS en `stopEngine` y `stopEngineWithFade`, dentro del
 * mutex de LIFECYCLE y en `Dispatchers.Default`: sondea, no bloquea un hilo. Las variantes
 * `*Sync` (deprecadas) NO esperan, a propósito (D11): bloquear el hilo que llama un fade entero
 * le congelaba la UI a NoisyPad. Para ellas "Unit" sigue significando "aceptado".
 */
internal object EngineStopWait {

    /** `EngineState::Stopped` de `core/AudioEngine.h`. */
    private const val STOPPED = 0

    /** Cada cuánto se pregunta. Es un sondeo de control, no del thread de audio. */
    private const val POLL_MS = 5L

    /**
     * Lo que el nativo puede tardar en cerrar DESPUÉS del fade, en el peor caso:
     * - el worker de `stopWithFade` duerme `fade + 50 ms` en tramos de 10 ms;
     * - `stop()` cierra el stream: en Oboe, `AudioStream::stop()` espera el cambio de estado
     *   hasta 2000 ms por defecto; `LibusbBackend::stop()` joinea sus threads;
     * - después espera los callbacks activos hasta 500 ms (camino de `BackendManager`) o
     *   1000 ms (Oboe directo).
     * 4000 ms cubren eso con margen. El techo es para no colgar el LIFECYCLE para siempre, no
     * una promesa de que el motor no pare después: un `Timeout` dice "no paró dentro del
     * techo", no "no va a parar".
     */
    const val SETTLE_MARGIN_MS = 4_000L

    /** El techo para un stop con [fadeMs] de fade (negativo = sin fade, el default nativo). */
    fun ceilingFor(fadeMs: Int): Long = fadeMs.coerceAtLeast(0) + SETTLE_MARGIN_MS

    /**
     * Espera a que [engineState] diga Stopped, con [ceilingMs] de techo.
     *
     * @return `success` con el motor parado; `failure(NativeBridgeException.Timeout)` si venció
     *   el techo. Nunca éxito sin haberlo visto parado.
     */
    suspend fun awaitStopped(operation: String, ceilingMs: Long, engineState: () -> Int): Result<Unit> {
        val stopped = withTimeoutOrNull(ceilingMs) {
            while (engineState() != STOPPED) delay(POLL_MS)
            true
        }
        return if (stopped == true) {
            Result.success(Unit)
        } else {
            Result.failure(NativeBridgeException.Timeout(operation, ceilingMs))
        }
    }

    /**
     * [awaitStopped] sólo si el nativo ACEPTÓ el stop: un `failure` (sin motor, por ejemplo)
     * se devuelve tal cual, sin esperar a un Stopped que no va a llegar por este pedido.
     */
    suspend fun afterAccepted(
        accepted: Result<Unit>,
        operation: String,
        ceilingMs: Long,
        engineState: () -> Int,
    ): Result<Unit> = if (accepted.isFailure) accepted else awaitStopped(operation, ceilingMs, engineState)
}
