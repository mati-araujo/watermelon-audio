package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.api.config.AudioEngineConfig
import com.watermellonstudios.audio.domain.device.DeviceCapabilities
import com.watermellonstudios.audio.domain.device.DeviceCapabilitiesSnapshot
import com.watermellonstudios.audio.domain.device.DevicePlatform
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * WA-1.2 — la abstracción de capacidades y la política que se deriva de ella.
 *
 * La política (`tunedFor`) se prueba con dispositivos **hipotéticos** construidos a
 * mano: es la única forma de cubrir el caso de gama baja, porque ni el JVM de los
 * unit tests ni el simulador de iOS son un teléfono flojo. La detección real
 * (`currentDeviceCapabilities`) sólo se puede afirmar por invariantes estructurales,
 * que es lo que hace el último bloque.
 */
class DeviceCapabilitiesTest {

    private fun caps(
        lowEnd: Boolean = false,
        lowLatency: Boolean = true,
    ): DeviceCapabilities = DeviceCapabilitiesSnapshot(
        platform = DevicePlatform.ANDROID,
        apiLevel = 34,
        totalRamMb = if (lowEnd) 1536 else 8192,
        cpuCoreCount = if (lowEnd) 4 else 8,
        supportsLowLatencyAudio = lowLatency,
        isLowEndDevice = lowEnd,
    )

    // --- Política: tunedFor -------------------------------------------------

    @Test
    fun lowEndDeviceGetsAShorterEffectChain() {
        val tuned = AudioEngineConfig.tunedFor(caps(lowEnd = true))

        assertEquals(AudioEngineConfig.LOW_END_MAX_EFFECTS, tuned.maxEffects)
    }

    @Test
    fun normalDeviceKeepsTheDefaultEffectChain() {
        val tuned = AudioEngineConfig.tunedFor(caps(lowEnd = false))

        assertEquals(AudioEngineConfig.DEFAULT.maxEffects, tuned.maxEffects)
    }

    /**
     * El recorte es un techo, no un valor fijo: un consumidor que pidió menos efectos
     * que el techo de gama baja no debería terminar con **más** de los que pidió.
     */
    @Test
    fun tuningNeverRaisesAnExplicitlyLowerEffectLimit() {
        val base = AudioEngineConfig.builder().maxEffects(3).build()

        val tuned = AudioEngineConfig.tunedFor(caps(lowEnd = true), base)

        assertEquals(3, tuned.maxEffects)
    }

    @Test
    fun lowLatencyIsDisabledWhenThePlatformDoesNotOfferIt() {
        val tuned = AudioEngineConfig.tunedFor(caps(lowLatency = false))

        assertFalse(tuned.enableLowLatency)
    }

    @Test
    fun lowLatencySurvivesOnAPlatformThatOffersIt() {
        val tuned = AudioEngineConfig.tunedFor(caps(lowLatency = true))

        assertTrue(tuned.enableLowLatency)
    }

    /**
     * Lo que no es una capacidad del hardware es decisión del consumidor, y el ajuste
     * por dispositivo no tiene por qué opinar. Si esto se rompe, alguien agregó un
     * campo a la política que no le corresponde.
     */
    @Test
    fun tuningLeavesConsumerDecisionsAlone() {
        val base = AudioEngineConfig.builder()
            .sampleRate(44100)
            .bufferSize(256)
            .defaultFadeMs(120)
            .build()

        val tuned = AudioEngineConfig.tunedFor(caps(lowEnd = true), base)

        assertEquals(base.sampleRate, tuned.sampleRate)
        assertEquals(base.bufferSize, tuned.bufferSize)
        assertEquals(base.defaultFadeMs, tuned.defaultFadeMs)
        assertEquals(base.defaultOscillator, tuned.defaultOscillator)
        assertEquals(base.logger, tuned.logger)
        assertEquals(base.analyticsListener, tuned.analyticsListener)
    }

    // --- Detección real: invariantes que valen en cualquier host ------------

    /**
     * No se puede afirmar el valor —depende de la máquina donde corra— pero sí que
     * ninguna rama devuelva algo imposible. Un `cpuCoreCount` en 0 rompería cualquier
     * heurística que divida por él.
     */
    @Test
    fun theCurrentDeviceReportsPlausibleValues() {
        val current = currentDeviceCapabilities()

        assertTrue(current.cpuCoreCount >= 1, "cores = ${current.cpuCoreCount}")
        assertTrue(current.totalRamMb >= 0, "ram = ${current.totalRamMb}")
        assertTrue(current.apiLevel >= 0, "apiLevel = ${current.apiLevel}")
    }

    /** Se cachea por proceso: dos llamadas tienen que dar exactamente lo mismo. */
    @Test
    fun theCurrentDeviceIsStableAcrossCalls() {
        assertEquals(currentDeviceCapabilities(), currentDeviceCapabilities())
    }
}
