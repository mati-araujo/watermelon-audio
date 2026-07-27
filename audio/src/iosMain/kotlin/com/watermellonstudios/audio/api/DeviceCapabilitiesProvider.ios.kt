package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.device.DeviceCapabilities
import com.watermellonstudios.audio.domain.device.DeviceCapabilitiesSnapshot
import com.watermellonstudios.audio.domain.device.DevicePlatform
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.useContents
import platform.Foundation.NSProcessInfo

/**
 * Menos de 3 GB de RAM en iOS es un A11 o anterior (iPhone 8 y menores, SE de 1ª y 2ª).
 *
 * El umbral no es el de Android por una razón concreta: el piso de iOS es más alto.
 * El deployment target son 15.0, así que el dispositivo más flojo que puede correr
 * esto es un iPhone 6s/SE1 con 2 GB. 3 GB deja del lado "gama baja" justo a la
 * generación donde una cadena de efectos completa empieza a no entrar.
 */
private const val LOW_RAM_THRESHOLD_MB = 3072L

/**
 * 2 núcleos o menos.
 *
 * Mucho más bajo que el umbral de Android a propósito: los núcleos de Apple no son
 * comparables uno a uno con los de un big.LITTLE de gama baja, y un hexa-core A-series
 * mueve el motor sin problema. Acá el que discrimina de verdad es el umbral de RAM;
 * los núcleos sólo atrapan a los A9/A10 de doble núcleo.
 */
private const val LOW_CORE_COUNT = 2

private val cached: DeviceCapabilities by lazy { readCapabilities() }

/**
 * Actual iOS, sobre `NSProcessInfo`.
 *
 * Se eligió `NSProcessInfo` y **no** `UIDevice` a propósito: da todo lo que hace falta
 * (memoria física, núcleos activos, versión del OS), no obliga a arrastrar UIKit dentro
 * de una librería de audio, y no exige estar en el main thread.
 *
 * **Ojo en el simulador**: `physicalMemory` y `activeProcessorCount` reportan los del
 * Mac anfitrión, no los de un iPhone. Así que en el simulador esto casi siempre dice
 * "gama alta". Es correcto —el proceso *tiene* esos recursos— pero no sirve para
 * probar el path de gama baja: para eso hay que construir un
 * [DeviceCapabilitiesSnapshot] a mano.
 */
actual fun currentDeviceCapabilities(): DeviceCapabilities = cached

@OptIn(ExperimentalForeignApi::class)
private fun readCapabilities(): DeviceCapabilities {
    val processInfo = NSProcessInfo.processInfo
    val ramMb = (processInfo.physicalMemory / (1024uL * 1024uL)).toLong()
    val cores = processInfo.activeProcessorCount.toInt()
    // `operatingSystemVersion` es un NSOperatingSystemVersion por valor; cinterop lo
    // entrega como CValue, así que hay que entrar con useContents para leer el campo.
    val majorVersion = processInfo.operatingSystemVersion.useContents { majorVersion.toInt() }

    return DeviceCapabilitiesSnapshot(
        platform = DevicePlatform.IOS,
        apiLevel = majorVersion,
        totalRamMb = ramMb,
        cpuCoreCount = cores,
        // Core Audio siempre expone el path de baja latencia; lo que se negocia es
        // cuánto concede, y eso lo pide AudioSessionManager con
        // preferredIOBufferDuration (WA-3.4).
        supportsLowLatencyAudio = true,
        isLowEndDevice = (ramMb in 1 until LOW_RAM_THRESHOLD_MB) || cores <= LOW_CORE_COUNT,
    )
}
