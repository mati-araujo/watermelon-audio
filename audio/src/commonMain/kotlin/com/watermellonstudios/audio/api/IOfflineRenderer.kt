package com.watermellonstudios.audio.api

/**
 * Una instancia de **render offline** del motor, independiente del motor en vivo (REQ-055 S2,
 * ADR-0008), tal como la ve un consumidor: la canción programada ([ISongBridge]), su mezcla
 * ([ISongMixBridge]) y los taps ([IOfflineTapRenderer]) más el ciclo de vida del render.
 *
 * Se obtiene con `OfflineRendererFactory.create()` (Android y desktop; iOS no la implementa en v1).
 * Cerrada, toda operación que devuelve `Result` falla con `EngineNotInitialized` sin cruzar al
 * nativo. El contrato de determinismo, los hilos y el orden de la canción están en el KDoc de la
 * implementación (`docs/dev/render-offline.md`, `docs/dev/mezcla-y-master.md`).
 *
 * El orden de un export: [setLooperCapabilities] → [start] → perfil RAW (`mixSetOutputProfile`) →
 * cargas → agenda → [renderTaps] por bloque → [close].
 */
interface IOfflineRenderer : ISongBridge, ISongMixBridge, IOfflineTapRenderer, AutoCloseable {
    /**
     * Arranca (o re-arranca) un render, reseteando la cadena. [maxBlockFrames] 1..4096. Cambiar el
     * sample rate invalida los clips ya cargados y apaga la agenda; vuelve el perfil a `PROTECTED`.
     */
    fun start(sampleRate: Int, maxBlockFrames: Int): Result<Unit>

    /** Renderiza [frames] frames de la salida del motor en [output] (estéreo intercalado, `frames * 2` floats). */
    fun render(output: FloatArray, frames: Int): Result<Unit>

    /** Detiene el render. Idempotente. */
    fun stop(): Result<Unit>

    /**
     * El techo del looper de esta instancia: el presupuesto de memoria de los clips y las pistas
     * activas. Una instancia nace con 48 MiB y 8 pistas. **0 deja el valor como estaba**;
     * [budgetBytes] fuera de `0..64 GiB` (en armeabi-v7a y x86 el techo es `SIZE_MAX`),
     * [maxTracks] fuera de `0..16` o menor que la pista más alta de la agenda de la canción
     * publicada, falla con `PARAMETER_OUT_OF_RANGE` sin aplicar ninguno. Se llama **antes** de cargar clips: sólo
     * afecta a las cargas siguientes (bajarlo no libera lo ya cargado) y sobrevive a [start].
     */
    fun setLooperCapabilities(budgetBytes: Long, maxTracks: Int): Result<Unit>
}
