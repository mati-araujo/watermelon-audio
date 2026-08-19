#include "AnalysisThread.h"

#include <chrono>
#include <cmath>
#include <limits>

#if defined(WMA_TEST_HOOKS)
std::atomic<bool> gSnapshotHoldMidPublish{false};
std::atomic<bool> gSnapshotIsMidPublish{false};
std::atomic<bool> gSnapshotHoldMidRead{false};
std::atomic<bool> gSnapshotIsMidRead{false};
#endif

namespace wma::analysis {

namespace {
/// Piso por debajo del cual se reporta "sin senal". Lineal, ~-60 dBFS.
constexpr float kSilenceFloor = 0.001f;
/// Cada cuanto vuelve a mirar el ring cuando no habia nada. 5 ms es holgado
/// contra los ~170 ms que el ring aguanta antes de pisar.
constexpr auto kIdleNap = std::chrono::milliseconds(5);
}  // namespace

void AnalysisThread::start(int captureSampleRate) {
    if (mRunning.exchange(true, std::memory_order_acq_rel)) {
        return;   // ya estaba corriendo
    }
    mThread = std::thread([this, captureSampleRate] { drainLoop(captureSampleRate); });
}

void AnalysisThread::stop() {
    if (!mRunning.exchange(false, std::memory_order_acq_rel)) {
        if (mThread.joinable()) mThread.join();
        return;
    }
    if (mThread.joinable()) mThread.join();
}

void AnalysisThread::drainLoop(int captureSampleRate) {
    const float rate = static_cast<float>(captureSampleRate);
    const float nan = std::numeric_limits<float>::quiet_NaN();

    while (mRunning.load(std::memory_order_acquire)) {
        const int got = mRing.read(mScratch.data(), kDrainFrames);
        mTicks.fetch_add(1, std::memory_order_relaxed);

        if (got <= 0) {
            std::this_thread::sleep_for(kIdleNap);
            continue;
        }

        double sumSq = 0.0;
        for (int i = 0; i < got; ++i) {
            const double v = mScratch[static_cast<size_t>(i)];
            sumSq += v * v;
        }
        const float rms = static_cast<float>(std::sqrt(sumSq / got));
        mFramesAnalyzed += static_cast<uint64_t>(got);

        float values[kSnapshotValueCount];
        values[kSnapCaptureSampleRate] = rate;
        values[kSnapLevelRms]          = rms;
        values[kSnapFramesAnalyzed]    = static_cast<float>(mFramesAnalyzed);
        values[kSnapDroppedFrames]     = static_cast<float>(mRing.droppedFrames());
        values[kSnapState] = static_cast<float>(
            rms < kSilenceFloor ? kStateNoSignal : kStateNoLock);

        // NaN, no cero. `0.0` cents es un valor PLAUSIBLE —afinado exacto— y un
        // consumidor lo mostraria como medicion. Estos tres los llena S2; hasta
        // entonces la ausencia tiene que ser inconfundible.
        values[kSnapCents]         = nan;
        values[kSnapPhaseAngle]    = nan;
        values[kSnapUncertainty]   = nan;

        mSnapshot.publish(values);
    }
}

}  // namespace wma::analysis
