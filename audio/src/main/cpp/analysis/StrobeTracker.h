/**
 * StrobeTracker.h — REQ-001 S6. El modo que justifica el producto.
 *
 * Enganche de fase sobre el fundamental **y los 3 primeros armonicos**, con el
 * angulo publicado para que la app dibuje un disco que gira de verdad.
 *
 * POR QUE CUATRO PARCIALES Y NO SOLO EL FUNDAMENTAL
 * -------------------------------------------------
 * 1. En cuerdas graves el fundamental puede estar 20 dB por debajo del segundo
 *    parcial: medir solo el fundamental es medir la parte mas debil de la señal.
 * 2. Es el insumo de S7. El DESACUERDO entre los armonicos *es* la
 *    inarmonicidad, asi que rastrear cuatro fases deja el coeficiente B a la
 *    vista sin volver a analizar la señal (ver `partialCents`).
 *
 * LA COMBINACION SE PONDERA POR 1/σ², NO POR SNR
 * -----------------------------------------------
 * La tarea 6.10 pedia ponderar por SNR de cada parcial, y `PhaseSlopeEstimator`
 * no expone SNR — su archivo es de S2, que esta cerrada. No hace falta que la
 * matriz ceda: `uncertaintyCents()` sale del error estandar de la regresion de
 * pendiente, o sea un σ por parcial que YA codifica el SNR (un parcial debil da
 * una pendiente ruidosa y un σ grande).
 *
 * Ponderar por inverso de la varianza ademas vuelve cierta POR CONSTRUCCION la
 * propiedad que 6.2 exige medir:
 *
 *     σ²_combinado = 1 / Σ(1/σ²ᵢ)  ≤  min(σ²ᵢ)
 *
 * o sea que la lectura de cuatro parciales no PUEDE salir peor que la del
 * fundamental solo. Es la combinacion lineal insesgada de minima varianza.
 *
 * TODOS LOS PARCIALES MIDEN LA MISMA CANTIDAD, Y POR ESO SE PUEDEN PROMEDIAR
 * --------------------------------------------------------------------------
 * El parcial n se apunta a `n·f0`. Si la cuerda esta a `f0·(1+δ)`, su parcial n
 * esta a `n·f0·(1+δ)`: la desviacion en CENTS es la misma para los cuatro. Lo
 * que se promedia son cuatro estimaciones de un mismo numero, no cuatro numeros
 * distintos. (Con inarmonicidad dejan de coincidir — y ese desacuerdo es
 * justamente lo que S7 va a leer.)
 *
 * 🔴 EL RANGO DE CAPTURA SE ESTRECHA CON EL ORDEN DEL PARCIAL. El desenvuelto de
 * S2 acota |Δf| < fs/(2N) — 5,86 Hz a 48 kHz. El parcial n se desvia n veces mas
 * en Hz para la misma desviacion en cents, asi que su rango en cents es 1/n del
 * fundamental. Por eso este modo se especifica sobre CUERDAS (la mas aguda, A4)
 * y no sobre el rango A0-C7 entero, y por eso corre DESPUES de la deteccion
 * gruesa de S4, que ya dejo el objetivo cerca.
 *
 * RT: `prepare()` es del thread de control y es lo unico que asigna. `process()`
 * no asigna ni toma locks — corre en el thread de analisis.
 */
#pragma once

#include "PhaseSlopeEstimator.h"

#include <cmath>

namespace wma::analysis {

class StrobeTracker {
public:
    /// Fundamental + 3 armonicos.
    static constexpr int kPartials = 4;

    /// Por debajo de esta incertidumbre la lectura se declara convergida, en
    /// cents. Es el presupuesto del producto; `AnalysisThread` usa el mismo.
    static constexpr double kConvergedUncertaintyCents = 0.1;

    /**
     * Cuanto puede disentir un parcial de la mediana antes de considerarlo
     * "no esta midiendo la nota", en cents.
     *
     * El numero sale de la fisica, no del gusto: la inarmonicidad corre el
     * parcial n en `600·log2(1 + B·n²)`, y aun con el `B ≈ 5e-4` de una bordona
     * gruesa el 4to parcial se corre **~7 cents**. Cincuenta deja pasar con
     * holgura toda cuerda real —incluido lo que S7 va a medir— y descarta la
     * fuga espectral, que en el caso medido daba **-256 cents**. Entre 7 y 256
     * hay lugar de sobra para un umbral que no sea un ajuste fino.
     */
    static constexpr double kMaxPartialDisagreementCents = 50.0;

    void prepare(int sampleRate);

    /**
     * El fundamental contra el que se mide. 0 = ninguno.
     *
     * Cambiarlo REINICIA la integracion de los cuatro parciales: la fase
     * acumulada contra el objetivo viejo no dice nada del nuevo.
     */
    void setTarget(double fundamentalHz);

    double targetHz() const noexcept { return mTargetHz; }

    void reset();

    bool process(const float* mono, int numFrames);

    /// Desviacion combinada contra el objetivo, en cents. Solo vale si
    /// `hasMeasurement()`.
    double cents() const noexcept { return mCents; }

    /// σ de `cents()`. Decrece al integrar, y es lo que declara la convergencia.
    double uncertaintyCents() const noexcept { return mUncertaintyCents; }

    /// El angulo del disco, envuelto a (-π, π]. Es la fase de la desafinacion
    /// ACUMULADA del fundamental: gira a velocidad proporcional a la desviacion
    /// en cents, y al perder la señal se CONGELA en vez de saltar a cero.
    double phaseAngle() const noexcept { return mPartials[0].phaseAngle(); }

    bool hasSignal() const noexcept { return mHasSignal; }
    bool hasMeasurement() const noexcept { return mHasMeasurement; }
    bool converged() const noexcept {
        return mHasMeasurement && mUncertaintyCents <= kConvergedUncertaintyCents;
    }

    // --- 6.12: las cuatro fases, para que S7 no re-analice ------------------
    double partialCents(int i) const noexcept { return mPartials[i].cents(); }
    double partialUncertaintyCents(int i) const noexcept {
        return mPartials[i].uncertaintyCents();
    }
    double partialPhase(int i) const noexcept { return mPartials[i].phaseAngle(); }
    bool partialHasMeasurement(int i) const noexcept {
        return mPartials[i].hasMeasurement();
    }
    double partialTargetHz(int i) const noexcept {
        return mTargetHz > 0.0 ? mTargetHz * (i + 1) : 0.0;
    }

private:
    PhaseSlopeEstimator mPartials[kPartials];
    double mTargetHz{0.0};
    double mCents{0.0};
    double mUncertaintyCents{0.0};
    bool mHasSignal{false};
    bool mHasMeasurement{false};
};

}  // namespace wma::analysis
