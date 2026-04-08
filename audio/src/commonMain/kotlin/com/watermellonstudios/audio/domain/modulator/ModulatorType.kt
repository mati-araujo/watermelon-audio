package com.watermellonstudios.audio.domain.modulator

/**
 * Types of modulators available in the audio engine.
 *
 * @property id Native ID used by the C++ engine
 * @property displayName Human-readable name for UI
 */
enum class ModulatorType(val id: Int, val displayName: String) {
    NONE(0, "None"),
    BURST(1, "Burst"),
    AM(2, "AM"),
    FM(3, "FM"),
    PWM(4, "PWM"),
    ENVELOPE(5, "Envelope"),
    RING(6, "Ring"),
    GATE(7, "Gate");

    companion object {
        fun fromId(id: Int): ModulatorType = entries.find { it.id == id } ?: NONE
    }
}
