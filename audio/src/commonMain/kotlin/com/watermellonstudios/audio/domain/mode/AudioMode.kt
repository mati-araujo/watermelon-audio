package com.watermellonstudios.audio.domain.mode

/**
 * Audio modes that define signal routing in the audio engine.
 *
 * @property id Native ID used for JNI communication
 * @property displayName Human-readable name for UI
 */
enum class AudioMode(val id: Int, val displayName: String) {
    /**
     * Oscillators only (original mode).
     * XY pad controls frequency and amplitude.
     */
    CHAOS_PAD(0, "ChaosPad"),

    /**
     * Audio input with effects only.
     * XY pad controls filter cutoff and wet/dry mix.
     * Requires RECORD_AUDIO permission.
     */
    INPUT_FX(1, "Input FX"),

    /**
     * Mix of oscillators and input.
     * XY pad controls crossfade and frequency.
     * Requires RECORD_AUDIO permission.
     */
    MIX(2, "Mix");

    /**
     * Whether this mode requires audio input permission.
     */
    val requiresInput: Boolean
        get() = this != CHAOS_PAD

    companion object {
        /**
         * Get AudioMode from native ID.
         * @param id The native mode ID
         * @return The corresponding AudioMode, or CHAOS_PAD if not found
         */
        fun fromId(id: Int): AudioMode = entries.find { it.id == id } ?: CHAOS_PAD
    }
}
