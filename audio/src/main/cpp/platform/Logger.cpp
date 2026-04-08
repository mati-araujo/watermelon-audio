#include "Logger.h"

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include <cstdio>
#include <cstdarg>

namespace wma {

// Atomic callback pointer — nullptr means "use platform default"
static std::atomic<LogCallback> g_logCallback{nullptr};

void setLogCallback(LogCallback callback) {
    g_logCallback.store(callback, std::memory_order_release);
}

LogCallback getLogCallback() {
    return g_logCallback.load(std::memory_order_acquire);
}

void logMessage(LogLevel level, const char* tag, const char* fmt, ...) {
    // Format the message into a stack buffer
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Check for custom callback first
    auto callback = g_logCallback.load(std::memory_order_acquire);
    if (callback) {
        callback(level, tag, buf);
        return;
    }

    // Platform default
#if defined(__ANDROID__)
    int androidLevel;
    switch (level) {
        case LogLevel::DEBUG: androidLevel = ANDROID_LOG_DEBUG; break;
        case LogLevel::INFO:  androidLevel = ANDROID_LOG_INFO;  break;
        case LogLevel::WARN:  androidLevel = ANDROID_LOG_WARN;  break;
        case LogLevel::ERROR: androidLevel = ANDROID_LOG_ERROR; break;
        default:              androidLevel = ANDROID_LOG_INFO;   break;
    }
    __android_log_print(androidLevel, tag, "%s", buf);
#else
    // Fallback: stderr for non-Android platforms (desktop, tests, etc.)
    const char* levelStr;
    switch (level) {
        case LogLevel::DEBUG: levelStr = "D"; break;
        case LogLevel::INFO:  levelStr = "I"; break;
        case LogLevel::WARN:  levelStr = "W"; break;
        case LogLevel::ERROR: levelStr = "E"; break;
        default:              levelStr = "?"; break;
    }
    fprintf(stderr, "%s/%s: %s\n", levelStr, tag, buf);
#endif
}

} // namespace wma
