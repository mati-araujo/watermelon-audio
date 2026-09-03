/**
 * @file test_foreign_note.cpp
 * @brief REQ-029 S1 — una nota que no es la seleccionada NO es ausencia de señal.
 *
 * Reportado por un consumidor sobre 2.15.0 y presente tambien en 2.10.0. De las 36
 * combinaciones de objetivo x tono sobre las seis cuerdas, las 30 de afuera de la
 * diagonal publicaban `NO_SIGNAL` con `detectedHz` EXACTO y claridad 0,9999.
 *
 * Los tests del caso SIN instrumento declarado no viven aca todavia: son de REQ-029 S2
 * y estan escritos en su etapa. Un test rojo no se commitea, y uno saltado tampoco
 * cuenta como cobertura (misma regla que `regen-golden.sh`).
 *
 * 🔴 EL BUFFER VA ESTEREO INTERCALADO, y no es un detalle de estilo.
 * `analyzeBuffer` lee `frames*2` floats. El primer repro de este defecto le paso
 * MONO y dio "30 de 36" — el numero correcto — con `rms=0` y `hz=0` en las 36,
 * diagonales incluidas. O sea: reprodujo el conteo sobre SILENCIO. Las seis
 * diagonales son el control positivo y son lo unico que lo atrapo.
 */
#include "../AnalysisRing.h"
#include "../AnalysisSnapshot.h"
#include "../AnalysisThread.h"
#include "../OfflineAnalysis.h"
#include "support/SyntheticSignal.h"

#include <gtest/gtest.h>
#include <cmath>
#include <string>
#include <vector>

using namespace wma::analysis;

namespace {

constexpr int kRate = 44100;
constexpr int kFrames = kRate * 3;

/// Las seis de guitarra, del catalogo COMPARTIDO. Una tabla copiada aca seria una
/// segunda fuente de verdad que se desincroniza (la leccion de REQ-027 S3).
std::vector<double> guitarraHz() {
    std::vector<double> hz;
    for (const auto& s : wma_test::catalogStrings())
        if (std::string(s.name).rfind("guitarra", 0) == 0) hz.push_back(s.hz);
    return hz;
}

std::vector<const char*> guitarraNombres() {
    std::vector<const char*> n;
    for (const auto& s : wma_test::catalogStrings())
        if (std::string(s.name).rfind("guitarra", 0) == 0) n.push_back(s.name);
    return n;
}

std::vector<float> toStereo(const std::vector<float>& mono) {
    std::vector<float> b(mono.size() * 2, 0.0f);
    for (size_t i = 0; i < mono.size(); ++i) { b[i*2] = mono[i]; b[i*2+1] = mono[i]; }
    return b;
}

/// Cuerda pulsada: parciales 1..4 con decaimiento 1/n.
std::vector<float> cuerda(double f0) {
    return toStereo(wma_test::partialsWithAmplitudes(
        f0, 0.0, {0.5, 0.25, 0.125, 0.0625}, kRate, kFrames));
}

/// Zumbido de red: 50 Hz con sus armonicos. NO es una cuerda de ninguna afinacion.
std::vector<float> zumbido() {
    std::vector<float> mono(static_cast<size_t>(kFrames), 0.0f);
    for (int i = 0; i < kFrames; ++i) {
        const double ph = 2.0 * M_PI * 50.0 * i / kRate;
        mono[static_cast<size_t>(i)] = static_cast<float>(
            0.30 * (std::sin(ph) + 0.5 * std::sin(2*ph) + 0.33 * std::sin(3*ph)));
    }
    return toStereo(mono);
}

struct Lectura {
    bool ok = false;
    int state = -1;
    double cents = NAN, detectedHz = 0.0, clarity = 0.0, rms = 0.0;
};

/**
 * El MISMO lazo que `OfflineAnalysis::analyzeBuffer`, mas los candidatos.
 *
 * Se replica aca —y no se usa el puerto— porque el puerto todavia no los acepta:
 * eso es REQ-029 S2. Sin thread y sin relojes, igual que el puerto: el mismo
 * buffer da el mismo resultado siempre.
 */
Lectura analizar(const std::vector<float>& buf, double targetHz,
                 const std::vector<double>& candidatos) {
    Lectura r;
    AnalysisRing ring;
    AnalysisSnapshot snapshot;
    AnalysisThread analysis(ring, snapshot);

    ring.setCaptureRate(kRate);
    analysis.setTargetHz(targetHz);
    analysis.setCandidates(candidatos.empty() ? nullptr : candidatos.data(),
                           static_cast<int>(candidatos.size()));

    const int capacity = static_cast<int>(AnalysisRing::kCapacityFrames);
    int written = 0;
    while (written < kFrames) {
        const int chunk = (kFrames - written) < capacity ? (kFrames - written) : capacity;
        ring.writeStereo(buf.data() + static_cast<size_t>(written) * 2, chunk);
        written += chunk;
        while (analysis.drainOnce() != AnalysisThread::DrainOutcome::kRingEmpty) {}
    }

    float v[kSnapshotValueCount];
    r.ok = snapshot.read(v);
    if (!r.ok) return r;
    r.state      = static_cast<int>(v[kSnapState]);
    r.cents      = static_cast<double>(v[kSnapCents]);
    r.detectedHz = static_cast<double>(v[kSnapDetectedHz]);
    r.clarity    = static_cast<double>(v[kSnapDetectionClarity]);
    r.rms        = static_cast<double>(v[kSnapLevelRms]);
    return r;
}

const char* nombreEstado(int s) {
    switch (s) { case kStateNoSignal: return "NO_SIGNAL"; case kStateNoLock: return "NO_LOCK";
                 case kStateMeasuring: return "MEASURING"; case kStateConverged: return "CONVERGED";
                 default: return "?"; }
}

/// Lo mismo, pero POR EL PUERTO — que es el camino donde el consumidor mide.
Lectura porElPuerto(const std::vector<float>& buf, double targetHz,
                    const std::vector<double>& candidatos) {
    Lectura r;
    float v[kSnapshotValueCount];
    r.ok = analyzeBuffer(buf.data(), kFrames, kRate, targetHz,
                         candidatos.empty() ? nullptr : candidatos.data(),
                         static_cast<int>(candidatos.size()), v);
    if (!r.ok) return r;
    r.state      = static_cast<int>(v[kSnapState]);
    r.cents      = static_cast<double>(v[kSnapCents]);
    r.detectedHz = static_cast<double>(v[kSnapDetectedHz]);
    r.clarity    = static_cast<double>(v[kSnapDetectionClarity]);
    r.rms        = static_cast<double>(v[kSnapLevelRms]);
    return r;
}

}  // namespace

/**
 * AC-029.5 — el puerto con instrumento responde IGUAL que el camino en vivo.
 *
 * Es una EQUIVALENCIA sobre las 36 combinaciones, no dos afirmaciones sueltas: un
 * test que dijera "el puerto da NO_LOCK" y otro que dijera "en vivo da NO_LOCK"
 * pasarian los dos con dos motores distintos. Acá el oraculo de un lado ES el otro
 * lado, que es lo unico que puede detectar que el puerto derivo.
 *
 * Y AC-029.6 viaja adentro: las 6 diagonales tienen que CONVERGER. Sin eso, el
 * arreglo trivial —que el puerto devuelva siempre lo mismo que el lazo, con los dos
 * rotos— satisface la equivalencia y no significa nada.
 */
TEST(ForeignNote, ThePortWithAnInstrumentAnswersLikeTheLivePath) {
    const auto hz = guitarraHz();
    const auto nombres = guitarraNombres();
    ASSERT_EQ(hz.size(), 6u) << "el catalogo de guitarra cambio de tamano";

    int diagonalesConvergidas = 0, ajenas = 0;
    for (size_t o = 0; o < hz.size(); ++o) {
        for (size_t t = 0; t < hz.size(); ++t) {
            const auto buf = cuerda(hz[t]);
            const Lectura vivo   = analizar(buf, hz[o], hz);
            const Lectura puerto = porElPuerto(buf, hz[o], hz);
            ASSERT_TRUE(vivo.ok && puerto.ok)
                << "no publico con objetivo " << nombres[o] << " y tono " << nombres[t];

            EXPECT_EQ(puerto.state, vivo.state)
                << "el puerto y el camino en vivo difieren con objetivo " << nombres[o]
                << " y tono " << nombres[t] << ": puerto=" << nombreEstado(puerto.state)
                << " vivo=" << nombreEstado(vivo.state);
            EXPECT_NEAR(puerto.detectedHz, vivo.detectedHz, 1e-3);

            if (o == t) {
                ++diagonalesConvergidas;
                EXPECT_EQ(puerto.state, kStateConverged)
                    << "CONTROL POSITIVO roto: " << nombres[o] << " con su propio tono dio "
                    << nombreEstado(puerto.state) << " (hz=" << puerto.detectedHz << ")";
            } else {
                ++ajenas;
                EXPECT_NE(puerto.state, kStateNoSignal)
                    << "objetivo " << nombres[o] << " con tono " << nombres[t]
                    << " dio ausencia con el instrumento DECLARADO: hay una nota (hz="
                    << puerto.detectedHz << ", claridad=" << puerto.clarity << ")";
            }
        }
    }
    ASSERT_EQ(ajenas, 30);
    ASSERT_EQ(diagonalesConvergidas, 6);
}

/// AC-029.2 — el zumbido de red NO es ninguna cuerda: sigue siendo ausencia.
TEST(ForeignNote, MainsHumWithADeclaredInstrumentIsStillAbsence) {
    const auto hz = guitarraHz();
    const Lectura r = analizar(zumbido(), hz[5], hz);
    ASSERT_TRUE(r.ok);

    EXPECT_EQ(r.state, kStateNoSignal)
        << "el zumbido de 50 Hz dio " << nombreEstado(r.state) << ": con instrumento declarado "
        << "ninguna cuerda esta en rango, asi que no hay nada que afinar (hz=" << r.detectedHz << ")";
}

/// AC-029.7 — la ausencia REAL sigue siendo distinguible de la nota ajena.
TEST(ForeignNote, DigitalSilenceIsStillAbsence) {
    const auto hz = guitarraHz();
    const std::vector<float> silencio(static_cast<size_t>(kFrames) * 2, 0.0f);

    for (const auto& cands : {hz, std::vector<double>{}}) {
        const Lectura r = analizar(silencio, hz[5], cands);
        ASSERT_TRUE(r.ok);
        EXPECT_EQ(r.state, kStateNoSignal)
            << "el silencio digital dio " << nombreEstado(r.state)
            << " con " << cands.size() << " candidato(s)";
        EXPECT_LE(r.detectedHz, 0.0) << "publico una altura sobre silencio";
        EXPECT_NEAR(r.clarity, 0.0, 1e-6);
    }
}
