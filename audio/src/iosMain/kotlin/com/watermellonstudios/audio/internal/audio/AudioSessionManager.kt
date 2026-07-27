package com.watermellonstudios.audio.internal.audio

import kotlinx.cinterop.CPointer
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.ObjCObjectVar
import kotlinx.cinterop.alloc
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.ptr
import kotlinx.cinterop.value
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.buffer
import kotlinx.coroutines.flow.callbackFlow
import platform.AVFAudio.AVAudioSession
import platform.AVFAudio.AVAudioSessionCategoryOptionAllowBluetoothA2DP
import platform.AVFAudio.AVAudioSessionCategoryOptionDefaultToSpeaker
import platform.AVFAudio.AVAudioSessionCategoryPlayAndRecord
import platform.AVFAudio.AVAudioSessionCategoryPlayback
import platform.AVFAudio.AVAudioSessionInterruptionNotification
import platform.AVFAudio.AVAudioSessionInterruptionOptionKey
import platform.AVFAudio.AVAudioSessionInterruptionTypeKey
import platform.AVFAudio.AVAudioSessionRouteChangeNotification
import platform.AVFAudio.AVAudioSessionRouteChangeReasonKey
import platform.AVFAudio.IOBufferDuration
import platform.AVFAudio.inputLatency
import platform.AVFAudio.outputLatency
import platform.AVFAudio.sampleRate
import platform.AVFAudio.setActive
import platform.AVFAudio.setPreferredIOBufferDuration
import platform.AVFAudio.setPreferredSampleRate
import platform.Foundation.NSError
import platform.Foundation.NSNotification
import platform.Foundation.NSNotificationCenter
import platform.Foundation.NSNumber

/**
 * Qué le pasó a la sesión de audio del sistema mientras el motor corría.
 *
 * Se expone como stream y **no** se actúa sobre el motor desde acá: quién decide
 * pausar o reanudar es el consumidor (NoisyPad), que es el único que sabe si el
 * usuario estaba grabando, si hay un loop en curso, o si conviene tirar un aviso
 * en pantalla. Un manager que pausa por su cuenta le saca esa decisión.
 */
sealed class AudioSessionEvent {

    /**
     * Algo se apropió del audio: una llamada entrante, Siri, otra app.
     *
     * **El sistema ya silenció el motor cuando esto llega.** No hace falta
     * apurarse a pausar para que deje de sonar; lo que sí conviene es actualizar
     * la UI y congelar cualquier estado que dependa del tiempo (un loop grabando,
     * por ejemplo).
     */
    data object InterruptionBegan : AudioSessionEvent()

    /**
     * Terminó la interrupción.
     *
     * @param shouldResume el sistema **sugiere** reanudar (la llamada terminó y el
     *   usuario no se fue a otra app de audio). Si es `false`, reanudar por las
     *   tuyas es mala idea: probablemente haya otra app sonando ahora.
     */
    data class InterruptionEnded(val shouldResume: Boolean) : AudioSessionEvent()

    /**
     * Cambió la ruta de audio: auriculares enchufados o desenchufados, un
     * Bluetooth que se conectó, el receiver que pasó a parlante.
     *
     * @param reason por qué cambió. [RouteChangeReason.OldDeviceUnavailable] es el
     *   caso clásico de "desenchufaron los auriculares", donde la convención de
     *   iOS es **pausar** — si no, la música sale de golpe por el parlante.
     */
    data class RouteChanged(val reason: RouteChangeReason) : AudioSessionEvent()
}

/** Motivos de cambio de ruta, mapeados desde `AVAudioSessionRouteChangeReason`. */
enum class RouteChangeReason {
    Unknown,
    NewDeviceAvailable,

    /** Se fue el dispositivo que estaba sonando (auriculares desenchufados). */
    OldDeviceUnavailable,
    CategoryChange,
    Override,
    WakeFromSleep,
    NoSuitableRouteForCategory,
    RouteConfigurationChange,
}

/**
 * Configuración y observación de `AVAudioSession` para iOS (WA-3.4).
 *
 * `AVAudioSession` es el árbitro del sistema: decide el sample rate real, el
 * tamaño de buffer real, y puede quitarle el audio a la app en cualquier momento.
 * El motor C++ no sabe nada de eso —ni debería—, así que esta clase es la costura
 * entre la política del sistema y el motor.
 *
 * ## Lo que pedís no es lo que obtenés
 *
 * `preferredSampleRate` y `preferredIOBufferDuration` son **preferencias**. iOS
 * puede ignorarlas según el hardware, la ruta activa y lo que estén haciendo otras
 * apps. Por eso [actualSampleRate] y [actualIOBufferDuration] existen y hay que
 * leerlos **después** de activar: son los valores con los que el motor realmente
 * va a trabajar, y son los que hay que pasarle a `prepare()`.
 *
 * ## Regla RT (D6)
 *
 * Nada de acá corre en el thread de audio. Las notificaciones llegan por
 * `NSNotificationCenter` en threads normales y el `Flow` se consume desde una
 * corrutina. El render de CoreAudio nunca entra a Kotlin.
 */
@OptIn(ExperimentalForeignApi::class)
class AudioSessionManager(
    private val session: AVAudioSession = AVAudioSession.sharedInstance(),
) {

    /**
     * Configura la sesión.
     *
     * @param enableInput `true` usa `playAndRecord`, necesario para guitar FX y
     *   full-duplex. `false` usa `playback`, que es **preferible cuando no hace
     *   falta grabar**: no dispara el permiso de micrófono y suele conseguir mejor
     *   latencia de salida.
     * @param preferredSampleRate en Hz. 48000 es lo nativo en el hardware moderno
     *   de Apple; pedir 44100 obliga a un resampleo del sistema.
     * @param preferredBufferDuration en **segundos** (no ms — es la unidad de
     *   `AVAudioSession`). 256 frames a 48 kHz ≈ 0.0053.
     */
    fun configure(
        enableInput: Boolean = false,
        preferredSampleRate: Double = 48_000.0,
        preferredBufferDuration: Double = 0.005,
    ): Result<Unit> {
        val category = if (enableInput) {
            AVAudioSessionCategoryPlayAndRecord
        } else {
            AVAudioSessionCategoryPlayback
        }

        // DefaultToSpeaker sólo aplica a playAndRecord: sin él, grabar manda la
        // salida al receiver (el auricular de las llamadas) y el usuario cree que
        // se rompió el sonido. A2DP deja que se enrute a un parlante Bluetooth.
        val options = if (enableInput) {
            AVAudioSessionCategoryOptionDefaultToSpeaker or
                AVAudioSessionCategoryOptionAllowBluetoothA2DP
        } else {
            AVAudioSessionCategoryOptionAllowBluetoothA2DP
        }

        // Se corta en el primer fallo: con la categoría mal puesta, pedir sample
        // rate o buffer no significa nada.
        sessionCall("setCategory") { session.setCategory(category, options, it) }
            .onFailure { return Result.failure(it) }
        sessionCall("setPreferredSampleRate") {
            session.setPreferredSampleRate(preferredSampleRate, it)
        }.onFailure { return Result.failure(it) }

        return sessionCall("setPreferredIOBufferDuration") {
            session.setPreferredIOBufferDuration(preferredBufferDuration, it)
        }
    }

    /**
     * Activa la sesión. Recién **después** de esto tienen sentido
     * [actualSampleRate] y [actualIOBufferDuration].
     */
    fun activate(): Result<Unit> =
        sessionCall("setActive(true)") { session.setActive(true, it) }

    /**
     * Desactiva la sesión y le devuelve el audio al sistema.
     *
     * Conviene llamarlo cuando el motor se detiene de verdad: una sesión activa
     * con nada sonando le impide a otras apps recuperar el control.
     */
    fun deactivate(): Result<Unit> =
        sessionCall("setActive(false)") { session.setActive(false, it) }

    /** Sample rate que el sistema concedió de verdad. */
    val actualSampleRate: Double get() = session.sampleRate

    /** Duración de buffer concedida, en segundos. */
    val actualIOBufferDuration: Double get() = session.IOBufferDuration

    /** Latencia de salida informada por el sistema, en segundos. */
    val outputLatency: Double get() = session.outputLatency

    /** Latencia de entrada informada por el sistema, en segundos. */
    val inputLatency: Double get() = session.inputLatency

    /**
     * Interrupciones y cambios de ruta.
     *
     * El buffer explícito no es cosmético: `trySend` desde un callback de
     * `NSNotificationCenter` **no puede suspender**, así que sin capacidad
     * suficiente un evento se perdería en silencio. 16 slots sobran para eventos
     * de sesión, que son raros.
     *
     * `DROP_OLDEST` por si aun así se llenara: si el consumidor se atrasó, el
     * evento **viejo** es el que sobra — para cuando lo procese, lo que importa es
     * el estado actual.
     *
     * Los observers se sueltan solos cuando se cancela la colección.
     */
    val events: Flow<AudioSessionEvent> = callbackFlow {
        val center = NSNotificationCenter.defaultCenter

        val interruptionObserver = center.addObserverForName(
            name = AVAudioSessionInterruptionNotification,
            `object` = null,
            queue = null,
        ) { notification ->
            parseInterruption(notification)?.let { trySend(it) }
        }

        val routeObserver = center.addObserverForName(
            name = AVAudioSessionRouteChangeNotification,
            `object` = null,
            queue = null,
        ) { notification ->
            trySend(AudioSessionEvent.RouteChanged(parseRouteChangeReason(notification)))
        }

        awaitClose {
            center.removeObserver(interruptionObserver)
            center.removeObserver(routeObserver)
        }
    }.buffer(capacity = 16, onBufferOverflow = BufferOverflow.DROP_OLDEST)

    /**
     * `internal` y no `private` a propósito: los números mágicos del `userInfo`
     * son la parte frágil de esta clase, y probarlos a través del `Flow` obliga a
     * sincronizar con el registro del observer —`NSNotificationCenter` entrega
     * sincrónicamente, así que una notificación posteada antes de tiempo se pierde
     * y el test cuelga—. Testear el parseo directo lo hace determinista.
     */
    internal fun parseInterruption(notification: NSNotification?): AudioSessionEvent? {
        val type = (notification?.userInfo?.get(AVAudioSessionInterruptionTypeKey) as? NSNumber)
            ?.unsignedLongValue
            ?: return null

        return when (type) {
            INTERRUPTION_BEGAN -> AudioSessionEvent.InterruptionBegan
            INTERRUPTION_ENDED -> {
                val options =
                    (notification.userInfo?.get(AVAudioSessionInterruptionOptionKey) as? NSNumber)
                        ?.unsignedLongValue ?: 0uL
                AudioSessionEvent.InterruptionEnded(
                    shouldResume = (options and INTERRUPTION_OPTION_SHOULD_RESUME) != 0uL,
                )
            }
            // Un tipo desconocido se descarta en vez de inventarle un significado:
            // adivinar acá puede reanudar el motor arriba de una llamada.
            else -> null
        }
    }

    internal fun parseRouteChangeReason(notification: NSNotification?): RouteChangeReason {
        val raw = (notification?.userInfo?.get(AVAudioSessionRouteChangeReasonKey) as? NSNumber)
            ?.unsignedLongValue
            ?: return RouteChangeReason.Unknown

        return when (raw) {
            1uL -> RouteChangeReason.NewDeviceAvailable
            2uL -> RouteChangeReason.OldDeviceUnavailable
            3uL -> RouteChangeReason.CategoryChange
            4uL -> RouteChangeReason.Override
            6uL -> RouteChangeReason.WakeFromSleep
            7uL -> RouteChangeReason.NoSuitableRouteForCategory
            8uL -> RouteChangeReason.RouteConfigurationChange
            else -> RouteChangeReason.Unknown
        }
    }

    /**
     * Los setters de `AVAudioSession` **no lanzan**: devuelven `false` y llenan un
     * `NSError`. Envolverlos en `runCatching` daría éxito siempre, incluso cuando
     * el sistema rechaza la configuración — que es justo el caso que interesa
     * detectar. Esto traduce ese contrato al `Result` del resto de la librería,
     * conservando el mensaje del sistema.
     */
    private inline fun sessionCall(
        operation: String,
        block: (CPointer<ObjCObjectVar<NSError?>>) -> Boolean,
    ): Result<Unit> = memScoped {
        val errorRef = alloc<ObjCObjectVar<NSError?>>()
        if (block(errorRef.ptr)) {
            Result.success(Unit)
        } else {
            val detail = errorRef.value?.localizedDescription ?: "sin detalle del sistema"
            Result.failure(IllegalStateException("AVAudioSession.$operation falló: $detail"))
        }
    }

    private companion object {
        // Valores de AVAudioSessionInterruptionType / ...Option. Se escriben acá
        // porque las constantes del enum no están expuestas como tales en los
        // bindings de Kotlin/Native; son parte del ABI publico de iOS y no cambian.
        const val INTERRUPTION_BEGAN: ULong = 1uL
        const val INTERRUPTION_ENDED: ULong = 0uL
        const val INTERRUPTION_OPTION_SHOULD_RESUME: ULong = 1uL
    }
}
