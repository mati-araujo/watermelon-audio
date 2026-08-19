#include "AnalysisThread.h"

#include <chrono>
#include <cmath>
#include <limits>

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
    // El rate que llega aca es la SEMILLA: describe lo que se sabia al
    // arrancar. La fuente viva es el estampado del escritor, que viaja con las
    // muestras (ver AnalysisRing::setCaptureRate). Sembrar el ring en vez de
    // guardar una copia propia deja UNA sola fuente de verdad.
    if (captureSampleRate > 0 && mRing.captureRate() <= 0) {
        mRing.setCaptureRate(captureSampleRate);
    }
    mThread = std::thread([this] { drainLoop(); });
}

void AnalysisThread::stop() {
    if (!mRunning.exchange(false, std::memory_order_acq_rel)) {
        if (mThread.joinable()) mThread.join();
        return;
    }
    if (mThread.joinable()) mThread.join();
}

void AnalysisThread::drainLoop() {
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

        // Se lee POR TICK, no una vez: es lo unico que hace que un cambio de
        // rate en caliente aparezca en el snapshot siguiente.
        float values[kSnapshotValueCount];
        values[kSnapCaptureSampleRate] = static_cast<float>(mRing.captureRate());
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
