/**
 * IntonationMode.h — REQ-001 S9. El modo del 12º traste.
 *
 * Mide el ARMONICO del 12º traste y la nota PISADA en el 12º de la misma cuerda,
 * y reporta su diferencia en cents. Es lo que hace falta para ajustar los saddles
 * de un puente.
 *
 * CERO DSP NUEVO
 * ---------------
 * Es S6 corrido dos veces y una resta. Todo el trabajo de esta etapa esta en el
 * PROTOCOLO —que las dos medidas sean comparables— y en no mentir sobre que se
 * comparo.
 *
 * EL REGALO: EL ERROR DE RELOJ SE CANCELA SOLO
 * ---------------------------------------------
 * Las dos medidas salen del mismo dispositivo y van contra el MISMO objetivo (el
 * 12º traste es la octava, asi que las dos apuntan a 2·f₀). Un ADC cuyo reloj
 * corre un factor (1+ε) desplaza las DOS lecturas en `1200·log2(1+ε)` cents, y la
 * resta lo borra. O sea que este modo tiene la exactitud RELATIVA entera del
 * strobe y no la absoluta degradada: es donde el ±0,1 cent es mas defendible.
 *
 * Y por la misma razon se cancela la correccion por inarmonicidad de S7: es la
 * misma cuerda, o sea el mismo B, o sea el mismo desplazamiento en las dos.
 * **Aplicarla a UNA SOLA seria un sesgo puro**, y por eso este tipo no ofrece
 * ninguna forma de hacerlo: la correccion es propiedad de la COMPARACION, no de
 * cada medida. La 9.6 no se cumple por disciplina del llamador sino por
 * construccion.
 *
 * EL INVARIANTE, QUE ES LO QUE HACE HONESTO AL MODO
 * --------------------------------------------------
 * **No hay lectura de intonacion hasta que las DOS medidas convergieron, y si una
 * caduca, el resultado caduca con ella.** Un modo que promedie una medida
 * convergida con una que no lo esta entrega un numero con pinta de resultado.
 */
#pragma once

#include "StrobeTracker.h"

#include <cmath>

namespace wma::analysis {

class IntonationMode {
public:
    /// Las dos medidas. El armonico va primero porque es la referencia: es la que
    /// no depende de donde el musico apoye el dedo.
    enum Slot { kHarmonic = 0, kFretted = 1, kSlotCount = 2 };

    enum State {
        kNeedHarmonic = 0,   ///< falta la primera medida
        kNeedFretted  = 1,   ///< falta la segunda
        kReady        = 2,   ///< las dos convergidas y de la misma cuerda
        kStringMismatch = 3, ///< las dos existen pero NO son de la misma cuerda
    };

    void reset() noexcept {
        for (int i = 0; i < kSlotCount; ++i) {
            mHas[i] = false;
            mCents[i] = 0.0;
            mTargetHz[i] = 0.0;
        }
    }

    /**
     * Toma la medida del slot desde el strobe.
     *
     * La ACEPTA solo si el strobe converged(): una medida a medio integrar no
     * entra al calculo ni siquiera marcada, porque el unico uso posible de un
     * dato asi es restarlo por accidente.
     *
     * @return true si se guardo.
     */
    bool capture(Slot slot, const StrobeTracker& strobe) noexcept {
        if (slot < 0 || slot >= kSlotCount) return false;
        if (!strobe.converged()) return false;
        const double target = strobe.targetHz();
        if (!(target > 0.0)) return false;

        mCents[slot] = strobe.cents();
        mTargetHz[slot] = target;
        mHas[slot] = true;
        return true;
    }

    /**
     * La señal se fue o el musico cambio de cuerda: el resultado CADUCA.
     *
     * No se conserva "el ultimo valido": un numero viejo mostrado como actual es
     * peor que no tener numero, porque el usuario ajusta un saddle con el.
     */
    void invalidate() noexcept { reset(); }

    State state() const noexcept {
        if (!mHas[kHarmonic]) return kNeedHarmonic;
        if (!mHas[kFretted]) return kNeedFretted;
        return sameString() ? kReady : kStringMismatch;
    }

    /// Las dos medidas son de la misma cuerda si fueron contra el mismo objetivo.
    /// Se compara con tolerancia relativa: el objetivo lo empuja el consumidor y
    /// puede venir de un `float`.
    bool sameString() const noexcept {
        if (!mHas[kHarmonic] || !mHas[kFretted]) return false;
        const double a = mTargetHz[kHarmonic], b = mTargetHz[kFretted];
        if (!(a > 0.0) || !(b > 0.0)) return false;
        return std::abs(a - b) <= 1e-4 * a;
    }

    bool hasResult() const noexcept { return state() == kReady; }

    /**
     * Cuanto esta la nota pisada respecto del armonico, en cents.
     *
     * Positivo = la pisada suena MAS AGUDA que el armonico ⇒ hay que alargar la
     * cuerda, o sea correr el saddle hacia atras. Es la convencion de todo
     * afinador: por encima del objetivo es positivo.
     *
     * Sin resultado devuelve NaN, y no 0: cero es un valor PLAUSIBLE —intonacion
     * perfecta— y un consumidor lo mostraria como medicion.
     */
    double differenceCents() const noexcept {
        if (!hasResult()) return std::nan("");
        return mCents[kFretted] - mCents[kHarmonic];
    }

    bool has(Slot slot) const noexcept {
        return slot >= 0 && slot < kSlotCount && mHas[slot];
    }
    double capturedCents(Slot slot) const noexcept {
        return (slot >= 0 && slot < kSlotCount) ? mCents[slot] : std::nan("");
    }
    double capturedTargetHz(Slot slot) const noexcept {
        return (slot >= 0 && slot < kSlotCount) ? mTargetHz[slot] : 0.0;
    }

private:
    bool mHas[kSlotCount]{false, false};
    double mCents[kSlotCount]{0.0, 0.0};
    double mTargetHz[kSlotCount]{0.0, 0.0};
};

}  // namespace wma::analysis
