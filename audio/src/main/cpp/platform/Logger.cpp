#include "Logger.h"
#include "LogCaptureBuffer.h"

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/log.h>
#define WMA_USE_OS_LOG 1
#endif
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

    // Second sink (App V §3.2): capture to the in-memory ring when enabled. This
    // runs REGARDLESS of the callback below so the export is the full history;
    // it's a no-op relaxed atomic load when capture is off.
    LogCaptureBuffer::instance().capture(level, tag, buf);

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
#elif defined(WMA_USE_OS_LOG)
    // iOS / simulator: os_log, the symmetric counterpart of logcat (WA-2.3).
    // Visible in Console.app and in `log stream --predicate 'subsystem ==
    // "com.watermellonstudios.audio"'`.
    //
    // Gated on TARGET_OS_IPHONE rather than plain __APPLE__ on purpose: a macOS
    // build here is the host googletest suite, and ctest --output-on-failure
    // shows stderr, not the unified log. Sending those runs to os_log would
    // make the macOS CI job harder to debug, not easier. A macOS *app*, if one
    // ever exists, can install its own callback.
    //
    // os_log takes subsystem/category at handle creation, not per message, so
    // the engine's per-call tag goes into the message body. Both strings need
    // %{public}s — os_log redacts dynamic strings as <private> by default,
    // which would make every line useless.
    static os_log_t s_log = os_log_create("com.watermellonstudios.audio", "engine");

    os_log_type_t osType;
    switch (level) {
        case LogLevel::DEBUG: osType = OS_LOG_TYPE_DEBUG;   break;
        case LogLevel::INFO:  osType = OS_LOG_TYPE_INFO;    break;
        // os_log has no dedicated warning level; DEFAULT is the documented
        // stand-in and is the lowest level persisted to disk by default.
        case LogLevel::WARN:  osType = OS_LOG_TYPE_DEFAULT; break;
        case LogLevel::ERROR: osType = OS_LOG_TYPE_ERROR;   break;
        default:              osType = OS_LOG_TYPE_DEFAULT; break;
    }
    os_log_with_type(s_log, osType, "%{public}s: %{public}s", tag, buf);
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
