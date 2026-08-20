#include "FastModeTracker.h"

namespace wma::analysis {

int FastModeTracker::nearest(double hz) const noexcept {
    if (mCount <= 0 || !(hz > 0.0)) return -1;
    int best = 0;
    double bestAbs = std::abs(centsBetween(hz, mCandidates[0]));
    for (int i = 1; i < mCount; ++i) {
        const double d = std::abs(centsBetween(hz, mCandidates[i]));
        if (d < bestAbs) { bestAbs = d; best = i; }
    }
    return best;
}

void FastModeTracker::update(double detectedHz, double clarity) noexcept {
    // --- sin señal ----------------------------------------------------------
    if (!(detectedHz > 0.0) || !(clarity >= kMinClarity) || !std::isfinite(detectedHz)) {
        if (++mSilent >= kSilentUpdatesToRelease) {
            release();
            mState = kNoSignal;
            mLastHz = 0.0;
        }
        // Antes del umbral se CONSERVA el enganche: una sola lectura sin señal
        // puede ser el hueco entre dos pulsaciones, y soltar ahi haria parpadear
        // la cuerda enganchada en la pantalla mientras el musico toca normal.
        return;
    }
    mSilent = 0;

    mPrevHz = mHavePrev ? mLastHz : detectedHz;
    mLastHz = detectedHz;
    const double moved =
        mHavePrev ? std::abs(centsBetween(mLastHz, mPrevHz)) : 0.0;
    const bool steady = mHavePrev && moved <= kStableCentsPerUpdate;
    // Un glissando es CONTINUO; un cambio de cuerda es DISCONTINUO. Esta es la
    // unica forma de conmutar dentro de los 150 ms: esperar a que la nota nueva
    // se asiente cuesta dos lecturas (170 ms), porque la primera todavia tiene la
    // cuerda vieja como anterior.
    const bool jumped = mHavePrev && moved > kJumpCents;
    mHavePrev = true;

    const int near = nearest(detectedHz);
    if (near < 0) { mState = kNoLock; return; }
    const double nearCents = std::abs(centsBetween(detectedHz, mCandidates[near]));

    if (mLocked < 0) {
        // --- enganche inicial: si mira distancia -----------------------------
        if (nearCents <= kLockCents) {
            mLocked = near;
            mState = kLocked;
        } else {
            mState = kNoLock;   // ninguna cuerda en rango ⇒ cromatico
        }
        return;
    }

    // --- ya enganchado: conmutar SOLO si otro es el mas cercano Y esta quieto -
    //
    // La distancia al objetivo actual NO decide. Barriendo una cuerda desde
    // floja te alejas cientos de cents del objetivo y hay que quedarse igual;
    // lo que hace conmutar es que la altura se ASIENTE cerca de otra cuerda.
    if (near != mLocked && (steady || jumped) && nearCents <= kLockCents) {
        mLocked = near;
    }
    mState = kLocked;
}

double FastModeTracker::chromaticHz() const noexcept {
    if (!(mLastHz > 0.0)) return 0.0;
    // Semitonos desde A4 = 440, redondeado a la nota mas cercana.
    const double semis = std::round(12.0 * std::log2(mLastHz / 440.0));
    return 440.0 * std::pow(2.0, semis / 12.0);
}

}  // namespace wma::analysis
