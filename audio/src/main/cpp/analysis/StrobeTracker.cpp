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
    double vals[kPartials];
    double sigmas[kPartials];
    int valid = 0;
    for (const auto& p : mPartials) {
        if (!p.hasMeasurement()) continue;
        const double sigma = p.uncertaintyCents();
        if (!(sigma > 0.0) || !std::isfinite(sigma)) continue;
        if (!std::isfinite(p.cents())) continue;
        vals[valid] = p.cents();
        sigmas[valid] = sigma;
        ++valid;
    }

    // --- descarte del parcial que NO esta midiendo la nota -------------------
    //
    // 🔴 σ NO SABE SI EL BIN TIENE SEÑAL, y esa es la falla que este bloque
    // ataja. Con el fundamental AUSENTE (el "fundamental faltante" de un bajo
    // por un parlante chico), el Goertzel apuntado a f0 sigue viendo la FUGA
    // ESPECTRAL de los armonicos: produce una fase que avanza suave, o sea un
    // ajuste lineal bueno, o sea una σ CHICA. Medido en B0 sin fundamental:
    // p0 daba -256,6 cents con σ = 0,081 mientras los otros tres daban +1,00
    // con σ ≈ 0,007. Con 1/σ² eso le tocaba apenas 0,26 % del peso — y 0,26 %
    // de -256 cents son -0,67 cents de error, casi siete veces el presupuesto.
    // O sea: una estimacion CONFIADAMENTE equivocada, que la ponderacion sola no
    // puede descartar por mas correcta que sea.
    //
    // La defensa se apoya en la propiedad que este tracker ya afirma: los cuatro
    // parciales miden LA MISMA cantidad. Se descarta el que disiente de la
    // mediana por mas de lo que ninguna cuerda real puede disentir.
    //
    // Se usa la MEDIANA y no la media porque la media ya esta contaminada por el
    // outlier que se quiere encontrar.
    if (valid > 2) {
        double sorted[kPartials];
        for (int i = 0; i < valid; ++i) sorted[i] = vals[i];
        for (int i = 1; i < valid; ++i) {           // insercion: valid ≤ 4
            const double key = sorted[i];
            int j = i - 1;
            while (j >= 0 && sorted[j] > key) { sorted[j + 1] = sorted[j]; --j; }
            sorted[j + 1] = key;
        }
        const double median = (valid % 2 == 1)
                                  ? sorted[valid / 2]
                                  : 0.5 * (sorted[valid / 2 - 1] + sorted[valid / 2]);
        int kept = 0;
        for (int i = 0; i < valid; ++i) {
            if (std::abs(vals[i] - median) > kMaxPartialDisagreementCents) continue;
            vals[kept] = vals[i];
            sigmas[kept] = sigmas[i];
            ++kept;
        }
        valid = kept;
    }

    double sumWeights = 0.0;
    double sumWeighted = 0.0;
    for (int i = 0; i < valid; ++i) {
        const double w = 1.0 / (sigmas[i] * sigmas[i]);
        if (!std::isfinite(w)) continue;
        sumWeights += w;
        sumWeighted += w * vals[i];
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
