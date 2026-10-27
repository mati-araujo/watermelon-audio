package com.watermellonstudios.audio.api

/**
 * La canción programada por frame (REQ-055 S11): una agenda de secciones, cada una con un
 * clip por pista, que el callback mezcla EN LUGAR de las tomas del looper. Contrato de la
 * C API: `wma_song_*` en `watermelon_audio.h`; motor y fundidos: `docs/dev/cancion-programada.md`.
 *
 * ## El tiempo
 *
 * Los frames de [songSetSchedule] son ABSOLUTOS, del play frame del Transport
 * ([songGetPlayFrame]): un entero de 64 bits monótono que **nunca se reinicia**. La sección
 * `k` rige en `[start_k, start_{k+1})` y la última hasta `endFrame`; antes del primer start
 * y desde `endFrame` las pistas callan (no caen a las tomas del looper mientras la agenda
 * esté publicada).
 *
 * ## Quién la implementa
 *
 * El puente vivo (`AudioNativeBridge.getInstance()`) y `OfflineRenderer`: ninguno cruza al
 * otro. iOS no la implementa en v1.
 *
 * - **Offline: primero `start`, después [songLoadClip].** El start puede cambiar el sample
 *   rate del motor, y eso invalida los clips cargados (están resampleados al viejo) y apaga
 *   la agenda. El ancla `P0` es [songGetPlayFrame] tras el `start`; la agenda va en
 *   `P0 + offsets`.
 * - **Vivo:** un cambio de sample rate del motor también invalida los clips.
 * - Mientras hay una agenda publicada, grabar o armar el looper no hace nada, y
 *   [songSetSchedule] da `INVALID_OPERATION` si hay una grabación armada o en curso.
 *
 * Los errores son `Result.failure(NativeBridgeException.fromCode(código, …))`, como el resto
 * del bridge. No es RT: no se llama desde un callback de audio.
 */
interface ISongBridge {
    /**
     * Decodifica y resamplea [path] a [sampleRate] (el del motor) fuera del hilo de audio,
     * hornea el recorte `[regionStartFrame, regionEndFrame)` (frames YA a [sampleRate];
     * `regionEndFrame <= 0` = toda la toma) y lo normaliza a [loopFrames]. Devuelve el id del
     * clip (`0 until MAX_CLIPS`).
     *
     * Falla con `MEMORY_BUDGET` (los clips cuentan contra el presupuesto del looper),
     * `UNSUPPORTED_FORMAT`, `IO`, `MEMORY` o `PARAMETER_OUT_OF_RANGE` (pool lleno,
     * `loopFrames <= 0`, `sampleRate <= 0`).
     */
    fun songLoadClip(
        path: String,
        sampleRate: Int,
        regionStartFrame: Long,
        regionEndFrame: Long,
        loopFrames: Long,
    ): Result<Int>

    /** `INVALID_OPERATION` si el clip no existe o lo referencia la agenda publicada. */
    fun songUnloadClip(clipId: Int): Result<Unit>

    /**
     * Publica la agenda ENTERA. `clips.size == sectionStarts.size * MAX_TRACKS`: una fila de
     * `MAX_TRACKS` ids por sección, `SILENCE` = pista en silencio. [sectionStarts] vacío
     * apaga la canción (y el looper vuelve a sus tomas). Habilita el looper.
     *
     * `PARAMETER_OUT_OF_RANGE`: largos que no cierran, más de `MAX_SECTIONS`, starts no
     * estrictamente crecientes, sección más corta que dos fundidos, clip en una pista
     * inactiva. `INVALID_OPERATION`: clip inexistente, de otro `loopFrames`, o grabación.
     */
    fun songSetSchedule(
        loopFrames: Long,
        sectionStarts: LongArray,
        clips: IntArray,
        endFrame: Long,
    ): Result<Unit>

    /**
     * Índice, **dentro de la agenda publicada**, de la sección que contiene el último frame
     * del bloque más reciente; `-1` inactiva, antes del primer start o pasado el `endFrame`.
     */
    fun songGetSection(): Int

    /** Adopciones que cambiaron lo que sonaba en el frame de adopción (cada una funde `F`). */
    fun songGetLateAdoptions(): Long

    /** El play frame absoluto del Transport: el reloj de la agenda. */
    fun songGetPlayFrame(): Long

    companion object {
        const val MAX_TRACKS = 16
        const val SILENCE = -1
        const val MAX_SECTIONS = 1024
        const val MAX_CLIPS = 64
    }
}
