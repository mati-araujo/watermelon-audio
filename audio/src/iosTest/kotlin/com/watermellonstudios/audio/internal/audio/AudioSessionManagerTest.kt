package com.watermellonstudios.audio.internal.audio

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import platform.AVFAudio.AVAudioSessionInterruptionNotification
import platform.AVFAudio.AVAudioSessionInterruptionOptionKey
import platform.AVFAudio.AVAudioSessionInterruptionTypeKey
import platform.AVFAudio.AVAudioSessionRouteChangeNotification
import platform.AVFAudio.AVAudioSessionRouteChangeReasonKey
import platform.Foundation.NSNotification
import platform.Foundation.NSNotificationCenter
import platform.Foundation.NSNumber
import platform.Foundation.notificationWithName
import platform.Foundation.numberWithUnsignedLong
import kotlin.test.AfterTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * WA-3.4 — contrato de [AudioSessionManager] en el simulador.
 *
 * Los tests están partidos en dos por una razón concreta:
 *
 * 1. **Parseo del `userInfo`** (la mayoría). Es la parte frágil —tipos y opciones
 *    son enteros mágicos del ABI de iOS— y se prueba llamando a los parsers
 *    directamente. Determinista y rápido.
 * 2. **Cableado del `Flow`** (uno solo). `NSNotificationCenter` entrega
 *    **sincrónicamente**, así que una notificación posteada antes de que el
 *    observer esté registrado se pierde sin dejar rastro. Ese test reintenta con
 *    timeout en vez de asumir un orden que no controla.
 *
 * La primera versión de este archivo probaba todo por el `Flow` bajo `runTest`, y
 * los cinco tests colgaban hasta el timeout: tiempo virtual + entrega síncrona no
 * se llevan bien.
 */
class AudioSessionManagerTest {

    private val manager = AudioSessionManager()

    @AfterTest
    fun cleanup() {
        manager.deactivate()
    }

    // ==================== Configuración ====================

    @Test
    fun configureSucceedsForPlaybackOnly() {
        val result = manager.configure(enableInput = false)
        assertTrue(result.isSuccess, "configure falló: ${result.exceptionOrNull()?.message}")
    }

    @Test
    fun configureSucceedsForPlayAndRecord() {
        val result = manager.configure(enableInput = true)
        assertTrue(result.isSuccess, "configure falló: ${result.exceptionOrNull()?.message}")
    }

    /**
     * Lo que se pide no es lo que se obtiene: acá sólo se verifica que el sistema
     * informe valores **plausibles** tras activar, no que respete la preferencia.
     */
    @Test
    fun activatedSessionReportsUsableValues() {
        manager.configure(enableInput = false)
        val activated = manager.activate()
        assertTrue(activated.isSuccess, "activate falló: ${activated.exceptionOrNull()?.message}")

        assertTrue(
            manager.actualSampleRate > 0.0,
            "sample rate no plausible: ${manager.actualSampleRate}",
        )
        assertTrue(
            manager.actualIOBufferDuration > 0.0,
            "buffer duration no plausible: ${manager.actualIOBufferDuration}",
        )
    }

    // ==================== Parseo del userInfo ====================

    @Test
    fun interruptionBeganIsParsed() {
        val event = manager.parseInterruption(interruptionNotification(INTERRUPTION_BEGAN))
        assertEquals(AudioSessionEvent.InterruptionBegan, event)
    }

    /**
     * `shouldResume` es el bit que decide si el motor vuelve a sonar. Si se leyera
     * mal, la app arrancaría audio arriba de una llamada en curso.
     */
    @Test
    fun interruptionEndedCarriesShouldResume() {
        val event = manager.parseInterruption(
            interruptionNotification(INTERRUPTION_ENDED, OPTION_SHOULD_RESUME),
        )
        assertEquals(AudioSessionEvent.InterruptionEnded(shouldResume = true), event)
    }

    @Test
    fun interruptionEndedWithoutResumeOptionDoesNotSuggestResuming() {
        val event = manager.parseInterruption(interruptionNotification(INTERRUPTION_ENDED, 0uL))
        assertEquals(AudioSessionEvent.InterruptionEnded(shouldResume = false), event)
    }

    /**
     * Un tipo desconocido se descarta en vez de mapearse a algo. Adivinar acá
     * puede reanudar el motor arriba de una llamada.
     */
    @Test
    fun unknownInterruptionTypeIsDiscarded() {
        assertNull(manager.parseInterruption(interruptionNotification(99uL)))
    }

    @Test
    fun interruptionWithoutTypeKeyIsDiscarded() {
        val bare = NSNotification.Companion.notificationWithName(
            aName = AVAudioSessionInterruptionNotification,
            `object` = null,
            userInfo = null,
        )
        assertNull(manager.parseInterruption(bare))
    }

    /**
     * "Desenchufaron los auriculares" — el caso donde la convención de iOS es
     * pausar. Si se mapeara a otro motivo, la música saldría de golpe por el
     * parlante.
     */
    @Test
    fun oldDeviceUnavailableIsParsed() {
        assertEquals(
            RouteChangeReason.OldDeviceUnavailable,
            manager.parseRouteChangeReason(routeChangeNotification(2uL)),
        )
    }

    @Test
    fun newDeviceAvailableIsParsed() {
        assertEquals(
            RouteChangeReason.NewDeviceAvailable,
            manager.parseRouteChangeReason(routeChangeNotification(1uL)),
        )
    }

    @Test
    fun unknownRouteChangeReasonDegradesToUnknown() {
        assertEquals(
            RouteChangeReason.Unknown,
            manager.parseRouteChangeReason(routeChangeNotification(999uL)),
        )
    }

    @Test
    fun routeChangeWithoutReasonKeyDegradesToUnknown() {
        val bare = NSNotification.Companion.notificationWithName(
            aName = AVAudioSessionRouteChangeNotification,
            `object` = null,
            userInfo = null,
        )
        assertEquals(RouteChangeReason.Unknown, manager.parseRouteChangeReason(bare))
    }

    // ==================== Cableado del Flow ====================

    /**
     * Que el observer quede efectivamente registrado y el evento llegue al `Flow`.
     *
     * Reintenta el post porque no hay forma de observar desde afuera el momento en
     * que `callbackFlow` registra el observer, y `NSNotificationCenter` entrega
     * sincrónicamente: una notificación posteada un instante antes se pierde.
     */
    @Test
    fun theFlowDeliversRealNotifications() = runBlocking {
        val received = withTimeoutOrNull(10_000) {
            coroutineScope {
                val collector = async(Dispatchers.Default) { manager.events.first() }
                val poster = launch(Dispatchers.Default) {
                    while (collector.isActive) {
                        NSNotificationCenter.defaultCenter.postNotificationName(
                            aName = AVAudioSessionRouteChangeNotification,
                            `object` = null,
                            userInfo = mapOf<Any?, Any?>(
                                AVAudioSessionRouteChangeReasonKey to
                                    NSNumber.numberWithUnsignedLong(1uL),
                            ),
                        )
                        delay(50)
                    }
                }
                val event = collector.await()
                poster.cancel()
                event
            }
        }

        assertEquals(
            AudioSessionEvent.RouteChanged(RouteChangeReason.NewDeviceAvailable),
            received,
            "el Flow no entregó la notificación en 10s",
        )
    }

    // ==================== Helpers ====================

    private fun interruptionNotification(type: ULong, options: ULong? = null): NSNotification {
        val userInfo = buildMap<Any?, Any?> {
            put(AVAudioSessionInterruptionTypeKey, NSNumber.numberWithUnsignedLong(type))
            options?.let {
                put(AVAudioSessionInterruptionOptionKey, NSNumber.numberWithUnsignedLong(it))
            }
        }
        return NSNotification.Companion.notificationWithName(
            aName = AVAudioSessionInterruptionNotification,
            `object` = null,
            userInfo = userInfo,
        )
    }

    private fun routeChangeNotification(reason: ULong): NSNotification =
        NSNotification.Companion.notificationWithName(
            aName = AVAudioSessionRouteChangeNotification,
            `object` = null,
            userInfo = mapOf<Any?, Any?>(
                AVAudioSessionRouteChangeReasonKey to NSNumber.numberWithUnsignedLong(reason),
            ),
        )

    private companion object {
        const val INTERRUPTION_BEGAN: ULong = 1uL
        const val INTERRUPTION_ENDED: ULong = 0uL
        const val OPTION_SHOULD_RESUME: ULong = 1uL
    }
}
