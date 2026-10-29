#include "StrobeTracker.h"

#include <algorithm>

namespace wma::analysis {

namespace {

/**
 * REQ-058 S1 — LAS COMPUERTAS POR PARCIAL, compartidas por los dos caminos y en el orden de hoy
 * (el veredicto las aplicaba en linea hasta REQ-057; el texto de cada una es el de entonces):
 *
 *   medicion → veredicto de tendencia → σ → rango denso → energia → signo
 *
 * El camino temprano pasa `trendEvaluable = true` (no tiene 12 ventanas: su defensa contra el glide
 * es el termino de deriva de D-1) y `arbitrateSign` solo lejos del borde (D46).
 */
PartialAdmission gatePartialReading(bool hasMeasurement, bool trendEvaluable, double cents, double sigma,
                                    double binToRms, const InterleavedPhaseEstimator& grids, double coarse,
                                    bool domainVerified, bool arbitrateSign) {
    if (!hasMeasurement) return PartialAdmission::kNoMeasurement;
    // REQ-036 (R-PITCH-62) — sin veredicto de tendencia no hay admision: son las primeras 12
    // ventanas, donde el rizado de las cuerdas graves y la falta de dos mitades hacen que "es
    // una recta" no se pueda afirmar. Va antes que todo lo demas por la misma razon que el piso
    // de energia: un parcial que no se puede verificar no vota.
    if (!trendEvaluable) return PartialAdmission::kNoTrendVerdict;
    if (!(sigma > 0.0) || !std::isfinite(sigma) || !std::isfinite(cents)) return PartialAdmission::kBadUncertainty;
    // Sin control no se descarta nada: no se PUEDE saber quien esta en
    // dominio, y adivinarlo con la propia fase es el auto-engaño que
    // documenta `setCoarseFrequencyHz`. Lo que corresponde entonces es que
    // la lectura salga sin verificar —`domainVerified()` en false— y que
    // quien publica decida; aca no se inventa un veredicto.
    //
    // REQ-057 (D22/D28): el rango es el de la rejilla DENSA del parcial (hop 1024, κ = 0,9).
    if (domainVerified && !grids.canMeasureDenseDeviation(coarse)) return PartialAdmission::kOutOfRange;
    //
    // El dominio va ANTES que la energia solo para que la sonda diga la causa: un parcial
    // fuera de su rango tambien se queda sin energia en su bin (el pico cae fuera del lobulo
    // de Hann), y es fisica del rango, no ausencia de señal. Los dos solo descartan.
    //
    // REQ-027 — ?habia algo que medir en este bin? Va ANTES del descarte por
    // mediana, y el orden importa: un parcial que no mide
    // nada no puede VOTAR el consenso que decide a quien se descarta. Con
    // tono puro los tres parciales de fuga son MAYORIA, asi que la mediana la
    // fijaban ellos y el unico dato bueno quedaba de outlier.
    if (binToRms < StrobeTracker::kMinBinToRmsRatio) return PartialAdmission::kNoEnergy;
    // REQ-014 S2 (AC-014.3) — y ademas el SIGNO tiene que coincidir con el
    // control. La guarda de arriba se apoya en la MAGNITUD de la gruesa, y
    // con una cuerda inarmonica esa magnitud queda corta justo en el borde:
    // deja pasar un fundamental aliasado que vuelve con el signo dado
    // vuelta. Publicar eso le dice al musico que afloje lo que hay que
    // apretar, que es peor que no decir nada. Ver `contradictsControl`.
    if (arbitrateSign && domainVerified && StrobeTracker::contradictsControl(cents, coarse)) {
        return PartialAdmission::kSignContradiction;
    }
    return PartialAdmission::kAdmitted;
}

/**
 * REQ-058 S1 — el descarte por CONSENSO, compartido por los dos caminos: la mediana (con el parcial
 * mejor medido a salvo) y «un sobreviviente solo, contradicho por sus hermanos, no es una lectura».
 * Compacta los arreglos in situ y devuelve cuantos quedan. `admissionOut` (por indice de parcial)
 * recibe las causas de descarte; puede ser nullptr.
 */
int pruneByConsensus(double* vals, double* sigmas, int* orders, int* pidx, int valid, int* contradicted,
                     PartialAdmission* admissionOut) {
    // --- descarte del parcial que NO esta midiendo la nota -------------------
    //
    // (El porque entero esta en `StrobeTracker::combineVerdictReadings`, en el bloque que precede a
    // la llamada: σ no sabe si el bin tiene señal, y la mediana —no la media— dice el consenso.)
    if (valid > 2) {
        double sorted[StrobeTracker::kPartials];
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
        // 🔴 AL PARCIAL MEJOR MEDIDO NO SE LO DESCARTA NUNCA, Y ESO NO ES UNA
        // EXCEPCION COMODA: es lo que hace que la regla funcione en LOS DOS
        // casos degenerados, que son opuestos entre si.
        //
        //  · Fundamental AUSENTE (bajo por parlante chico): p0 es fuga y da
        //    -256 cents con σ=0,081; los otros tres dan +1,00 con σ≈0,007. La
        //    mediana vale +1,00, p0 se va, y p0 NO era el de menor σ. Se descarta.
        //  · Tono PURO (diapason, flauta, referencia electronica): el unico
        //    parcial con energia es p0 —+1,0000 con σ=0,000011— y los otros tres
        //    son fuga: -35,5, +14,2, -7,1. La mediana vale -3,07, o sea que el
        //    CONSENSO es de los que no estan midiendo nada. Sin esta regla, un
        //    umbral mas ajustado tiraria justamente el unico dato bueno.
        //
        // La mediana dice cual es el consenso; σ dice quien midio de verdad. Y
        // cuando se contradicen, manda σ: tres parciales de acuerdo en basura
        // siguen siendo basura.
        //
        // ⚠️ HONESTIDAD SOBRE ESTA LINEA: **ningun test la ejercita**. Se mutó
        // —quitando `i != best`— y la suite entera queda VERDE, incluido el
        // barrido de 16 casos degenerados. Se conserva igual, y la razon es un
        // numero medido, no una corazonada: en ese barrido la desviacion del
        // mejor parcial respecto de la mediana llega a **40,46 cents** (E1 con
        // solo el 4to parcial) contra un umbral de 50. O sea que el peor caso
        // conocido usa el **81 %** del margen. Un respaldo que cuesta una
        // comparacion, para un peligro medido a esa distancia, se paga solo.
        // Si algun dia un caso lo cruza, este bloque es lo que evita que el
        // afinador tire el unico dato bueno que tenia.
        int best = 0;
        for (int i = 1; i < valid; ++i) {
            if (sigmas[i] < sigmas[best]) best = i;
        }
        int kept = 0;
        for (int i = 0; i < valid; ++i) {
            if (i != best &&
                std::abs(vals[i] - median) > StrobeTracker::kMaxPartialDisagreementCents) {
                if (admissionOut != nullptr) admissionOut[pidx[i]] = PartialAdmission::kMedianOutlier;
                ++*contradicted;
                continue;
            }
            vals[kept] = vals[i];
            sigmas[kept] = sigmas[i];
            orders[kept] = orders[i];
            pidx[kept] = pidx[i];
            ++kept;
        }
        valid = kept;
    }

    // --- REQ-036: UN SOBREVIVIENTE SOLO, CONTRADICHO POR SUS HERMANOS, NO ES UNA LECTURA ------
    //
    // Con un parcial no hay serie que ajustar ni B que estimar: se publica su valor tal cual. Eso
    // vale para el tono puro y para el parcial que carga toda la energia —los otros bins no tienen
    // señal, nadie lo contradice— y NO vale para el que queda solo porque a los demas los descarto
    // el desacuerdo (signo contra el control, mediana). Medido sobre audio con huecos sostenidos
    // (test_non_contiguous): los cuatro leian +17 / −2,9 / +4,1 / −1,8 c, el signo se llevo a dos,
    // el dominio a otro, y el que quedo publicaba −2,9 con σ 0,07 —CONVERGIDO y 2,1 c equivocado—
    // porque SU fase, en ese regimen, si era una recta. Un parcial al que sus hermanos con energia
    // contradicen no esta midiendo la nota: esta midiendo lo que sea que le toco a su bin.
    //
    // Dominio y energia no cuentan como desacuerdo: son fisica (un parcial fuera de su rango de
    // captura, un bin sin señal), y REQ-003 exige que los parciales se caigan de a uno sin apagar la
    // lectura hasta que quede el fundamental solo.
    if (valid == 1 && *contradicted > 0) {
        if (admissionOut != nullptr) admissionOut[pidx[0]] = PartialAdmission::kContradictedAlone;
        valid = 0;
    }
    return valid;
}

}  // namespace



void StrobeTracker::prepare(int sampleRate) {
    for (auto& p : mPartials) p.prepare(sampleRate);
    for (auto& a : mAdmission) a = PartialAdmission::kNoMeasurement;
    // `prepare()` de cada parcial reinicia su objetivo, asi que el nuestro deja
    // de estar aplicado: re-aplicarlo es responsabilidad de quien nos prepara.
    mTargetHz = 0.0;
    mCents = 0.0;
    mUncertaintyCents = 0.0;
    mHasSignal = false;
    mHasMeasurement = false;
    mSawDiscontinuity = false;
    clearAttackTracking();
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
    mSuppressUntilNextClose = false;
    mWindowRestarts = 0;
    for (auto& a : mAdmission) a = PartialAdmission::kNoMeasurement;
    clearAttackTracking();
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
    // Un reinicio PEDIDO —cambio de fuente, de objetivo, de rate— no arrastra
    // ninguna sospecha sobre la entrada: la marca de REQ-009 se baja.
    mSawDiscontinuity = false;
}

void StrobeTracker::noteInputDiscontinuity() {
    // El orden importa: `reset()` BAJA la marca (un reinicio pedido no acusa a
    // la entrada), asi que levantarla despues es lo unico que la deja en pie.
    reset();
    mSawDiscontinuity = true;
}

bool StrobeTracker::process(const float* mono, int numFrames) {
    if (mono == nullptr || numFrames <= 0 || mTargetHz <= 0.0) return false;

    // REQ-058 (D-3, D-5): el audio entra en tramos que no cruzan un borde de la grilla de 1024 de las
    // rejillas, para que el detector de ataque decida EN el borde y antes de que cierre la ventana
    // siguiente: asi el segmento nuevo no depende del bloque con que se alimente. Las rejillas ya
    // parten asi por dentro, y la combinacion de abajo corre UNA vez al final con `closed` acumulado:
    // el camino del veredicto ve exactamente lo que veia antes.
    bool closed = false;
    int pos = 0;
    while (pos < numFrames) {
        const int64_t start = mPartials[0].framesSinceReset();
        const int64_t toBoundary = kAttackBlockFrames - (start % kAttackBlockFrames);
        const int seg = static_cast<int>(std::min<int64_t>(toBoundary, numFrames - pos));
        for (auto& p : mPartials) {
            // La rejilla 0 de los cuatro cierra en la MISMA muestra: mismo largo, misma alimentacion.
            closed = p.process(mono + pos, seg) || closed;
        }
        trackAttackBlock(mono + pos, seg, start + seg);
        pos += seg;
    }
    bool anySignal = false;
    for (auto& p : mPartials) anySignal = anySignal || p.grid(0).hasSignal();
    mHasSignal = anySignal;

    mReadingIsEarly = false;
    // El control de dominio de ESTA llamada. `combineVerdictReadings` lo vuelve a calcular igual (el
    // veredicto queda bit a bit); hace falta aca porque en espera el veredicto no corre y el camino
    // temprano no puede admitir con el control del drenado anterior al ataque (review de S1, B1).
    mDomainVerified = std::isfinite(coarseDeviationCents());
    if (mSegmentWaiting) {
        // D-3: en espera, el veredicto no publica ni juzga quiebres (sus fases mezclan dos segmentos sin
        // agrupar); la lectura sale del camino temprano del segmento nuevo.
        mPartialsUsed = 0;
        mHasMeasurement = false;
        combineEarlyReadings();
        settleWaitingSegment();
        return true;
    }
    combineVerdictReadings(closed);
    if (!mHasMeasurement) combineEarlyReadings();
    // D-3: la ULTIMA lectura del grupo vigente (del veredicto o temprana: la que se publico) es contra
    // la que se juzga «misma nota» en el proximo ataque. Una nota corta que nunca llego al veredicto
    // (ukelele_A4 en corpus-v1) igual tiene lecturas tempranas con su σ.
    //
    // Solo cuenta una lectura VERIFICADA contra la gruesa (la que `AnalysisThread` puede publicar): sobre
    // ruido de sala sin altura gruesa el camino temprano puede tener lectura interna, y agrupar un punteo
    // contra eso juntaria fases de ruido (review de S1, B2). Y la referencia caduca: ver
    // `kGroupReadingMaxAgeFrames`.
    if (mHasMeasurement && mDomainVerified) {
        mHaveGroupReading = true;
        mGroupCents = mCents;
        mGroupSigma = mUncertaintyCents;
        mGroupReadingFrame = mPartials[0].framesSinceReset();
    }
    return true;
}

void StrobeTracker::combineVerdictReadings(bool closed) {
    if (closed) mSuppressUntilNextClose = false;

    // --- combinacion por inverso de la varianza -----------------------------
    //
    // Solo entran los parciales que TIENEN medicion y un σ estrictamente
    // positivo. Un σ de cero no es "certeza infinita" sino una ventana que
    // todavia no tiene de donde sacar dispersion, y meterlo como 1/0 haria
    // colapsar la combinacion sobre ese unico parcial.
    // --- REQ-003: DESCARTE POR DOMINIO, antes de cualquier otra cosa --------
    //
    // Un parcial cuya desviacion cae fuera de SU rango de captura no esta
    // midiendo de menos: esta midiendo MAL, con σ ≈ 0 porque la pendiente
    // aliasada sigue siendo lineal. Meterlo en la combinacion la envenena.
    //
    // 🔴 EL FILTRO DE DESACUERDO DE ABAJO NO ALCANZA, Y ESA ERA LA CAUSA RAIZ.
    // `kMaxPartialDisagreementCents` = 50 esta pensado para basura evidente
    // (fuga espectral, fundamental ausente). Un parcial RECIEN salido de dominio
    // no es basura evidente: cerca del borde el pliegue es chico, asi que
    // disiente POCO y pasa el filtro. Medido el 2026-08-21 en E4 a −8 cents: el
    // parcial 4 devuelve +7,39, disiente 15 cents de la mediana —bien por debajo
    // de 50— y arrastra la combinada a −4,767. En E2 a −32 el mismo parcial
    // disiente 61 y SI se descarta, y por eso E2 parecia sano. La diferencia
    // entre las dos cuerdas no era el aliasing: era si el filtro lo agarraba.
    //
    // Descartando por dominio la lectura queda exacta hasta el rango del
    // FUNDAMENTAL —~4x mas ancho que el del parcial 4— con error 0,0003 cents y
    // σ ≤ 3e-4 en las 14 cuerdas del catalogo, incluso cuando queda UN solo
    // parcial vivo.
    const double coarse = coarseDeviationCents();
    mDomainVerified = std::isfinite(coarse);

    double vals[kPartials];
    double sigmas[kPartials];
    int orders[kPartials];      // el orden del parcial (1..4): lo pide el ajuste de S2
    int pidx[kPartials];        // el indice del parcial, para la sonda de admision
    int valid = 0;
    int order = 0;
    bool breaks = false;        // REQ-036: algun parcial admisible tiene la fase fuera de una recta
    int contradicted = 0;       // REQ-036: parciales CON energia descartados por desacuerdo (signo, mediana)
    for (int pi = 0; pi < kPartials; ++pi) {
        const InterleavedPhaseEstimator& grids = mPartials[pi];
        const PhaseSlopeEstimator& p = grids.grid(0);   // D-B: se publica la rejilla 0
        ++order;
        // REQ-058 S1: las compuertas por parcial viven en `gatePartialReading` (el camino temprano usa
        // las mismas, en el mismo orden). Aca, con el veredicto de tendencia y el signo siempre.
        mAdmission[pi] = gatePartialReading(p.hasMeasurement(), p.phaseTrendEvaluable(), p.cents(),
                                            p.uncertaintyCents(), p.goertzelBinToRmsRatio(), grids, coarse,
                                            mDomainVerified, /*arbitrateSign=*/true);
        if (mAdmission[pi] == PartialAdmission::kSignContradiction) ++contradicted;
        if (mAdmission[pi] != PartialAdmission::kAdmitted) continue;
        // REQ-036 — el parcial es admisible por todo lo demas: si su fase no es una recta, la
        // ventana ENTERA se quebro (un glide sesga a los cuatro por igual) y se reinicia abajo.
        breaks = breaks || phaseBreaksTheLine(p.phaseTrendScore(), p.phaseTrendDeltaCents());
        vals[valid] = p.cents();
        sigmas[valid] = p.uncertaintyCents();
        orders[valid] = order;
        pidx[valid] = pi;
        ++valid;
    }

    // --- REQ-036 (R-PITCH-63): LA VENTANA ADAPTATIVA -------------------------
    //
    // Se juzga SOLO en el cierre de una ventana: entre cierres la evidencia no cambio, y
    // reaccionar a ella otra vez seria reiniciar en cascada. Y se reinician LOS CUATRO en el
    // mismo punto: reiniciar por parcial los deja rebrotando en ventanas distintas y la lectura
    // pasa por un solo parcial, que publica su estiramiento inarmonico sin corregir (medido en
    // S1: +1,38 c con el 4to solo). Al quebrarse, este cierre no publica; el siguiente juzga la
    // mitad nueva.
    if (closed && breaks) {
        for (auto& p : mPartials) p.restartAtBreak();
        mSuppressUntilNextClose = true;
        ++mWindowRestarts;
    }
    if (mSuppressUntilNextClose) {
        mPartialsUsed = 0;
        mHasMeasurement = false;
        return;
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
    // REQ-058 S1: la mediana y «solo y contradicho» viven en `pruneByConsensus` (el camino temprano
    // usa la misma regla). Ver ahi el porque del mejor parcial y del sobreviviente contradicho.
    valid = pruneByConsensus(vals, sigmas, orders, pidx, valid, &contradicted, mAdmission);

    mPartialsUsed = valid;

    // --- REQ-027 S2: la serie ESTIRADA, que es lo que es una cuerda real -----
    //
    // Los cuatro parciales no miden la misma cantidad: el parcial n esta en
    // `n·f0·√(1+B·n²)`, asi que discrepan POR FISICA. Promediarlos como si
    // discreparan por ruido corre la lectura hacia los agudos —y encima el peso
    // 1/σ² es MAYOR en los parciales altos, que son los mas estirados—. Se ajusta
    // el modelo y se publica C, la desviacion del FUNDAMENTAL.
    //
    // σ sale de los RESIDUOS: es lo unico que mide cuanto NO explica el modelo, y
    // por eso AC-027.5 sale estructural — un parcial que la serie estirada no
    // puede explicar infla σ y apaga CONVERGIDO por el umbral que ya existia.
    {
        double fitC = 0.0;
        double fitSigma = 0.0;
        if (fitStretchedSeries(vals, sigmas, orders, valid, &fitC, &fitSigma)) {
            mCents = fitC;
            mUncertaintyCents = fitSigma;
            mHasMeasurement = true;
            mSawDiscontinuity = false;   // ver la nota de REQ-009 S2 mas abajo
            return;
        }
    }

    // Con k <= 2 no hay grados de libertad para una σ con sentido: se conserva la
    // combinacion de siempre. Ver `kMinPartialsForStretchFit`.
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
        // REQ-009 S2 — ACA se baja la marca, y este es el punto exacto en que la
        // integracion vuelve a ser confiable: `noteInputDiscontinuity()` tiro
        // todo, asi que una medicion que exista DESPUES sale de ventanas que son
        // enteramente audio posterior al hueco. Mientras no la haya, la marca
        // sigue en pie y el consumidor puede decir "la entrada llego rota" en vez
        // de "todavia no" (AC-009.3).
        //
        // Bajarla en cualquier otro lado seria mentir por adelantado: bajarla al
        // primer `process()` la apagaria con la ventana todavia a medio llenar.
        mSawDiscontinuity = false;
    } else {
        // Sin ningun parcial utilizable no se inventa una lectura: se conserva
        // la ultima y se declara que no es actual, igual que hace S2.
        mHasMeasurement = false;
    }
}


// ---------------------------------------------------------------------------
// REQ-058 S1 — el camino temprano y el detector de ataque
// ---------------------------------------------------------------------------

void StrobeTracker::clearAttackTracking() noexcept {
    mAttackBlockSumSq = 0.0;
    for (auto& h : mAttackHistory) h = 0.0;
    mAttackHistoryCount = 0;
    mAttackRefractoryBlocks = 0;
    mAttacksDetected = 0;
    mSegmentStartFrame = 0;
    mReadingIsEarly = false;
    mVerdictSegment = 0;
    mSegmentWaiting = false;
    mHaveGroupReading = false;
    mGroupCents = 0.0;
    mGroupSigma = 0.0;
    mGroupReadingFrame = 0;
    mEarlySigmaNoise = 0.0;
    mEarlyDrift = 0.0;
    mEarlyMinPhases = 0;
}

void StrobeTracker::trackAttackBlock(const float* mono, int numFrames, int64_t endFrame) {
    for (int i = 0; i < numFrames; ++i) {
        const double x = static_cast<double>(mono[i]);
        mAttackBlockSumSq += x * x;
    }
    if (endFrame % kAttackBlockFrames != 0) return;
    const double rms = std::sqrt(mAttackBlockSumSq / static_cast<double>(kAttackBlockFrames));
    mAttackBlockSumSq = 0.0;
    // La referencia es el MAXIMO de los `kAttackHistoryBlocks` bloques anteriores, no el anterior solo:
    // en B0 (30,87 Hz, 1429 frames de periodo a 44,1 k) un bloque de 1024 es menos de un periodo y su rms
    // oscila con la fase de la onda; contra el bloque anterior eso disparaba ataques en medio de la nota
    // sostenida (medido: `StrobeTrackerTest.ItStillMeetsTheBudgetWithTheFundamentalTwentyDbDown`, B0 leia
    // 2,03 c en vez de 1,00). Cuatro bloques son 2,9 periodos de B0.
    double reference = 0.0;
    for (int i = 0; i < mAttackHistoryCount; ++i) reference = std::max(reference, mAttackHistory[i]);
    const bool previousSilent = mAttackHistoryCount > 0 && mAttackHistory[0] < kAttackFloor;
    for (int i = kAttackHistoryBlocks - 1; i > 0; --i) mAttackHistory[i] = mAttackHistory[i - 1];
    mAttackHistory[0] = rms;
    if (mAttackHistoryCount < kAttackHistoryBlocks) ++mAttackHistoryCount;
    if (mAttackRefractoryBlocks > 0) {
        --mAttackRefractoryBlocks;
        return;
    }
    // El primer bloque desde el reset no tiene con que compararse: no es un ataque, es el comienzo.
    if (endFrame <= kAttackBlockFrames) return;
    if (!(rms >= kAttackFloor) || !(rms >= kAttackRiseRatio * reference)) return;
    mAttackRefractoryBlocks = kAttackRefractoryBlocks;
    ++mAttacksDetected;
    mSegmentStartFrame = endFrame - kAttackBlockFrames;
    // (1) de D-3: la serie densa del segmento nuevo no acepta ventanas que empiecen antes del bloque
    // del ataque. La ventana que cerro en este mismo borde empezo 3072 frames antes: se va con el resto.
    for (auto& p : mPartials) p.openDenseSegmentAt(mSegmentStartFrame);

    // (2) de D-3, en el camino del VEREDICTO. Un ataque sin ninguna fase todavia (los primeros 4096 frames
    // de la integracion) no lo toca: eso deja bit a bit las notas de una sola pulsada. Y uno que sale del
    // SILENCIO tampoco: el silencio ya reinicia cada rejilla en su ventana (rms < 0,001, como hoy), y
    // abrir un segmento encima descartaria ventanas que hoy si producen fase.
    if (previousSilent) return;
    bool anyPhases = false;
    for (const auto& p : mPartials) anyPhases = anyPhases || p.hasRegressionPhases();
    if (!anyPhases) return;
    const unsigned char previousWaiting = mVerdictSegment;
    const bool wasWaiting = mSegmentWaiting;
    ++mVerdictSegment;
    for (auto& p : mPartials) {
        // Primero se abre (descarta la ventana que contiene el ataque con el inicio de ventana vigente) y
        // DESPUES, si habia un re-punteo esperando, se descarta ese segmento: no llego a asentarse.
        p.openPhaseSegment(mSegmentStartFrame, mVerdictSegment);
        if (wasWaiting) p.dropPhaseSegment(previousWaiting);
    }
    mSuppressUntilNextClose = false;
    // Una referencia vieja (la nota se apago en el ruido y el control la perdio) no es «el segmento
    // anterior»: el punteo nuevo reinicia.
    if (mHaveGroupReading && endFrame - mGroupReadingFrame > kGroupReadingMaxAgeFrames) mHaveGroupReading = false;
    const double coarse = coarseDeviationCents();
    const bool coarseElsewhere = mHaveGroupReading && std::isfinite(coarse) &&
                                 std::fabs(coarse - mGroupCents) > kSameNoteCoarseCents;
    if (!mHaveGroupReading || coarseElsewhere) {
        // Sin ninguna lectura vieja contra la que juzgar, o la gruesa ya esta en otra nota (criterio 2): reinicio.
        for (auto& p : mPartials) p.keepOnlyPhaseSegment(mVerdictSegment);
        mHaveGroupReading = false;
        mSegmentWaiting = false;
        return;
    }
    mSegmentWaiting = true;
}

void StrobeTracker::settleWaitingSegment() {
    if (!mHasMeasurement || !mReadingIsEarly) return;   // el segmento nuevo todavia no lee
    if (!mDomainVerified) return;                       // sin control no se juzga «misma nota»
    if (mEarlyMinPhases < InterleavedPhaseEstimator::kEarlyWindow) return;
    if (mEarlyDrift > std::max(mEarlySigmaNoise, kSettledDriftFloorCents)) return;   // sigue el glide
    const double coarse = coarseDeviationCents();
    const bool coarseElsewhere = std::isfinite(coarse) && std::fabs(coarse - mGroupCents) > kSameNoteCoarseCents;
    const double tolerance =
        kCompatibleSigmas * std::sqrt(mEarlySigmaNoise * mEarlySigmaNoise + mGroupSigma * mGroupSigma);
    const bool compatible = !coarseElsewhere && std::fabs(mCents - mGroupCents) <= tolerance;
    for (auto& p : mPartials) {
        if (compatible) {
            // Se une al grupo DESDE EL ASIENTO: lo que el segmento nuevo junto hasta aca (el glide, si lo
            // hubo) queda afuera, como hace R-PITCH-63; las fases que cierren desde ahora se agrupan.
            p.dropPhaseSegment(mVerdictSegment);
        } else {
            // Clavija girada o nota distinta: los viejos se descartan (R-PITCH-63).
            p.keepOnlyPhaseSegment(mVerdictSegment);
        }
    }
    if (!compatible) mHaveGroupReading = false;
    mSegmentWaiting = false;
}

void StrobeTracker::combineEarlyReadings() {
    const double coarse = coarseDeviationCents();
    double vals[kPartials];
    double sigmas[kPartials];
    double drifts[kPartials];
    double driftSigmas[kPartials];
    int orders[kPartials];
    int pidx[kPartials];
    int valid = 0;
    int contradicted = 0;
    for (int pi = 0; pi < kPartials; ++pi) {
        const InterleavedPhaseEstimator& grids = mPartials[pi];
        const InterleavedPhaseEstimator::DenseEarlyFit& f = grids.denseEarlyFit();
        // D46: el arbitraje de signo (R-PITCH-38) se relaja SOLO aca y SOLO lejos del borde del rango.
        const bool nearEdge = earlyPathArbitratesSign(coarse, f.cents, grids.denseCaptureRangeCents());
        const PartialAdmission a =
            gatePartialReading(f.n >= InterleavedPhaseEstimator::kEarlyMinPhases, /*trendEvaluable=*/true, f.cents,
                               f.sigmaCents, f.binToRmsRatio, grids, coarse, mDomainVerified, nearEdge);
        if (a == PartialAdmission::kSignContradiction) ++contradicted;
        if (a != PartialAdmission::kAdmitted) continue;
        vals[valid] = f.cents;
        sigmas[valid] = f.sigmaCents;
        drifts[valid] = f.driftCents;
        driftSigmas[valid] = f.driftSigmaCents;
        orders[valid] = pi + 1;
        pidx[valid] = pi;
        ++valid;
    }
    // La deriva acompaña a su parcial en la compactacion: se reordena aparte, por indice de parcial.
    double driftOf[kPartials] = {0.0, 0.0, 0.0, 0.0};
    double driftSigmaOf[kPartials] = {0.0, 0.0, 0.0, 0.0};
    for (int i = 0; i < valid; ++i) {
        driftOf[pidx[i]] = drifts[i];
        driftSigmaOf[pidx[i]] = driftSigmas[i];
    }
    valid = pruneByConsensus(vals, sigmas, orders, pidx, valid, &contradicted, nullptr);
    if (valid == 0) return;   // mHasMeasurement sigue en false: no se inventa una lectura

    double C = vals[0];
    double sigmaResiduals = sigmas[0];
    double B = 0.0;
    if (!fitStretchedSeries(vals, sigmas, orders, valid, &C, &sigmaResiduals, &B)) {
        C = vals[0];
        sigmaResiduals = sigmas[0];
    }
    // D-1: k ≥ 3, la σ de los residuos con la PROPAGADA como piso (con 1 g.l. los residuos pasan
    // |t| ≤ 2 solo el 70 % de las veces aun con el modelo perfecto); k = 2 (D29, D48), la propagacion
    // del ajuste de dos puntos y NO `√Σσ²`; k = 1, la del parcial.
    const double sigmaProp = propagatedStretchSigma(sigmas, orders, valid, B);
    double sigmaC = sigmas[0];
    if (valid >= 3) sigmaC = std::max(sigmaResiduals, sigmaProp);
    else if (valid == 2) sigmaC = sigmaProp;
    // D-1: la deriva SIN SESGO por parcial, `√max(0, deriva_n² − σ(deriva_n)²)` (en una nota estable la
    // curvatura es ruido y su esperanza al cuadrado es su varianza: el termino se anula en vez de inflar
    // σ), y la MEDIA de los admitidos (un glide corre a los cuatro parciales los mismos cents).
    //
    // 🔴 PROBADO Y NO ADOPTADO (REQ-058 S1, bifurcacion abierta de AC-058.5): promediar las curvaturas
    // CON SIGNO antes del modulo baja la no inflacion de la sintesis estable de 3,36/3,40 a 2,68/2,72
    // (3/4 parciales), pero la cobertura de 2σ de los 24 glides cae de 0,983 a 0,941 y la de k = 2 de
    // 0,946 a 0,912: rompe AC-058.4. Fuente: `EarlyReading.AStableNoteDoesNotInflateSigma` y
    // `EarlyReading.GlidesAreCoveredAtTwoSigma` con la variante.
    double drift = 0.0;
    for (int i = 0; i < valid; ++i) {
        const double d = driftOf[pidx[i]];
        const double sd = driftSigmaOf[pidx[i]];
        drift += std::sqrt(std::max(0.0, d * d - sd * sd));
    }
    drift /= static_cast<double>(valid);
    const double sigma = std::sqrt(sigmaC * sigmaC + drift * drift);
    if (!std::isfinite(C) || !std::isfinite(sigma)) return;
    int minPhases = InterleavedPhaseEstimator::kMaxDensePhases;
    for (int i = 0; i < valid; ++i) minPhases = std::min(minPhases, mPartials[pidx[i]].denseEarlyFit().n);
    mEarlySigmaNoise = sigmaC;
    mEarlyDrift = drift;
    mEarlyMinPhases = minPhases;
    mCents = C;
    mUncertaintyCents = sigma;
    mPartialsUsed = valid;
    mHasMeasurement = true;
    mReadingIsEarly = true;
    // REQ-009 S2: la marca del hueco NO se baja aca. La baja la primera lectura del VEREDICTO, como
    // siempre: una temprana sale de 8 fases densas y no es la integracion que AC-009.3 describe.
}

}  // namespace wma::analysis
