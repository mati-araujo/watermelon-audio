package com.watermellonstudios.audio.api

/**
 * La mezcla PROPIA de la canción programada (REQ-055 S12, ADR-0013): ganancia, paneo y envío
 * por pista, el bus «Espacio» y el master. Contrato de la C API: sección 25 de
 * `watermelon_audio.h`; el diseño y las mediciones: `docs/dev/mezcla-y-master.md`.
 *
 * ## Cambio de contrato (2.24.0)
 *
 * Con la canción activa, el volumen, paneo, mute y envío a efectos del `TrackBuffer`
 * ([ILooperBridge]) **ya no alcanzan a las voces** y ninguna voz entra a la cadena de la escena:
 * la canción se gobierna sólo por esta mezcla (y la agenda de [ISongBridge]).
 *
 * ## Quién la implementa
 *
 * El puente vivo (`AudioNativeBridge.getInstance()`) y `OfflineRenderer`; ninguno cruza al otro.
 * iOS no la implementa en v1. No infla [ISongBridge] ni [ILooperBridge] a propósito.
 *
 * Los valores no finitos o fuera de rango fallan con `PARAMETER_OUT_OF_RANGE` y **no se
 * aplican**. Son stores atómicos: se pueden llamar con el motor sonando; el callback los suaviza.
 * La primera llamada de pista o de espacio crea el bus (en el hilo que llama) y vive hasta
 * destruir el motor. En un offline, lo fijado se aplica desde el frame 0 sin rampa.
 */
interface ISongMixBridge {
    /**
     * Ganancia lineal 0..2 (0 = mute, 2 = +6 dB), paneo −1..1 (equal-power, −3 dB al centro) y
     * envío 0..1 de la pista [track] (post-fader y post-paneo). [track] es una pista activa.
     */
    fun mixSetTrack(track: Int, gain: Float, pan: Float, send: Float): Result<Unit>

    fun mixSetSpace(space: SongSpace): Result<Unit>

    /** El master que usa el perfil [OutputProfile.MASTER]. */
    fun mixSetMaster(master: MasterParams): Result<Unit>

    /** `RAW` sólo en una instancia offline; en un motor con device da `INVALID_OPERATION`. */
    fun mixSetOutputProfile(profile: OutputProfile): Result<Unit>

    /**
     * Xruns que reportó el backend desde que abrió el stream (monótono; se refresca cada ~500
     * callbacks, así que va atrasado). 0 en una instancia offline y antes del init.
     */
    fun getXRunCount(): Int
}
