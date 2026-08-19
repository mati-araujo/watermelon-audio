#include "PhaseSlopeEstimator.h"

#include <cmath>

namespace wma::analysis {

namespace {

/// Cuantas ventanas hacen falta antes de reportar. Dos dan una recta, pero una
/// recta por dos puntos no tiene residuos y su incertidumbre seria 0 — o sea,
/// mentirosa. Con cuatro la regresion ya tiene de donde sacar un error.
constexpr int kMinWindows = 4;

/// Escala de cents por unidad de pendiente, alrededor de Δf pequeño.
/// slope [rad/ventana] → Δf = slope·fs/(2π·N) → cents = 1200·log2(1 + Δf/f_t).
double slopeToHz(double slope, int sampleRate, int windowFrames) {
    return slope * static_cast<double>(sampleRate)
           / (2.0 * M_PI * static_cast<double>(windowFrames));
}

}  // namespace

void PhaseSlopeEstimator::prepare(int sampleRate) {
    mSampleRate = sampleRate > 0 ? sampleRate : 0;

    // UNICO punto que asigna. Ver la nota de RT-safety en el header.
    mHann.assign(static_cast<size_t>(kWindowFrames), 0.0);
    for (int i = 0; i < kWindowFrames; ++i) {
        mHann[static_cast<size_t>(i)] =
            0.5 - 0.5 * std::cos(2.0 * M_PI * i / static_cast<double>(kWindowFrames));
    }
    mPhases.assign(static_cast<size_t>(kMaxWindows), 0.0);

    reset();
}

void PhaseSlopeEstimator::setTarget(double targetHz) {
    mTargetHz = targetHz;
    if (mSampleRate > 0 && targetHz > 0.0) {
        mOmega = 2.0 * M_PI * targetHz / static_cast<double>(mSampleRate);
        mCoeff = 2.0 * std::cos(mOmega);
        // Ver la nota de `mRefAdvance` en el header: sin esto se mide la fase
        // del objetivo, que no dice nada, en vez de la deriva contra el.
        mRefAdvance = std::fmod(mOmega * static_cast<double>(kWindowFrames), 2.0 * M_PI);
    } else {
        mOmega = 0.0;
        mCoeff = 0.0;
        mRefAdvance = 0.0;
    }
    // La fase acumulada contra el objetivo VIEJO no dice nada del nuevo: seguir
    // integrando sobre ella daria una pendiente que mezcla dos mediciones.
    reset();
}

void PhaseSlopeEstimator::reset() {
    mQ1 = mQ2 = mSumSq = 0.0;
    mFilled = 0;
    mCount = 0;
    mUnwrapped = 0.0;
    mLastRawPhase = 0.0;
    mHavePrevPhase = false;
    mCents = 0.0;
    mUncertaintyCents = 0.0;
    mWrappedPhase = 0.0;
    mHasSignal = false;
    mHasMeasurement = false;
    mWindowsTotal = 0;
}

bool PhaseSlopeEstimator::process(const float* mono, int numFrames) {
    if (mono == nullptr || numFrames <= 0 || mSampleRate <= 0 || mTargetHz <= 0.0) {
        return false;
    }
    if (mHann.empty()) return false;   // sin prepare() no hay donde trabajar

    bool produced = false;
    for (int i = 0; i < numFrames; ++i) {
        const double x = static_cast<double>(mono[i]);
        mSumSq += x * x;

        // Goertzel sobre la señal ENVENTANADA, incremental.
        const double xw = x * mHann[static_cast<size_t>(mFilled)];
        const double q0 = mCoeff * mQ1 - mQ2 + xw;
        mQ2 = mQ1;
        mQ1 = q0;

        if (++mFilled >= kWindowFrames) {
            closeWindow();
            produced = true;
        }
    }
    return produced;
}

void PhaseSlopeEstimator::closeWindow() {
    // --- nivel: lo primero, porque decide si esta ventana cuenta -------------
    const double rms = std::sqrt(mSumSq / static_cast<double>(kWindowFrames));
    mHasSignal = rms >= static_cast<double>(kSilenceFloor);

    const double real = mQ1 - mQ2 * std::cos(mOmega);
    const double imag = mQ2 * std::sin(mOmega);

    // Arranca la proxima ventana. Va ANTES de cualquier `return`: si no, una
    // ventana silenciosa dejaria el estado del Goertzel contaminando la que
    // sigue, y el afinador se equivocaria justo despues de un silencio.
    mQ1 = mQ2 = mSumSq = 0.0;
    mFilled = 0;
    ++mWindowsTotal;

    if (!mHasSignal) {
        // Sin señal NO se integra. Y `mHasSignal` es lo que le dice al
        // consumidor que `cents()` es viejo: la lectura anterior no se borra,
        // pero tampoco se presenta como actual.
        mHavePrevPhase = false;   // el hilo de fase se corto
        mCount = 0;
        mHasMeasurement = false;
        return;
    }

    const double raw = std::atan2(imag, real);
    mWrappedPhase = raw;

    if (!mHavePrevPhase) {
        mUnwrapped = raw;
        mLastRawPhase = raw;
        mHavePrevPhase = true;
    } else {
        // Desenvuelto: el salto real entre ventanas consecutivas esta acotado a
        // ±π, y todo lo que se vea fuera de ahi es el envolvimiento del atan2.
        //
        // Esto acota el RANGO de captura: |Δf| < fs/(2N) = 5,86 Hz a 48 kHz. En
        // A0 son ~360 cents y en C7 apenas ~4,8. No es una limitacion oculta: el
        // objetivo se lo da S4 (deteccion gruesa), y esta primitiva sólo tiene
        // que afinar alrededor de el.
        // Se le resta el avance de la REFERENCIA: lo que queda es la deriva
        // contra el objetivo, que es lo unico que mide desafinacion.
        double d = (raw - mLastRawPhase) - mRefAdvance;
        while (d > M_PI) d -= 2.0 * M_PI;
        while (d < -M_PI) d += 2.0 * M_PI;
        mUnwrapped += d;
        mLastRawPhase = raw;
    }

    // --- ventana deslizante de fases ----------------------------------------
    if (mCount < kMaxWindows) {
        mPhases[static_cast<size_t>(mCount++)] = mUnwrapped;
    } else {
        for (int i = 1; i < kMaxWindows; ++i) {
            mPhases[static_cast<size_t>(i - 1)] = mPhases[static_cast<size_t>(i)];
        }
        mPhases[static_cast<size_t>(kMaxWindows - 1)] = mUnwrapped;
    }

    if (mCount < kMinWindows) {
        mHasMeasurement = false;
        return;
    }

    // --- regresion lineal: fase contra indice de ventana ---------------------
    const int n = mCount;
    const double dn = static_cast<double>(n);
    double sumX = 0.0, sumY = 0.0, sumXX = 0.0, sumXY = 0.0;
    for (int i = 0; i < n; ++i) {
        const double x = static_cast<double>(i);
        const double y = mPhases[static_cast<size_t>(i)];
        sumX += x; sumY += y; sumXX += x * x; sumXY += x * y;
    }
    const double sxx = sumXX - sumX * sumX / dn;
    if (sxx <= 0.0) {
        mHasMeasurement = false;
        return;
    }
    const double slope = (sumXY - sumX * sumY / dn) / sxx;
    const double intercept = (sumY - slope * sumX) / dn;

    const double deltaHz = slopeToHz(slope, mSampleRate, kWindowFrames);
    mCents = 1200.0 * std::log2((mTargetHz + deltaHz) / mTargetHz);

    // --- incertidumbre: error estandar de la pendiente, en cents -------------
    double sse = 0.0;
    for (int i = 0; i < n; ++i) {
        const double r = mPhases[static_cast<size_t>(i)]
                       - (intercept + slope * static_cast<double>(i));
        sse += r * r;
    }
    const double slopeStdErr = (n > 2) ? std::sqrt(sse / (dn - 2.0) / sxx) : 0.0;
    // Los cents son casi lineales en Δf alrededor del objetivo, asi que la
    // conversion del error usa la misma derivada.
    const double centsPerHz = 1200.0 / (std::log(2.0) * mTargetHz);
    mUncertaintyCents =
        std::abs(slopeToHz(slopeStdErr, mSampleRate, kWindowFrames)) * centsPerHz;

    mHasMeasurement = true;
}

}  // namespace wma::analysis
