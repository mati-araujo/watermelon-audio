package com.watermellonstudios.audio.internal.bridge

import kotlin.test.AfterTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * `IArpeggiatorBridge` sobre cinterop, corriendo en el simulador.
 *
 * Los 19 miembros existían en la C API desde siempre y en Kotlin sólo del lado JNI;
 * esta suite es lo que separa "compila" de "llega al motor".
 *
 * ## Lo que estos tests SÍ prueban, y lo que no
 *
 * **No arranca el motor**, por el mismo motivo que [IosAudioBridgeTest]: abrir un
 * stream de CoreAudio vuelve la suite flaky por algo ajeno al bridge.
 *
 * Eso tiene una consecuencia que conviene tener escrita, porque si no el próximo que
 * lea esto va a pensar que faltan aserciones: **con el motor parado, tres de los
 * cuatro getters del arpegiador no pueden distinguir una implementación buena de una
 * rota.** `regenerateArpPattern()` no reconstruye nada — sólo levanta
 * `mNeedsPatternRebuild`, y el patrón se arma dentro de `process()`, en el thread de
 * audio. Sin ese thread, `mPatternLengthForUI` se queda en su inicializador.
 *
 * Por eso el round-trip de verdad es el de [isArpEnabled], que lee un
 * `std::atomic<bool>` sin intermediarios. Los otros tres se verifican por su valor de
 * reposo, que es una afirmación más débil pero real: descarta que el binding devuelva
 * basura o que la convención de llamada esté mal.
 *
 * El comportamiento con el motor andando es WA-4.3, en device.
 */
class IosArpeggiatorBridgeTest {

    private val bridge = IosAudioBridge()

    @AfterTest
    fun cleanup() {
        bridge.setArpEnabled(false)
        bridge.setArpTouchActive(false)
        bridge.setArpLatch(false)
    }

    /**
     * El round-trip que prueba que el binding llega al motor.
     *
     * Es el único de esta suite que puede fallar por una implementación rota y pasar
     * por una buena, y por eso existe [isArpEnabled] en la interfaz.
     */
    @Test
    fun enabledRoundTripsThroughTheEngine() {
        assertFalse(bridge.isArpEnabled(), "el arpegiador debería arrancar apagado")

        bridge.setArpEnabled(true)
        assertTrue(bridge.isArpEnabled(), "setArpEnabled(true) no llegó al motor")

        bridge.setArpEnabled(false)
        assertFalse(bridge.isArpEnabled(), "setArpEnabled(false) no llegó al motor")
    }

    /**
     * Con el motor parado los tres getters de UI se quedan en reposo.
     *
     * La aserción no es "el arpegiador anda" sino "el binding no devuelve basura":
     * un tipo de retorno mal declarado en cinterop asomaría acá como un entero
     * cualquiera, no como un 0 limpio.
     */
    @Test
    fun theUiGettersRestAtZeroWithoutAnAudioThread() {
        bridge.setArpScaleIntervals(intArrayOf(0, 4, 7))
        bridge.setArpOctaveRange(2)
        bridge.setArpPattern(0)
        bridge.regenerateArpPattern()

        assertEquals(0, bridge.getArpTotalSteps(), "sin process() no hay patrón que contar")
        assertEquals(0, bridge.getArpCurrentStep(), "sin process() la secuencia no avanza")
        assertFalse(bridge.isArpGateOpen(), "sin process() el gate no se abre")
    }

    /**
     * Los 14 setters, uno detrás de otro, en sus bordes.
     *
     * No hay qué leer de vuelta, así que lo que este test cubre es lo que sí puede
     * romperse sin readback: que ninguna llamada crashee ni corrompa el motor. La
     * prueba de que no lo corrompió es que el round-trip de [isArpEnabled] sigue
     * andando **después** de todas — si alguna hubiera pisado memoria del secuenciador,
     * ese `std::atomic` sería lo primero en irse.
     */
    @Test
    fun everySetterIsCallableAndLeavesTheEngineIntact() {
        bridge.setArpPattern(0)
        bridge.setArpSubdivision(0.25f)
        bridge.setArpOctaveRange(1)
        bridge.setArpGateLength(0.5f)
        bridge.setArpSwing(0.0f)
        bridge.setArpLatch(true)
        bridge.setArpVelocity(0.8f)
        bridge.setArpVelocityVariation(0.2f)
        bridge.setArpProbability(1.0f)
        bridge.setArpRatchet(true)
        bridge.setArpTouchActive(true)
        bridge.setArpBaseFrequency(440.0f)
        bridge.setArpScaleIntervals(intArrayOf(0, 2, 4, 5, 7, 9, 11))
        bridge.regenerateArpPattern()

        bridge.setArpEnabled(true)
        assertTrue(bridge.isArpEnabled(), "el motor quedó inconsistente tras los 14 setters")
    }

    /**
     * Un `IntArray` vacío es válido y significa "sin escala".
     *
     * Importa porque el borde se maneja en Kotlin, no en C: `addressOf(0)` sobre un
     * array de largo cero no está definido, así que [IosAudioBridge] corta antes y
     * pasa `null`. Del otro lado `wma_arp_set_scale_intervals` tiene el guard
     * `if (!intervals || count <= 0) return;`, o sea que el resultado es el mismo
     * no-op que ve Android — que llega ahí por el camino del JNI.
     *
     * Sin esta rama, esta llamada sería una lectura fuera de rango.
     */
    @Test
    fun anEmptyScaleIsANoOpAndNotACrash() {
        bridge.setArpScaleIntervals(intArrayOf(0, 4, 7))
        bridge.setArpScaleIntervals(intArrayOf())

        bridge.setArpEnabled(true)
        assertTrue(bridge.isArpEnabled(), "la escala vacía dejó el motor inconsistente")
    }

    /** 12 es el techo que impone `ArpSequencer::setScaleIntervals`; pasarse no puede romper. */
    @Test
    fun aScaleLongerThanTheEngineCeilingIsClamped() {
        bridge.setArpScaleIntervals(IntArray(24) { it })

        bridge.setArpEnabled(true)
        assertTrue(bridge.isArpEnabled(), "una escala de 24 intervalos dejó el motor inconsistente")
    }
}
