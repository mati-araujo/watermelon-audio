package com.watermellonstudios.audio.internal.mode

import android.util.Log
import com.watermellonstudios.audio.api.IEffectManager
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
    private val config: ModeTransitionConfig
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
            else -> throw IllegalStateException("Invalid transition state")
        }

        // Already in target mode
        if (fromMode == mode) {
            send(1.0f)
            return@channelFlow
        }

        Log.d(TAG, "Starting transition: $fromMode -> $mode")

        try {
            // Phase 1: Prepare
            updateProgress(fromMode, mode, TransitionPhase.PREPARING)
            send(TransitionPhase.PREPARING.progress)
            prepareTransition(fromMode, mode)

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

            Log.d(TAG, "Transition complete: $fromMode -> $mode")

        } catch (e: CancellationException) {
            Log.d(TAG, "Transition cancelled")
            attemptRollback(fromMode)
            throw e

        } catch (e: Exception) {
            Log.e(TAG, "Transition failed", e)
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
     * Prepares for mode transition.
     * @param fromMode Current mode (reserved for future permission checks)
     * @param toMode Target mode (reserved for future resource preloading)
     */
    @Suppress("UNUSED_PARAMETER") // Parameters reserved for future use
    private suspend fun prepareTransition(fromMode: AudioMode, toMode: AudioMode) {
        // Future: Validate permissions based on toMode.requiresInput
        // Future: Preload resources needed by toMode
        delay(50) // Allow UI state to propagate
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
                    Log.d(TAG, "Configuring effects for INPUT_FX mode")
                }

                AudioMode.CHAOS_PAD -> {
                    // CHAOS_PAD: effects process XY oscillator
                    Log.d(TAG, "Configuring effects for CHAOS_PAD mode")
                }

                AudioMode.MIX -> {
                    // MIX: effects process mixed signal
                    Log.d(TAG, "Configuring effects for MIX mode")
                }
            }
        }
    }

    private fun updateModeProperties(mode: AudioMode) {
        _modeProperties.update {
            when (mode) {
                AudioMode.CHAOS_PAD -> ModeProperties(
                    oscillatorLevel = 1.0f,
                    inputLevel = 0.0f,
                    crossfadePosition = 0.0f
                )
                AudioMode.INPUT_FX -> ModeProperties(
                    oscillatorLevel = 0.0f,
                    inputLevel = 1.0f,
                    crossfadePosition = 1.0f
                )
                AudioMode.MIX -> ModeProperties(
                    oscillatorLevel = 0.5f,
                    inputLevel = 0.5f,
                    crossfadePosition = 0.5f
                )
            }
        }
    }

    // =========================================================================
    // ROLLBACK AND CANCELLATION
    // =========================================================================

    private suspend fun attemptRollback(originalMode: AudioMode) {
        try {
            Log.d(TAG, "Attempting rollback to $originalMode")
            stateWriter.setAudioMode(originalMode)
            _transitionState.value = ModeTransitionState.Idle(originalMode)
        } catch (e: Exception) {
            Log.e(TAG, "Rollback failed", e)
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

    override fun setCrossfadePosition(position: Float): Result<Unit> {
        val current = _transitionState.value
        if (current !is ModeTransitionState.Idle || current.currentMode != AudioMode.MIX) {
            return Result.failure(IllegalStateException("Crossfade only available in MIX mode"))
        }

        val clampedPosition = position.coerceIn(0f, 1f)
        _modeProperties.update {
            it.copy(
                crossfadePosition = clampedPosition,
                oscillatorLevel = 1f - clampedPosition,
                inputLevel = clampedPosition
            )
        }

        // Update native layer asynchronously using the manager's scope
        // NOTE (Phase 4 Fix): Changed from GlobalScope to scope to prevent memory leaks
        scope.launch(Dispatchers.IO) {
            stateWriter.setCrossfadePosition(clampedPosition)
        }

        return Result.success(Unit)
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
        Log.d(TAG, "ModeTransitionManager disposed")
    }
}
