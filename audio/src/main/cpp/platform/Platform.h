#pragma once

/**
 * @file Platform.h
 * @brief Platform-agnostic abstractions for hardware-specific operations.
 *
 * Provides:
 * - Denormal number flushing (prevents 10-100x CPU slowdown in FP audio DSP)
 * - Thread priority helpers for audio threads
 * - SIMD capability queries
 *
 * Implementations are in platform-specific .cpp files:
 * - PlatformAndroid.cpp (ARM64 NEON, ARMv7 NEON, x86_64 SSE)
 * - PlatformDesktop.cpp (future: macOS/Linux/Windows)
 */

#include <cstdint>

namespace wma { namespace platform {

/**
 * Flush denormal floating-point numbers to zero.
 *
 * Denormal (subnormal) numbers are very small FP values near zero that require
 * special CPU handling, causing 10-100x slowdown in audio DSP. This function
 * sets the CPU's flush-to-zero mode so denormals are treated as 0.0.
 *
 * Implementation:
 * - ARM64: Sets FPCR.FZ (bit 24) and FPCR.DN (bit 25)
 * - ARMv7: Sets FPSCR.FZ (bit 24)
 * - x86/x86_64: Sets MXCSR.FZ (bit 15) and DAZ (bit 6)
 * - Other: no-op
 *
 * Call once per thread that processes audio (audio callback thread, USB event thread).
 * NOT RT-safe (logs on first call), but the FP register write itself is instant.
 */
void flushDenormals();

/**
 * Set the current thread to real-time audio priority.
 * Implementation varies by platform (pthread, Windows API, etc.)
 * NOT RT-safe — call before entering the audio processing loop.
 */
void setAudioThreadPriority();

/**
 * @return true if the platform supports NEON SIMD instructions.
 */
bool hasNeonSupport();

/**
 * @return true if the platform supports SSE SIMD instructions.
 */
bool hasSseSupport();

}} // namespace wma::platform
