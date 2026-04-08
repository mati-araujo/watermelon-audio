#pragma once

/**
 * Default values for effect initialization.
 * Effects receive the actual sample rate via setSampleRate() at runtime.
 * This is only used as constructor default before setSampleRate() is called.
 */
constexpr int DEFAULT_SAMPLE_RATE = 48000;
