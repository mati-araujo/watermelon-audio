package com.watermellonstudios.audio.domain.looper

/**
 * La serie de pitch de una pista, offline, en frames del buffer (REQ-043, WV-3.2).
 *
 * Tres arrays paralelos del mismo largo: el punto `k` es `(frames[k], hz[k], confidence[k])`.
 * El contrato de cada campo —el eje, el origen, el hop, qué es un 0/0— vive en el KDoc de
 * [com.watermellonstudios.audio.api.ILooperBridge.looperAnalyzePitch]; acá sólo el tipo.
 *
 * **`size == 0` es "no hay dato"** (R-API-59): pista inactiva o sin contenido, región más
 * corta que una ventana, o hop inválido. Aun así [hopFrames] puede venir distinto de 0:
 * el motor lo calcula antes de mirar la pista, para que un consumidor pueda dimensionar.
 *
 * No es `data class` a propósito: `equals` sobre arrays compara identidad, y un `==`
 * entre dos series "iguales" que diera `false` sería una trampa para el test de
 * determinismo del consumidor. Compará los arrays con `contentEquals`.
 */
class PitchSeries(
    /** `round(hopMs · sr / 1000)`, redondeado una vez. 0 sólo si el hop pedido era inválido. */
    val hopFrames: Int,
    /** Frame ABSOLUTO del buffer, al CENTRO de la ventana analizada. Ascendente, paso [hopFrames]. */
    val frames: IntArray,
    /** Frecuencia en Hz; `0` exacto donde no hay altura. Nunca interpolada. */
    val hz: FloatArray,
    /** Claridad NSDF del pico elegido, 0..1; `0` exacto donde [hz] es 0. */
    val confidence: FloatArray,
) {
    init {
        require(frames.size == hz.size && hz.size == confidence.size) {
            "los tres arrays de PitchSeries van paralelos: frames=${frames.size} hz=${hz.size} " +
                "confidence=${confidence.size}"
        }
    }

    /** Cuántos puntos trae. `0` = no hay dato. */
    val size: Int get() = frames.size

    fun isEmpty(): Boolean = frames.isEmpty()
}

/**
 * La envolvente RMS de una pista, cruda y lineal `[0, 1]`, decimada (REQ-043, WV-3.1).
 *
 * El bin `k` cubre `[firstFrame + k·hopFrames, firstFrame + (k+1)·hopFrames)` en frames del
 * buffer. Contrato completo en
 * [com.watermellonstudios.audio.api.ILooperBridge.looperGetLevelEnvelope].
 *
 * **`size == 0` es "no hay dato"** (R-API-59), con las mismas causas que [PitchSeries]; y
 * también acá [hopFrames] puede venir aunque no haya bins. [firstFrame] sólo tiene sentido
 * con `size > 0`: sin bins vale `0` y no dice nada.
 */
class LevelEnvelope(
    /** `loopStart` de la pista al momento de analizar. `0` cuando no hay dato. */
    val firstFrame: Int,
    /** `round(sr / binsPerSecond)`, redondeado una vez. 0 sólo si `binsPerSecond` era inválido. */
    val hopFrames: Int,
    /** RMS lineal por bin, sobre mono `(L+R)/2`. Sin normalizar, sin dB, sin suavizar. */
    val rms: FloatArray,
) {
    /** Cuántos bins trae. `0` = no hay dato. */
    val size: Int get() = rms.size

    fun isEmpty(): Boolean = rms.isEmpty()
}
