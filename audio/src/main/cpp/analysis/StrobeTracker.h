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

#include <algorithm>
#include <cmath>
#include <limits>

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
     * El piso sale de la fisica: la inarmonicidad corre el parcial n en
     * `600·log2(1 + B·n²)`, y aun con el `B ≈ 5e-4` de una bordona gruesa el 4to
     * parcial se corre **~7 cents**. Cincuenta deja pasar con holgura toda
     * cuerda real, incluido lo que S7 va a medir.
     *
     * 🔴 EL TECHO ES MUCHO MAS AJUSTADO DE LO QUE PARECE, Y POR ESO EXISTE LA
     * REGLA DEL MEJOR PARCIAL. Cuando un solo parcial tiene energia, la mediana
     * la fijan los tres que ven FUGA, y esa mediana se aleja tanto mas cuanto mas
     * ancho es el rango de captura — o sea en los graves. Barrido de 32 casos
     * degenerados (4 cuerdas x 4 "solo el parcial k"): en las medias-altas la
     * desviacion del mejor parcial no pasa de 12 cents, pero **en E1 con solo el
     * 4to parcial llega a 40,46**. Contra un umbral de 50 eso es 1,2x de margen,
     * no un orden de magnitud.
     *
     * Subir el umbral no es la salida —lo acercaria a los 256 de la fuga que hay
     * que descartar—. La salida es que al parcial MEJOR MEDIDO no se lo descarte
     * nunca, y entonces el margen fino deja de decidir nada.
     */
    static constexpr double kMaxPartialDisagreementCents = 50.0;

    /**
     * REQ-027 — EL PISO DE ADMISION POR ENERGIA DEL BIN.
     *
     * Un parcial cuyo bin no tiene señal propia integra la FUGA espectral del
     * vecino. La fuga avanza de fase suave, asi que da un ajuste lineal bueno, o
     * sea σ chica: sale CONVERGIDA y equivocada. El filtro de mediana no la
     * agarra — con tono puro exacto en E2 queda a 41,96 cents de la mediana
     * contra un umbral de 50, o sea el 83,9 % del margen, y pasa.
     *
     * DE DONDE SALE EL NUMERO. `|bin|/rms` de un parcial CON energia va de 0,296
     * (el cuarto de una cuerda 1/n) a 1,414 (seno puro). La fuga llega a
     * **2,63e-02** en el peor caso del catalogo, que es B0 (30,87 Hz) a 48 kHz:
     * ahi el bin del segundo parcial esta a solo 2,63 bins del fundamental,
     * contra 7,65 bins en E2 a 44,1 kHz. 0,05 deja 1,9x sobre esa fuga y 5,9x por
     * debajo del parcial legitimo mas debil.
     *
     * 🔴 EL PRIMER VALOR CONSIDERADO FUE 0,02, y estaba MAL por medir la muestra
     * equivocada: sobre las seis cuerdas de guitarra la peor fuga es 9,04e-04 y
     * el margen parecia de 327x. B0 a 48 kHz lo pasa por arriba, o sea que 0,02
     * habria dejado el defecto intacto justo en la cuerda mas grave. El margen
     * real del catalogo es **11,3x**, y se aprieta a ~3,4x con caida armonica
     * 1/n² — que es lo que vigila el criterio de muerte de REQ-027.
     *
     * 🔴 HUBO UN SEGUNDO PISO, RELATIVO AL PARCIAL MAS FUERTE, Y SE FUE POR
     * MUTACION. Se lo justificaba diciendo que "sigue a la señal cuando el nivel
     * baja, para que un decaimiento no descarte parciales de a uno". El
     * razonamiento estaba **al reves**: los dos pisos se combinaban con AND, asi
     * que el relativo solo podia descartar MAS, nunca sostener a un parcial
     * debil — o sea que empeoraba exactamente el caso que decia proteger. Y su
     * mutante (ponerlo en 0) sobrevivia con la suite entera en verde. Una defensa
     * que ningun mutante mata y cuyo argumento apunta para el otro lado es una
     * linea que el proximo va a creer load-bearing. Si el corpus que decae de S3
     * pide algo asi, vuelve CON el test que mata a su mutante.
     */
    static constexpr double kMinBinToRmsRatio = 0.05;

    /**
     * REQ-027 S2 — CUANTOS PARCIALES HACEN FALTA PARA AJUSTAR `C` Y `B`.
     *
     * Tres, y el numero sale de los grados de libertad, no del gusto: el modelo
     * tiene DOS parametros, asi que con dos puntos el ajuste es exacto, los
     * residuos valen cero y σ seria arbitrariamente chica. Una σ confiada y falsa
     * es exactamente lo que REQ-027 existe para sacar, asi que con k ≤ 2 se
     * conserva la combinacion por inverso de la varianza de siempre.
     *
     * Medido: con k = 2 el ajuste recupera C exacto (−12,0000) pero su σ da NaN.
     * Se prefiere el statu quo a un numero que no significa nada.
     */
    static constexpr int kMinPartialsForStretchFit = 3;

    /// Techo de la busqueda de B. 5e-3 cubre con holgura el rango publicado de
    /// cuerdas reales (~1e-5 nylon a ~5e-4 acero); `physicsB()` da 1,28e-5 para
    /// la prima de una guitarra y 1,05e-4 para su bordona.
    static constexpr double kMaxInharmonicityB = 5e-3;

    /// Iteraciones de la busqueda por seccion aurea. FIJAS, para que el costo sea
    /// acotado y la funcion siga siendo apta para el camino RT: 40 dejan el
    /// intervalo en 5e-3·0,618⁴⁰ ≈ 4e-11, muy por debajo de lo que cualquier
    /// medicion puede distinguir.
    static constexpr int kStretchFitIterations = 40;

    /**
     * El estiramiento inarmonico del parcial `n`, en cents: `600·log2(1+B·n²)`.
     *
     * Es el modelo EXACTO y no su linealizacion `K·n²`. 🔴 LA RAZON NO ES LA QUE
     * ESTE COMENTARIO DECIA, y la corrigio un mutante que sobrevivio. Decia que
     * la lineal erra 0,11 cents "y apagaria CONVERGIDO sobre cuerdas sanas": es
     * falso, porque esa cuenta compara los dos modelos al MISMO B, y dentro del
     * ajuste B es libre y absorbe casi toda la diferencia. Medido, a −12 cents:
     *
     *     B        C exacto / σ         C lineal / σ
     *     1e-04    -12,0000 / 0,00000   -11,9998 / 0,00007
     *     1e-03    -12,0000 / 0,00000   -11,9823 / 0,00722
     *
     * La lectura de la lineal queda DENTRO del presupuesto. Lo que se rompe es σ:
     * 0,0072 sobre una cuerda que el modelo describe perfectamente es error de
     * MODELO disfrazado de discrepancia de MEDICION, 140x el del exacto. Y que σ
     * signifique lo que dice es la entrega entera de esta etapa, asi que el
     * modelo exacto se paga con un `log2` y se queda.
     */
    static double stretchCents(double B, int n) noexcept {
        return 600.0 * std::log2(1.0 + B * static_cast<double>(n) * static_cast<double>(n));
    }

    /**
     * Ajusta `cents_n = C + 600·log2(1+B·n²)` a los parciales admitidos.
     *
     * Para B fijo, C es lineal (es la media de los residuos), asi que alcanza con
     * una busqueda 1-D acotada sobre B. Sin asignar, sin loguear, sin locks y con
     * iteraciones fijas: apta para el camino RT.
     *
     * 🔴 EL AJUSTE ES NO PONDERADO, A PROPOSITO. Ponderar por 1/σ² dejaria que el
     * parcial de σ mas chica domine, y la σ del estimador de fase es una
     * PRECISION —cuan bien encaja una recta— y no una exactitud. Ponderar por
     * ella es como la precision se vuelve a hacer pasar por exactitud, que es el
     * defecto que este REQ arregla.
     *
     * @param outC       la desviacion del FUNDAMENTAL, en cents.
     * @param outSigmaC  su incertidumbre, sacada de los RESIDUOS del ajuste.
     * @return false si no hay grados de libertad para una σ con sentido.
     */
    static bool fitStretchedSeries(const double* cents, const int* orders, int k,
                                   double* outC, double* outSigmaC) noexcept {
        if (k < kMinPartialsForStretchFit) return false;

        const auto sseFor = [&](double B, double* Cout) {
            double sum = 0.0;
            for (int i = 0; i < k; ++i) sum += cents[i] - stretchCents(B, orders[i]);
            const double C = sum / static_cast<double>(k);
            double sse = 0.0;
            for (int i = 0; i < k; ++i) {
                const double r = cents[i] - (C + stretchCents(B, orders[i]));
                sse += r * r;
            }
            if (Cout != nullptr) *Cout = C;
            return sse;
        };

        constexpr double kPhi = 0.6180339887498949;
        double lo = 0.0;
        double hi = kMaxInharmonicityB;
        double b1 = hi - kPhi * (hi - lo);
        double b2 = lo + kPhi * (hi - lo);
        double f1 = sseFor(b1, nullptr);
        double f2 = sseFor(b2, nullptr);
        for (int it = 0; it < kStretchFitIterations; ++it) {
            if (f1 < f2) {
                hi = b2; b2 = b1; f2 = f1;
                b1 = hi - kPhi * (hi - lo); f1 = sseFor(b1, nullptr);
            } else {
                lo = b1; b1 = b2; f1 = f2;
                b2 = lo + kPhi * (hi - lo); f2 = sseFor(b2, nullptr);
            }
        }

        double C = 0.0;
        const double sse = sseFor(0.5 * (lo + hi), &C);
        const int dof = k - 2;
        if (dof <= 0 || !std::isfinite(sse) || !std::isfinite(C)) return false;

        *outC = C;
        // σ del intercepto a partir de los RESIDUOS. No se propaga la σ por
        // parcial: ver la nota de arriba.
        *outSigmaC = std::sqrt(sse / static_cast<double>(dof) / static_cast<double>(k));
        return true;
    }

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

    /**
     * @brief El audio que alimenta esta integracion dejo de ser CONTIGUO (REQ-009 S2).
     *
     * La llama `AnalysisThread` cuando el ring piso frames entre dos vueltas: es
     * el unico que puede saberlo —tiene el ring— y este es el unico que sabe que
     * hacer con la noticia, porque es el que tiene la ventana.
     *
     * QUE HACE: tira la integracion y la vuelve a arrancar, dejando el objetivo,
     * el rate y la señal en su lugar. **Es la misma regla que el repo ya acepta
     * para el cambio de objetivo** —`PhaseSlopeEstimator::setTarget()` llama a
     * `reset()` porque *"la fase acumulada contra el objetivo VIEJO no dice nada
     * del nuevo: seguir integrando sobre ella daria una pendiente que mezcla dos
     * mediciones"*— y el argumento vale palabra por palabra para un hueco: la
     * fase acumulada ANTES del hueco no dice nada de la señal de DESPUES.
     *
     * 🔴 POR QUE CUALQUIER Δ > 0 Y NO UN UMBRAL. El barrido de la spec (P6) dejo
     * un caso incomodo: con 4.096 frames pisados la lectura salio EXACTA, y con
     * 6.144 ya estaba 1,4 cents afuera. Elegir un numero entre esos dos habria
     * que defenderlo con evidencia que no existe. Reiniciar siempre cuesta
     * **latencia de aguja, no correctitud**: el caso de 4.096 tarda un poco mas
     * en dibujar, y ninguna lectura se publica mezclando dos trozos de señal.
     *
     * 🔴 NO ES `reset()` A SECAS, Y LA DIFERENCIA ES AC-009.3. Ademas de tirar la
     * integracion levanta una marca que sobrevive hasta que la integracion
     * vuelva a tener una medicion PROPIA —o sea entera de audio posterior al
     * hueco—. Sin ella el consumidor ve "midiendo" y no puede distinguir
     * *"todavia no"* (esperar) de *"la entrada llego rota"* (revisar el cable),
     * que son dos cosas distintas para el usuario.
     */
    void noteInputDiscontinuity();

    /**
     * `true` mientras la integracion viva arrastre un hueco: se levanta en
     * `noteInputDiscontinuity()` y se baja sola cuando vuelve a haber medicion,
     * que es el momento en que la ventana entera es audio de despues del hueco.
     *
     * Es el insumo de `kSnapInputDiscontinuity` (AC-009.3).
     */
    bool sawInputDiscontinuity() const noexcept { return mSawDiscontinuity; }

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

    /**
     * REQ-003 (AC-003.7) — EL CONTROL, que tiene que ser INDEPENDIENTE DE LA FASE.
     *
     * `hz` es la frecuencia que midio la deteccion gruesa. 0 = no hay control.
     * Con el se decide QUE PARCIALES estan dentro de su dominio; sin el, esa
     * pregunta no se puede contestar y `domainVerified()` queda en false.
     *
     * 🔴 POR QUE NO SE DERIVA DEL PROPIO ESTIMADOR, QUE ERA LO OBVIO. El parcial
     * 1 tiene el dominio 4x mas ancho que el 4, asi que parece el arbitro
     * natural. **Medido el 2026-08-21: se auto-engaña.** Con E4 (dominio del
     * fundamental 30,50 c) y la señal real a −31,5 c, el parcial 1 publica
     * **+30,07** —aliasado, signo invertido— y como |30,07| < 30,50 se declara
     * EN DOMINIO, 12 corridas de 12.
     *
     * Un control que vive en la primitiva que se quiere acotar no puede
     * acotarla: al cruzar su propio borde, su valor vuelve a caer adentro del
     * dominio declarado. Por eso el control entra DESDE AFUERA.
     */
    void setCoarseFrequencyHz(double hz) noexcept {
        mCoarseHz = hz > 0.0 ? hz : 0.0;
    }

    /// La desviacion que ve el control, en cents contra el objetivo. NaN si no
    /// hay control o no hay objetivo.
    double coarseDeviationCents() const noexcept {
        if (mCoarseHz <= 0.0 || mTargetHz <= 0.0) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return 1200.0 * std::log2(mCoarseHz / mTargetHz);
    }

    /**
     * `true` si esta lectura se pudo verificar contra un control externo.
     *
     * Sin control **no se sabe** si los parciales estan en su dominio, y lo que
     * corresponde publicar entonces es ausencia (AC-003.8) — no la lectura sin
     * verificar, que es por donde reentra el defecto entero.
     */
    bool domainVerified() const noexcept { return mDomainVerified; }

    /**
     * `true` si la lectura de un parcial CONTRADICE EN SIGNO al control externo.
     *
     * 🔴 ES LA FIRMA DEL ALIASING, Y ES LO QUE `canMeasureDeviation` NO VE.
     * Esa guarda compara la desviacion GRUESA contra el rango del parcial, o
     * sea que usa la gruesa como sustituto de la desviacion verdadera. Con una
     * cuerda INARMONICA —que es lo que es una cuerda real— ese sustituto queda
     * OPTIMISTA justo en el borde: medido a 48 kHz sobre E4 con B = 1e-3, con
     * la cuerda a −35,0 cents la gruesa informa −28,56 y, como |−28,56| < 30,50,
     * la guarda declara "en dominio". Pero el fundamental si esta afuera de su
     * rango de captura: aliasa y vuelve como **+27,53** — el numero del reporte
     * de campo, que decia +27,0 con la cuerda a −35.
     *
     * La MAGNITUD de la gruesa deja de ser confiable ahi; **su SIGNO no**. Por
     * eso el arbitraje es por signo, que ademas es literalmente lo que pide
     * AC-014.3. Y va POR PARCIAL: asi el que aliaso se cae solo y los que
     * todavia miden bien siguen entrando en la combinacion.
     *
     * 🔴 NO TIENE ZONA MUERTA, Y ESO SE DECIDIO MIDIENDO, NO OMITIENDO. La
     * primera version no arbitraba cuando alguna de las dos magnitudes caia por
     * debajo de 1 cent, con el argumento de que cerca de "afinado" el signo es
     * ruido. El argumento es cierto —a 0,00 cents exactos con ruido el control
     * informa −0,0121 y la lectura fina +0,0001, o sea signos opuestos— pero la
     * proteccion **no protegia nada**: comparando los valores publicados en 42
     * escenarios de cuasi-afinacion, con y sin la zona muerta, salieron
     * IDENTICOS. Cuando el fundamental se cae por un empate tecnico, los otros
     * parciales sostienen la lectura igual.
     *
     * Dos mutantes que le sacaban una mitad sobrevivian, y el que la sacaba
     * entera tambien. Una defensa que ningun mutante puede matar y cuya
     * remocion no cambia un solo numero no es una defensa: es una linea que la
     * proxima persona va a creer load-bearing. Se fue.
     *
     * 🔴 LA ZONA MUERTA VOLVIO EN REQ-027, Y EL RIESGO RESIDUAL DE ARRIBA ES
     * EXACTAMENTE LO QUE PASO — PEOR DE LO DECLARADO.
     *
     * Este bloque declaraba: *"si ALGUNA vez los cuatro parciales discrepan a la
     * vez con un control cerca de cero, la lectura se apagaria en afinado. No se
     * observo en 42 escenarios."* Se observo. Y no se apago: publico **+38,70
     * cents con estado CONVERGIDO** sobre una cuerda afinada EXACTA.
     *
     * Las dos mitades del razonamiento viejo eran ciertas por separado y falsas
     * juntas:
     *
     *   · *"cerca de afinado el signo es ruido"* — cierto, y es la causa. A 0,00
     *     cents el control informa −0,0121 y la lectura fina +0,0001: signos
     *     opuestos, o sea que el fundamental se descarta JUSTO cuando la cuerda
     *     esta bien. Es un empate tecnico entre dos numeros que valen cero.
     *
     *   · *"los otros parciales sostienen la lectura igual"* — esto es lo que se
     *     cayo. Se midio sobre 42 escenarios que TODOS tenian cuatro parciales
     *     con energia, que es el mismo punto ciego que tenia el corpus de tests
     *     entero (los ocho tests de extremo a extremo generaban el estimulo con
     *     `for (int n = 1; n <= 4; ++n)`). Con un tono puro **no hay otros
     *     parciales que sostengan nada**: los otros tres son fuga espectral, y
     *     lo que "sostenia" la lectura era justamente la basura.
     *
     * O sea que "sacarla no cambia un solo numero" era verdad **en la muestra
     * medida**, y la muestra no contenia el caso. La leccion no es que la zona
     * muerta sea sagrada: es que su remocion se valido contra un corpus que no
     * podia refutarla.
     *
     * EL UMBRAL SALE DE LA RESOLUCION DEL CONTROL, no de un numero redondo: la
     * deteccion gruesa mide 0,21 cents en el peor caso sobre A0-C7, asi que por
     * debajo de ~5x eso su SIGNO no transporta informacion. Arbitrar con el
     * seria arbitrar con ruido.
     */
    static constexpr double kSignArbitrationDeadZoneCents = 1.0;

    static bool contradictsControl(double partialCents, double coarse) noexcept {
        // SIN CONTROL NO SE ARBITRA, y esto se chequea ACA y no solo en el
        // llamador. Con `coarse` NaN la comparacion de abajo no falla: `NaN > 0`
        // es false, asi que la funcion descartaria en silencio todo parcial de
        // lectura positiva — un veredicto inventado a partir de la ausencia de
        // control, que es el auto-engaño exacto que documenta
        // `setCoarseFrequencyHz`. Una funcion que puede recibir NaN se defiende
        // sola en vez de confiar en que el llamador se acuerde.
        if (!std::isfinite(coarse)) return false;
        // ZONA MUERTA: con cualquiera de las dos magnitudes por debajo del piso,
        // el signo es un empate tecnico y no una contradiccion. Ver arriba.
        if (std::fabs(coarse) < kSignArbitrationDeadZoneCents) return false;
        if (std::fabs(partialCents) < kSignArbitrationDeadZoneCents) return false;
        return (partialCents > 0.0) != (coarse > 0.0);
    }

    /// Cuantos parciales entraron en la ultima combinacion. 0 = ninguno pudo
    /// medir esta desviacion.
    int partialsUsed() const noexcept { return mPartialsUsed; }

    /**
     * El dominio de la lectura COMBINADA, en cents: el del parcial de orden mas
     * bajo, que es el mas ancho. Es lo que un consumidor necesita para dibujar
     * hasta donde vale la aguja (lo publica S2, AC-003.4).
     */
    double usableRangeCents() const noexcept { return mPartials[0].captureRangeCents(); }

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
    double mCoarseHz{0.0};
    bool mHasSignal{false};
    bool mHasMeasurement{false};
    bool mDomainVerified{false};
    /// REQ-009 S2. La integracion viva arranco despues de un hueco y todavia no
    /// produjo una medicion propia. Ver `noteInputDiscontinuity()`.
    bool mSawDiscontinuity{false};
    int mPartialsUsed{0};
};

}  // namespace wma::analysis
