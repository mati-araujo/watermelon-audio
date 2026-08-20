#include "StrobeTracker.h"

namespace wma::analysis {

void StrobeTracker::prepare(int sampleRate) {
    for (auto& p : mPartials) p.prepare(sampleRate);
    // `prepare()` de cada parcial reinicia su objetivo, asi que el nuestro deja
    // de estar aplicado: re-aplicarlo es responsabilidad de quien nos prepara.
    mTargetHz = 0.0;
    mCents = 0.0;
    mUncertaintyCents = 0.0;
    mHasSignal = false;
    mHasMeasurement = false;
}

void StrobeTracker::setTarget(double fundamentalHz) {
    const double f0 = fundamentalHz > 0.0 ? fundamentalHz : 0.0;
    mTargetHz = f0;
    for (int i = 0; i < kPartials; ++i) {
        // El parcial i se apunta a (i+1)·f0. Con f0 = 0 se apagan los cuatro.
        mPartials[i].setTarget(f0 > 0.0 ? f0 * (i + 1) : 0.0);
    }
    mCents = 0.0;
    mUncertaintyCents = 0.0;
    mHasMeasurement = false;
}

void StrobeTracker::reset() {
    for (auto& p : mPartials) p.reset();
    // `reset()` de S2 tambien borra el objetivo, asi que hay que re-aplicarlo o
    // el tracker quedaria vivo pero midiendo contra nada. Se re-aplica el mismo
    // que ya teniamos: `reset()` promete "indistinguible de recien construido y
    // configurado", no "desconfigurado".
    const double f0 = mTargetHz;
    mTargetHz = 0.0;
    setTarget(f0);
    mHasSignal = false;
}

bool StrobeTracker::process(const float* mono, int numFrames) {
    if (mono == nullptr || numFrames <= 0 || mTargetHz <= 0.0) return false;

    bool anySignal = false;
    for (auto& p : mPartials) {
        p.process(mono, numFrames);
        anySignal = anySignal || p.hasSignal();
    }
    mHasSignal = anySignal;

    // --- combinacion por inverso de la varianza -----------------------------
    //
    // Solo entran los parciales que TIENEN medicion y un σ estrictamente
    // positivo. Un σ de cero no es "certeza infinita" sino una ventana que
    // todavia no tiene de donde sacar dispersion, y meterlo como 1/0 haria
    // colapsar la combinacion sobre ese unico parcial.
    double sumWeights = 0.0;
    double sumWeighted = 0.0;
    for (const auto& p : mPartials) {
        if (!p.hasMeasurement()) continue;
        const double sigma = p.uncertaintyCents();
        if (!(sigma > 0.0) || !std::isfinite(sigma)) continue;
        const double w = 1.0 / (sigma * sigma);
        if (!std::isfinite(w)) continue;
        sumWeights += w;
        sumWeighted += w * p.cents();
    }

    if (sumWeights > 0.0) {
        mCents = sumWeighted / sumWeights;
        mUncertaintyCents = std::sqrt(1.0 / sumWeights);
        mHasMeasurement = true;
    } else {
        // Sin ningun parcial utilizable no se inventa una lectura: se conserva
        // la ultima y se declara que no es actual, igual que hace S2.
        mHasMeasurement = false;
    }
    return true;
}

}  // namespace wma::analysis
