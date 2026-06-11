#include "../../platform/Logger.h"
#include <atomic>
#include <cstdarg>

namespace wma {
namespace {
std::atomic<LogCallback> gCallback{nullptr};
}

void setLogCallback(LogCallback callback) {
    gCallback.store(callback, std::memory_order_relaxed);
}

LogCallback getLogCallback() {
    return gCallback.load(std::memory_order_relaxed);
}

void logMessage(LogLevel level, const char* tag, const char* fmt, ...) {
    (void)level;
    (void)tag;
    (void)fmt;
}
}
