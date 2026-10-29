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
#include <cstdint>
#include <limits>
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

    /// Separacion entre rejillas vecinas (REQ-057): un cuarto de ventana.
    static constexpr int kHopFrames = kWindowFrames / 4;

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

    /**
     * REQ-027 — CUANTA DE LA ENERGIA DE LA VENTANA ESTA DE VERDAD EN ESTE BIN.
     *
     * Magnitud del fasor de Goertzel (normalizada a amplitud) dividida por el RMS
     * de la ventana. Un seno puro exacto en el objetivo da **√2 ≈ 1,414**; un bin
     * que solo ve la FUGA de otro parcial da entre 1e-06 y 3e-02.
     *
     * 🔴 EXISTE PORQUE σ NO PUEDE CONTESTAR ESTA PREGUNTA. Un bin sin señal
     * propia integra la fuga espectral del vecino, que avanza de fase suave: da
     * un ajuste lineal bueno, o sea σ CHICA, o sea una lectura confiadamente
     * equivocada. Medido sobre tono puro en E2, ese parcial publicaba +38,70
     * cents con σ = 0,0028 y estado CONVERGIDO. `uncertaintyCents()` dice **que
     * tan bien encaja una recta**; esto dice **si habia algo que medir**.
     *
     * Es del ULTIMO ventaneo cerrado, igual que `cents()`. Vale 0 mientras no
     * haya cerrado ninguno.
     *
     * El nombre es deliberadamente largo y especifico: un `magnitude()` o un
     * `level()` mas en el arbol vuelve ambigua alguna llamada del walker de
     * `check-rt-safety.py` y le apaga cobertura con el lint en verde — paso dos
     * veces el 2026-08-19.
     */
    double goertzelBinToRmsRatio() const noexcept { return mBinToRmsRatio; }

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

    /**
     * REQ-036 — LA FASE QUE NO ES UNA RECTA NO SE ADMITE COMO MEDICION (R-PITCH-62).
     *
     * Un glide de ataque dentro de la ventana deja la fase como un palo de hockey, y una recta
     * ajustada a un palo de hockey deja residuos CHICOS: `uncertaintyCents()` sale de esos
     * residuos, asi que es CIEGA al glide — medido en S1: sobre 24 glides sinteticos la lectura
     * salia CONVERGIDA a 0,34 s con σ ≤ 0,0004 y hasta −13,6 c de error; sobre el corpus, hasta
     * 0,73 c con σ 0,054. Y no lo ve la σ del ajuste entre parciales tampoco: un glide sesga a los
     * cuatro por igual.
     *
     * Lo que SI lo ve es la TENDENCIA: las dos mitades de la ventana tienen pendientes distintas.
     * En cada cierre se regresan por separado y se publican dos numeros:
     *   · `phaseTrendScore()`      T = (b₂ − b₁) / √(se₁² + se₂²) — la significancia contra el
     *                              ruido de la propia ventana;
     *   · `phaseTrendDeltaCents()` Δ = (b₂ − b₁) en cents — la magnitud. Hace falta ademas de T
     *                              porque sobre señal limpia T SATURA (la discrepancia y su error
     *                              escalan juntos): una cola de 0,001 c da el mismo T que una de 1 c.
     * El estimador solo MIDE; quien decide que hacer con eso es el strobe (`StrobeTracker`), con
     * sus umbrales, igual que con el piso de energia de REQ-027.
     *
     * 🔴 EL VEREDICTO VALE DESDE 12 VENTANAS, NO DESDE 8 (que es lo que dos mitades con residuos
     * necesitan). En las cuerdas mas graves a 44,1 kHz (B0, E1) el rizado de la imagen negativa
     * deja a las mitades de una ventana corta con pendientes distintas sobre una nota ESTABLE
     * (|T| hasta 4,2 con |Δ| 0,33 c en n = 8–11, 3,57 con 0,14 c en n = 12, 1,49 con 0,09 desde 16)
     * con la lectura correcta a +0,007 c. Con menos de 12 fases `phaseTrendEvaluable()` es false y
     * el strobe no admite el parcial: la primera CONVERGIDA posible pasa de 4 ventanas (0,34 s) a 12
     * (1,02 s a 48 k, 1,11 a 44,1). Con 16 seria mas seguro sobre sintesis, pero las notas cortas
     * del ukelele (1,49–1,81 s) dejan de converger: medido sobre el corpus, 30 contra 35.
     */
    static constexpr int kMinWindowsForTrendVerdict = 12;

    bool phaseTrendEvaluable() const noexcept { return mTrendEvaluable; }
    double phaseTrendScore() const noexcept { return mTrendScore; }
    double phaseTrendDeltaCents() const noexcept { return mTrendDeltaCents; }

    /**
     * REQ-036 — LA VENTANA ADAPTATIVA (R-PITCH-63): reinicia la integracion en el punto de quiebre.
     *
     * Descarta la mitad VIEJA de las fases y sigue con la nueva, sin tocar el Goertzel ni la fase
     * acumulada: la señal es la misma, lo que cambia es desde donde se la regresa. La lectura y la
     * tendencia se recalculan sobre lo que queda.
     *
     * 🔴 POR QUE NO ALCANZA CON NO ADMITIR. Mientras el glide esta en la ventana esta en la primera
     * mitad, asi que una compuerta que solo excluye tarda hasta que el glide SALE de la ventana
     * entera: medido, 3,3–3,8 s despues de que la nota se asienta, contra los 3 s del AC — y sobre
     * el corpus deja 16–21 lecturas convergidas de 33. Reiniciando en el quiebre: ≤ 1,42 s, y 35.
     */
    void restartKeepingNewestHalf();

    /**
     * REQ-057 — como `restartKeepingNewestHalf()`, pero conservando las `keep` fases mas nuevas
     * (acotado a las que hay). Las rejillas 1-3 de `InterleavedPhaseEstimator` la usan para
     * descartar toda ventana que empiece antes que la mas vieja que conservo la rejilla 0.
     */
    void restartKeepingNewestPhases(int keep);

    /**
     * REQ-057 (D-A) — EL CONTEXTO DE LA SERIE DENSA para el PROXIMO cierre.
     *
     * Lo fija `InterleavedPhaseEstimator` justo antes de que cierre esta rejilla: `valid` dice que
     * los cinco `raw` de los ultimos cierres densos existen (ninguno fue silencio),
     * `priorIncrements` son los tres primeros incrementos `wrap(raw_{j+1} - raw_j -
     * refHopAdvance())` y `prevDenseRaw` el `raw` del cierre denso inmediato anterior. Con el, el
     * cierre completa los cuatro incrementos `e_j`, `S = Σ e_j`, y con `M = round((S - d)/2π)`
     * distinto de cero suma las vueltas que el desenvuelto de 4096 no puede ver (`d += 2π·M`).
     *
     * GUARDA DE COHERENCIA: `M` se aplica SOLO si los cuatro `e_j` son coherentes entre si
     * (`kDenseCoherenceMaxRad`). Un patron periodico (huecos de captura sin reportar) hace que la
     * serie densa parezca una recta a otra frecuencia, con `M != 0` y los `e_j` dispares: ahi no se
     * aplica y el incremento queda marcado sin resolver.
     *
     * 🔴 SIN CONTEXTO VALIDO EL CIERRE HACE EXACTAMENTE LA CUENTA DE SIEMPRE, y el contexto se
     * consume: vale para UN cierre y se borra. Un cierre silencioso lo borra tambien.
     */
    void setDenseWrapContext(bool valid, const double* priorIncrements, double prevDenseRaw) noexcept {
        mCtxValid = valid;
        for (int i = 0; i < 3; ++i) mCtxPrior[i] = valid ? priorIncrements[i] : 0.0;
        mCtxPrevRaw = prevDenseRaw;
    }

    /**
     * Cota de `max_j |wrap(e_j - mediana(e))|` para aplicar `M` (rad), tal como la expone
     * `lastCoherenceSpreadRad()` en cada cierre con `M != 0`.
     *
     * FUENTE VERSIONADA: `InterleavedGrids.TheCoherenceThresholdSitsBetweenHealthyClosesAndAnUnreportedGap`
     * mide e imprime las dos cifras (`[coherencia] ...`, propiedades `healthyMaxRad` y `gapMinRad`):
     *   · cadenas sanas (B0, E2, G3, A4, E5 × ±50/±25 c × 44,1/48 k × ruido 0/0,01 × B 0/1e-4/5e-4,
     *     3 s, parciales dentro de su rango): maximo 0,095 rad;
     *   · hueco de captura sin reportar (`CaptureDiscontinuity.NobodyReportedItSoNobodyCanKnow`, A4 a
     *     48 k, 1024 frames cada 4 bloques): minimo 0,370 rad, maximo 0,755.
     * Medicion del scratchpad de la etapa, NO versionada (barrido grande, guarda apagada, B tambien
     * 1e-4, 246 022 cierres sanos): maximo sano 0,178 rad; 285 cierres del hueco, 0,370 a 0,755.
     * 0,3 deja ≥ 1,7x sobre lo sano y 19 % bajo el hueco PERIODICO. Un hueco AISLADO no reportado
     * deja cierres a 0,272–0,274 rad que se aceptan (ver
     * `InterleavedGrids.ASingleUnreportedLostGapStillPublishesAConvergedReadingOffByLessThanOneCent`).
     */
    static constexpr double kDenseCoherenceMaxRad = 0.3;

    /// Cuanto avanza la fase de la referencia en `kHopFrames`: `fmod(ω·1024, 2π)`.
    double refHopAdvance() const noexcept { return mRefHop; }

    /// El `atan2` del ultimo cierre con señal. Solo vale si `lastCloseProducedPhase()`.
    double lastRawPhase() const noexcept { return mLastRawPhase; }

    /// false si el ultimo cierre fue silencioso (rms < `kSilenceFloor`): no hay fase nueva.
    bool lastCloseProducedPhase() const noexcept { return mLastCloseProducedPhase; }

    /// Vueltas de 2π (en valor absoluto) que la serie densa agrego a los incrementos desde el reset.
    int turnsAdded() const noexcept { return mTurnsAdded; }

    /// `max_j |wrap(e_j - mediana(e))|` (rad) del ultimo cierre, o −1 si ese cierre no llego a
    /// calcularlo (silencio, sin contexto de cadena, o `M = 0`). Es la cantidad que la guarda
    /// compara con `kDenseCoherenceMaxRad`, se calcule o no el rechazo.
    double lastCoherenceSpreadRad() const noexcept { return mLastCoherenceSpread; }

    /**
     * REQ-057 (invariante 3) — `true` si alguna fase de la ventana de regresion tiene un incremento
     * que se resolvio SIN la cadena densa completa. Mientras sea true, el rango de captura efectivo
     * de esta rejilla es el de hop 4096: ese incremento solo es confiable dentro de el.
     */
    bool regressionHasUnresolvedIncrement() const noexcept {
        for (int i = 0; i < mCount; ++i) {
            if (mUnresolved[static_cast<size_t>(i)]) return true;
        }
        return false;
    }

    /**
     * REQ-036 S1 — SONDA DE SOLO LECTURA sobre la ventana de regresion: cuantas fases tiene hoy
     * (`regressionPhaseCount()`, hasta `kMaxWindows`) y la fase desenvuelta de la ventana `i`
     * (`regressionPhaseAt(i)`, la mas vieja en 0). Es EXACTAMENTE lo que la regresion de
     * `closeWindow()` acaba de ajustar.
     *
     * Existe porque σ es ciega a un glide que sesga a todos los parciales por igual: los residuos
     * de una recta ajustada a un palo de hockey son chicos. Para elegir el estadistico de
     * TENDENCIA que lo delate (y su umbral, con nota estable y vibrato como controles) el test
     * tiene que poder leer las fases que el estimador ya almacena, no reconstruirlas con un
     * segundo Goertzel que seria otra fuente de verdad. Produccion no la llama: esta declarada
     * `sonda-de-tests` en `scripts/mechanism-callers-baseline.txt`. S2 mueve el estadistico
     * adentro de `closeWindow()`.
     */
    int regressionPhaseCount() const noexcept { return mCount; }
    double regressionPhaseAt(int i) const noexcept { return mPhases[static_cast<size_t>(i)]; }

    // ---- REQ-058 S1 (D-3): SEGMENTOS — pendiente comun, intercepto por segmento ----------------
    //
    // Cada fase guardada lleva el id del segmento en que cerro su ventana. Con UN solo id en la ventana
    // la regresion es la de siempre (`regressPhaseLine`, bit a bit); con varios, la pendiente es COMUN y
    // cada segmento tiene su intercepto: `b = Σ_s Σ(x−x̄_s)(y−ȳ_s) / Σ_s Σ(x−x̄_s)²`, g.l. `n − S − 1`. La
    // tendencia de R-PITCH-62 se calcula igual, por mitades, cada mitad con sus interceptos. El
    // estimador solo regresa: quien decide que segmentos se agrupan es `StrobeTracker`.

    /// El id que llevan las fases que cierren desde ahora.
    void setPhaseSegment(unsigned char id) noexcept { mSegmentId = id; }
    /// Cuantos segmentos distintos hay en la ventana de regresion. Sonda para tests.
    int regressionSegmentCount() const noexcept;
    /// Descarta la fase mas NUEVA (su ventana contiene el ataque) y recalcula.
    void dropNewestPhase();
    /// Conserva solo las fases del segmento `id` (descarta los demas) y recalcula.
    void keepOnlyPhasesOfSegment(unsigned char id);
    /// Descarta las fases del segmento `id` y recalcula.
    void dropPhasesOfSegment(unsigned char id);

private:
    void closeWindow();
    /// La regresion y la tendencia sobre `mPhases[0, mCount)`. La llaman `closeWindow()` y
    /// `restartKeepingNewestHalf()`: es aritmetica fija sobre ≤ 48 fases, sin asignar.
    void regressOverWindow();

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
    /// Paralelo a `mPhases`: el incremento que llego a esa fase se resolvio sin cadena completa.
    std::vector<unsigned char> mUnresolved;
    /// REQ-058 (D-3): paralelo a `mPhases`, el id de segmento de cada fase. Arreglo fijo del objeto.
    unsigned char mPhaseSegment[kMaxWindows]{};
    unsigned char mSegmentId{0};
    /// Compacta la ventana conservando las fases con `keep(i)` verdadero, y recalcula.
    template <typename Keep>
    void compactPhases(Keep keep);
    int mCount{0};
    double mUnwrapped{0.0};
    double mLastRawPhase{0.0};
    bool mHavePrevPhase{false};

    double mCents{0.0};
    double mUncertaintyCents{0.0};
    /// REQ-036: la tendencia del ultimo cierre. Ver `phaseTrendScore()`.
    double mTrendScore{0.0};
    double mTrendDeltaCents{0.0};
    bool mTrendEvaluable{false};
    /// Ver `goertzelBinToRmsRatio()`. Se recalcula en cada `closeWindow()`.
    double mBinToRmsRatio{0.0};
    double mWrappedPhase{0.0};
    /// REQ-057: ver `setDenseWrapContext()`.
    bool mCtxValid{false};
    double mCtxPrior[3]{0.0, 0.0, 0.0};
    double mCtxPrevRaw{0.0};
    double mRefHop{0.0};
    bool mLastCloseProducedPhase{false};
    int mTurnsAdded{0};
    double mLastCoherenceSpread{-1.0};
    bool mHasSignal{false};
    bool mHasMeasurement{false};
    int mWindowsTotal{0};
};

/**
 * REQ-057 S1 — CUATRO REJILLAS INTERCALADAS (hop 1024) SOBRE EL ESTIMADOR DE HOY.
 *
 * Cada rejilla es un `PhaseSlopeEstimator` (ventana 4096, regresion y tendencia de siempre) que
 * arranca `g·1024` frames despues de la anterior, o sea que cada 1024 frames cierra exactamente
 * una. Los cierres de las cuatro forman la SERIE DENSA, y el `raw` de los ultimos cinco es la
 * CADENA DE VUELTAS: la rejilla que cierra pliega su diferencia de 4096 a ±π como siempre (`d`), y
 * la cadena dice cuantas vueltas de 2π le faltan (`M`, ver `setDenseWrapContext()`). Dentro del
 * rango de hoy `M = 0` y la rejilla 0 es, bit a bit, el estimador de hoy; fuera de el, la serie
 * densa desenvuelve hasta `|Δf| < fs/(2·1024)`, cuatro veces mas lejos.
 *
 * La cadena NO es la serie que se regresa: sobrevive a un quiebre de fase (el audio es contiguo)
 * y se borra con `reset()`, `setTarget()`, `prepare()` y con todo cierre silencioso.
 *
 * RT-SAFETY: `prepare()` es el unico que asigna; el resto es aritmetica fija sobre arrays de
 * tamaño fijo.
 */
class InterleavedPhaseEstimator {
public:
    static constexpr int kGrids = 4;
    static constexpr int kHopFrames = PhaseSlopeEstimator::kHopFrames;

    /**
     * El margen del rango denso (D28): el borde del hop 1024 es el nulo del lobulo principal de
     * Hann (±2 bins = ±fs/2048), y un parcial admitido ahi lee mal. Medido por el architect en
     * prototipo (REQ-057 D-C): hasta 0,85 del rango, 0 fallas > 0,1 c; a 0,90, 3 de 120 (A1 p4 a
     * +145 c con ruido); a 0,93-0,98, parciales que leen hasta 52 c mal.
     */
    static constexpr double kRangeMargin = 0.9;

    /// `1200·log2(1 + κ·fs/(2·hop·f))`: el rango de un parcial apuntado a `targetHz`. 0 sin rate
    /// o sin objetivo.
    static double denseRangeCents(int sampleRate, double targetHz) noexcept {
        if (sampleRate <= 0 || targetHz <= 0.0) return 0.0;
        const double dfMax = kRangeMargin * static_cast<double>(sampleRate)
                           / (2.0 * static_cast<double>(kHopFrames));
        return 1200.0 * std::log2(1.0 + dfMax / targetHz);
    }

    void prepare(int sampleRate);
    void setTarget(double targetHz);
    void reset();

    /// @return true si cerro la rejilla 0 (la que se publica) en esta llamada.
    bool process(const float* mono, int numFrames);

    /// Rejilla `g` (0..3). La 0 es el estimador de hoy.
    const PhaseSlopeEstimator& grid(int g) const noexcept { return mGrid[g]; }

    /**
     * El rango del parcial: el denso, salvo que la rejilla 0 arrastre un incremento resuelto sin
     * cadena completa (invariante 3 de D-A), que solo es confiable dentro del rango de hop 4096.
     */
    ///
    /// 🔴 NO SE LLAMA `captureRangeCents`/`canMeasureDeviation` COMO LOS DEL ESTIMADOR SUELTO: un
    /// segundo metodo con el mismo nombre simple vuelve ambigua la llamada y `check-rt-safety.py`
    /// deja de seguirla (medido: el walker perdio `PhaseSlopeEstimator::captureRangeCents`).
    double denseCaptureRangeCents() const noexcept {
        if (mGrid[0].regressionHasUnresolvedIncrement()) return mGrid[0].captureRangeCents();
        return denseRangeCents(mSampleRate, mTargetHz);
    }

    bool canMeasureDenseDeviation(double cents) const noexcept {
        const double range = denseCaptureRangeCents();
        return range > 0.0 && std::fabs(cents) < range;
    }

    /// El quiebre de fase (R-PITCH-63) en las cuatro rejillas a la vez: la 0 conserva su mitad
    /// nueva (`count - count/2`) y las 1-3 descartan toda ventana que empiece antes que la mas
    /// vieja que conservo la 0 (`min(count, keep0 - 1)`). La cadena de vueltas no se toca.
    void restartAtBreak();

    // ---- REQ-058 S1 (D-1): LA SERIE DENSA DEL SEGMENTO Y SU AJUSTE CORTO -------------------------
    //
    // En cada cierre con fase, sea cual sea la rejilla, la fase cruda entra a una serie DESENVUELTA:
    // `D_k = D_{k-1} + wrap(raw_k - raw_{k-1} - refHop)`, el mismo incremento `e_j` de la cadena de
    // vueltas (dentro del rango denso es exacto). Sobre las ultimas `n = min(cuenta, kEarlyWindow)`
    // fases se ajusta un cuadratico CENTRADO `D = a + b·u + c·(u² − ū²)`, `u = i − (n−1)/2`: con abscisas
    // simetricas `b` es la frecuencia en el CENTRO de la ventana (lo que la cuerda hizo, no una
    // prediccion: R-PITCH-67) y `c` la curvatura (el glide). Lo consume el camino temprano de
    // `StrobeTracker`; la rejilla 0 y su regresion no se tocan (bit a bit).
    //
    // La serie es la del SEGMENTO vigente: una ventana que empieza antes de `openDenseSegmentAt()` no
    // entra, y un cierre silencioso la vacia. Sobrevive a un quiebre de R-PITCH-63 (es lo que publica
    // despues de uno). Memoria del objeto, no de `prepare()`.

    /// Fases de la serie densa que se guardan. 64 = 16 ventanas de 4096: holgado sobre `kEarlyWindow`.
    static constexpr int kMaxDensePhases = 64;

    /**
     * W = 8 fases densas (D-1). Medido por el architect en prototipo sobre corpus-v1 (REQ-058 S1,
     * «Umbrales provisionales»): cobertura de 2σ 0,677 con W = 8 contra 0,661 (12) y 0,645 (16), y p90
     * de `t_primera` 0,276 s contra 0,342 s. La ventana corta es la que sigue al glide.
     */
    static constexpr int kEarlyWindow = 8;

    /**
     * n ≥ 5 (D-1): el cuadratico tiene 3 parametros, asi que con 5 fases quedan 2 grados de libertad
     * para la σ de los residuos. Con 4 (1 g.l.) se adelanta 23 ms y la σ empeora (prototipo).
     * Primera lectura posible: 4096 + 4·1024 = 8192 frames desde el inicio del segmento.
     */
    static constexpr int kEarlyMinPhases = 5;

    /**
     * La correccion por SOLAPE de 75 % de las ventanas Hann (D-1), por n = 4..8 (indice n − 4). La
     * correlacion del ruido entre fases vecinas es `ρ(d) = Σw(m)w(m+d·1024)/Σw²` = 1 / 0,6592 / 0,1667 /
     * 0,0075 (d = 0..3), y el factor es √(varianza verdadera del estimador MCO bajo esa covarianza /
     * la que reporta la formula iid con el `s²` de los residuos, en esperanza). Es geometria de la
     * ventana (como la ganancia coherente N/4 de REQ-027), no una calibracion: la recomputa de la Hann
     * `EarlyReading.OverlapFactorMatchesTheHannWindow` (tolerancia 1e-4).
     */
    static constexpr double kOverlapSlopeFactor[5] = {3.3263, 2.6546, 2.3470, 2.1783, 2.0730};
    static constexpr double kOverlapCurvatureFactor[5] = {2.1429, 1.9814, 1.8901, 1.8358, 1.8004};
    // El ajuste corto indexa estas tablas con `n - 4`, n en [kEarlyMinPhases, kEarlyWindow]: que nunca se
    // salga (security-auditor de REQ-058).
    static_assert(kEarlyMinPhases >= 4 && kEarlyMinPhases <= kEarlyWindow, "n minimo del ajuste corto fuera de rango");
    static_assert(kEarlyWindow - 4 < static_cast<int>(sizeof(kOverlapSlopeFactor) / sizeof(kOverlapSlopeFactor[0])) &&
                      kEarlyWindow - 4 < static_cast<int>(sizeof(kOverlapCurvatureFactor) / sizeof(kOverlapCurvatureFactor[0])),
                  "kEarlyWindow excede las tablas del factor de solape");

    /// El ajuste corto de la serie densa (D-1), por parcial. `n == 0` = no hay ajuste (menos de
    /// `kEarlyMinPhases` fases, o sin objetivo).
    struct DenseEarlyFit {
        int n = 0;
        double cents = 0.0;        ///< de `b`: la frecuencia del centro de la ventana
        double sigmaCents = 0.0;   ///< error estandar de `b` con residuos del cuadratico × f_s(n)
        double driftCents = 0.0;   ///< `c·(n−1)` CON SIGNO: cuanto cambia la frecuencia entre el centro y el borde
        double driftSigmaCents = 0.0;   ///< error estandar de `driftCents` × f_c(n)
        double binToRmsRatio = 0.0;     ///< energia del bin en la ventana del ULTIMO cierre denso
    };

    /// El ultimo ajuste corto. Se recalcula UNA vez por cierre denso (D-6), no por llamada.
    const DenseEarlyFit& denseEarlyFit() const noexcept { return mEarlyFit; }

    /// Abre un segmento (D-3): vacia la serie densa y desde ahora no acepta ventanas que empiecen
    /// antes de `startFrame` (frames desde el ultimo reset de este estimador). La rejilla 0 y la
    /// cadena de vueltas no se tocan.
    void openDenseSegmentAt(int64_t startFrame) noexcept;

    /// Cuantas fases tiene la serie densa del segmento. Sonda para tests.
    int densePhaseCount() const noexcept { return mDenseCount; }

    /// Frames consumidos desde el ultimo reset (la grilla de 1024 de las rejillas parte de 0).
    int64_t framesSinceReset() const noexcept { return mFrame; }

    /// El ajuste corto (D-1) sobre una serie dada: puro, para que un test lo ejercite sin audio.
    static DenseEarlyFit fitDensePhases(const double* phases, int count, int sampleRate, double targetHz) noexcept;

    // ---- REQ-058 S1 (D-3): el segmento en el camino del VEREDICTO ----------------------------------

    /// Abre el segmento `id` en las cuatro rejillas desde `startFrame`: descarta la fase de la ventana que
    /// contiene el ataque (empezo en `(startFrame − 4096, startFrame)`) y, de ahora en mas, las que cierren
    /// habiendo empezado ahi; las siguientes llevan `id`. Las fases viejas quedan con su id: agruparlas o
    /// descartarlas lo decide `StrobeTracker` (`keepOnlyPhaseSegment` / `dropPhaseSegment`).
    void openPhaseSegment(int64_t startFrame, unsigned char id);
    void keepOnlyPhaseSegment(unsigned char id);
    void dropPhaseSegment(unsigned char id);
    /// `true` si alguna rejilla tiene fases en su ventana de regresion.
    bool hasRegressionPhases() const noexcept {
        for (const auto& g : mGrid)
            if (g.regressionPhaseCount() > 0) return true;
        return false;
    }

private:
    PhaseSlopeEstimator mGrid[kGrids];
    int mSampleRate{0};
    double mTargetHz{0.0};
    double mDense[kMaxDensePhases]{};
    int mDenseCount{0};
    bool mDenseHavePrev{false};
    double mDensePrevRaw{0.0};
    int64_t mDenseSegmentStart{0};
    DenseEarlyFit mEarlyFit{};
    /// D-3: las ventanas que empezaron en `(mRejectWindowsBefore − 4096, mRejectWindowsBefore)` contienen
    /// el ataque y no entran a la regresion. −1 = ninguna.
    int64_t mRejectWindowsBefore{-1};
    /// Inicio de la ventana de la fase mas nueva de cada rejilla (−1 = sin fase).
    int64_t mNewestWindowStart[kGrids]{-1, -1, -1, -1};
    /// Frames consumidos desde el ultimo reset: la rejilla `g` ve el audio desde `g·kHopFrames`.
    /// 64 bits: en armeabi-v7a/x86 `long` son 32 y desbordaria a las 2^31 frames (12,4 h a 48 k).
    int64_t mFrame{0};
    /// Los `raw` de los ultimos cinco cierres densos (el mas nuevo al final) y si cada uno tenia fase.
    double mChainRaw[kGrids + 1]{};
    bool mChainOk[kGrids + 1]{};
    int mChainCount{0};

public:
    using FrameCounter = decltype(mFrame);
    static_assert(std::numeric_limits<FrameCounter>::digits >= 63,
                  "mFrame debe sobrevivir 2^31 frames en ABIs de 32 bits");
private:
    void clearChain() noexcept;
    void pushChain(double raw, bool ok) noexcept;
    void clearDenseSeries() noexcept;
    void pushDensePhase(const PhaseSlopeEstimator& closing, int64_t windowStart) noexcept;
};

}  // namespace wma::analysis
