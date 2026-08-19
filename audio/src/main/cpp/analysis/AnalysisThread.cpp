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
        // --- la configuracion se mira ANTES de drenar ------------------------
        //
        // El orden no es cosmetico: al cambiar el objetivo hay que descartar lo
        // que quedo en el ring, y eso sólo sirve si se hace antes de leerlo.
        const int rate = mRing.captureRate();
        const double target = mTargetHz.load(std::memory_order_acquire);

        if (rate > 0 && rate != mPreparedRate) {
            // EL RATE MEDIDO, NO 48000. Preparar el estimador con un rate
            // asumido escala todo lo que mida: a 32 kHz son +702 cents. Es el
            // mismo defecto que las tareas 1.16-1.19 sacaron del camino, y este
            // es el ultimo lugar donde se podia volver a perder — justo al
            // usarlo.
            mEstimator.prepare(rate);
            mDetector.prepare(rate);
            mPreparedRate = rate;
            mAppliedTarget = 0.0;      // `prepare()` reinicia: hay que re-aplicar
        }
        if (target != mAppliedTarget && mPreparedRate > 0) {
            mEstimator.setTarget(target);
            mAppliedTarget = target;
            // Lo que quedo en el ring es de la cuerda ANTERIOR. Ver
            // AnalysisRing::skipToNewest().
            mRing.skipToNewest();
        }
        const bool measuring = mPreparedRate > 0 && mAppliedTarget > 0.0;

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
        // `prepare()` asigna y `setTarget()` reinicia la integracion, asi que
        // llamarlos por tick tiraria la medicion antes de que converja: por eso
        // arriba se comparan contra lo ultimo aplicado.
        if (measuring) {
            mEstimator.process(mScratch.data(), got);
        }
        // La deteccion gruesa corre SIEMPRE que haya rate, con objetivo o sin el: su trabajo
        // es justamente decir que nota hay cuando nadie lo sabe todavia.
        if (mPreparedRate > 0) {
            mDetector.process(mScratch.data(), got);
        }

        float values[kSnapshotValueCount];
        values[kSnapCaptureSampleRate] = static_cast<float>(rate);
        values[kSnapLevelRms]          = rms;
        values[kSnapFramesAnalyzed]    = static_cast<float>(mFramesAnalyzed);
        values[kSnapDroppedFrames]     = static_cast<float>(mRing.droppedFrames());

        const bool haveReading =
            measuring && mEstimator.hasSignal() && mEstimator.hasMeasurement();

        if (haveReading) {
            values[kSnapCents]       = static_cast<float>(mEstimator.cents());
            values[kSnapPhaseAngle]  = static_cast<float>(mEstimator.phaseAngle());
            values[kSnapUncertainty] = static_cast<float>(mEstimator.uncertaintyCents());
        } else {
            // NaN, no cero. `0.0` cents es un valor PLAUSIBLE —afinado exacto— y
            // un consumidor lo mostraria como medicion. Sin objetivo, o antes de
            // que la integracion tenga de donde sacar una pendiente, la ausencia
            // tiene que ser inconfundible.
            values[kSnapCents]       = nan;
            values[kSnapPhaseAngle]  = nan;
            values[kSnapUncertainty] = nan;
        }

        // El estado dice EN QUE PUNTO esta la medicion, y los cuatro casos son
        // distintos para el usuario: "sin señal" pide revisar el cable, "sin
        // enganche" pide elegir una cuerda o tocar mas limpio, "midiendo" es un
        // spinner y no un error.
        int state;
        if (rms < kSilenceFloor) {
            state = kStateNoSignal;
        } else if (!measuring) {
            state = kStateNoLock;          // hay señal, pero nadie dijo contra que medir
        } else if (!haveReading) {
            state = kStateMeasuring;       // integrando, todavia sin pendiente
        } else {
            state = mEstimator.uncertaintyCents() <= kConvergedUncertaintyCents
                        ? kStateConverged
                        : kStateMeasuring;
        }
        values[kSnapState] = static_cast<float>(state);

        values[kSnapDetectedHz] = mDetector.hasPitch()
                                      ? static_cast<float>(mDetector.frequencyHz())
                                      : 0.0f;
        values[kSnapDetectionClarity] = static_cast<float>(mDetector.clarity());

        mSnapshot.publish(values);
    }
}

}  // namespace wma::analysis
