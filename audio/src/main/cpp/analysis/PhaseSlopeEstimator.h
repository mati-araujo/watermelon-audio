#pragma once

/**
 * @file PhaseSlopeEstimator.h
 * @brief La primitiva que decide si el producto existe (REQ-001 S2).
 *
 * POR QUE FASE Y NO FRECUENCIA
 * ----------------------------
 * 0,1 cent en E2 (82,41 Hz) son **4,76 mHz**; en B0 (30,87 Hz), **1,78 mHz**.
 * Resolver eso por FFT pediria ventanas de 210 s y 560 s. Es inviable, y por eso
 * los strobe no estiman frecuencia: miden **fase**.
 *
 * Un Goertzel a la frecuencia objetivo `f_t` sobre una ventana de N muestras
 * devuelve un fasor complejo. Si la señal real esta en `f_t + Δf`, la fase de
 * ese fasor **avanza linealmente** ventana a ventana, a razon de
 * `2π·Δf·N/fs` radianes por ventana. Se estima Δf de la PENDIENTE de esa fase.
 *
 * La precision crece con el TIEMPO DE INTEGRACION, no con el largo de la
 * ventana. De ahi salen dos consecuencias que conviene tener presentes:
 *
 *   - **Funciona mejor en graves**, al reves que un tuner por FFT. El problema
 *     del bajo de 5 cuerdas (B0 = 30,87 Hz) se disuelve antes de existir.
 *   - Es **O(N) por objetivo** y no necesita FFT, que es lo que hace posible el
 *     NFR-1 (<5 % de CPU) con varios objetivos simultaneos en S5.
 *
 * CONVENCION DE SIGNO — FIJADA, Y NO ES COSMETICA
 * -----------------------------------------------
 * **Señal por encima del objetivo ⇒ POSITIVO** (sostenido). Es lo que hace
 * cualquier afinador comercial y lo que un musico espera de la aguja. S6 hereda
 * esta convencion para el sentido de giro del disco de strobe, asi que
 * invertirla aca invierte el disco alla.
 *
 * EL TAMAÑO DE BLOQUE DEL LLAMADOR NO PUEDE CAMBIAR EL RESULTADO
 * --------------------------------------------------------------
 * El estimador acumula en su propia ventana interna de `kWindowFrames`, no en
 * los bloques que le entregan. Alimentarlo de a 16 frames o de a 1024 tiene que
 * dar **bit a bit** lo mismo: si no, el resultado depende de donde cayo el corte
 * del buffer, que es un eje que este repo ya uso para cazar un semitono de error
 * en Karplus-Strong. Hay un test que lo exige.
 *
 * RT-SAFETY
 * ---------
 * `prepare()` es el UNICO que asigna. `process()` no asigna, no loguea y no toma
 * locks: hoy lo llama el thread de analisis (no el de audio), pero la primitiva
 * tiene que poder vivir en el camino RT sin cambiar, porque S5 la va a correr
 * con varios objetivos a la vez.
 */

#include <cmath>
#include <cstddef>
#include <vector>

namespace wma::analysis {

class PhaseSlopeEstimator {
public:
    /**
     * Ventana interna del Goertzel, en frames.
     *
     * 4096 a 48 kHz son 85,3 ms, o sea ~2,6 periodos de B0 (30,87 Hz): alcanza
     * para que el fasor tenga sentido en la nota mas grave del rango y deja ~35
     * ventanas en los 3 s de integracion que pide el AC — suficientes para que
     * la regresion tenga de donde sacar una pendiente.
     *
     * Potencia de dos por costumbre del repo, no por necesidad: acá no hay FFT.
     */
    static constexpr int kWindowFrames = 4096;

    /// Ventanas que entran en la regresion. 48 a 48 kHz son ~4,1 s: cubre los
    /// 3 s del AC con margen, y acota la memoria a un `double[48]`.
    static constexpr int kMaxWindows = 48;

    /// Piso de nivel por debajo del cual se reporta "sin señal", lineal
    /// (~-60 dBFS). Igual que el del `AnalysisThread`, y por la misma razon: un
    /// afinador que mide ruido reporta basura con cara de medicion.
    static constexpr float kSilenceFloor = 0.001f;

    /**
     * Reserva el estado interno. **Unico punto que asigna.**
     * @param sampleRate rate REAL de captura, no uno asumido. Ver REQ-001 S1.
     */
    void prepare(int sampleRate);

    /// La frecuencia contra la que se mide. Cambiarla resetea la integracion:
    /// la fase acumulada contra el objetivo viejo no dice nada del nuevo.
    void setTarget(double targetHz);

    /// Deja el estimador indistinguible de recien construido y preparado.
    /// **No** borra el rate ni el objetivo: eso es configuracion, no estado.
    void reset();

    /**
     * Consume audio mono. El tamaño de `numFrames` es libre y **no puede
     * cambiar el resultado** (ver la nota de arriba).
     * @return true si esta ventana produjo una medida nueva.
     */
    bool process(const float* mono, int numFrames);

    /**
     * REQ-003 — EL DOMINIO DE VALIDEZ, EN CENTS, DE ESTE ESTIMADOR.
     *
     * El desenvuelto de `process()` pliega la diferencia de fase entre ventanas
     * a (-π, π], asi que sobre |Δf| >= fs/(2N) el resultado **aliasa**: no sale
     * NaN, sale MAL, y con σ ≈ 0 porque la pendiente aliasada sigue siendo
     * lineal. Fuera de este rango `cents()` no significa nada.
     *
     * Vive ACA y no en el que combina, a proposito (REQ-003 S1 · 1.5): el que
     * sabe hasta donde puede medir es el que mide. Un `if` del lado del
     * consumidor se lo olvida el proximo que agregue un campo.
     *
     * Devuelve 0 si no hay rate o no hay objetivo — sin ellos no hay dominio que
     * declarar, y un llamador no puede leer "0" como "todo vale".
     */
    double captureRangeCents() const noexcept {
        if (mSampleRate <= 0 || mTargetHz <= 0.0) return 0.0;
        const double dfMax = static_cast<double>(mSampleRate)
                           / (2.0 * static_cast<double>(kWindowFrames));
        return 1200.0 * std::log2(1.0 + dfMax / mTargetHz);
    }

    /// `true` si una desviacion de `cents` contra el objetivo cae dentro del
    /// dominio en el que este estimador puede medirla.
    bool canMeasureDeviation(double cents) const noexcept {
        const double range = captureRangeCents();
        return range > 0.0 && std::fabs(cents) < range;
    }

    /// Desviacion contra el objetivo. **Positivo = señal por encima**.
    /// Sin dato mientras `hasSignal()` sea false o falten ventanas.
    double cents() const noexcept { return mCents; }

    /**
     * Incertidumbre de `cents()`, en cents, del mismo signo que un error
     * estandar: sale de los residuos de la regresion.
     *
     * Existe para que la app pueda decir "todavia no convergio" en vez de
     * mostrar un numero falso — y para que un cambio brusco de nota se VEA:
     * cuando el objetivo deja de describir a la señal, los residuos saltan y
     * esto sube ANTES de que el error baje.
     */
    double uncertaintyCents() const noexcept { return mUncertaintyCents; }

    /// Angulo de fase acumulado, radianes, envuelto a ±π. Lo consume S6 para
    /// el disco: se publica ya integrado para que la app no tenga que hacerlo
    /// —un frame perdido le correria la fase para siempre.
    double phaseAngle() const noexcept { return mWrappedPhase; }

    /// false cuando el nivel esta por debajo del piso. **La lectura anterior no
    /// se congela como si fuera actual**: es la diferencia entre "no hay dato" y
    /// "el dato es viejo", y confundirlas es lo que hace mentir a un afinador.
    bool hasSignal() const noexcept { return mHasSignal; }

    /// true cuando hay al menos dos ventanas y por lo tanto una pendiente.
    bool hasMeasurement() const noexcept { return mHasMeasurement; }

    /// Ventanas completas consumidas desde el ultimo reset. Lo leen los tests
    /// para saber que la integracion avanzo, en vez de dormir y suponer.
    int windowsAnalyzed() const noexcept { return mWindowsTotal; }

private:
    void closeWindow();

    int mSampleRate{0};
    double mTargetHz{0.0};

    /// Coeficiente del Goertzel para el objetivo, precomputado en setTarget().
    double mCoeff{0.0};
    double mOmega{0.0};

    /**
     * Cuanto avanza la fase de la REFERENCIA entre ventanas: `2π·f_t·N/fs`,
     * reducido a [0, 2π).
     *
     * Hay que restarlo, y no restarlo fue el primer defecto que esta etapa
     * encontro. El Goertzel **reinicia su oscilador en cada ventana**, asi que
     * la fase que devuelve esta medida contra una referencia que arranca de cero
     * cada vez. La diferencia entre dos ventanas consecutivas no es
     * `2π·Δf·N/fs` —lo que se quiere— sino `2π·(f_t+Δf)·N/fs`, y el termino del
     * objetivo es ENORME al lado del otro: a 110 Hz son 2,43 rad contra los
     * 0,0085 rad que produce 1 cent. El desenvuelto plegaba esa diferencia a
     * ±π y la medicion salia ~70 cents equivocada, igual con y sin ruido — que
     * fue justamente la pista de que era sistematico y no un problema de SNR.
     */
    double mRefAdvance{0.0};

    /**
     * Ventana de Hann, precomputada en `prepare()`.
     *
     * NO es opcional. Con ventana rectangular, la imagen de frecuencia negativa
     * se filtra en el fasor y le mete a la fase un rizado periodico que, en las
     * notas graves, es VARIAS VECES mas grande que el avance por ventana que se
     * quiere medir: en A0 el rizado da ~0,034 rad contra 0,0085 rad de avance
     * para 1 cent. Hann lo baja lo suficiente para que la pendiente se lea. Es
     * simetrica, asi que su fase es lineal y sólo agrega un desplazamiento
     * constante — la PENDIENTE, que es lo unico que se usa, no cambia.
     */
    std::vector<double> mHann;

    /// Estado del Goertzel, que corre INCREMENTAL a medida que entran muestras.
    /// Correrlo incremental —en vez de bufferear la ventana y procesarla al
    /// final— es lo que hace que el tamaño de bloque del llamador no pueda
    /// cambiar el resultado: las mismas operaciones, en el mismo orden.
    double mQ1{0.0};
    double mQ2{0.0};
    double mSumSq{0.0};
    int mFilled{0};

    /// Fase DESENVUELTA por ventana. La regresion corre sobre esto.
    std::vector<double> mPhases;
    int mCount{0};
    double mUnwrapped{0.0};
    double mLastRawPhase{0.0};
    bool mHavePrevPhase{false};

    double mCents{0.0};
    double mUncertaintyCents{0.0};
    double mWrappedPhase{0.0};
    bool mHasSignal{false};
    bool mHasMeasurement{false};
    int mWindowsTotal{0};
};

}  // namespace wma::analysis
