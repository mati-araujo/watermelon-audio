package com.watermellonstudios.audio.domain.mode

/**
 * Phases of a mode transition.
 *
 * Each phase represents a distinct step in the transition process,
 * with associated progress percentage and description for UI feedback.
 *
 * @property progress Progress value (0.0 to 1.0) when this phase starts
 * @property description Human-readable description of the phase
 */
enum class TransitionPhase(val progress: Float, val description: String) {
    /**
     * Initial phase: validating preconditions, checking permissions.
     */
    PREPARING(0.1f, "Preparing..."),

    /**
     * Fading out current audio to avoid clicks.
     */
    FADING_OUT(0.2f, "Fade out..."),

    /**
     * Calling native code to switch mode.
     */
    SWITCHING_MODE(0.4f, "Switching mode..."),

    /**
     * Waiting for native confirmation that mode switch completed.
     */
    WAITING_NATIVE(0.6f, "Waiting for C++..."),

    /**
     * Reconfiguring effects for the new mode.
     */
    CONFIGURING_EFFECTS(0.8f, "Configuring effects..."),

    /**
     * Fading in audio in new mode.
     */
    FADING_IN(0.9f, "Fade in..."),

    /**
     * Transition completed successfully.
     */
    COMPLETE(1.0f, "Completed")
}
