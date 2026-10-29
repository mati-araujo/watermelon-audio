package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.internal.bridge.OfflineRenderer

/**
 * La puerta pública al render offline (REQ-055 S12b): una instancia fresca por trabajo, sin nombrar
 * `audio.internal.bridge`. Android y desktop (jvmShared); iOS no la implementa en v1.
 *
 * Cada [create] es una instancia independiente del motor en vivo y de las demás; el que la crea la
 * cierra ([IOfflineRenderer.close]). Falla con `MemoryAllocationFailed` si no hay memoria.
 */
object OfflineRendererFactory {
    fun create(): Result<IOfflineRenderer> = OfflineRenderer.create()
}
