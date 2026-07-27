package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.IAudioNativeBridge
import com.watermellonstudios.audio.api.InternalWatermelonApi

/**
 * Platform-specific factory for obtaining the [IAudioNativeBridge] singleton.
 *
 * - Android: returns [AudioNativeBridge.getInstance()]
 * - iOS: returns the cinterop-based [IosAudioBridge]
 *
 * **Es la única puerta al puente nativo, y por eso es la que lleva la anotación.**
 * Marcar acá y no en `IAudioNativeBridge` es deliberado: la interfaz la implementan
 * y la reciben un montón de piezas internas —`AudioInputImpl`, `EffectManagerImpl`,
 * `StateSynchronizer`— y anotarla obligaría a salpicar `@OptIn` por todo el motor
 * sin agregar una sola garantía. Lo que hay que hacer explícito es **obtener** el
 * puente; sin esta función no hay forma de tener uno. Ver [InternalWatermelonApi].
 */
@InternalWatermelonApi
expect fun getAudioBridge(): IAudioNativeBridge
