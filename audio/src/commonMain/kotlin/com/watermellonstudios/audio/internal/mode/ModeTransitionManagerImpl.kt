package com.watermellonstudios.audio.internal.mode

import com.watermellonstudios.audio.api.IEffectManager
import com.watermellonstudios.audio.callback.AudioLogger
import com.watermellonstudios.audio.callback.platformDefaultAudioLogger
import com.watermellonstudios.audio.api.IModeStateWriter
import com.watermellonstudios.audio.api.IModeTransitionHandler
import com.watermellonstudios.audio.api.ModeTransitionConfig
import com.watermellonstudios.audio.domain.mode.AudioMode
import com.watermellonstudios.audio.domain.mode.ModeChangeTimeoutException
import com.watermellonstudios.audio.domain.mode.ModeProperties
import com.watermellonstudios.audio.domain.mode.ModeTransitionFailedException
import com.watermellonstudios.audio.domain.mode.ModeTransitionState
import com.watermellonstudios.audio.domain.mode.TransitionInProgressException
import com.watermellonstudios.audio.domain.mode.TransitionPhase
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.channelFlow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlin.coroutines.coroutineContext
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.Dispatchers

/**
 * Implementation of [IModeTransitionHandler] with state machine-based transitions.
 *
 * Transition flow:
 * 1. Prepare (validate, check permissions)
 * 2. Fade out (if audio playing)
 * 3. Switch mode in C++
 * 4. Wait for native confirmation
 * 5. Reconfigure effects for new mode
 * 6. Fade in
 *
 * On failure, attempts rollback to original mode.
 *
 * @param stateWriter Interface to write mode changes to native C++
 * @param effectManager Optional effect manager for reconfiguring effects
 * @param scope CoroutineScope for StateFlow operations
 * @param config Configuration parameters
 */
internal class ModeTransitionManagerImpl(
    private val stateWriter: IModeStateWriter,
    private val effectManager: IEffectManager?,
    private val scope: CoroutineScope,
    private val config: ModeTransitionConfig,
    /**
     * Por defecto, el de la plataforma: en Android esto seguía saliendo por logcat con
     * `android.util.Log` y bajar la clase a común con un no-op lo habría apagado sin
     * ruido. Ver `platformDefaultAudioLogger`.
     */
    private val logger: AudioLogger = platformDefaultAudioLogger,
) : IModeTransitionHandler {

    companion object {
        private const val TAG = "ModeTransition"
    }

    private val _transitionState = MutableStateFlow<ModeTransitionState>(
        ModeTransitionState.Idle(AudioMode.CHAOS_PAD)
    )
    override val transitionState: StateFlow<ModeTransitionState> = _transitionState.asStateFlow()

    override val currentMode: StateFlow<AudioMode> = transitionState
        .map { state ->
            when (state) {
                is ModeTransitionState.Idle -> state.currentMode
                is ModeTransitionState.InProgress -> state.fromMode
                is ModeTransitionState.Failed -> state.fromMode
            }
        }
        .stateIn(scope, SharingStarted.Eagerly, AudioMode.CHAOS_PAD)

    private val _modeProperties = MutableStateFlow(ModeProperties())
    override val modeProperties: StateFlow<ModeProperties> = _modeProperties.asStateFlow()

    private var transitionJob: Job? = null
    private var lastFailedTransition: Pair<AudioMode, AudioMode>? = null

    // =========================================================================
    // MAIN TRANSITION
    // =========================================================================

    override fun transitionTo(mode: AudioMode): Flow<Float> = channelFlow {
        // Cancel any previous transition before starting a new one
        transitionJob?.cancel()

        // Capture the current Job for cancellation support
        // FIX P0.3: This was previously never assigned, breaking cancelTransition()
        transitionJob = coroutineContext[Job]

        val currentState = _transitionState.value

        // Validate no transition in progress
        if (currentState is ModeTransitionState.InProgress) {
            throw TransitionInProgressException()
        }

        val fromMode = when (currentState) {
            is ModeTransitionState.Idle -> currentState.currentMode
            is ModeTransitionState.Failed -> currentState.fromMode
        }

        // Already in target mode
        if (fromMode == mode) {
            send(1.0f)
            return@channelFlow
        }

        logger.debug(TAG, "Starting transition: $fromMode -> $mode")

        try {
            // Phase 1: Prepare
            updateProgress(fromMode, mode, TransitionPhase.PREPARING)
            send(TransitionPhase.PREPARING.progress)
            prepareTransition()

            // Phase 2: Fade out
            updateProgress(fromMode, mode, TransitionPhase.FADING_OUT)
            send(TransitionPhase.FADING_OUT.progress)
            awaitFadeOutStabilization()

            // Phase 3: Switch mode in C++
            updateProgress(fromMode, mode, TransitionPhase.SWITCHING_MODE)
            send(TransitionPhase.SWITCHING_MODE.progress)
            val result = stateWriter.setAudioMode(mode)
            if (result.isFailure) {
                throw ModeTransitionFailedException("Native mode change failed: ${result.exceptionOrNull()?.message}")
            }

            // Phase 4: Wait for native confirmation
            updateProgress(fromMode, mode, TransitionPhase.WAITING_NATIVE)
            send(TransitionPhase.WAITING_NATIVE.progress)
            waitForModeChange(mode)

            // Phase 5: Reconfigure effects
            updateProgress(fromMode, mode, TransitionPhase.CONFIGURING_EFFECTS)
            send(TransitionPhase.CONFIGURING_EFFECTS.progress)
            reconfigureEffectsForMode(mode)

            // Phase 6: Fade in
            updateProgress(fromMode, mode, TransitionPhase.FADING_IN)
            send(TransitionPhase.FADING_IN.progress)
            awaitFadeInStabilization()

            // Complete
            updateProgress(fromMode, mode, TransitionPhase.COMPLETE)
            send(1.0f)
            _transitionState.value = ModeTransitionState.Idle(mode)
            updateModeProperties(mode)
            lastFailedTransition = null

            logger.debug(TAG, "Transition complete: $fromMode -> $mode")

        } catch (e: CancellationException) {
            logger.debug(TAG, "Transition cancelled")
            attemptRollback(fromMode)
            throw e

        } catch (e: Exception) {
            logger.error(TAG, "Transition failed", e)
            lastFailedTransition = fromMode to mode
            _transitionState.value = ModeTransitionState.Failed(
                fromMode = fromMode,
                toMode = mode,
                error = e,
                canRetry = true
            )
            attemptRollback(fromMode)
            throw e
        }
    }.flowOn(Dispatchers.Default)

    // =========================================================================
    // TRANSITION PHASES
    // =========================================================================

    /**
     * Deja que el estado de UI se propague antes de arrancar la transición.
     *
     * Tenía dos parámetros (`fromMode`, `toMode`) que no leía, con un
     * `@Suppress("UNUSED_PARAMETER")` y el comentario "reserved for future use".
     * Se fueron en la limpieza del 2026-07-27: es privada y su único llamador está
     * doce líneas más arriba, así que agregarlos de vuelta el día que hagan falta
     * cuesta lo mismo que hoy — y mientras tanto la firma decía algo que no era.
     *
     * Lo que sí valía la pena conservar es la intención: cuando esto crezca, acá van
     * la validación de permisos según `toMode.requiresInput` y la precarga de
     * recursos del modo destino.
     */
    private suspend fun prepareTransition() {
        delay(50)
    }

    /**
     * Waits for any native fade-out to complete.
     * NOTE: Actual fade is handled by native layer. This is just a stabilization delay.
     */
    private suspend fun awaitFadeOutStabilization() {
        if (stateWriter.isEngineRunning()) {
            delay(config.fadeDurationMs.toLong() / 2)
        }
    }

    /**
     * Waits for any native fade-in to complete.
     * NOTE: Actual fade is handled by native layer. This is just a stabilization delay.
     */
    private suspend fun awaitFadeInStabilization() {
        delay(config.fadeDurationMs.toLong() / 2)
    }

    private suspend fun waitForModeChange(targetMode: AudioMode) {
        try {
            withTimeout(config.modeChangeTimeoutMs) {
                while (stateWriter.getAudioMode() != targetMode.id) {
                    delay(config.pollingIntervalMs)
                }
            }
        } catch (e: kotlinx.coroutines.TimeoutCancellationException) {
            throw ModeChangeTimeoutException(config.modeChangeTimeoutMs, targetMode)
        }
    }

    private suspend fun reconfigureEffectsForMode(mode: AudioMode) {
        // Effect reconfiguration based on mode
        // This is optional and depends on effect types present
        effectManager?.let { manager ->
            val effects = manager.effectsState.value

            when (mode) {
                AudioMode.INPUT_FX -> {
                    // INPUT_FX: effects process microphone input
                    // Vocoder uses internal oscillator as carrier
                    logger.debug(TAG, "Configuring effects for INPUT_FX mode")
                }

                AudioMode.CHAOS_PAD -> {
                    // CHAOS_PAD: effects process XY oscillator
                    logger.debug(TAG, "Configuring effects for CHAOS_PAD mode")
                }

                AudioMode.MIX -> {
                    // MIX: effects process mixed signal
                    logger.debug(TAG, "Configuring effects for MIX mode")
                }
            }
        }
    }

    private fun updateModeProperties(mode: AudioMode) {
        _modeProperties.update {
            when (mode) {
                AudioMode.CHAOS_PAD -> ModeProperties(
                    oscillatorLevel = 1.0f,
                    inputLevel = 0.0f
                )
                AudioMode.INPUT_FX -> ModeProperties(
                    oscillatorLevel = 0.0f,
                    inputLevel = 1.0f
                )
                AudioMode.MIX -> ModeProperties(
                    oscillatorLevel = 0.5f,
                    inputLevel = 0.5f
                )
            }
        }
    }

    // =========================================================================
    // ROLLBACK AND CANCELLATION
    // =========================================================================

    private suspend fun attemptRollback(originalMode: AudioMode) {
        try {
            logger.debug(TAG, "Attempting rollback to $originalMode")
            stateWriter.setAudioMode(originalMode)
            _transitionState.value = ModeTransitionState.Idle(originalMode)
        } catch (e: Exception) {
            logger.error(TAG, "Rollback failed", e)
            // State remains in Failed, user must intervene
        }
    }

    override suspend fun cancelTransition() {
        val current = _transitionState.value
        if (current is ModeTransitionState.InProgress) {
            transitionJob?.cancel()
            attemptRollback(current.fromMode)
        }
    }

    override suspend fun retryLastTransition(): Result<Unit> {
        val last = lastFailedTransition ?: return Result.failure(
            IllegalStateException("No failed transition to retry")
        )

        return try {
            transitionTo(last.second).collect { }
            Result.success(Unit)
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    // =========================================================================
    // UTILITIES
    // =========================================================================

    override fun canTransitionTo(mode: AudioMode): Boolean {
        val current = _transitionState.value
        return current is ModeTransitionState.Idle && current.currentMode != mode
    }

    private fun updateProgress(
        fromMode: AudioMode,
        toMode: AudioMode,
        phase: TransitionPhase
    ) {
        _transitionState.value = ModeTransitionState.InProgress(
            fromMode = fromMode,
            toMode = toMode,
            progress = phase.progress,
            phase = phase
        )
    }

    override fun dispose() {
        transitionJob?.cancel()
        logger.debug(TAG, "ModeTransitionManager disposed")
    }
}
