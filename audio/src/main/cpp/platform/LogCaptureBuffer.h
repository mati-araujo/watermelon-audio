#pragma once

/**
 * LogCaptureBuffer.h
 *
 * Bounded in-memory capture of the engine's formatted log lines (App V, §3.2).
 * Hangs off the existing non-RT logging path (Logger.cpp::logMessage) as a
 * SECOND sink: logcat still receives everything; when capture is enabled each
 * line is also appended here so the app can show/export the driver's own logs
 * without a USB cable (the DAC occupies the only port during validation).
 *
 * NOT RT-safe by design — same contract as logMessage(), which is explicitly
 * documented non-RT. A mutex is therefore valid. When capture is DISABLED
 * (the default / production) capture() is a single relaxed atomic load and
 * returns before taking the lock, so there is zero overhead on the log path.
 *
 * Bound: kCapacity lines; when full the oldest line is dropped and a dropped
 * counter increments so the UI can show that the window rolled.
 */

#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "Logger.h"

namespace wma {

class LogCaptureBuffer {
public:
    static constexpr size_t kCapacity = 4000;  // ~1 MB at ≤256 B/line

    static LogCaptureBuffer& instance() {
        static LogCaptureBuffer buffer;
        return buffer;
    }

    void setEnabled(bool enabled) {
        mEnabled.store(enabled, std::memory_order_relaxed);
    }

    bool isEnabled() const { return mEnabled.load(std::memory_order_relaxed); }

    /** Append a formatted line. No-op (lock-free) when disabled. */
    void capture(LogLevel level, const char* tag, const char* msg) {
        if (!mEnabled.load(std::memory_order_relaxed)) return;

        char line[300];
        std::snprintf(line, sizeof(line), "%c/%s: %s", levelChar(level),
                      tag ? tag : "", msg ? msg : "");

        std::lock_guard<std::mutex> lock(mMutex);
        if (mLines.size() >= kCapacity) {
            mLines.pop_front();
            mDropped.fetch_add(1, std::memory_order_relaxed);
        }
        mLines.emplace_back(line);
    }

    /** Return and remove everything accumulated since the last drain. */
    std::vector<std::string> drain() {
        std::lock_guard<std::mutex> lock(mMutex);
        std::vector<std::string> out(mLines.begin(), mLines.end());
        mLines.clear();
        return out;
    }

    int droppedCount() const { return mDropped.load(std::memory_order_relaxed); }

    void clear() {
        std::lock_guard<std::mutex> lock(mMutex);
        mLines.clear();
        mDropped.store(0, std::memory_order_relaxed);
    }

private:
    LogCaptureBuffer() = default;

    static char levelChar(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return 'D';
            case LogLevel::INFO:  return 'I';
            case LogLevel::WARN:  return 'W';
            case LogLevel::ERROR: return 'E';
            default:              return '?';
        }
    }

    std::atomic<bool> mEnabled{false};
    std::atomic<int>  mDropped{0};
    mutable std::mutex mMutex;
    std::deque<std::string> mLines;
};

}  // namespace wma
