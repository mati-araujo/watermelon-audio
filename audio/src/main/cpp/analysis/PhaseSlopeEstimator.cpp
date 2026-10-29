#include "PhaseSlopeEstimator.h"

#include <algorithm>
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

/**
 * Regresion lineal de `y[0..n)` contra el indice: pendiente, ordenada y error estandar de la
 * pendiente a partir de los residuos. Es la misma cuenta que siempre hizo `closeWindow()`, sacada a
 * una funcion porque REQ-036 la corre tres veces por cierre (la ventana entera y sus dos mitades).
 * @return false si no hay dispersion en x (n < 2).
 */
bool regressPhaseLine(const double* y, int n, double* slope, double* intercept, double* slopeStdErr) {
    if (n < 2) return false;
    const double dn = static_cast<double>(n);
    double sumX = 0.0, sumY = 0.0, sumXX = 0.0, sumXY = 0.0;
    for (int i = 0; i < n; ++i) {
        const double x = static_cast<double>(i);
        sumX += x; sumY += y[i]; sumXX += x * x; sumXY += x * y[i];
    }
    const double sxx = sumXX - sumX * sumX / dn;
    if (sxx <= 0.0) return false;
    *slope = (sumXY - sumX * sumY / dn) / sxx;
    *intercept = (sumY - *slope * sumX) / dn;
    double sse = 0.0;
    for (int i = 0; i < n; ++i) {
        const double r = y[i] - (*intercept + *slope * static_cast<double>(i));
        sse += r * r;
    }
    *slopeStdErr = (n > 2) ? std::sqrt(sse / (dn - 2.0) / sxx) : 0.0;
    return true;
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
    mUnresolved.assign(static_cast<size_t>(kMaxWindows), 0);

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
        mRefHop = std::fmod(mOmega * static_cast<double>(kHopFrames), 2.0 * M_PI);
    } else {
        mOmega = 0.0;
        mCoeff = 0.0;
        mRefAdvance = 0.0;
        mRefHop = 0.0;
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
    mTrendScore = 0.0;
    mTrendDeltaCents = 0.0;
    mTrendEvaluable = false;
    mBinToRmsRatio = 0.0;
    mWrappedPhase = 0.0;
    mHasSignal = false;
    mHasMeasurement = false;
    mWindowsTotal = 0;
    mCtxValid = false;
    mLastCloseProducedPhase = false;
    mTurnsAdded = 0;
    mLastCoherenceSpread = -1.0;
    std::fill(mUnresolved.begin(), mUnresolved.end(), static_cast<unsigned char>(0));
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

    // REQ-027 — cuanta de la energia de la ventana esta en ESTE bin.
    //
    // `kWindowFrames/4` es la ganancia coherente del Goertzel bajo Hann (la
    // ventana suma N/2, y el fasor reparte en dos imagenes), asi que dividir por
    // ella devuelve la AMPLITUD. Contra el RMS de la ventana, un seno puro exacto
    // da √2. Todo aca es aritmetica sobre lo que ya estaba calculado: no asigna,
    // no loguea y no toma locks — la primitiva tiene que poder vivir en el
    // camino RT (ver la nota de RT-SAFETY del header).
    const double binAmplitude =
        std::sqrt(real * real + imag * imag) / (static_cast<double>(kWindowFrames) / 4.0);
    mBinToRmsRatio = (rms > 0.0) ? binAmplitude / rms : 0.0;

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
        mLastCloseProducedPhase = false;
        mLastCoherenceSpread = -1.0;
        mCtxValid = false;        // un cierre silencioso invalida la cadena densa
        mCount = 0;
        mHasMeasurement = false;
        return;
    }

    const double raw = std::atan2(imag, real);
    mLastCloseProducedPhase = true;
    mLastCoherenceSpread = -1.0;
    bool unresolved = false;

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
        if (mCtxValid) {
            // REQ-057 (D-A): el cuarto incremento cierra la cadena de cinco cierres densos. Si la
            // suma de los cuatro (cada uno acotado a ±π por su propio pliegue) difiere de `d` en
            // vueltas enteras, esas vueltas son las que el pliegue de 4096 no pudo ver.
            double e[4] = {mCtxPrior[0], mCtxPrior[1], mCtxPrior[2], (raw - mCtxPrevRaw) - mRefHop};
            while (e[3] > M_PI) e[3] -= 2.0 * M_PI;
            while (e[3] < -M_PI) e[3] += 2.0 * M_PI;
            const double turns = std::round((e[0] + e[1] + e[2] + e[3] - d) / (2.0 * M_PI));
            if (!std::isfinite(turns)) {
                // Un Inf en el audio (o un objetivo Inf) da `raw` NaN: `NaN != 0` es true y la guarda de
                // coherencia no lo frena (`std::max(worst, NaN)` devuelve `worst`), asi que sin esto se
                // convertia NaN a int, que es UB. Sin vuelta que sumar, el incremento queda sin resolver.
                unresolved = true;
            } else if (turns != 0.0) {
                // Mediana de 4 = promedio de los dos centrales, sin ordenar un array: el minimo y el
                // maximo salen de la suma menos los extremos.
                double lo = e[0], hi = e[0];
                for (int j = 1; j < 4; ++j) { lo = std::min(lo, e[j]); hi = std::max(hi, e[j]); }
                const double median = 0.5 * (e[0] + e[1] + e[2] + e[3] - lo - hi);
                double worst = 0.0;
                for (int j = 0; j < 4; ++j) {
                    double dev = e[j] - median;
                    while (dev > M_PI) dev -= 2.0 * M_PI;
                    while (dev < -M_PI) dev += 2.0 * M_PI;
                    worst = std::max(worst, std::fabs(dev));
                }
                mLastCoherenceSpread = worst;
                if (worst < kDenseCoherenceMaxRad) {
                    d += 2.0 * M_PI * turns;
                    mTurnsAdded += static_cast<int>(std::fabs(turns));
                } else {
                    unresolved = true;
                }
            }
        } else {
            unresolved = true;
        }
        mUnwrapped += d;
        mLastRawPhase = raw;
    }
    mCtxValid = false;   // el contexto vale para UN cierre

    // El angulo que consume el strobe es la fase de la DESAFINACION acumulada,
    // envuelta a ±π — no la fase cruda del Goertzel.
    //
    // Publicar `raw` fue un defecto, y lo encontro 2.11c: `raw` lleva adentro el
    // giro del PROPIO objetivo, que a 110 Hz avanza 2,43 rad por ventana contra
    // los 0,034 que produce 1 cent de desafinacion. Un disco girando a eso no
    // representa nada: gira rapido y en un sentido que depende de donde cayo el
    // alias de la frecuencia objetivo contra la ventana, no de si la cuerda esta
    // alta o baja.
    mWrappedPhase = std::remainder(mUnwrapped, 2.0 * M_PI);

    // --- ventana deslizante de fases ----------------------------------------
    if (mCount < kMaxWindows) {
        mUnresolved[static_cast<size_t>(mCount)] = unresolved ? 1 : 0;
        mPhases[static_cast<size_t>(mCount++)] = mUnwrapped;
    } else {
        for (int i = 1; i < kMaxWindows; ++i) {
            mPhases[static_cast<size_t>(i - 1)] = mPhases[static_cast<size_t>(i)];
            mUnresolved[static_cast<size_t>(i - 1)] = mUnresolved[static_cast<size_t>(i)];
        }
        mPhases[static_cast<size_t>(kMaxWindows - 1)] = mUnwrapped;
        mUnresolved[static_cast<size_t>(kMaxWindows - 1)] = unresolved ? 1 : 0;
    }

    regressOverWindow();
}

void PhaseSlopeEstimator::regressOverWindow() {
    if (mCount < kMinWindows) {
        mHasMeasurement = false;
        mTrendEvaluable = false;
        return;
    }

    // --- regresion lineal: fase contra indice de ventana ---------------------
    const int n = mCount;
    double slope = 0.0, intercept = 0.0, slopeStdErr = 0.0;
    if (!regressPhaseLine(mPhases.data(), n, &slope, &intercept, &slopeStdErr)) {
        mHasMeasurement = false;
        mTrendEvaluable = false;
        return;
    }

    const double deltaHz = slopeToHz(slope, mSampleRate, kWindowFrames);
    mCents = 1200.0 * std::log2((mTargetHz + deltaHz) / mTargetHz);

    // --- incertidumbre: error estandar de la pendiente, en cents -------------
    // Los cents son casi lineales en Δf alrededor del objetivo, asi que la
    // conversion del error usa la misma derivada.
    const double centsPerHz = 1200.0 / (std::log(2.0) * mTargetHz);
    mUncertaintyCents =
        std::abs(slopeToHz(slopeStdErr, mSampleRate, kWindowFrames)) * centsPerHz;

    mHasMeasurement = true;

    // --- REQ-036: la tendencia entre las dos mitades (ver el header) ---------
    //
    // Va DESPUES de la medicion y no la toca: el estimador publica cents, σ y ademas T y Δ;
    // quien no admite es el strobe. Aritmetica fija sobre ≤ 48 fases, sin asignar.
    mTrendEvaluable = false;
    if (n >= kMinWindowsForTrendVerdict) {
        const int mid = n / 2;
        double s1 = 0.0, i1 = 0.0, e1 = 0.0, s2 = 0.0, i2 = 0.0, e2 = 0.0;
        if (regressPhaseLine(mPhases.data(), mid, &s1, &i1, &e1) &&
            regressPhaseLine(mPhases.data() + mid, n - mid, &s2, &i2, &e2)) {
            const double diff = s2 - s1;
            const double se = std::sqrt(e1 * e1 + e2 * e2);
            mTrendScore = se > 0.0 ? diff / se : (diff == 0.0 ? 0.0 : 1e300);
            mTrendDeltaCents = slopeToHz(diff, mSampleRate, kWindowFrames) * centsPerHz;
            mTrendEvaluable = true;
        }
    }
}

void PhaseSlopeEstimator::restartKeepingNewestHalf() {
    restartKeepingNewestPhases(mCount - mCount / 2);
}

void PhaseSlopeEstimator::restartKeepingNewestPhases(int keepWanted) {
    if (mCount <= 0) return;
    const int keep = std::clamp(keepWanted, 0, mCount);
    const int from = mCount - keep;
    for (int i = 0; i < keep; ++i) {
        mPhases[static_cast<size_t>(i)] = mPhases[static_cast<size_t>(from + i)];
        mUnresolved[static_cast<size_t>(i)] = mUnresolved[static_cast<size_t>(from + i)];
    }
    mCount = keep;
    regressOverWindow();
}

// ---------------------------------------------------------------------------
// REQ-057 S1 — InterleavedPhaseEstimator
// ---------------------------------------------------------------------------

namespace {
double wrapToPi(double x) {
    while (x > M_PI) x -= 2.0 * M_PI;
    while (x < -M_PI) x += 2.0 * M_PI;
    return x;
}
}  // namespace

InterleavedPhaseEstimator::DenseEarlyFit InterleavedPhaseEstimator::fitDensePhases(const double* phases, int count,
                                                                                   int sampleRate,
                                                                                   double targetHz) noexcept {
    DenseEarlyFit f;
    const int n = std::min(count, kEarlyWindow);
    if (phases == nullptr || n < kEarlyMinPhases || sampleRate <= 0 || targetHz <= 0.0) return f;
    const double* y = phases + (count - n);
    // Abscisas centradas: Σu = 0 y Σu·(u² − ū²) = 0, asi que las tres columnas son ortogonales y cada
    // coeficiente sale de su propia suma. `b` es a la vez el lineal del cuadratico y la pendiente de
    // la recta sola.
    const double half = 0.5 * static_cast<double>(n - 1);
    double su2 = 0.0, ym = 0.0;
    for (int i = 0; i < n; ++i) {
        const double u = static_cast<double>(i) - half;
        su2 += u * u;
        ym += y[i];
    }
    ym /= static_cast<double>(n);
    const double u2m = su2 / static_cast<double>(n);
    double suy = 0.0, sv2 = 0.0, svy = 0.0;
    for (int i = 0; i < n; ++i) {
        const double u = static_cast<double>(i) - half;
        const double v = u * u - u2m;
        suy += u * (y[i] - ym);
        sv2 += v * v;
        svy += v * (y[i] - ym);
    }
    const double b = suy / su2;
    const double c = sv2 > 0.0 ? svy / sv2 : 0.0;
    double rss = 0.0;
    for (int i = 0; i < n; ++i) {
        const double u = static_cast<double>(i) - half;
        const double r = y[i] - (ym + b * u + c * (u * u - u2m));
        rss += r * r;
    }
    const double s2 = rss / static_cast<double>(n - 3);
    const int fi = n - 4;   // n en [kEarlyMinPhases, kEarlyWindow] = [5, 8]
    const double seB = std::sqrt(s2 / su2) * kOverlapSlopeFactor[fi];
    const double seC = sv2 > 0.0 ? std::sqrt(s2 / sv2) * kOverlapCurvatureFactor[fi] : 0.0;
    const double hzPerRad = static_cast<double>(sampleRate) / (2.0 * M_PI * static_cast<double>(kHopFrames));
    const double centsPerHz = 1200.0 / (std::log(2.0) * targetHz);
    const double span = static_cast<double>(n - 1);
    f.n = n;
    f.cents = 1200.0 * std::log2(1.0 + b * hzPerRad / targetHz);
    f.sigmaCents = std::fabs(seB * hzPerRad * centsPerHz);
    f.driftCents = c * span * hzPerRad * centsPerHz;
    f.driftSigmaCents = std::fabs(seC * span * hzPerRad * centsPerHz);
    if (!std::isfinite(f.cents) || !std::isfinite(f.sigmaCents) || !std::isfinite(f.driftCents) ||
        !std::isfinite(f.driftSigmaCents)) {
        return DenseEarlyFit{};   // NaN/Inf en el audio: no hay ajuste, no una lectura inventada
    }
    return f;
}

void InterleavedPhaseEstimator::clearDenseSeries() noexcept {
    mDenseCount = 0;
    mDenseHavePrev = false;
    mDensePrevRaw = 0.0;
    mEarlyFit = DenseEarlyFit{};
}

void InterleavedPhaseEstimator::openDenseSegmentAt(int64_t startFrame) noexcept {
    clearDenseSeries();
    mDenseSegmentStart = startFrame;
}

void InterleavedPhaseEstimator::pushDensePhase(const PhaseSlopeEstimator& closing, int64_t windowStart) noexcept {
    if (!closing.lastCloseProducedPhase()) {   // silencio: el hilo de fase se corto
        clearDenseSeries();
        return;
    }
    if (windowStart < mDenseSegmentStart) return;   // la ventana empezo antes del segmento
    const double raw = closing.lastRawPhase();
    double d = 0.0;
    if (mDenseHavePrev && mDenseCount > 0) {
        d = mDense[mDenseCount - 1] + wrapToPi(raw - mDensePrevRaw - mGrid[0].refHopAdvance());
    }
    if (mDenseCount == kMaxDensePhases) {
        for (int i = 1; i < kMaxDensePhases; ++i) mDense[i - 1] = mDense[i];
        --mDenseCount;
    }
    mDense[mDenseCount++] = d;
    mDensePrevRaw = raw;
    mDenseHavePrev = true;
    mEarlyFit = fitDensePhases(mDense, mDenseCount, mSampleRate, mTargetHz);
    mEarlyFit.binToRmsRatio = closing.goertzelBinToRmsRatio();
}

void InterleavedPhaseEstimator::clearChain() noexcept {
    mChainCount = 0;
    for (int i = 0; i <= kGrids; ++i) { mChainRaw[i] = 0.0; mChainOk[i] = false; }
}

void InterleavedPhaseEstimator::pushChain(double raw, bool ok) noexcept {
    for (int i = 0; i < kGrids; ++i) { mChainRaw[i] = mChainRaw[i + 1]; mChainOk[i] = mChainOk[i + 1]; }
    mChainRaw[kGrids] = raw;
    mChainOk[kGrids] = ok;
    if (mChainCount <= kGrids) ++mChainCount;
}

void InterleavedPhaseEstimator::prepare(int sampleRate) {
    mSampleRate = sampleRate > 0 ? sampleRate : 0;
    mTargetHz = 0.0;
    for (auto& g : mGrid) g.prepare(sampleRate);
    mFrame = 0;
    clearChain();
    openDenseSegmentAt(0);
}

void InterleavedPhaseEstimator::setTarget(double targetHz) {
    mTargetHz = targetHz > 0.0 ? targetHz : 0.0;
    for (auto& g : mGrid) g.setTarget(targetHz);
    mFrame = 0;
    clearChain();
    openDenseSegmentAt(0);
}

void InterleavedPhaseEstimator::reset() {
    for (auto& g : mGrid) g.reset();
    mFrame = 0;
    clearChain();
    openDenseSegmentAt(0);
}

bool InterleavedPhaseEstimator::process(const float* mono, int numFrames) {
    if (mono == nullptr || numFrames <= 0 || mSampleRate <= 0 || mTargetHz <= 0.0) return false;

    constexpr int64_t kHop = kHopFrames;
    constexpr int64_t kWindow = PhaseSlopeEstimator::kWindowFrames;
    bool closedGrid0 = false;
    int pos = 0;
    while (pos < numFrames) {
        // Segmentos que no cruzan un multiplo de 1024: a lo sumo una rejilla cierra, y al final.
        const int64_t toBoundary = kHop - (mFrame % kHop);
        const int seg = static_cast<int>(std::min<int64_t>(toBoundary, numFrames - pos));
        const int64_t endFrame = mFrame + seg;
        int closing = -1;
        if (endFrame % kHop == 0 && endFrame >= kWindow) {
            closing = static_cast<int>((endFrame / kHop - kGrids) % kGrids);
            // Cuatro cierres anteriores mas este = la cadena de cinco. Los tres incrementos que ya
            // estan completos se suman aca; el cuarto lo cierra la rejilla con su propio `raw`.
            const bool valid = mChainCount >= kGrids && mChainOk[1] && mChainOk[2] && mChainOk[3] && mChainOk[4];
            double prior[3] = {0.0, 0.0, 0.0};
            if (valid) {
                const double ref = mGrid[0].refHopAdvance();
                for (int i = 1; i < kGrids; ++i) prior[i - 1] = wrapToPi(mChainRaw[i + 1] - mChainRaw[i] - ref);
            }
            mGrid[closing].setDenseWrapContext(valid, prior, mChainRaw[kGrids]);
        }
        for (int g = 0; g < kGrids; ++g) {
            if (endFrame <= static_cast<int64_t>(g) * kHop) continue;   // la rejilla g aun no arranco
            if (mGrid[g].process(mono + pos, seg) && g == 0) closedGrid0 = true;
        }
        mFrame = endFrame;
        pos += seg;
        if (closing >= 0) {
            pushChain(mGrid[closing].lastRawPhase(), mGrid[closing].lastCloseProducedPhase());
            pushDensePhase(mGrid[closing], endFrame - kWindow);   // REQ-058 (D-1)
        }
    }
    return closedGrid0;
}

void InterleavedPhaseEstimator::restartAtBreak() {
    const int count0 = mGrid[0].regressionPhaseCount();
    const int keep0 = count0 - count0 / 2;
    mGrid[0].restartKeepingNewestPhases(keep0);
    for (int g = 1; g < kGrids; ++g) {
        mGrid[g].restartKeepingNewestPhases(std::min(mGrid[g].regressionPhaseCount(), std::max(keep0 - 1, 0)));
    }
}

}  // namespace wma::analysis
