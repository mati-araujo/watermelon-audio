package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.device.DevicePlatform
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/**
 * WA-1.2 / WA-3.3 — el actual iOS sobre `NSProcessInfo`.
 *
 * Corre en el simulador, así que `physicalMemory` y `activeProcessorCount` son los del
 * Mac anfitrión: acá no se puede afirmar nada sobre la gama del dispositivo. Lo que sí
 * se verifica es que `NSProcessInfo` se está leyendo de verdad —no que hay un stub
 * devolviendo ceros— y que la versión del OS sale del struct por valor, que es la
 * parte del cinterop fácil de romper en silencio.
 */
class IosDeviceCapabilitiesTest {

    @Test
    fun reportsTheIosPlatform() {
        assertEquals(DevicePlatform.IOS, currentDeviceCapabilities().platform)
    }

    /**
     * El deployment target son 15.0, así que cualquier host que pueda correr este test
     * está en 15 o más. Un `0` acá significaría que `useContents` no leyó el struct.
     */
    @Test
    fun readsTheOperatingSystemMajorVersion() {
        val apiLevel = currentDeviceCapabilities().apiLevel

        assertTrue(apiLevel >= 15, "apiLevel = $apiLevel")
    }

    @Test
    fun readsPhysicalMemoryAndCoreCount() {
        val current = currentDeviceCapabilities()

        assertTrue(current.totalRamMb > 0, "ram = ${current.totalRamMb} MB")
        assertTrue(current.cpuCoreCount >= 1, "cores = ${current.cpuCoreCount}")
    }

    /** Core Audio siempre ofrece el path; lo que se negocia es cuánto concede. */
    @Test
    fun coreAudioAlwaysOffersTheLowLatencyPath() {
        assertTrue(currentDeviceCapabilities().supportsLowLatencyAudio)
    }
}
