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
 *   `P0 + offsets`. Todo `start` (también el reinicio) deja la agenda apagada, como en una
 *   instancia fresca: se publica DESPUÉS del `start` (una publicada antes se descarta).
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
     * `loopFrames <= 0`, `sampleRate <= 0`), e `INVALID_OPERATION` si el sample rate del motor
     * cambió mientras se decodificaba (el clip quedó al rate viejo y se descarta).
     *
     * Decodificar no frena al resto: [songSetSchedule], [songUnloadClip] y el cambio de sample
     * rate no esperan a una carga en curso (sí otra [songLoadClip]: se serializan).
     */
    fun songLoadClip(
        path: String,
        sampleRate: Int,
        regionStartFrame: Long,
        regionEndFrame: Long,
        loopFrames: Long,
    ): Result<Int>

    /**
     * `INVALID_OPERATION` si el clip no existe o lo referencia la agenda publicada. La memoria
     * se libera cuando el callback ya no puede estar leyéndolo; con el motor sin stream, al
     * instante (se verifica antes de liberar). Offline, en la próxima llamada de control tras un
     * bloque en el que ninguna cola ni la agenda adoptada lo lean: un `unload` + `load` sin render
     * en el medio, cerca del tope, puede dar `MEMORY_BUDGET` o `PARAMETER_OUT_OF_RANGE`.
     */
    fun songUnloadClip(clipId: Int): Result<Unit>

    /**
     * Publica la agenda ENTERA. `clips.size == sectionStarts.size * MAX_TRACKS`: una fila de
     * `MAX_TRACKS` ids por sección, `SILENCE` = pista en silencio. [sectionStarts] vacío
     * apaga la canción (y el looper vuelve a sus tomas). Habilita el looper.
     *
     * Apagar funde la canción a silencio en `F`, pero las tomas del looper vuelven de golpe,
     * sin fundido de entrada: con una toma sonando en una pista, el paso de la canción a la
     * toma puede clickear. Studio no mezcla tomas con canción.
     *
     * `PARAMETER_OUT_OF_RANGE`: largos que no cierran, más de `MAX_SECTIONS`, starts no
     * estrictamente crecientes o negativos, sección más corta que dos fundidos, `endFrame` menor
     * que el último start + 2 fundidos (o `Long.MAX_VALUE`), clip en una pista inactiva. `INVALID_OPERATION`: clip inexistente, de otro `loopFrames`, o grabación.
     */
    fun songSetSchedule(
        loopFrames: Long,
        sectionStarts: LongArray,
        clips: IntArray,
        endFrame: Long,
    ): Result<Unit>

    /**
     * Índice, **dentro de la agenda ADOPTADA por el callback**, de la sección que contiene el
     * último frame del bloque más reciente; `-1` inactiva, antes del primer start o pasado el
     * `endFrame`. Puede ir un bloque atrás de la agenda recién publicada: tras reprogramar, el
     * índice sigue siendo el de la agenda vieja hasta que el callback adopta la nueva.
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
