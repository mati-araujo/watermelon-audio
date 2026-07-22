#pragma once

/**
 * @file Logger.h
 * @brief Platform-agnostic logging abstraction for the audio engine.
 *
 * Provides LOG macros (LOGI, LOGW, LOGE, LOGD) backed by a configurable callback.
 * Default implementation uses Android's __android_log_print.
 * For non-Android platforms, set a custom callback via wma::setLogCallback().
 *
 * NOT RT-safe — do not call from the audio thread.
 */

#include <cstdarg>
#include <cstdio>
#include <atomic>

namespace wma {

enum class LogLevel : int {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3
};

/**
 * Log callback function pointer type.
 * @param level  Log severity
 * @param tag    Module/component tag
 * @param msg    Formatted message (already formatted, no varargs)
 */
using LogCallback = void(*)(LogLevel level, const char* tag, const char* msg);

/**
 * Set a custom log callback. If nullptr, reverts to platform default.
 * Thread-safe: uses atomic store. NOT RT-safe.
 */
void setLogCallback(LogCallback callback);

/**
 * Get the currently active log callback (or nullptr for platform default).
 */
LogCallback getLogCallback();

// Internal: dispatch a log message. NOT RT-safe.
#if defined(__clang__)
// clang must be checked BEFORE __GNUC__: it defines __GNUC__ too, but rejects
// the gnu_printf archetype (-Wignored-attributes, fatal under -Werror). Its
// plain `printf` archetype already validates with C99 semantics, so %zu/%lld
// are accepted — the MinGW problem described below is GCC-specific.
void logMessage(LogLevel level, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
#elif defined(__GNUC__)
// gnu_printf (not printf) so the format check uses C99/GNU semantics — %zu,
// %lld etc. — matching the Android/bionic target. With the plain `printf`
// archetype, MinGW hosts validate against msvcrt semantics and false-flag the
// valid %zu used throughout the engine. Real format bugs are still caught.
void logMessage(LogLevel level, const char* tag, const char* fmt, ...)
    __attribute__((format(__gnu_printf__, 3, 4)));
#else
void logMessage(LogLevel level, const char* tag, const char* fmt, ...);
#endif

} // namespace wma

// ==================== Convenience Macros ====================
// These match the existing LOGI/LOGW/LOGE/LOGD signatures used everywhere.
// Default tag can be overridden by defining WMA_LOG_TAG before including this header.

#ifndef WMA_LOG_TAG
#define WMA_LOG_TAG "NoisyPad"
#endif

#define WMA_LOGD(...) wma::logMessage(wma::LogLevel::DEBUG, WMA_LOG_TAG, __VA_ARGS__)
#define WMA_LOGI(...) wma::logMessage(wma::LogLevel::INFO,  WMA_LOG_TAG, __VA_ARGS__)
#define WMA_LOGW(...) wma::logMessage(wma::LogLevel::WARN,  WMA_LOG_TAG, __VA_ARGS__)
#define WMA_LOGE(...) wma::logMessage(wma::LogLevel::ERROR, WMA_LOG_TAG, __VA_ARGS__)
