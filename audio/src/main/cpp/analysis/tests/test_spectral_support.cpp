/**
 * @file test_spectral_support.cpp
 * @brief REQ-031 — la bandera de SOPORTE ESPECTRAL: ¿la altura publicada está en la señal?
 *
 * 🔴 POR QUE ESTE ARCHIVO EXISTE
 * ------------------------------
 * El detector lee ciertos timbres un tercio o un quinto ABAJO de su altura verdadera, y con
 * instrumento declarado el motor lo publicaba CONVERGIDO sobre la cuerda equivocada, con una
 * desviacion plausible (−1,955 cents: la coincidencia entre el 3.er armonico de A2 y el f0 real
 * de E4). Es el modo de falla que los dos repos prohiben.
 *
 * El motor de la falla NO es "H3 domina": es que falten los parciales que DESAMBIGUAN el
 * periodo. Con H2 presente el periodo queda fijado y no hay error, aunque el fundamental este
 * ausente del todo. Por eso el estimulo del falso es f0 debil + H3 + H5, y por eso el criterio
 * pregunta por el fundamental **y su octava**: una bordona legitima —fundamental 40 dB por
 * debajo de H2— tiene el fundamental MAS hundido que el falso, y una pregunta sobre el
 * fundamental solo la mataria.
 *
 * ESTA ETAPA (S1) AFIRMA LA BANDERA, NO EL ESTADO — Y ES A PROPOSITO
 * -----------------------------------------------------------------
 * Los tests de aca dicen que la bandera LEE bien: sana = 1, bordona = 1, falso = 0. El estado
 * del falso sigue siendo CONVERGED hasta S2, y fijarlo aca seria escribir el defecto en el
 * contrato. Lo que SI se afirma del falso es que el estimulo reprodujo la lectura falsa
 * (`detectedHz` ≈ f0/3): sin eso, un 0 en la bandera podria salir por otra razon.
 *
 * 🔴 EL BUFFER VA ESTEREO INTERCALADO. Ver `test_foreign_note.cpp`: pasarle mono a este camino
 * da `rms=0` y `hz=0` en TODOS los casos, y una bandera en NaN sobre silencio seria verde sobre
 * nada. Los casos que exigen 1 son el control positivo que lo atrapa.
 */
#include "../AnalysisRing.h"
#include "../AnalysisSnapshot.h"
#include "../AnalysisThread.h"
#include "support/SyntheticSignal.h"

#include <gtest/gtest.h>
#include <cmath>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

using namespace wma::analysis;

namespace {

constexpr int kRate = 44100;
constexpr int kFrames = kRate * 5;   // 5 s: el presupuesto de convergencia del producto

constexpr double kE4 = 329.6276;
constexpr double kE2 = 82.4069;

/// Las seis de guitarra estandar, del catalogo COMPARTIDO (la leccion de REQ-027 S3).
std::vector<double> guitarraHz() {
    std::vector<double> hz;
    for (const auto& s : wma_test::catalogStrings())
        if (std::string(s.name).rfind("guitarra", 0) == 0) hz.push_back(s.hz);
    return hz;
}

std::vector<float> toStereo(const std::vector<float>& mono) {
    std::vector<float> b(mono.size() * 2, 0.0f);
    for (size_t i = 0; i < mono.size(); ++i) { b[i*2] = mono[i]; b[i*2+1] = mono[i]; }
    return b;
}

double dB(double d) { return std::pow(10.0, d / 20.0); }

/// Cuerda pulsada: parciales 1..4 con decaimiento 1/n. El control positivo de siempre.
std::vector<float> cuerdaSana(double f0) {
    return toStereo(wma_test::partialsWithAmplitudes(
        f0, 0.0, {0.5, 0.25, 0.125, 0.0625}, kRate, kFrames));
}

/**
 * EL FALSO: una E4 con el fundamental `f0Db` por debajo de H3, H3 a 0 dB y H5 a −6 dB, y
 * NADA en H2 ni H4. Sin H2 el periodo es ambiguo y el detector lo lee en f0/3 = 109,87 Hz,
 * que con las seis de guitarra declaradas engancha A2. Medido: entre −24 y −16 dB el motor
 * converge sobre A2 a −1,955 cents.
 */
std::vector<float> falsoSubarmonico(double f0Db) {
    return toStereo(wma_test::partialsWithAmplitudes(
        kE4, 0.0, {0.5 * dB(f0Db), 0.0, 0.5, 0.0, 0.5 * dB(-6.0)}, kRate, kFrames));
}

/// Bordona E2 con el fundamental `dbBelowSecond` por debajo de H2 (R-PITCH-35).
std::vector<float> bordona(double dbBelowSecond) {
    return toStereo(wma_test::stringWithWeakFundamental(kE2, 0.0, 6, kRate, kFrames,
                                                        dbBelowSecond));
}

/// El espectro que el consumidor midio sobre su archivo: f0 −4,8 · H2 −1,8 · H3 0 · −8 · −12.
std::vector<float> timbreDelConsumidor(double B) {
    return toStereo(wma_test::partialsWithAmplitudes(
        kE4, B, {0.5 * dB(-4.8), 0.5 * dB(-1.8), 0.5, 0.5 * dB(-8.0), 0.5 * dB(-12.0)},
        kRate, kFrames));
}

struct Lectura {
    bool ok = false;
    int state = -1;
    double cents = NAN, detectedHz = 0.0;
    float support = NAN;
    /// La bandera en CADA publicacion a partir del segundo 1: para preguntar por parpadeo.
    std::vector<float> supportTrail;
    /// El estado de esas mismas publicaciones, en el mismo orden (S2: el invariante de R-PITCH-37).
    std::vector<int> stateTrail;
};

/**
 * El mismo lazo que `OfflineAnalysis::analyzeBuffer`, sin thread y sin relojes: el mismo
 * buffer da el mismo resultado siempre (REQ-002). Ademas de la ultima lectura, guarda la
 * bandera de cada publicacion posterior al primer segundo, que es lo que un consumidor VE.
 */
Lectura analizar(const std::vector<float>& buf, double targetHz,
                 const std::vector<double>& candidatos) {
    AnalysisRing ring;
    AnalysisSnapshot snapshot;
    AnalysisThread analysis(ring, snapshot);

    ring.setCaptureRate(kRate);
    analysis.setTargetHz(targetHz);
    analysis.setCandidates(candidatos.empty() ? nullptr : candidatos.data(),
                           static_cast<int>(candidatos.size()));

    Lectura r;
    const int capacity = static_cast<int>(AnalysisRing::kCapacityFrames);
    int written = 0;
    while (written < kFrames) {
        const int chunk = (kFrames - written) < capacity ? (kFrames - written) : capacity;
        ring.writeStereo(buf.data() + static_cast<size_t>(written) * 2, chunk);
        written += chunk;
        while (analysis.drainOnce() != AnalysisThread::DrainOutcome::kRingEmpty) {
            float v[kSnapshotValueCount];
            if (written > kRate && snapshot.read(v)) {
                r.supportTrail.push_back(v[kSnapSpectralSupport]);
                r.stateTrail.push_back(static_cast<int>(v[kSnapState]));
            }
        }
    }

    float v[kSnapshotValueCount];
    r.ok = snapshot.read(v);
    if (!r.ok) return r;
    r.state      = static_cast<int>(v[kSnapState]);
    r.cents      = static_cast<double>(v[kSnapCents]);
    r.detectedHz = static_cast<double>(v[kSnapDetectedHz]);
    r.support    = v[kSnapSpectralSupport];
    return r;
}

/// Cuantos valores DISTINTOS tomo la bandera (NaN cuenta como uno). 1 = no parpadeo.
size_t valoresDistintos(const std::vector<float>& trail) {
    std::set<int> seen;
    for (float f : trail) seen.insert(std::isnan(f) ? -1 : static_cast<int>(f));
    return seen.size();
}

}  // namespace

// ===========================================================================================
// AC-031.4 — la bandera compañera de `detectedHz`, publicada SIEMPRE
// ===========================================================================================

/**
 * AC-031.4 · el control positivo, primera mitad: una cuerda sana tiene soporte.
 *
 * Bug plausible: una sonda que pregunte por energia RELATIVA AL TOTAL con un umbral alto, o
 * que mida sobre un bloque demasiado corto, marcaria 0 tambien aqui. Este test y el de la
 * bordona son lo que impide que "nunca confies" pase por arreglo.
 */
TEST(SpectralSupport, AC0314_AHealthyStringHasSupport) {
    const auto conCandidatos = analizar(cuerdaSana(kE4), kE4, guitarraHz());
    ASSERT_TRUE(conCandidatos.ok);
    ASSERT_NEAR(conCandidatos.detectedHz, kE4, 1.0) << "el estimulo no reprodujo la nota";
    EXPECT_EQ(conCandidatos.support, 1.0f) << "una cuerda sana tiene que tener soporte";

    // Y SIN candidatos, midiendo contra la cuerda directamente: la bandera califica a
    // `detectedHz`, no al objetivo, asi que no puede depender de como se eligio el objetivo.
    const auto sinCandidatos = analizar(cuerdaSana(kE4), kE4, {});
    ASSERT_TRUE(sinCandidatos.ok);
    EXPECT_EQ(sinCandidatos.support, 1.0f);
}

/**
 * AC-031.4 · el control positivo, segunda mitad — y NO es decorativa: la bordona a −40 dB de
 * H2 es el caso que REFUTO la variante A ("energia en la banda de detectedHz"). Ahi el
 * fundamental esta MAS hundido que en el falso (−44,1 contra −37,5 dB), asi que cualquier
 * sonda que pregunte solo por el fundamental la rechaza. Lo que la salva es su OCTAVA: en una
 * cuerda real sin fundamental, H2 es el pico.
 *
 * Bug plausible: `X(det)` en vez de `max(X(det), X(2·det))`. Mata cuerdas reales — el lado que
 * el consumidor verifico en hardware (R-PITCH-35).
 */
TEST(SpectralSupport, AC0314_ALegitimateBassStringFortyDbBelowItsOctaveHasSupport) {
    for (double db : {6.0, 20.0, 40.0}) {
        SCOPED_TRACE("fundamental " + std::to_string(static_cast<int>(db)) + " dB bajo H2");
        const auto conCandidatos = analizar(bordona(db), kE4, guitarraHz());
        ASSERT_TRUE(conCandidatos.ok);
        ASSERT_NEAR(conCandidatos.detectedHz, kE2, 1.0) << "el estimulo no reprodujo la E2";
        EXPECT_EQ(conCandidatos.support, 1.0f)
            << "la bordona legitima tiene su octava: rechazarla es el falso negativo que "
               "R-PITCH-35 prohibe";

        const auto sinCandidatos = analizar(bordona(db), kE2, {});
        ASSERT_TRUE(sinCandidatos.ok);
        EXPECT_EQ(sinCandidatos.support, 1.0f);
    }
}

/**
 * AC-031.4 · el timbre REAL del consumidor —H3 dominante, f0 a −4,8 dB— tiene soporte, con y
 * sin inarmonicidad. Es el caso que NO falla (el periodo lo fija H2), y es el que queda mas
 * cerca del umbral por el lado de lo aceptado: max(X(det), X(2·det)) = −1,8 dB.
 *
 * Bug plausible: un umbral demasiado alto (−1 dB, "el fundamental o la octava tiene que SER
 * el pico") lo mata. Es la otra mitad del control: el umbral no puede subir sin que esto
 * lo diga.
 */
TEST(SpectralSupport, AC0314_TheConsumersRealTimbreHasSupport) {
    for (double B : {0.0, 4e-4}) {
        SCOPED_TRACE("B = " + std::to_string(B));
        const auto r = analizar(timbreDelConsumidor(B), kE4, guitarraHz());
        ASSERT_TRUE(r.ok);
        ASSERT_NEAR(r.detectedHz, kE4, 2.0) << "con H2 presente el periodo queda fijado";
        EXPECT_EQ(r.support, 1.0f);
    }
}

/**
 * AC-031.4 · EL FALSO: la altura publicada no tiene soporte, y la bandera lo dice.
 *
 * Se afirma que el estimulo reprodujo la lectura falsa (`detectedHz` ≈ f0/3): es la
 * precondicion, no el desenlace. El ESTADO no se afirma: hoy es CONVERGED y ese es el defecto
 * que S2 da vuelta; fijarlo aca lo escribiria en el contrato.
 *
 * Bug plausible: una sonda que mire el NSDF en vez de la señal. El NSDF de un submultiplo es
 * alto POR CONSTRUCCION —un multiplo del periodo tambien es un periodo—, asi que diria 1.
 * Igual la claridad: 0,9946 sobre este falso.
 */
TEST(SpectralSupport, AC0314_TheFalseSubharmonicHasNoSupportAndTheFlagSaysSo) {
    for (double f0Db : {-24.0, -20.0, -16.0}) {
        SCOPED_TRACE("f0 a " + std::to_string(static_cast<int>(f0Db)) + " dB");
        const auto r = analizar(falsoSubarmonico(f0Db), kE4, guitarraHz());
        ASSERT_TRUE(r.ok);
        ASSERT_NEAR(r.detectedHz, kE4 / 3.0, 0.5)
            << "el estimulo no reprodujo la lectura en f0/3: sin eso el 0 no dice nada";
        EXPECT_EQ(r.support, 0.0f)
            << "ni el fundamental publicado ni su octava estan en la señal";
    }
}

/**
 * AC-031.4 · SIEMPRE, no solo al no converger — en las dos direcciones.
 *
 * Sin altura no hay nada que calificar y la bandera es NaN, no 0: un 0 diria "vi una altura
 * y no le creo". Y con altura pero SIN estado convergido —el zumbido de red, que la compuerta
 * de ausencia declara NO_SIGNAL con `detectedHz` = 50— la bandera igual califica: 1, porque
 * los 50 Hz estan en la señal.
 *
 * Bug plausible: publicar la bandera solo dentro de `haveReading`, o derivarla del estado.
 */
TEST(SpectralSupport, AC0314_TheFlagQualifiesThePitchRegardlessOfTheState) {
    // Silencio: el snapshot se publica (NO_SIGNAL) y no hay altura.
    const auto silencio = analizar(std::vector<float>(static_cast<size_t>(kFrames) * 2, 0.0f),
                                   kE4, guitarraHz());
    ASSERT_TRUE(silencio.ok);
    EXPECT_EQ(silencio.state, kStateNoSignal);
    EXPECT_EQ(silencio.detectedHz, 0.0);
    EXPECT_TRUE(std::isnan(silencio.support)) << "sin altura no hay nada que calificar";

    // Zumbido de red: hay altura (50 Hz, no es cuerda de nada) y el estado no es convergido.
    std::vector<float> mono(static_cast<size_t>(kFrames), 0.0f);
    for (int i = 0; i < kFrames; ++i) {
        const double p = 2.0 * M_PI * 50.0 * i / kRate;
        mono[static_cast<size_t>(i)] = static_cast<float>(
            0.30 * (std::sin(p) + 0.5 * std::sin(2 * p) + 0.33 * std::sin(3 * p)));
    }
    const auto zumbido = analizar(toStereo(mono), kE4, guitarraHz());
    ASSERT_TRUE(zumbido.ok);
    ASSERT_NE(zumbido.state, kStateConverged);
    ASSERT_NEAR(zumbido.detectedHz, 50.0, 1.0) << "el estimulo no reprodujo el zumbido";
    EXPECT_EQ(zumbido.support, 1.0f) << "los 50 Hz ESTAN en la señal: la bandera lo dice aunque "
                                        "el estado sea de ausencia";
}

/**
 * AC-031.4 · la bandera NO PARPADEA sobre una señal estable, en ninguna de las tres
 * poblaciones. Es la disciplina de REQ-014 / MINI-010 (la ventana ciega de 4 marcos),
 * verificada antes de que haga falta: un valor por tick puede oscilar en el borde, y si
 * oscilara la salida seria una ventana, nunca bajar el umbral.
 *
 * Bug plausible, MEDIDO en 1.1: la sonda sin ventana de Hann. Sobre 2048 frames
 * rectangulares el falso oscila entre −24 y −15 dB de bloque a bloque, o sea que cruza el
 * umbral de −25 a veces si y a veces no. Con Hann queda en −61 ± 0,7.
 */
TEST(SpectralSupport, AC0314_TheFlagDoesNotFlickerOnASteadySignal) {
    struct Caso { const char* nombre; std::vector<float> buf; float esperado; };
    const std::vector<Caso> casos = {
        {"cuerda sana",       cuerdaSana(kE4),        1.0f},
        {"bordona −40 dB",    bordona(40.0),          1.0f},
        {"falso −24 dB",      falsoSubarmonico(-24.0), 0.0f},
        {"falso −16 dB",      falsoSubarmonico(-16.0), 0.0f},
    };
    for (const auto& c : casos) {
        SCOPED_TRACE(c.nombre);
        const auto r = analizar(c.buf, kE4, guitarraHz());
        ASSERT_TRUE(r.ok);
        ASSERT_GT(r.supportTrail.size(), 50u) << "el rastro es demasiado corto para hablar";
        EXPECT_EQ(valoresDistintos(r.supportTrail), 1u)
            << "la bandera cambio de valor sobre una señal estable";
        EXPECT_EQ(r.supportTrail.back(), c.esperado);
    }
}

/**
 * AC-031.4 · la bandera sigue a la ALTURA que califica, no a la ultima evaluacion.
 *
 * El detector puede perder la altura sin producir un veredicto nuevo: `onSourceChanged()` lo
 * resetea, y hasta que complete una ventana propia `detectedHz` es 0. En ese hueco la ultima
 * evaluacion de la sonda todavia es la de la fuente VIEJA, y publicarla seria calificar una
 * altura que ya no existe. A 48 kHz el detector decima por 2 y necesita 4096 frames, asi que
 * un bloque de 2048 despues del cambio es exactamente ese hueco — a 44,1 kHz no se ve, porque
 * la primera vuelta ya trae veredicto.
 *
 * Bug plausible, MEDIDO como mutante que sobrevivia: derivar la bandera de `mSupportDb`
 * solo, sin preguntarle `hasPitch()` al detector al publicar.
 */
TEST(SpectralSupport, AC0314_TheFlagFollowsTheDetectedPitchNotTheLastEvaluation) {
    constexpr int kRate48 = 48000;
    const int frames = kRate48 * 2;
    const auto buf = toStereo(wma_test::partialsWithAmplitudes(
        kE4, 0.0, {0.5, 0.25, 0.125, 0.0625}, kRate48, frames));

    AnalysisRing ring;
    AnalysisSnapshot snapshot;
    AnalysisThread analysis(ring, snapshot);
    ring.setCaptureRate(kRate48);
    analysis.setTargetHz(kE4);

    const int capacity = static_cast<int>(AnalysisRing::kCapacityFrames);
    int written = 0;
    while (written < frames) {
        const int chunk = (frames - written) < capacity ? (frames - written) : capacity;
        ring.writeStereo(buf.data() + static_cast<size_t>(written) * 2, chunk);
        written += chunk;
        while (analysis.drainOnce() != AnalysisThread::DrainOutcome::kRingEmpty) {}
    }
    float v[kSnapshotValueCount];
    ASSERT_TRUE(snapshot.read(v));
    ASSERT_NEAR(v[kSnapDetectedHz], kE4, 1.0) << "antes del cambio hay altura";
    ASSERT_EQ(v[kSnapSpectralSupport], 1.0f) << "y tiene soporte";

    // Cambio de fuente. La primera vuelta consume la bandera con el ring vacio (descarta y
    // resetea); despues entra UN bloque de 2048 frames: menos que la ventana del detector.
    analysis.onSourceChanged();
    ASSERT_EQ(analysis.drainOnce(), AnalysisThread::DrainOutcome::kRingEmpty);
    ring.writeStereo(buf.data(), AnalysisThread::kDrainFrames);
    ASSERT_EQ(analysis.drainOnce(), AnalysisThread::DrainOutcome::kPublished);

    ASSERT_TRUE(snapshot.read(v));
    ASSERT_EQ(v[kSnapDetectedHz], 0.0f)
        << "el detector reseteado no tiene altura hasta completar una ventana";
    EXPECT_TRUE(std::isnan(v[kSnapSpectralSupport]))
        << "sin altura publicada, la bandera no puede seguir calificando la vieja";
}

// ===========================================================================================
// REQ-031 S2 — la compuerta: lo que no tiene soporte no se da por bueno
// ===========================================================================================

/**
 * AC-031.2 + AC-031.5 + AC-031.6 · EL CONTROL NEGATIVO. Nacio ROJO sobre S1: ahi el falso daba
 * `CONVERGED`, 109,874 Hz, −1,955 cents — "A2, casi afinada" sobre una E4.
 *
 * Con la compuerta: el estado es `NO_LOCK` (AC-031.5) —"hay señal pero el estimador no
 * enganchó", no `MEASURING`, que es el spinner eterno que el consumidor describio como peor
 * que declarar ausencia—, la bandera es 0, y `detectedHz` SIGUE saliendo (AC-031.6): "vi
 * 109,87 y no le creo" es mas util que el silencio. Los cents NO salen: una desviacion contra
 * una cuerda que no es la que suena no mide nada.
 *
 * Bug plausible: compuerta derivada de un SEGUNDO calculo (la clase de REQ-030), o que cambie
 * el estado a `MEASURING` en vez de `NO_LOCK`, o que anule `detectedHz` al rechazar.
 */
TEST(SpectralSupport, AC0312_TheFalseSubharmonicIsNoLongerPublishedAsConverged) {
    for (double f0Db : {-24.0, -20.0, -16.0}) {
        SCOPED_TRACE("f0 a " + std::to_string(static_cast<int>(f0Db)) + " dB");
        const auto r = analizar(falsoSubarmonico(f0Db), kE4, guitarraHz());
        ASSERT_TRUE(r.ok);
        ASSERT_NEAR(r.detectedHz, kE4 / 3.0, 0.5)
            << "el estimulo no reprodujo la lectura en f0/3: sin eso el test no dice nada";
        ASSERT_EQ(r.support, 0.0f) << "la bandera de S1 tiene que seguir en 0";

        EXPECT_NE(r.state, kStateConverged)
            << "AC-031.2: una altura sin soporte no se publica como convergida";
        EXPECT_EQ(r.state, kStateNoLock)
            << "AC-031.5: con la bandera en 0 el estado es NO_LOCK, no MEASURING";
        EXPECT_TRUE(std::isnan(r.cents))
            << "una desviacion contra una cuerda que no suena no es una medicion";
        EXPECT_NEAR(r.detectedHz, kE4 / 3.0, 0.5)
            << "AC-031.6: la altura que vio se sigue publicando, marcada como no confiable";
    }
}

/**
 * AC-031.5 · el invariante, publicacion por publicacion: NUNCA sale un snapshot con la
 * bandera en 0 y un estado distinto de NO_LOCK, ni uno CONVERGIDO con la bandera distinta de
 * 1. Es R-PITCH-37 ("convergido con bandera en 0") verificado sobre TODO el rastro, no sobre
 * la ultima lectura — la compuerta y la bandera salen del mismo calculo, y esto es lo que lo
 * afirma.
 *
 * Bug plausible: dos derivaciones del mismo concepto que se pisan en la transicion (la
 * compuerta de nivel y `haveReading` de REQ-014, otra vez).
 */
TEST(SpectralSupport, AC0315_AFlagAtZeroAlwaysComesWithNoLockAndConvergedAlwaysWithOne) {
    struct Caso { const char* nombre; std::vector<float> buf; };
    const std::vector<Caso> casos = {
        {"falso −20 dB",   falsoSubarmonico(-20.0)},
        {"cuerda sana",    cuerdaSana(kE4)},
        {"bordona −40 dB", bordona(40.0)},
    };
    for (const auto& c : casos) {
        SCOPED_TRACE(c.nombre);
        const auto r = analizar(c.buf, kE4, guitarraHz());
        ASSERT_TRUE(r.ok);
        ASSERT_EQ(r.supportTrail.size(), r.stateTrail.size());
        ASSERT_GT(r.stateTrail.size(), 50u);
        for (size_t i = 0; i < r.stateTrail.size(); ++i) {
            const float flag = r.supportTrail[i];
            const int state = r.stateTrail[i];
            if (flag == 0.0f) {
                EXPECT_EQ(state, kStateNoLock) << "publicacion " << i;
            }
            if (state == kStateConverged) {
                EXPECT_EQ(flag, 1.0f) << "publicacion " << i << ": convergido sin soporte";
            }
        }
    }
}

/**
 * AC-031.3 · EL CONTROL POSITIVO, EN SUS DOS MITADES, con y sin candidatos. La segunda mitad
 * NO es decorativa: la bordona a −40 dB de H2 es el caso que refuto la variante A. Sin ella,
 * "no converjas nunca" satisface AC-031.1 y AC-031.2 — y el falso negativo es el lado que el
 * consumidor verifico en hardware (R-PITCH-35).
 *
 * "La exactitud de hoy" es el presupuesto del producto: 0,1 cents, el mismo que
 * `test_fast_mode_wiring.cpp` exige.
 */
TEST(SpectralSupport, AC0313_HealthyAndLegitimateBassStringsStillConvergeWithTodaysAccuracy) {
    struct Caso { const char* nombre; std::vector<float> buf; double target; bool candidatos; double hz; };
    const std::vector<Caso> casos = {
        {"sana E4, sin candidatos",      cuerdaSana(kE4), kE4, false, kE4},
        {"sana E4, con candidatos",      cuerdaSana(kE4), kE4, true,  kE4},
        {"bordona −40, sin candidatos",  bordona(40.0),   kE2, false, kE2},
        {"bordona −40, con candidatos (reenganche desde E4)", bordona(40.0), kE4, true, kE2},
    };
    for (const auto& c : casos) {
        SCOPED_TRACE(c.nombre);
        const auto r = analizar(c.buf, c.target, c.candidatos ? guitarraHz() : std::vector<double>{});
        ASSERT_TRUE(r.ok);
        ASSERT_NEAR(r.detectedHz, c.hz, 1.0) << "el estimulo no reprodujo la nota";
        EXPECT_EQ(r.support, 1.0f);
        EXPECT_EQ(r.state, kStateConverged) << "la compuerta no puede matar una cuerda real";
        EXPECT_NEAR(r.cents, 0.0, 0.1) << "el presupuesto de exactitud del producto";
    }
}

/**
 * AC-031.3 · la AUSENCIA no se toca: silencio sigue siendo NO_SIGNAL, no NO_LOCK. La compuerta
 * solo actua sobre una altura publicada sin soporte, y sobre silencio no hay altura.
 *
 * Bug plausible: derivar la bandera como 0 (y no NaN) sin altura, y que la compuerta la lea.
 */
TEST(SpectralSupport, AC0313_SilenceIsStillNoSignalNotNoLock) {
    const auto silencio = analizar(std::vector<float>(static_cast<size_t>(kFrames) * 2, 0.0f),
                                   kE4, guitarraHz());
    ASSERT_TRUE(silencio.ok);
    EXPECT_EQ(silencio.state, kStateNoSignal);
    EXPECT_TRUE(std::isnan(silencio.support));
}

/**
 * AC-031.5 · SIN altura publicada la compuerta NO ACTUA. La bandera es NaN, y NaN no es 0.
 *
 * Tras el falso (bandera 0, NO_LOCK) entra UN bloque de ruido: el detector produce un
 * veredicto sin altura, la compuerta de ausencia todavia no dispara (espera
 * `kQuietUpdatesToDeclare` veredictos mudos, REQ-019) y el estado vuelve a ser el de siempre
 * para un hueco del detector: MEASURING (MINI-010). Que sea NO_LOCK ahi significaria que la
 * compuerta juzgo una altura que no se publico — la bandera y el estado dejarian de hablar
 * de lo mismo.
 *
 * Bug plausible, MEDIDO como mutante que sobrevivia: `unsupported = flag != 1` en vez de
 * `flag == 0`, que trata "no hay altura" como "no le creo".
 */
TEST(SpectralSupport, AC0315_WithoutAPublishedPitchTheGateDoesNotAct) {
    const auto falso = falsoSubarmonico(-20.0);
    AnalysisRing ring;
    AnalysisSnapshot snapshot;
    AnalysisThread analysis(ring, snapshot);
    ring.setCaptureRate(kRate);
    analysis.setTargetHz(kE4);
    const auto cand = guitarraHz();
    analysis.setCandidates(cand.data(), static_cast<int>(cand.size()));

    const int capacity = static_cast<int>(AnalysisRing::kCapacityFrames);
    const int frames = kRate * 3;
    int written = 0;
    while (written < frames) {
        const int chunk = (frames - written) < capacity ? (frames - written) : capacity;
        ring.writeStereo(falso.data() + static_cast<size_t>(written) * 2, chunk);
        written += chunk;
        while (analysis.drainOnce() != AnalysisThread::DrainOutcome::kRingEmpty) {}
    }
    float v[kSnapshotValueCount];
    ASSERT_TRUE(snapshot.read(v));
    ASSERT_NEAR(v[kSnapDetectedHz], kE4 / 3.0, 0.5) << "el estimulo no reprodujo el falso";
    ASSERT_EQ(v[kSnapSpectralSupport], 0.0f);
    ASSERT_EQ(static_cast<int>(v[kSnapState]), kStateNoLock) << "la compuerta actuo sobre el falso";

    // Un bloque de ruido blanco, bien por encima del piso de nivel y sin altura.
    std::vector<float> ruido(static_cast<size_t>(AnalysisThread::kDrainFrames) * 2, 0.0f);
    uint32_t seed = 777u;
    for (size_t i = 0; i < ruido.size(); i += 2) {
        seed = seed * 1664525u + 1013904223u;
        const float x = 0.2f * ((static_cast<float>(seed) / 2147483648.0f) - 1.0f);
        ruido[i] = ruido[i + 1] = x;
    }
    // El detector decima por 2 a 44,1 kHz (`lround(44100/24000)`), asi que su ventana son
    // 4096 frames: el primer veredicto sin altura llega en el segundo bloque de ruido, o en
    // el tercero si la ventana quedo a caballo del falso. La ausencia necesita TRES
    // veredictos mudos, o sea que el primero la encuentra todavia sin declarar.
    bool sinAltura = false;
    for (int bloque = 0; bloque < 4 && !sinAltura; ++bloque) {
        ring.writeStereo(ruido.data(), AnalysisThread::kDrainFrames);
        ASSERT_EQ(analysis.drainOnce(), AnalysisThread::DrainOutcome::kPublished);
        ASSERT_TRUE(snapshot.read(v));
        sinAltura = v[kSnapDetectedHz] == 0.0f;
    }
    ASSERT_TRUE(sinAltura) << "el detector nunca solto la altura sobre ruido: el test no vio el caso";
    ASSERT_NE(static_cast<int>(v[kSnapState]), kStateNoSignal)
        << "la ausencia ya se declaro: el test llego tarde al hueco";
    ASSERT_TRUE(std::isnan(v[kSnapSpectralSupport])) << "sin altura la bandera es NaN, no 0";
    EXPECT_EQ(static_cast<int>(v[kSnapState]), kStateMeasuring)
        << "sin altura publicada la compuerta de soporte no puede decidir NO_LOCK: el hueco "
           "del detector sigue siendo MEASURING hasta que la ausencia se declare (REQ-019)";
}

// ===========================================================================================
// ROJO CONOCIDO, declarado: el reproductor minimo de Tunio (nota del 07/09 b, §3)
// ===========================================================================================

/**
 * 🔴 CONTROL NEGATIVO QUE NACE ROJO, Y LO DICE. Es el reproductor mínimo que el consumidor
 * destiló de `guitarra-limpia_E4` con su tabla por tramos: SIETE senos armonicos con los niveles
 * medidos en el tramo estable (H1 −7,2 · H2 −3,2 · H3 −0,6 · H4 0 · H5 −15,1 · H6 −15,0) y H7 a
 * −9,6 dB. Con seis el motor converge en E4; con el septimo lee f0/3 (109,87 Hz) y la bandera de
 * REQ-031 dice 0. Reproducido aca el 2026-09-07, identico: con candidatos NO_LOCK, sin candidatos
 * NO_SIGNAL, 109,874 Hz en los dos.
 *
 * NO es el caso de REQ-031 (ahi faltaban los parciales que desambiguan el periodo; aca estan
 * TODOS hasta H7 y el detector igual elige 3τ): es un defecto del DETECTOR GRUESO con espectros
 * ricos, y es un REQ propio, propuesto y todavia sin abrir — decision de producto. Hasta que
 * arranque, este test corre la medicion, la IMPRIME en el veredicto y sale SKIPPED, nunca
 * PASSED (la regla de `regen-golden.sh` y del corpus: una corrida que no verifico no cuenta). Con
 * `WMA_RUN_PENDING=1` deja de saltear y muestra el rojo. El control POSITIVO (seis parciales) si
 * se afirma siempre: si algun dia esto pasa "solo", que sea porque el detector cambio, no porque
 * la sintesis se rompio.
 *
 * Un rojo conocido que no dice que es conocido se re-investiga cada vez; por eso el veredicto
 * lleva el numero y la referencia.
 */
TEST(SpectralSupport, PendingReq_SevenHarmonicsWithAStrongSeventhAreDetectedAtTheirFundamental) {
    const std::vector<double> seis =
        {0.5 * dB(-7.2), 0.5 * dB(-3.2), 0.5 * dB(-0.6), 0.5 * dB(0.0), 0.5 * dB(-15.1), 0.5 * dB(-15.0)};
    std::vector<double> siete = seis;
    siete.push_back(0.5 * dB(-9.6));

    const auto conSeis = analizar(toStereo(wma_test::partialsWithAmplitudes(kE4, 0.0, seis, kRate, kFrames)),
                                  kE4, guitarraHz());
    ASSERT_TRUE(conSeis.ok);
    ASSERT_NEAR(conSeis.detectedHz, kE4, 1.0) << "el control positivo (seis parciales) dejo de converger";
    ASSERT_EQ(conSeis.state, kStateConverged);
    ASSERT_EQ(conSeis.support, 1.0f);

    const auto buf = toStereo(wma_test::partialsWithAmplitudes(kE4, 0.0, siete, kRate, kFrames));
    const auto conCand = analizar(buf, kE4, guitarraHz());
    const auto sinCand = analizar(buf, kE4, {});
    ASSERT_TRUE(conCand.ok);
    ASSERT_TRUE(sinCand.ok);

    if (std::getenv("WMA_RUN_PENDING") == nullptr) {
        GTEST_SKIP() << "ROJO CONOCIDO (REQ propuesto: el septimo armonico fuerte lee f0/3; nota de "
                        "Tunio 2026-09-07 b, §3). Medido ahora: con candidatos state=" << conCand.state
                     << " detectedHz=" << conCand.detectedHz << " support=" << conCand.support
                     << " | sin candidatos state=" << sinCand.state << " detectedHz=" << sinCand.detectedHz
                     << " support=" << sinCand.support << ". Se espera E4 (" << kE4
                     << ") con soporte. WMA_RUN_PENDING=1 lo muestra rojo. NO cuenta como cobertura.";
    }
    EXPECT_NEAR(conCand.detectedHz, kE4, 1.0) << "con candidatos: el detector leyo f0/3";
    EXPECT_EQ(conCand.support, 1.0f);
    EXPECT_NEAR(sinCand.detectedHz, kE4, 1.0) << "sin candidatos: el detector leyo f0/3";
    EXPECT_EQ(sinCand.support, 1.0f);
}
