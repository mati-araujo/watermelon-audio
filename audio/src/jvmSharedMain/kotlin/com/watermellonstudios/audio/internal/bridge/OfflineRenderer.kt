package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.IOfflineRenderer
import com.watermellonstudios.audio.api.IOfflineTapRenderer
import com.watermellonstudios.audio.api.ISongBridge
import com.watermellonstudios.audio.api.ISongMixBridge
import com.watermellonstudios.audio.api.MasterParams
import com.watermellonstudios.audio.api.OutputProfile
import com.watermellonstudios.audio.api.SongSpace
import com.watermellonstudios.audio.domain.error.NativeBridgeException
import com.watermellonstudios.audio.domain.error.NativeErrorCode

/**
 * Una instancia de **render offline** del motor, independiente del motor en vivo
 * (REQ-055 S2, ADR-0008). La consume Studio (NoisyPad desktop) para exportar la
 * canción y los stems y re-renderizar tomas mientras la escucha sigue sonando.
 *
 * Posee un handle nativo opaco: lo crea [create], lo libera [close] **una sola vez**
 * (idempotente) y, después de cerrado, toda operación falla con
 * [NativeBridgeException.EngineNotInitialized] **sin cruzar al nativo**. Del otro lado
 * el handle es un id en un registro, así que un handle destruido tampoco puede
 * alcanzar memoria muerta aunque alguien lo pase a mano.
 *
 * ## El contrato (ADR-0008 §2)
 *
 * Misma configuración + la misma secuencia de llamadas en los mismos frames + el
 * mismo tamaño de bloque ⇒ **el mismo buffer bit a bit**, en una instancia fresca o
 * reusada: [start] resetea el estado de la cadena (colas de reverb y delay, LFOs y
 * sus generadores aleatorios, fases, voces, engines, arpegiador). Entre tamaños de
 * bloque distintos también es bit a bit, salvo que cambien a mitad del render el
 * synth volume o un parámetro de engine (se aplican una vez por bloque): un export
 * fija su bloque. Números: `docs/dev/render-offline.md`.
 *
 * ## Hilos
 *
 * Toda operación de una instancia (start, render, stop y los setters) se serializa del
 * lado nativo: un setter llamado desde otro hilo durante un [render] espera a que el
 * bloque termine (en vivo eso no hace falta porque el callback es corto y RT; acá no).
 * Dos instancias distintas corren en paralelo. Nada de esto toca el motor en vivo ni
 * sus locks. No es RT: no se llama desde un callback de audio.
 *
 * Los setters que en la C API son `void` (tipo y parámetro de engine, volumen, x/y,
 * notas) sólo pueden fallar por el handle: la C API no informa un valor rechazado.
 *
 * ## La canción programada ([ISongBridge], REQ-055 S11)
 *
 * Mismas operaciones que el vivo, sobre el handle. El orden importa: [start] **primero** y
 * [songLoadClip] después (el start puede cambiar el sample rate y eso invalida los clips); el
 * ancla `P0` es [songGetPlayFrame] tras el start. El start apaga la agenda (también al reiniciar,
 * como una instancia fresca): [songSetSchedule] va después. Con la instancia cerrada, las que devuelven
 * `Result` fallan con `EngineNotInitialized` sin cruzar, y las lecturas dan `-1` / `0` / `-1`.
 *
 * ## La mezcla de la canción y los taps ([ISongMixBridge], [IOfflineTapRenderer], REQ-055 S12)
 *
 * Mismos setters que el vivo, sobre el handle, entre renders. Un export fija el perfil
 * [OutputProfile.RAW] **después** del [start] (parar o reiniciar vuelve a `PROTECTED`) y renderiza
 * con [renderTaps]: UN [FloatArray] `[premezcla | FX | pista 0 | …]` por bloque. [getXRunCount] da
 * 0: una instancia offline no tiene stream.
 *
 * La superficie es la mínima que pide S2 (motor, SoundFont y preset, cadena de
 * efectos, parámetros, gestos x/y y notas). Lo que falte se agrega como un
 * pasamanos más a la `wma_*` que ya toma un `WmaEngine*`, no como una copia del
 * bridge global.
 */
class OfflineRenderer private constructor(
    private val bridge: AudioNativeBridge,
    initialHandle: Long,
) : IOfflineRenderer {

    private val lock = Any()

    @Volatile
    private var handle: Long = initialHandle

    companion object {
        /** El bloque más grande que acepta [start] (el scratch de la cadena de efectos). */
        const val MAX_BLOCK_FRAMES = 4096

        /** Crea una instancia offline. Falla con [NativeBridgeException.MemoryAllocationFailed]. */
        fun create(): Result<OfflineRenderer> = create(AudioNativeBridge.getInstance())

        internal fun create(bridge: AudioNativeBridge): Result<OfflineRenderer> {
            val handle = bridge.offlineCreate()
            return if (handle == 0L) {
                Result.failure(NativeBridgeException.fromCode(NativeErrorCode.MEMORY_ALLOCATION_FAILED.code, "OfflineRenderer.create"))
            } else {
                Result.success(OfflineRenderer(bridge, handle))
            }
        }
    }

    /** `true` hasta el primer [close]. */
    val isOpen: Boolean get() = handle != 0L

    /** El id nativo; 0 una vez cerrado. Sólo para diagnóstico y para los tests del cruce. */
    internal val nativeHandle: Long get() = handle

    /** Arranca (o re-arranca) un render, reseteando la cadena. Bloque máximo 1..4096. */
    override fun start(sampleRate: Int, maxBlockFrames: Int): Result<Unit> =
        call("start") { bridge.offlineStart(it, sampleRate, maxBlockFrames) }

    /**
     * Renderiza [frames] frames en [output] (estéreo intercalado: `frames * 2` floats,
     * se sobreescriben). [frames] no puede pasar el `maxBlockFrames` de [start].
     */
    override fun render(output: FloatArray, frames: Int): Result<Unit> {
        if (frames <= 0 || frames > MAX_BLOCK_FRAMES || output.size < frames * 2) {
            return Result.failure(
                NativeBridgeException.fromCode(NativeErrorCode.PARAMETER_OUT_OF_RANGE.code, "OfflineRenderer.render"),
            )
        }
        return call("render") { bridge.offlineRender(it, output, frames) }
    }

    /** Detiene el render. Idempotente. */
    override fun stop(): Result<Unit> = call("stop") { bridge.offlineStop(it) }

    fun setEngineType(type: Int): Result<Unit> = call("setEngineType") { bridge.offlineSetEngineType(it, type) }

    fun setEngineParameter(paramId: Int, value: Float): Result<Unit> =
        call("setEngineParameter") { bridge.offlineSetEngineParameter(it, paramId, value) }

    fun setMasterVolume(volume: Float): Result<Unit> =
        call("setMasterVolume") { bridge.offlineSetMasterVolume(it, volume) }

    fun setXY(x: Float, y: Float): Result<Unit> = call("setXY") { bridge.offlineSetXY(it, x, y) }

    fun setFrequencyAndAmplitude(frequency: Float, amplitude: Float): Result<Unit> =
        call("setFrequencyAndAmplitude") { bridge.offlineSetFrequencyAndAmplitude(it, frequency, amplitude) }

    fun loadSoundFont(path: String): Result<Unit> =
        call("loadSoundFont") { bridge.offlineLoadSoundFontFromPath(it, path) }

    fun setSoundFontPreset(presetIndex: Int): Result<Unit> =
        call("setSoundFontPreset") { bridge.offlineSetSoundFontPreset(it, presetIndex) }

    fun soundFontNoteOn(touchId: Int, midiNote: Int, velocity: Float): Result<Unit> =
        call("soundFontNoteOn") { bridge.offlineSfNoteOn(it, touchId, midiNote, velocity) }

    fun soundFontNoteOff(touchId: Int): Result<Unit> =
        call("soundFontNoteOff") { bridge.offlineSfNoteOff(it, touchId) }

    /** Agrega un efecto al final de la cadena; devuelve su índice. */
    fun addEffect(typeId: Int): Result<Int> = withHandle("addEffect") { h ->
        val code = bridge.offlineAddEffect(h, typeId)
        if (code >= 0) Result.success(code) else Result.failure(NativeBridgeException.fromCode(code, "OfflineRenderer.addEffect"))
    }

    fun clearAllEffects(): Result<Unit> = call("clearAllEffects") { bridge.offlineClearAllEffects(it) }

    fun setEffectParameter(index: Int, paramId: Int, value: Float): Result<Unit> =
        call("setEffectParameter") { bridge.offlineSetEffectParameter(it, index, paramId, value) }

    /**
     * El techo del looper de esta instancia (REQ-055 S12b): el presupuesto de memoria de los clips
     * de la canción y las pistas activas. Una instancia nace con 48 MiB y 8 pistas (el default del
     * looper, no el que el consumidor use en vivo): un export cuyos clips pasen de eso falla con
     * [NativeBridgeException.MemoryBudgetExceeded] y una pista >= 8 con `PARAMETER_OUT_OF_RANGE`
     * en [mixSetTrack].
     *
     * Como en la C API, **0 deja el valor como estaba**. A diferencia de ella, lo que no cabe no se
     * recorta: [budgetBytes] fuera de `0..64 GiB` (en armeabi-v7a y x86 el techo es `SIZE_MAX`),
     * [maxTracks] fuera de `0..16` o menor que la pista más alta de la agenda de la canción
     * publicada (el looper no mira las voces de la canción y la apagaría en silencio) falla con
     * `PARAMETER_OUT_OF_RANGE` sin aplicar ninguno de los dos.
     *
     * Cuándo: **antes de [songLoadClip]**. El presupuesto se mide en cada carga contra lo ya
     * cargado, así que valerlo después sólo afecta a las cargas siguientes; bajarlo con clips
     * adentro no libera nada (la próxima carga falla si ya se pasó) y [maxTracks] no baja de
     * la pista más alta de la agenda. [start] no lo toca: sobrevive a reiniciar el render.
     */
    override fun setLooperCapabilities(budgetBytes: Long, maxTracks: Int): Result<Unit> =
        call("setLooperCapabilities") { bridge.offlineLooperSetCapabilities(it, budgetBytes, maxTracks) }

    override fun songLoadClip(
        path: String,
        sampleRate: Int,
        regionStartFrame: Long,
        regionEndFrame: Long,
        loopFrames: Long,
    ): Result<Int> = withHandle("songLoadClip") { h ->
        val outId = IntArray(1) { ISongBridge.SILENCE }
        val code = bridge.offlineSongLoadClip(h, path, sampleRate, regionStartFrame, regionEndFrame, loopFrames, outId)
        if (code == NativeErrorCode.SUCCESS.code) {
            Result.success(outId[0])
        } else {
            Result.failure(NativeBridgeException.fromCode(code, "OfflineRenderer.songLoadClip($path)"))
        }
    }

    override fun songUnloadClip(clipId: Int): Result<Unit> =
        call("songUnloadClip") { bridge.offlineSongUnloadClip(it, clipId) }

    override fun songSetSchedule(
        loopFrames: Long,
        sectionStarts: LongArray,
        clips: IntArray,
        endFrame: Long,
    ): Result<Unit> = call("songSetSchedule") {
        bridge.offlineSongSetSchedule(it, loopFrames, sectionStarts, clips, endFrame)
    }

    override fun songGetSection(): Int = handle.let { if (it == 0L) -1 else bridge.offlineSongGetSection(it) }

    override fun songGetLateAdoptions(): Long =
        handle.let { if (it == 0L) 0L else bridge.offlineSongGetLateAdoptions(it) }

    override fun songGetPlayFrame(): Long =
        handle.let { if (it == 0L) -1L else bridge.offlineSongGetPlayFrame(it) }

    override fun mixSetTrack(track: Int, gain: Float, pan: Float, send: Float): Result<Unit> =
        call("mixSetTrack") { bridge.offlineMixSetTrack(it, track, gain, pan, send) }

    override fun mixSetSpace(space: SongSpace): Result<Unit> =
        call("mixSetSpace") { bridge.offlineMixSetSpace(it, space.toNativeArray()) }

    override fun mixSetMaster(master: MasterParams): Result<Unit> =
        call("mixSetMaster") { bridge.offlineMixSetMaster(it, master.toNativeArray()) }

    override fun mixSetOutputProfile(profile: OutputProfile): Result<Unit> =
        call("mixSetOutputProfile") { bridge.offlineMixSetOutputProfile(it, profile.code) }

    override fun getXRunCount(): Int = 0

    override fun renderTaps(buffer: FloatArray, frames: Int, trackCount: Int): Result<Unit> {
        if (frames <= 0 || frames > MAX_BLOCK_FRAMES || trackCount < 0 || trackCount > ISongBridge.MAX_TRACKS ||
            buffer.size.toLong() < frames.toLong() * 2L * (2L + trackCount.toLong())
        ) {
            return Result.failure(
                NativeBridgeException.fromCode(NativeErrorCode.PARAMETER_OUT_OF_RANGE.code, "OfflineRenderer.renderTaps"),
            )
        }
        return call("renderTaps") { bridge.offlineRenderTaps(it, buffer, frames, trackCount) }
    }

    /**
     * Libera la instancia nativa. Idempotente: la segunda llamada no cruza. Un render
     * que otro hilo tenga en curso termina sobre la instancia viva y recién ahí se
     * destruye (lo garantiza el registro nativo).
     */
    override fun close() {
        val h = synchronized(lock) {
            val current = handle
            handle = 0L
            current
        }
        if (h != 0L) {
            bridge.offlineDestroy(h)
        }
    }

    private inline fun call(operation: String, op: (Long) -> Int): Result<Unit> = withHandle(operation) { h ->
        val code = op(h)
        if (code == NativeErrorCode.SUCCESS.code) {
            Result.success(Unit)
        } else {
            Result.failure(NativeBridgeException.fromCode(code, "OfflineRenderer.$operation"))
        }
    }

    private inline fun <T> withHandle(operation: String, op: (Long) -> Result<T>): Result<T> {
        val h = handle
        if (h == 0L) {
            return Result.failure(
                NativeBridgeException.fromCode(NativeErrorCode.ENGINE_NOT_INITIALIZED.code, "OfflineRenderer.$operation: cerrado"),
            )
        }
        return op(h)
    }
}
