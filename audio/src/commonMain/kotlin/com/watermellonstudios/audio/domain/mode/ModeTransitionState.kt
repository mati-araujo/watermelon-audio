package com.watermellonstudios.audio.domain.mode

/**
 * Represents the state of a mode transition.
 *
 * This sealed class provides type-safe representation of all possible
 * states during a mode transition, enabling exhaustive when checks.
 */
sealed class ModeTransitionState {
    /**
     * No transition in progress. System is stable in current mode.
     *
     * @property currentMode The current active audio mode
     */
    data class Idle(val currentMode: AudioMode) : ModeTransitionState()

    /**
     * Transition is in progress.
     *
     * @property fromMode The mode being transitioned from
     * @property toMode The target mode
     * @property progress Overall progress (0.0 to 1.0)
     * @property phase Current phase of the transition
     */
    data class InProgress(
        val fromMode: AudioMode,
        val toMode: AudioMode,
        val progress: Float,
        val phase: TransitionPhase
    ) : ModeTransitionState()

    /**
     * Transition failed.
     *
     * @property fromMode The mode that was being transitioned from
     * @property toMode The target mode that failed
     * @property error The exception that caused the failure
     * @property canRetry Whether the transition can be retried
     */
    data class Failed(
        val fromMode: AudioMode,
        val toMode: AudioMode,
        val error: Throwable,
        val canRetry: Boolean = true
    ) : ModeTransitionState()
}

/**
 * Additional mode state properties for crossfade control.
 *
 * Used in MIX mode to control the balance between oscillator and input.
 */
data class ModeProperties(
    val oscillatorLevel: Float = 1.0f,
    val inputLevel: Float = 0.0f,
    val crossfadePosition: Float = 0.5f
)
