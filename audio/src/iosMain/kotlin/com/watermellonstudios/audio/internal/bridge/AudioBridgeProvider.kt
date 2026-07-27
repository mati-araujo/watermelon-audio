package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.IAudioNativeBridge
import com.watermellonstudios.audio.api.InternalWatermelonApi

/**
 * iOS actual — [IosAudioBridge] sobre cinterop (WA-3.2 / WA-3.3).
 *
 * Hasta WA-3.2 esto lanzaba `NotImplementedError`: los targets iOS existían sólo
 * para publicar `commonMain` como metadata KMP (gate G1), y cualquier cosa que
 * llegara al motor de audio fallaba fuerte en vez de producir silencio en
 * silencio. Eso ya no hace falta — el puente existe.
 *
 * Instancia única y perezosa, igual que `AudioNativeBridge.getInstance()` en
 * Android: el motor nativo es un recurso de proceso, no algo de lo que convenga
 * tener dos. `lazy` en Kotlin/Native es thread-safe por defecto (`SYNCHRONIZED`).
 *
 * No se destruye: vive lo que vive el proceso, que es lo mismo que hace el lado
 * Android. Si alguna vez hace falta un teardown explícito —para tests de
 * integración, por ejemplo— va acá, no en el bridge.
 */
private val bridge: IAudioNativeBridge by lazy { IosAudioBridge() }

@InternalWatermelonApi
actual fun getAudioBridge(): IAudioNativeBridge = bridge
