package com.watermellonstudios.audio.api

/**
 * El render offline con los taps de la mezcla de la canción, en UNA pasada (REQ-055 S12,
 * ADR-0013 §5). Sólo lo implementa `OfflineRenderer`.
 *
 * El buffer es UN [FloatArray] con las regiones una detrás de otra, cada una `frames * 2` floats
 * (estéreo intercalado): `[premezcla | retorno del bus FX | pista 0 | … | pista trackCount−1]`.
 * La premezcla es la salida del motor (con el perfil [OutputProfile.RAW]: sin master ni
 * protección); las pistas, el seco post-fader y post-paneo, y los tres con el master del looper.
 * Con el volumen del synth en 0: `premezcla == FX + Σ pistas` hasta el redondeo de float.
 *
 * Mismo contrato de determinismo que `render`: misma configuración + mismas llamadas + mismo
 * bloque ⇒ mismos bits en cada región.
 */
interface IOfflineTapRenderer {
    /**
     * Renderiza [frames] frames (1..4096, hasta el `maxBlockFrames` del `start`) con [trackCount]
     * pistas (0..pistas activas). Un [buffer] de menos de `frames * 2 * (2 + trackCount)` floats
     * falla con `PARAMETER_OUT_OF_RANGE` **antes** de avanzar el motor y sin escribir.
     */
    fun renderTaps(buffer: FloatArray, frames: Int, trackCount: Int): Result<Unit>
}
