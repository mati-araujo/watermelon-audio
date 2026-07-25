package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.device.DeviceCapabilities

/**
 * Capacidades del dispositivo donde corre el proceso (WA-1.2).
 *
 * - **Android**: `/proc/meminfo` + `Runtime` + `Build.VERSION.SDK_INT`. Sin `Context`,
 *   así que se pierde la señal de `ActivityManager.isLowRamDevice` — para tenerla hay
 *   una sobrecarga `deviceCapabilities(context)` en androidMain.
 * - **iOS**: `NSProcessInfo`.
 *
 * El resultado se calcula una vez por proceso y se cachea: RAM, núcleos y versión del
 * OS no cambian mientras la app vive, y esto se llama desde el valor por defecto de
 * [AudioEngineFactory.create].
 */
expect fun currentDeviceCapabilities(): DeviceCapabilities
