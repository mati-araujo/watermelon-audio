package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.mode.AudioMode
import com.watermellonstudios.audio.domain.mode.ModeProperties
import com.watermellonstudios.audio.domain.mode.ModeTransitionState
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.StateFlow

/**
 * Public interface for managing audio mode transitions.
 *
 * Provides a state machine-based approach to mode transitions with:
 * - Observable transition state
 * - Progress tracking
 * - Cancellation support
 * - Automatic rollback on failure
 *
 * Thread Safety: All operations are thread-safe and can be called from any coroutine.
 *
 * Usage:
 * ```kotlin
 * val modeHandler = ModeTransitionFactory.create(scope)
 *
 * // Observe state
 * modeHandler.transitionState.collect { state ->
 *     when (state) {
 *         is ModeTransitionState.Idle -> showMode(state.currentMode)
 *         is ModeTransitionState.InProgress -> showProgress(state.progress)
 *         is ModeTransitionState.Failed -> showError(state.error)
 *     }
 * }
 *
 * // Transition to a new mode
 * modeHandler.transitionTo(AudioMode.INPUT_FX)
 *     .catch { e -> Log.e(TAG, "Transition failed", e) }
 *     .collect { progress -> updateProgressBar(progress) }
 * ```
 *
 * @see ModeTransitionFactory for creating instances
 */
interface IModeTransitionHandler {

    /**
     * Current state of the mode transition state machine.
     *
     * This is the single source of truth for mode state.
     * Possible states: Idle, InProgress, Failed
     */
    val transitionState: StateFlow<ModeTransitionState>

    /**
     * Current audio mode.
     *
     * Derived from transitionState:
     * - Idle: returns currentMode
     * - InProgress: returns fromMode (current mode until transition completes)
     * - Failed: returns fromMode (reverted to original)
     */
    val currentMode: StateFlow<AudioMode>

    /**
     * Additional mode properties (levels, crossfade).
     *
     * Updated during mode changes and crossfade adjustments.
     */
    val modeProperties: StateFlow<ModeProperties>

    /**
     * Starts a transition to a new audio mode.
     *
     * Returns a Flow that emits progress values from 0.0 to 1.0.
     * The flow completes when transition succeeds or throws on failure.
     *
     * @param mode The target audio mode
     * @return Flow emitting progress (0.0 to 1.0)
     * @throws TransitionInProgressException if a transition is already active
     * @throws PermissionDeniedException if mode requires permission not granted
     * @throws ModeTransitionFailedException if native layer fails
     * @throws ModeChangeTimeoutException if native confirmation times out
     */
    fun transitionTo(mode: AudioMode): Flow<Float>

    /**
     * Cancels the current transition if one is in progress.
     *
     * Attempts to rollback to the original mode.
     * No-op if no transition is active.
     */
    suspend fun cancelTransition()

    /**
     * Checks if a transition to the specified mode can be started.
     *
     * @param mode The target mode to check
     * @return true if transition can start (no transition in progress, not already in mode)
     */
    fun canTransitionTo(mode: AudioMode): Boolean

    /**
     * Retries the last failed transition.
     *
     * @return Result.success if retry succeeds, Result.failure if no failed transition
     *         or retry also fails
     */
    suspend fun retryLastTransition(): Result<Unit>

    // BREAKING (2.0.0): setCrossfadePosition() was removed.
    //
    // It never reached the engine. It updated modeProperties in Kotlin and then
    // called through to a writer whose body was `Result.success(Unit)` — no C
    // API for the mixer existed to call. Every layer under it reported success
    // for a value nothing consumed.
    //
    // For the instrument/input balance, use two independent controls instead of
    // one linked position: AudioInput.monitoringVolume (or inputGain, for
    // hardware calibration) on the input side, and the new synthVolume on the
    // instrument side. They are orthogonal on purpose — if a UI wants a single
    // equal-power knob, it composes them, because that curve is a presentation
    // choice and not something the engine should decide.

    /**
     * Disposes the handler and releases resources.
     *
     * After calling dispose(), the handler should not be used.
     */
    fun dispose()
}
