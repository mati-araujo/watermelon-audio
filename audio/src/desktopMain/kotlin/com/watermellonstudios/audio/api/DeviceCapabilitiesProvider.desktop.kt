package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.device.DeviceCapabilities

/**
 * Desktop: todavía no hay detección (REQ-055 S1 sólo hace compilar el target).
 * `DevicePlatform` no tiene un valor para desktop, y responder ANDROID o IOS sería
 * mentir; agregarlo es un cambio de API pública que decide la etapa que use
 * `AudioEngine` en desktop. Hasta entonces falla explícito.
 */
actual fun currentDeviceCapabilities(): DeviceCapabilities =
    throw UnsupportedOperationException(
        "currentDeviceCapabilities() no está implementado en desktop todavía (REQ-055)"
    )
