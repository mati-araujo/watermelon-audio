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
 * Flush denormal floating-point numbers to zero, RT-SAFE (WD-1.2).
 *
 * Denormal (subnormal) numbers are very small FP values near zero that require
 * special CPU handling, causing 10-100x slowdown in audio DSP. This writes the
 * CPU's flush-to-zero mode so denormals are treated as 0.0.
 *
 * Implementation:
 * - ARM64: Sets FPCR.FZ (bit 24) and FPCR.DN (bit 25)
 * - ARMv7: Sets FPSCR.FZ (bit 24)
 * - x86/x86_64: Sets MXCSR.FZ (bit 15) and DAZ (bit 6)
 * - Other: no-op
 *
 * **ESTO ES ESTADO POR THREAD.** FPCR y MXCSR son registros de control del
 * thread, no del proceso. Setearlos en el thread que llama a `start()` no hace
 * absolutamente nada por el thread de audio — que es lo que pasaba hasta
 * WD-1.2: los dos unicos call sites (`AudioEngine::start` y
 * `OboeBackend::start`) corren en el thread del llamador, y `CoreAudioBackend`
 * no llamaba a ninguno. El thread RT corria con denormales habilitados en las
 * tres plataformas, y los flushes manuales a `1e-20f` repartidos por ocho
 * archivos de efectos eran, sin que nadie lo supiera, la UNICA mitigacion.
 *
 * **Se llama en cada entrada al callback, sin guarda.** No hay `thread_local`
 * a proposito: en una libreria compartida el primer acceso a un `thread_local`
 * puede pasar por `__tls_get_addr` y alocar el bloque de TLS — o sea, un malloc
 * en el thread de audio, que es justo lo que se quiere evitar. El costo de la
 * alternativa es un read-modify-write de un registro de control por callback
 * (~decenas de ciclos, ~375 veces por segundo a 128 frames/48 kHz): del orden
 * del 0,001% de un core. La guarda costaba mas riesgo que el trabajo que ahorra.
 *
 * RT-safe: no aloca, no bloquea, no loguea, no toca TLS.
 */
void flushDenormalsRtSafe();

/**
 * Igual que flushDenormalsRtSafe(), mas una linea de log diciendo que rama de
 * ISA se tomo.
 *
 * NO ES RT-SAFE — es el diagnostico de arranque, para que un build no pueda
 * perder de vista silenciosamente con que implementacion quedo. Llamar desde el
 * thread de control, una vez, al iniciar. Nunca desde un callback.
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
