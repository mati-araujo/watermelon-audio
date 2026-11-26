package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.IMasterProcessor
import com.watermellonstudios.audio.api.MasterParams
import com.watermellonstudios.audio.domain.error.NativeBridgeException
import com.watermellonstudios.audio.domain.error.NativeErrorCode

/**
 * El procesador de master por handle (REQ-055 S12, ADR-0013 §2). Posee un id nativo que lo
 * crea [create] y lo libera [close] **una sola vez** (idempotente). El id vive en un registro del
 * lado nativo: uno cerrado, desconocido o ya destruido es `NOT_INITIALIZED` y nunca toca memoria
 * (el mismo modelo que [OfflineRenderer]). Una instancia sirve a un stream: las operaciones se
 * serializan del lado nativo, pero el estado del limitador no se comparte entre streams.
 */
class MasterProcessor private constructor(
    private val bridge: AudioNativeBridge,
    initialId: Long,
) : IMasterProcessor {

    private val lock = Any()

    @Volatile
    private var id: Long = initialId

    companion object {
        /** Frames máximos por llamada a [process]. */
        const val MAX_FRAMES = 1 shl 20

        /** Crea un procesador a [sampleRate] (8000..192000). Falla si el rate no sirve o no hay memoria. */
        fun create(sampleRate: Int): Result<MasterProcessor> = create(AudioNativeBridge.getInstance(), sampleRate)

        internal fun create(bridge: AudioNativeBridge, sampleRate: Int): Result<MasterProcessor> {
            val id = bridge.masterCreate(sampleRate)
            return if (id == 0L) {
                Result.failure(
                    NativeBridgeException.fromCode(NativeErrorCode.PARAMETER_OUT_OF_RANGE.code, "MasterProcessor.create($sampleRate)"),
                )
            } else {
                Result.success(MasterProcessor(bridge, id))
            }
        }
    }

    /** `true` hasta el primer [close]. */
    val isOpen: Boolean get() = id != 0L

    /** El id nativo; 0 una vez cerrado. Sólo para los tests del cruce. */
    internal val nativeId: Long get() = id

    override val latencyFrames: Int get() = id.let { if (it == 0L) 0 else bridge.masterLatency(it) }

    override fun configure(params: MasterParams): Result<Unit> =
        call("configure") { bridge.masterConfigure(it, params.toNativeArray()) }

    override fun reset(): Result<Unit> = call("reset") { bridge.masterReset(it) }

    override fun process(buffer: FloatArray, frames: Int): Result<Unit> {
        if (frames <= 0 || frames > MAX_FRAMES || buffer.size < frames * 2) {
            return Result.failure(
                NativeBridgeException.fromCode(NativeErrorCode.PARAMETER_OUT_OF_RANGE.code, "MasterProcessor.process"),
            )
        }
        return call("process") { bridge.masterProcess(it, buffer, frames) }
    }

    override fun close() {
        val current = synchronized(lock) {
            val c = id
            id = 0L
            c
        }
        if (current != 0L) bridge.masterDestroy(current)
    }

    private inline fun call(operation: String, op: (Long) -> Int): Result<Unit> {
        val current = id
        if (current == 0L) {
            return Result.failure(
                NativeBridgeException.fromCode(NativeErrorCode.ENGINE_NOT_INITIALIZED.code, "MasterProcessor.$operation: cerrado"),
            )
        }
        val code = op(current)
        return if (code == NativeErrorCode.SUCCESS.code) {
            Result.success(Unit)
        } else {
            Result.failure(NativeBridgeException.fromCode(code, "MasterProcessor.$operation"))
        }
    }
}
