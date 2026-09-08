#include "McLeodPitch.h"

#include <algorithm>
#include <cmath>

namespace wma::dsp {

void McLeodPitch::prepare(int sampleRate) {
    mSampleRate = sampleRate > 0 ? sampleRate : 0;
    if (mSampleRate <= 0) return;

    // Factor entero para caer lo mas cerca posible de kTargetRate, nunca menor que 1.
    mDecimation = static_cast<int>(std::lround(
        static_cast<double>(mSampleRate) / static_cast<double>(kTargetRate)));
    if (mDecimation < 1) mDecimation = 1;
    mWorkingRate = static_cast<double>(mSampleRate) / mDecimation;

    // Antialias a un tercio del nuevo Nyquist: deja pasar C7 (2093 Hz) con muchisimo margen
    // y corta bastante antes de que algo se pliegue.
    mAntiAlias.setSampleRate(static_cast<float>(mSampleRate));
    mAntiAlias.setLowpass(static_cast<float>(mWorkingRate / 3.0));

    // UNICO punto que asigna.
    mWindow.assign(static_cast<size_t>(kWindowFrames), 0.0f);

    mMinLag = static_cast<int>(std::floor(mWorkingRate / kMaxHz));
    mMaxLag = static_cast<int>(std::ceil(mWorkingRate / kMinHz));
    if (mMinLag < 2) mMinLag = 2;
    if (mMaxLag > kWindowFrames / 2) mMaxLag = kWindowFrames / 2;

    mNsdf.assign(static_cast<size_t>(mMaxLag + 1), 0.0);

    reset();
}

void McLeodPitch::reset() {
    mAntiAlias.reset();
    mPhase = 0;
    mFilled = 0;
    mFrequencyHz = 0.0;
    mClarity = 0.0;
    mHasPitch = false;
    mWindows = 0;
    mKeyCount = 0;
    for (auto& v : mWindow) v = 0.0f;
}

bool McLeodPitch::process(const float* mono, int numFrames) {
    if (mono == nullptr || numFrames <= 0 || mSampleRate <= 0 || mWindow.empty()) {
        return false;
    }

    bool produced = false;
    for (int i = 0; i < numFrames; ++i) {
        // Filtrar SIEMPRE, decimar despues: el filtro tiene que ver todas las muestras o no
        // esta filtrando nada. Decimar antes de filtrar es el error clasico — el alias ya
        // ocurrio y ningun filtro posterior lo saca.
        const float filtered = mAntiAlias.process(mono[i]);
        if (++mPhase < mDecimation) continue;
        mPhase = 0;

        mWindow[static_cast<size_t>(mFilled)] = filtered;
        if (++mFilled >= kWindowFrames) {
            analyzeWindow();
            mFilled = 0;
            produced = true;
        }
    }
    return produced;
}

double McLeodPitch::nsdfAt(int lag) const {
    // NSDF = 2·r[τ] / m[τ]. La normalizacion por la energia de las dos ventanas es lo que
    // distingue esto de una autocorrelacion cruda: sin ella los picos decaen con τ, y ese
    // decaimiento solo ya sesga la eleccion hacia los lags cortos — o sea, hacia la octava
    // equivocada.
    if (lag < 0 || lag >= kWindowFrames) return 0.0;
    double r = 0.0;
    double m = 0.0;
    const int n = kWindowFrames - lag;
    for (int i = 0; i < n; ++i) {
        const double a = mWindow[static_cast<size_t>(i)];
        const double b = mWindow[static_cast<size_t>(i + lag)];
        r += a * b;
        m += a * a + b * b;
    }
    return (m > 0.0) ? (2.0 * r / m) : 0.0;
}

void McLeodPitch::analyzeWindow() {
    ++mWindows;

    // --- nivel: por debajo del piso no hay nota, y no se inventa una ----------
    double energy = 0.0;
    for (int i = 0; i < kWindowFrames; ++i) {
        const double v = mWindow[static_cast<size_t>(i)];
        energy += v * v;
    }
    const double rms = std::sqrt(energy / kWindowFrames);
    if (rms < static_cast<double>(kSilenceFloor)) {
        mHasPitch = false;
        mClarity = 0.0;
        mFrequencyHz = 0.0;
        return;
    }

    // --- BUSQUEDA EN DOS PASADAS, Y EL PASO ES PROPORCIONAL AL LAG ------------
    //
    // La NSDF completa cuesta O(W·τmax) — a 24 kHz con τmax = 873 y ventana 2048 son ~1,4
    // millones de multiplicaciones por ventana, y a ritmo de tick eso no entra en el 5 % de
    // CPU del NFR-1 (medido: 92 % del tiempo real en el build de host).
    //
    // La salida no es bajar la calidad sino **muestrear el eje τ con paso proporcional**:
    // `τ/12`. Asi la densidad RELATIVA de muestras por periodo es constante —doce puntos por
    // lobulo, tanto en A0 como en C7— y la cantidad de lags evaluados pasa de 862 a ~50. Lo
    // que se pierde es la posicion exacta del pico, y eso lo recupera la segunda pasada.
    mKeyCount = 0;
    int bestLag = -1;
    double bestValue = -1.0;

    // --- una sola pasada, y el candidato tiene que ser MAXIMO LOCAL ----------
    //
    // La NSDF vale 1 en τ=0 —la señal correlacionada consigo misma— y baja despacio: en una
    // nota grave sigue por encima de 0,99 en τ=10. Una busqueda que tome el maximo a secas se
    // queda ahi, y la frecuencia que sale es la del lag, no la de la nota. Medido: A0 no se
    // detectaba en absoluto.
    //
    // La defensa NO es barrer ese lobulo hasta que cruce el cero —eso costaba 20 veces mas—
    // sino exigir que el candidato sea **maximo local entre puntos muestreados**. El lobulo de
    // τ=0 es monotono decreciente, asi que ningun punto suyo lo es. Sale gratis.
    //
    // El barrido arranca en `mMinLag` con paso proporcional. Ojo con la variante de arrancarlo
    // mas abajo y "saltar" el lobulo: al hacerlo, en una nota aguda el salto cae ADENTRO del
    // primer pico real y lo saltea entero — C7 se detectaba una octava abajo, un error de
    // octava provocado por el propio codigo que existe para evitarlos.
    int prevLag = -1;
    // Centinela POR ENCIMA del maximo posible de la NSDF (que vive en [-1, 1]): con 0.0 el
    // primer punto muestreado siempre parecia maximo local, y en una nota grave ese punto
    // esta en la ladera del lobulo de τ=0 — o sea justo el candidato que este criterio existe
    // para descartar. A0 volvia a no detectarse.
    double prevValue = 2.0;
    // EL CENTINELA QUE IMPORTA ES ESTE, no el de arriba. Medido con mutacion (T-AGENT-4,
    // 2026-08-19): `pendingValue = 0` mata cuatro tests —el primer lag muestreado, que en una
    // nota grave esta en la ladera del lobulo de τ=0, entra como candidato con NSDF 0,998 y se
    // lo elige por ser el primero: A0, B0 y E2 no se detectaban—, mientras que `prevValue = 0`
    // NO mata ninguno.
    //
    // Y esa asimetria tiene razon: en la primera vuelta el `if` corta en `pendingLag >= 0`
    // (vale -1) ANTES de leer `prevValue`, y acto seguido `prevValue` se pisa con
    // `pendingValue`. O sea que el inicializador de `prevValue` no se lee nunca: quien sostiene
    // la promesa es el corto-circuito, y el 2.0 de arriba es simetria legible, no defensa.
    // Se deja —cuesta cero y documenta la intencion— pero NO se lo cuente como guarda.
    double pendingValue = 2.0;
    int pendingLag = -1;

    for (int lag = mMinLag; lag <= mMaxLag; lag += std::max(1, lag / 12)) {
        const double v = nsdfAt(lag);
        mNsdf[static_cast<size_t>(lag)] = v;

        // El punto ANTERIOR es maximo local si supera a sus dos vecinos muestreados.
        if (pendingLag >= 0 && pendingValue > prevValue && pendingValue >= v) {
            if (mKeyCount < kMaxCandidates) mKeyLags[mKeyCount++] = pendingLag;
            if (pendingValue > bestValue) { bestValue = pendingValue; bestLag = pendingLag; }
        }
        prevLag = pendingLag;
        prevValue = pendingValue;
        pendingLag = lag;
        pendingValue = v;
    }
    // EL BORDE DERECHO, que no es un caso raro sino EL caso de la nota mas grave.
    //
    // El criterio de maximo local necesita un vecino a la derecha, y el ultimo punto
    // muestreado no lo tiene. En A0 el pico cae justo ahi —τ = 873 contra un mMaxLag de
    // 924— asi que sin este cierre la nota mas grave del rango NO SE DETECTA NUNCA. Medido
    // dos veces: es el borde que mas duele porque es el que justifica todo el metodo.
    if (pendingLag >= 0 && pendingValue > prevValue) {
        if (mKeyCount < kMaxCandidates) mKeyLags[mKeyCount++] = pendingLag;
        if (pendingValue > bestValue) { bestValue = pendingValue; bestLag = pendingLag; }
    }
    (void)prevLag;

    if (bestLag < 0 || bestValue < kMinClarity) {
        mHasPitch = false;
        mClarity = bestValue > 0.0 ? bestValue : 0.0;
        mFrequencyHz = 0.0;
        return;
    }

    // --- REQ-033: CADA CANDIDATO SE JUZGA POR SU PICO REAL, NO POR LA MUESTRA GRUESA -----
    //
    // El barrido de arriba deja cada pico ubicado con un error de hasta medio paso (τ/24), y
    // en un espectro rico el lobulo es ANGOSTO. Medido (REQ-033 S1) sobre la E4 de siete senos
    // con H7 a −9,6 dB, a 44,1 kHz: la NSDF vale 0,999 en τ = 67 y 0,816 en el lag 65 que el
    // barrido muestreo; 3τ cayo a 1,3 muestras de su grilla y vale 0,908. Aplicar el umbral
    // del primer pico sobre esos valores GRUESOS decidia por 0,001 a favor de 3τ. Y a 48 kHz,
    // con τ a 2,2 muestras de la grilla y 3τ a 0,4, hasta seis armonicos limpios leian f0/3.
    // O sea que la nota dependia de donde caia la grilla —del rate y de f0—, no de la senal.
    //
    // La salida NO es bajar el umbral (MINI-019: mueve el defecto) ni densificar el barrido
    // (vuelve el costo de la NSDF completa): es refinar CADA candidato —el maximo real en su
    // vecindad ±τ/12, que antes se hacia solo para el ya elegido— y recien despues aplicar el
    // umbral. Un candidato es maximo local entre muestras, asi que su pico real esta a menos
    // de un paso; en esa vecindad la NSDF es unimodal para armonicos hasta ~12 (los que la
    // grilla resuelve), y por eso el maximo de la vecindad ES el pico.
    //
    // Se refina TODO, sin corte temprano: "el primero que supera 0,9·máximo" necesita el
    // máximo VERDADERO, y un corte al primer pico ≥ 0,9 no lo conoce todavia. El costo se
    // mide en el test de costo y queda anotado en la etapa.
    for (int i = 0; i < mKeyCount; ++i) {
        const int lag = mKeyLags[i];
        const int span = std::max(1, lag / 12);
        const int from = std::max(mMinLag, lag - span);
        const int to = std::min(mMaxLag, lag + span);
        int peak = lag;
        double peakValue = mNsdf[static_cast<size_t>(lag)];
        for (int l = from; l <= to; ++l) {
            if (l == lag) continue;
            const double v = nsdfAt(l);
            mNsdf[static_cast<size_t>(l)] = v;
            if (v > peakValue) { peakValue = v; peak = l; }
        }
        mRefinedLags[i] = peak;
        mRefinedNsdf[i] = peakValue;
        if (peakValue > bestValue) { bestValue = peakValue; bestLag = peak; }
    }

    // --- LA DEFENSA CONTRA LA OCTAVA -----------------------------------------
    //
    // Se elige el PRIMER pico que supere `kPeakThreshold · máximo`, no el máximo. En una
    // bordona con el fundamental 20 dB por debajo del segundo parcial, el pico de 2·τ es mas
    // alto — pero el de τ es el primero en superar el umbral, y es el correcto. Bajar el
    // umbral a 1,0 convierte esto en "elegi el maximo" y trae la octava de vuelta.
    //
    // Desde REQ-033 los dos lados de la comparacion son picos REALES (refinados arriba), asi
    // que la eleccion ya no depende de donde cayo la grilla del barrido.
    const double threshold = kPeakThreshold * bestValue;
    int chosen = bestLag;
    for (int i = 0; i < mKeyCount; ++i) {
        if (mRefinedNsdf[i] >= threshold) { chosen = mRefinedLags[i]; break; }
    }
    // Los vecinos inmediatos, para que la parabola tenga sus tres puntos. Los cubre el
    // refinamiento salvo cuando el pico cae en el borde de su vecindad.
    if (chosen - 1 >= 0) mNsdf[static_cast<size_t>(chosen - 1)] = nsdfAt(chosen - 1);
    if (chosen + 1 <= mMaxLag) mNsdf[static_cast<size_t>(chosen + 1)] = nsdfAt(chosen + 1);

    // --- interpolacion parabolica: sin esto el error en la zona aguda se va ---
    //
    // A 24 kHz el periodo de C7 son 11,5 muestras: redondear al entero mas cercano ya cuesta
    // decenas de cents. Tres puntos alrededor del pico dan el vertice con precision
    // sub-muestra.
    double refined = static_cast<double>(chosen);
    if (chosen > 0 && chosen < mMaxLag) {
        const double y0 = mNsdf[static_cast<size_t>(chosen - 1)];
        const double y1 = mNsdf[static_cast<size_t>(chosen)];
        const double y2 = mNsdf[static_cast<size_t>(chosen + 1)];
        const double denom = 2.0 * (2.0 * y1 - y0 - y2);
        if (std::abs(denom) > 1e-12) {
            // La correccion se ACOTA a media muestra. El vertice de una parabola por tres
            // puntos con el maximo en el del medio no puede caer mas lejos que eso, asi que
            // una correccion mayor significa que los tres puntos no describen un pico — y
            // aplicarla manda el lag a cualquier lado. Medido: con el pico mal elegido, la
            // correccion llevaba el lag 10 a -0,06 y la frecuencia salia negativa.
            const double delta = (y2 - y0) / denom;
            refined += std::clamp(delta, -0.5, 0.5);
        }
    }

    mClarity = mNsdf[static_cast<size_t>(chosen)];
    mFrequencyHz = (refined > 0.0) ? (mWorkingRate / refined) : 0.0;
    mHasPitch = mFrequencyHz >= kMinHz && mFrequencyHz <= kMaxHz;
    if (!mHasPitch) mFrequencyHz = 0.0;
}

}  // namespace wma::dsp
