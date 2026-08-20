/**
 * FastModeTracker.h — REQ-001 S5. El modo que reconoce a que cuerda te referis.
 *
 * EL MODO DE FALLA QUE ESTA ETAPA EXISTE PARA EVITAR
 * ---------------------------------------------------
 * Cuerda nueva, afinando desde floja: barre hacia arriba y pasa cerca de varios
 * objetivos en el camino. Un afinador que siempre engancha al mas cercano
 * **salta de cuerda en cuerda mientras subis**, y es inusable justo en el momento
 * de mayor valor.
 *
 * EL DISCRIMINADOR NO ES LA DISTANCIA — ES SI LA ALTURA SE ESTA MOVIENDO
 * ----------------------------------------------------------------------
 * La primera idea es soltar el enganche cuando te alejas mucho. No sirve, y se
 * ve con dos casos que exigen respuestas opuestas:
 *
 *  · Barriendo una D3 desde floja se pasa JUSTO por A2 (a 127 Hz el candidato
 *    mas cercano es A2). Ahi hay que **no** conmutar.
 *  · Cambiando de cuerda de verdad, E2 → A2, hay que conmutar en ≤ 150 ms.
 *
 * En los dos casos el candidato mas cercano cambia y en los dos te acercas
 * mucho. Lo que los separa es que **en el barrido la altura se esta moviendo y
 * en el cambio se asienta**. Asi que el enganche conmuta cuando otro candidato es
 * el mas cercano **y la altura esta quieta**.
 *
 * LOS DOS NUMEROS ESTAN MEDIDOS, NO ELEGIDOS (tarea 5.4)
 * -------------------------------------------------------
 * Con la ventana de analisis de 85,3 ms a 48 kHz:
 *
 *   barrido de 665 cents en 8 s (lento)      →  7,09 cents/ventana
 *   barrido de 665 cents en 2 s (rapido)     → 28,37 cents/ventana
 *   nota sostenida limpia                    →  0,04 cents/ventana
 *   nota con vibrato leve                    →  0,68 cents/ventana
 *
 * `kStableCentsPerUpdate = 3` queda a **4x** del caso asentado mas ruidoso y a
 * **2,4x** del barrido mas lento. El hueco entre 0,68 y 7,09 es de un orden de
 * magnitud: no es un umbral fino.
 *
 * Y el enganche INICIAL si mira distancia: `kLockCents = 150` es menos de la
 * mitad del intervalo mas chico del catalogo de S3 (E4→G4 del ukelele, 300
 * cents), asi que dos candidatos no pueden disputarse el mismo enganche.
 *
 * EL ENGANCHE ES POR INDICE, NO POR FRECUENCIA (tarea 5.8)
 * ---------------------------------------------------------
 * Los candidatos llegan **en orden de cuerda**, no de frecuencia, y el enganche
 * guarda el INDICE. En un ukelele high-G o un banjo la cuerda 4 esta mas aguda
 * que la 3, y reportar "la cuerda mas cercana en Hz" le diria al ukelelista que
 * su cuarta esta una octava baja — el bug exacto de AC-001.15.
 */
#pragma once

#include <cmath>

namespace wma::analysis {

class FastModeTracker {
public:
    static constexpr int kMaxCandidates = 12;

    /// A cuantos cents de un candidato hay que estar para enganchar de cero.
    static constexpr double kLockCents = 150.0;

    /// Cuanto puede moverse la altura entre lecturas para considerarla quieta.
    /// Medido; ver la nota de arriba.
    static constexpr double kStableCentsPerUpdate = 3.0;

    /**
     * Un cambio mayor a esto en UNA lectura no es un glissando: es otra cuerda.
     *
     * Un glissando es CONTINUO y un cambio de cuerda es DISCONTINUO, y esa es la
     * unica forma de conmutar dentro del presupuesto de 150 ms. Esperar a que la
     * nota nueva se asiente cuesta DOS lecturas (170 ms) porque la primera todavia
     * tiene la cuerda vieja como anterior — y 170 > 150.
     *
     * Medido, en cents por ventana: barrido rapidisimo (665 cents en 2 s) 28,4;
     * barrido normal (6 s) 9,5; el cambio de cuerda mas chico del catalogo, 300.
     * Cien queda **3,5x por encima** del barrido mas rapido y **3x por debajo** del
     * cambio mas chico.
     */
    static constexpr double kJumpCents = 100.0;

    /// Claridad minima de la deteccion gruesa para creerle. Es la de S4.
    static constexpr double kMinClarity = 0.5;

    /// Lecturas sin señal seguidas antes de soltar. Una sola podria ser el hueco
    /// entre dos pulsaciones; el silencio de verdad dura.
    static constexpr int kSilentUpdatesToRelease = 3;

    enum State {
        kNoSignal = 0,  ///< no hay nada que medir
        kSearching = 1, ///< hay nota pero todavia no engancho a ninguna cuerda
        kLocked = 2,    ///< enganchado a una cuerda del instrumento
        kNoLock = 3,    ///< hay nota y NINGUNA cuerda esta en rango: cromatico
    };

    /**
     * Engancha a mano a la cuerda `index`, como cuando el musico ELIGE cual va a
     * afinar. -1 suelta.
     *
     * Hace falta porque el enganche automatico es por cercania —y tiene que
     * serlo, o AC-001.21 no podria reportar cromatico cuando ninguna cuerda esta
     * en rango—. Una cuerda **floja** esta lejos de todo: a 60 Hz hay 548 cents
     * hasta E2. Sin esta puerta, "afinar una cuerda nueva desde floja" no seria
     * expresable, que es justamente el caso que la etapa existe para servir.
     */
    void lockTo(int index) noexcept {
        if (index < 0 || index >= mCount) { release(); mState = kSearching; return; }
        mLocked = index;
        mState = kLocked;
    }

    /**
     * Los objetivos candidatos en Hz, **en orden de cuerda**. Cambiarlos suelta
     * el enganche: el indice viejo no significa nada en un instrumento nuevo.
     */
    void setCandidates(const double* hz, int count) noexcept {
        mCount = 0;
        if (hz != nullptr) {
            for (int i = 0; i < count && i < kMaxCandidates; ++i) {
                if (hz[i] > 0.0) mCandidates[mCount++] = hz[i];
            }
        }
        release();
    }

    int candidateCount() const noexcept { return mCount; }

    void reset() noexcept {
        release();
        mSilent = 0;
        mState = kNoSignal;
    }

    /**
     * Alimenta una deteccion gruesa (S4). `detectedHz <= 0` o claridad por debajo
     * del umbral cuentan como sin señal.
     */
    void update(double detectedHz, double clarity) noexcept;

    State state() const noexcept { return mState; }

    /// Indice de CUERDA enganchada, o -1. Nunca es un indice de frecuencia.
    int lockedIndex() const noexcept { return mLocked; }

    double lockedTargetHz() const noexcept {
        return (mLocked >= 0 && mLocked < mCount) ? mCandidates[mLocked] : 0.0;
    }

    /**
     * Desviacion contra el objetivo enganchado, en cents. NaN sin enganche.
     *
     * ⚠️ **ES GRUESA**: sale de la deteccion de S4, cuyo presupuesto es ±50 cents.
     * Sirve para dibujar de que lado estas y para la histeresis, **no** para el
     * ±0,5 cent que pide AC-001.2. Ese numero sale de apuntar el strobe de S6 a
     * `lockedTargetHz()`, que es lo que hace el motor — y da ±0,1.
     *
     * Se expone igual porque es lo que decide el enganche, y esconderlo obligaria
     * a recalcularlo afuera para depurar.
     */
    double centsFromTarget() const noexcept {
        if (mLocked < 0 || !(mLastHz > 0.0)) return std::nan("");
        return centsBetween(mLastHz, mCandidates[mLocked]);
    }

    /**
     * La nota cromatica mas cercana cuando NINGUNA cuerda esta en rango
     * (AC-001.21). 0 si no hay señal.
     *
     * Se computa de A4 = 440 en temperamento igual: es el respaldo, no el modelo
     * musical de S3 — que sabe de afinaciones y capos y vive en Kotlin.
     */
    double chromaticHz() const noexcept;
    double centsFromChromatic() const noexcept {
        const double c = chromaticHz();
        return (c > 0.0 && mLastHz > 0.0) ? centsBetween(mLastHz, c) : std::nan("");
    }

    static double centsBetween(double f, double reference) noexcept {
        return 1200.0 * std::log2(f / reference);
    }

private:
    void release() noexcept {
        mLocked = -1;
        mHavePrev = false;
    }

    /// El candidato mas cercano en cents, o -1 si no hay candidatos.
    int nearest(double hz) const noexcept;

    double mCandidates[kMaxCandidates]{};
    int mCount{0};

    int mLocked{-1};
    State mState{kNoSignal};

    double mLastHz{0.0};
    double mPrevHz{0.0};
    bool mHavePrev{false};
    int mSilent{0};
};

}  // namespace wma::analysis
