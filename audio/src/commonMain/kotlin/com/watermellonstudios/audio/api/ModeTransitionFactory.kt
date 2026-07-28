package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.internal.mode.ModeTransitionManagerImpl
import com.watermellonstudios.audio.internal.mode.NativeModeStateWriter
import kotlinx.coroutines.CoroutineScope

/**
 * Construye [IModeTransitionHandler].
 *
 * ```kotlin
 * val modeHandler = ModeTransitionFactory.create(
 *     scope = viewModelScope,
 *     effectManager = effectManager  // opcional, para reconfigurar efectos
 * )
 * ```
 *
 * ## Estaba en `androidMain` y no tenía por qué
 *
 * [IModeTransitionHandler] siempre fue común; lo que vivía del lado Android era **cómo
 * se construye**, y eso obligaba a inyectar la función constructora
 * `(CoroutineScope) -> IModeTransitionHandler` desde afuera para que un ViewModel común
 * pudiera usarla. De las 442 líneas que se mudaron, lo que las ataba a Android eran tres
 * cosas y ninguna de fondo: `android.util.Log`, el `AudioNativeBridge` concreto de
 * Android y dos `Dispatchers.IO` que envolvían código que no hace I/O.
 *
 * **La API pública no cambió** — misma firma, mismos defaults, mismo comportamiento en
 * Android, logcat incluido. Lo que cambió es que ahora también existe en iOS.
 */
object ModeTransitionFactory {

    /**
     * Default configuration for mode transitions.
     */
    val DEFAULT_CONFIG = ModeTransitionConfig()

    /**
     * Creates a new [IModeTransitionHandler] with default configuration.
     *
     * @param scope CoroutineScope for StateFlow operations
     * @param effectManager Optional effect manager for reconfiguring effects during mode changes
     * @return New IModeTransitionHandler instance
     */
    fun create(
        scope: CoroutineScope,
        effectManager: IEffectManager? = null
    ): IModeTransitionHandler {
        return create(scope, DEFAULT_CONFIG, effectManager)
    }

    /**
     * Creates a new [IModeTransitionHandler] with custom configuration.
     *
     * @param scope CoroutineScope for StateFlow operations
     * @param config Custom configuration
     * @param effectManager Optional effect manager for reconfiguring effects during mode changes
     * @return New IModeTransitionHandler instance
     */
    fun create(
        scope: CoroutineScope,
        config: ModeTransitionConfig,
        effectManager: IEffectManager? = null
    ): IModeTransitionHandler {
        val stateWriter = NativeModeStateWriter()
        return ModeTransitionManagerImpl(
            stateWriter = stateWriter,
            effectManager = effectManager,
            scope = scope,
            config = config
        )
    }
}

/**
 * Configuration for mode transitions.
 *
 * @property modeChangeTimeoutMs Timeout for waiting on native mode change confirmation
 * @property fadeDurationMs Duration of fade in/out during transitions
 * @property pollingIntervalMs Interval for polling native mode state
 */
data class ModeTransitionConfig(
    val modeChangeTimeoutMs: Long = 2000L,
    val fadeDurationMs: Int = 100,
    val pollingIntervalMs: Long = 16L
)
