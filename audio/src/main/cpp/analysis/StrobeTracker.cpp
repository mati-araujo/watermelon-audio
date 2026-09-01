#include "StrobeTracker.h"

namespace wma::analysis {

void StrobeTracker::prepare(int sampleRate) {
    for (auto& p : mPartials) p.prepare(sampleRate);
    // `prepare()` de cada parcial reinicia su objetivo, asi que el nuestro deja
    // de estar aplicado: re-aplicarlo es responsabilidad de quien nos prepara.
    mTargetHz = 0.0;
    mCents = 0.0;
    mUncertaintyCents = 0.0;
    mHasSignal = false;
    mHasMeasurement = false;
    mSawDiscontinuity = false;
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

    bool anySignal = false;
    for (auto& p : mPartials) {
        p.process(mono, numFrames);
        anySignal = anySignal || p.hasSignal();
    }
    mHasSignal = anySignal;

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

    // --- REQ-027: EL PISO DE ENERGIA, ANTES QUE CUALQUIER OTRA COSA ---------
    //
    // El parcial mas fuerte fija la escala. Se recorre entero primero porque el
    // piso relativo necesita el maximo antes de poder juzgar a nadie.
    double strongestBin = 0.0;
    for (const auto& p : mPartials) {
        if (!p.hasMeasurement()) continue;
        const double ratio = p.goertzelBinToRmsRatio();
        if (ratio > strongestBin) strongestBin = ratio;
    }

    double vals[kPartials];
    double sigmas[kPartials];
    int valid = 0;
    for (const auto& p : mPartials) {
        if (!p.hasMeasurement()) continue;
        const double sigma = p.uncertaintyCents();
        if (!(sigma > 0.0) || !std::isfinite(sigma)) continue;
        if (!std::isfinite(p.cents())) continue;
        // REQ-027 — ?habia algo que medir en este bin? Va ANTES del descarte por
        // dominio y del de mediana, y el orden importa: un parcial que no mide
        // nada no puede VOTAR el consenso que decide a quien se descarta. Con
        // tono puro los tres parciales de fuga son MAYORIA, asi que la mediana la
        // fijaban ellos y el unico dato bueno quedaba de outlier.
        const double binRatio = p.goertzelBinToRmsRatio();
        if (binRatio < kMinBinToRmsRatio) continue;
        if (binRatio < kMinFractionOfStrongestPartial * strongestBin) continue;
        // Sin control no se descarta nada: no se PUEDE saber quien esta en
        // dominio, y adivinarlo con la propia fase es el auto-engaño que
        // documenta `setCoarseFrequencyHz`. Lo que corresponde entonces es que
        // la lectura salga sin verificar —`domainVerified()` en false— y que
        // quien publica decida; aca no se inventa un veredicto.
        if (mDomainVerified && !p.canMeasureDeviation(coarse)) continue;
        // REQ-014 S2 (AC-014.3) — y ademas el SIGNO tiene que coincidir con el
        // control. La guarda de arriba se apoya en la MAGNITUD de la gruesa, y
        // con una cuerda inarmonica esa magnitud queda corta justo en el borde:
        // deja pasar un fundamental aliasado que vuelve con el signo dado
        // vuelta. Publicar eso le dice al musico que afloje lo que hay que
        // apretar, que es peor que no decir nada. Ver `contradictsControl`.
        if (mDomainVerified && contradictsControl(p.cents(), coarse)) continue;
        vals[valid] = p.cents();
        sigmas[valid] = sigma;
        ++valid;
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
    if (valid > 2) {
        double sorted[kPartials];
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
                std::abs(vals[i] - median) > kMaxPartialDisagreementCents) continue;
            vals[kept] = vals[i];
            sigmas[kept] = sigmas[i];
            ++kept;
        }
        valid = kept;
    }

    double sumWeights = 0.0;
    double sumWeighted = 0.0;
    for (int i = 0; i < valid; ++i) {
        const double w = 1.0 / (sigmas[i] * sigmas[i]);
        if (!std::isfinite(w)) continue;
        sumWeights += w;
        sumWeighted += w * vals[i];
    }

    mPartialsUsed = valid;

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
    return true;
}

}  // namespace wma::analysis
