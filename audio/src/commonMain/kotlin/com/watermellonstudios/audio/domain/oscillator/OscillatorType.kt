package com.watermellonstudios.audio.domain.oscillator

/**
 * Types of oscillators available in the audio engine.
 *
 * @property id Native ID used by the C++ engine
 * @property displayName Human-readable name for UI
 */
enum class OscillatorType(val id: Int, val displayName: String) {
    SINE(0, "Sine"),
    SQUARE(1, "Square"),
    SAW(2, "Sawtooth"),
    TRIANGLE(3, "Triangle"),
    NOISE(4, "Noise"),
    PULSE(5, "Pulse");

    companion object {
        fun fromId(id: Int): OscillatorType = entries.find { it.id == id } ?: SAW
    }
}
