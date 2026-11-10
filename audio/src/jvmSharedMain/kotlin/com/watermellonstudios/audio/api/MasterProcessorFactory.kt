package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.internal.bridge.MasterProcessor

/**
 * La puerta pública al procesador de master sin motor (REQ-055 S12b), sin nombrar
 * `audio.internal.bridge`. Android y desktop (jvmShared); iOS no la implementa en v1.
 *
 * Un procesador por stream; el que lo crea lo cierra ([IMasterProcessor.close]).
 */
object MasterProcessorFactory {
    /** Falla con `ParameterOutOfRange` si [sampleRate] no está en 8000..192000, o si no hay memoria. */
    fun create(sampleRate: Int): Result<IMasterProcessor> = MasterProcessor.create(sampleRate)
}
