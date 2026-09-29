package com.watermellonstudios.audio.domain.input

import com.watermellonstudios.audio.domain.error.NativeBridgeException

/**
 * Lo que un pedido de captura logró, tal como estaba al volver (REQ-045, D3).
 *
 * Espeja `WmaCaptureOutcome` de la C API, y son **tres** valores y no un `Boolean` por la
 * razón que explica `BackendManager::CaptureOutcome`: una reapertura del stream ya no
 * termina antes que la llamada, así que colapsar [PENDING] en [NOT_LIVE] haría
 * indistinguible *"todavía abriendo"* de *"el usuario negó el micrófono"* — la única
 * distinción para la que existe todo el camino de entrada.
 *
 * Hasta el 2026-09-28 este valor **se descartaba tres veces seguidas**:
 * `BackendManager::setFullDuplexEnabled` lo tiraba, `wma_set_usb_streaming_mode` era `void`,
 * y el envoltorio de Kotlin no tenía nada que devolver. O sea que pedir captura y que no
 * pasara nada era indistinguible de pedirla y que pasara.
 */
enum class CaptureOutcome {
    /** Está entregando frames ahora mismo. */
    LIVE,

    /** No está, y no hay nada en vuelo que lo vaya a cambiar. */
    NOT_LIVE,

    /** Hay una reapertura corriendo; se sondea con `isCaptureLive`. */
    PENDING,
    ;

    companion object {
        /**
         * Traduce el `int` de `wma_set_usb_streaming_mode`: `>= 0` es un outcome, `< 0` un
         * `WmaResult`.
         *
         * 🔴 **Vive en `commonMain` y la usan las DOS plataformas.** El mismo reparto
         * signo/valor transcripto dos veces es exactamente cómo Android y iOS terminaron
         * contestando distinto a la misma pregunta en D1 — el defecto que este REQ vino a
         * borrar. Una sola definición no lo puede hacer.
         */
        fun fromNativeCode(code: Int, operation: String): Result<CaptureOutcome> = when (code) {
            0 -> Result.success(NOT_LIVE)
            1 -> Result.success(LIVE)
            2 -> Result.success(PENDING)
            else -> Result.failure(NativeBridgeException.fromCode(code, operation))
        }
    }
}
