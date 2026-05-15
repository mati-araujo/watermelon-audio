#pragma once

#include "../dsp/LockFreeEventQueue.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace wm {

/**
 * @brief Event emitted from the audio thread when looper state changes
 *        materially. POD — trivially copyable for the SPSC queue.
 */
struct LooperEvent {
    enum class Type : int32_t {
        Progress = 0,
        PlayingChanged = 1,
        PeakChanged = 2,
    };

    Type    type;
    int32_t trackIndex;  // 0..7
    float   value;       // progress (0..1), 1.0/0.0 for bool, or peak level (linear 0..1)
};

/**
 * @class LooperEventDispatcher
 * @brief Drains looper state events off the RT thread.
 *
 * Thread model:
 * - Audio (RT) thread: pushFromRT() — lock-free, non-blocking, no allocation.
 *   On queue-full the event is dropped and counted; UI re-syncs at the next
 *   threshold-crossing event for that field.
 * - Worker thread: started by start(), polls the queue every ~15 ms via
 *   timed wait, invokes the sink for each event. Sink is set by JNI side
 *   under a mutex (UI thread). Sink is responsible for any JVM attachment.
 * - UI thread: start(), stop(), setSink().
 *
 * Why polling instead of condvar-notify from RT: notify_one() can fall into
 * a futex syscall, which is not RT-safe. 15 ms wakeups (~67 Hz) is well
 * within UI cadence and costs ~67 syscalls/sec on the worker.
 */
class LooperEventDispatcher {
public:
    static constexpr size_t kQueueCapacity = 256;
    static constexpr std::chrono::milliseconds kPollInterval{15};

    using Sink = std::function<void(const LooperEvent&)>;

    LooperEventDispatcher() = default;
    ~LooperEventDispatcher() { stop(); }

    LooperEventDispatcher(const LooperEventDispatcher&) = delete;
    LooperEventDispatcher& operator=(const LooperEventDispatcher&) = delete;

    /** Start the worker thread. Idempotent. UI thread. */
    void start() {
        if (mRunning.exchange(true, std::memory_order_acq_rel)) return;
        mWorker = std::thread([this] { workerLoop(); });
    }

    /** Stop and join the worker thread. Idempotent. UI thread. */
    void stop() {
        if (!mRunning.exchange(false, std::memory_order_acq_rel)) return;
        {
            std::lock_guard<std::mutex> lk(mCvMutex);
            mWakeFlag = true;
        }
        mCv.notify_all();
        if (mWorker.joinable()) mWorker.join();
        // Drop any remaining events.
        LooperEvent dropped{};
        while (mQueue.pop(dropped)) { /* discard */ }
    }

    /**
     * @brief RT-safe push from the audio thread.
     * @return false if the queue was full (event dropped + counter incremented).
     */
    bool pushFromRT(const LooperEvent& ev) noexcept {
        if (!mQueue.push(ev)) {
            mDropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    /** Install (or clear) the sink. UI thread. */
    void setSink(Sink sink) {
        std::lock_guard<std::mutex> lk(mSinkMutex);
        mSink = std::move(sink);
    }

    /** Dropped-event counter (telemetry). */
    int64_t getDroppedEvents() const {
        return mDropped.load(std::memory_order_relaxed);
    }

private:
    void workerLoop() {
        // Attach hook: the sink itself is responsible for any per-thread JVM
        // attach because the dispatcher is platform-agnostic.
        while (mRunning.load(std::memory_order_acquire)) {
            {
                std::unique_lock<std::mutex> lk(mCvMutex);
                mCv.wait_for(lk, kPollInterval, [this] { return mWakeFlag; });
                mWakeFlag = false;
            }
            drain();
        }
        drain();  // final flush
    }

    void drain() {
        // Snapshot the sink once per drain pass to avoid holding the mutex
        // across user code. If unregister happens mid-pass, in-flight events
        // dispatch to the previous sink — that's fine; the next pass picks up
        // the new (or null) sink.
        Sink sink;
        {
            std::lock_guard<std::mutex> lk(mSinkMutex);
            sink = mSink;
        }
        if (!sink) {
            // No sink — drain & discard so the queue doesn't backpressure RT.
            LooperEvent ev{};
            while (mQueue.pop(ev)) { /* discard */ }
            return;
        }
        LooperEvent ev{};
        while (mQueue.pop(ev)) {
            sink(ev);
        }
    }

    LockFreeEventQueue<LooperEvent, kQueueCapacity> mQueue;
    std::atomic<bool> mRunning{false};
    std::atomic<int64_t> mDropped{0};
    std::thread mWorker;

    std::mutex mCvMutex;
    std::condition_variable mCv;
    bool mWakeFlag{false};

    std::mutex mSinkMutex;
    Sink mSink;
};

}  // namespace wm
