/**
 * @file test_fast_mode_wiring.cpp
 * @brief REQ-030 S1 — el modo rapido CABLEADO en `AnalysisThread`, no aislado.
 *
 * 🔴 POR QUE ESTE ARCHIVO EXISTE
 * ------------------------------
 * `test_fast_mode.cpp` prueba `FastModeTracker` **solo**: le da alturas y le pregunta a que
 * cuerda engancha. Nueve tests, todos verdes desde agosto. Lo que NADIE probaba es el cableado:
 * que la decision del tracker sobreviva dentro del lazo de `AnalysisThread::drainOnce()`.
 *
 * Es la leccion de REQ-012 otra vez —entregar el mecanismo no es entregar el comportamiento— y
 * `check-mechanism-callers.py` no puede verla, porque el mecanismo SI tiene llamador de
 * produccion. La pregunta que falta no es "¿quien lo llama?" sino "¿alguien lo llama y despues
 * no se lo pisa?".
 *
 * ESTA ETAPA (S1) AFIRMA SOLO LOS CONTROLES, Y ES A PROPOSITO
 * ----------------------------------------------------------
 * Los tres casos de aca son los que HOY andan. El caso del defecto —objetivo ajeno CON
 * candidatos— llega en S2, junto con su arreglo: fijar 27 re-aplicaciones como expectativa
 * seria escribir el defecto en el contrato.
 *
 * 🔴 EL BUFFER VA ESTEREO INTERCALADO. Ver la nota de `test_foreign_note.cpp`: pasarle mono a
 * este camino da `rms=0` y `hz=0` en TODOS los casos, o sea que un test de ausencia saldria
 * verde sobre silencio. Los casos que exigen `CONVERGED` son el control positivo que lo atrapa.
 */
#include "../AnalysisRing.h"
#include "../AnalysisSnapshot.h"
#include "../AnalysisThread.h"
#include "support/SyntheticSignal.h"

#include <gtest/gtest.h>
#include <cmath>
#include <vector>

using namespace wma::analysis;

namespace {

constexpr int kRate = 44100;
constexpr int kFrames = kRate * 5;   // 5 s: el presupuesto de convergencia del producto

/// Las seis de guitarra estandar, del catalogo COMPARTIDO. Copiarlas aca seria una segunda
/// fuente de verdad que se desincroniza (la leccion de REQ-027 S3).
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

/// Cuerda pulsada: parciales 1..4 con decaimiento 1/n, el mismo estimulo de REQ-029.
std::vector<float> cuerda(double f0) {
    return toStereo(wma_test::partialsWithAmplitudes(
        f0, 0.0, {0.5, 0.25, 0.125, 0.0625}, kRate, kFrames));
}

struct Lectura {
    bool ok = false;
    int state = -1;
    double cents = NAN, detectedHz = 0.0, clarity = 0.0;
    uint64_t porElUsuario = 0, porElModoRapido = 0;
};

/**
 * El mismo lazo que `OfflineAnalysis::analyzeBuffer`. Sin thread y sin relojes: el mismo
 * buffer da el mismo resultado siempre, asi que no hay nada que esperar (REQ-002).
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

    const int capacity = static_cast<int>(AnalysisRing::kCapacityFrames);
    int written = 0;
    while (written < kFrames) {
        const int chunk = (kFrames - written) < capacity ? (kFrames - written) : capacity;
        ring.writeStereo(buf.data() + static_cast<size_t>(written) * 2, chunk);
        written += chunk;
        while (analysis.drainOnce() != AnalysisThread::DrainOutcome::kRingEmpty) {}
    }

    Lectura r;
    float v[kSnapshotValueCount];
    r.ok = snapshot.read(v);
    r.porElUsuario     = analysis.targetAppliedByUser();
    r.porElModoRapido  = analysis.targetAppliedByFastMode();
    if (!r.ok) return r;
    r.state      = static_cast<int>(v[kSnapState]);
    r.cents      = static_cast<double>(v[kSnapCents]);
    r.detectedHz = static_cast<double>(v[kSnapDetectedHz]);
    r.clarity    = static_cast<double>(v[kSnapDetectionClarity]);
    return r;
}

constexpr double kA2 = 110.0;
constexpr double kE4 = 329.6276;

}  // namespace

/**
 * AC-030.5 — el objetivo YA es el tono, SIN candidatos declarados.
 *
 * Es la linea de base contra la que se mide todo lo demas: sin modo rapido de por medio, el
 * objetivo se aplica UNA vez y la lectura converge. Si esto se moviera, cualquier conclusion
 * sobre el reenganche seria sobre otro motor.
 */
TEST(FastModeWiring, WithTheRightTargetAndNoCandidatesTheTargetIsAppliedExactlyOnce) {
    const auto r = analizar(cuerda(kA2), kA2, {});

    ASSERT_TRUE(r.ok) << "el snapshot nunca se publico";
    EXPECT_EQ(r.state, kStateConverged);
    EXPECT_NEAR(r.cents, 0.0, 0.1) << "el presupuesto de exactitud del producto";
    EXPECT_NEAR(r.detectedHz, kA2, 0.05);
    EXPECT_GT(r.clarity, 0.99);

    EXPECT_EQ(r.porElUsuario, 1u)
        << "el objetivo no cambia en toda la corrida: aplicarlo mas de una vez significa que "
           "algo lo esta re-aplicando, y cada re-aplicacion descarta el ring";
    EXPECT_EQ(r.porElModoRapido, 0u) << "sin candidatos no hay modo rapido que pueda reenganchar";
}

/**
 * AC-030.5 — el objetivo YA es el tono, CON el instrumento declarado.
 *
 * El caso que dice que declarar candidatos **no cuesta nada** cuando el objetivo ya era el
 * correcto: el tracker mira, no encuentra nada mejor, y no toca el objetivo. Sin este test, un
 * arreglo que apagara el modo rapido entero pasaria el resto de la suite.
 */
TEST(FastModeWiring, WithTheRightTargetCandidatesDoNotRelatchAnything) {
    const auto r = analizar(cuerda(kA2), kA2, guitarraHz());

    ASSERT_TRUE(r.ok) << "el snapshot nunca se publico";
    EXPECT_EQ(r.state, kStateConverged);
    EXPECT_NEAR(r.cents, 0.0, 0.1);
    EXPECT_NEAR(r.detectedHz, kA2, 0.05);
    EXPECT_GT(r.clarity, 0.99);

    EXPECT_EQ(r.porElUsuario, 1u);
    EXPECT_EQ(r.porElModoRapido, 0u)
        << "A2 ES uno de los candidatos y ya es el objetivo: el modo rapido no tiene a que "
           "reenganchar, asi que no puede tocar el objetivo";
}

/**
 * AC-030.5 — objetivo ajeno y SIN instrumento declarado: `NO_SIGNAL`, y el objetivo quieto.
 *
 * Es R-PITCH-56, de REQ-029: sin candidatos, una cuerda ajena y el ruido de una sala son
 * indistinguibles, y el motor no los trata distinto. Lo que S1 agrega es la otra mitad —que
 * ademas el objetivo NO se toca—, que es lo que hace que este caso sea comparable con el del
 * defecto que llega en S2.
 */
TEST(FastModeWiring, WithAForeignTargetAndNoCandidatesTheTargetIsNeverRelatched) {
    const auto r = analizar(cuerda(kA2), kE4, {});

    ASSERT_TRUE(r.ok) << "el snapshot nunca se publico";
    EXPECT_EQ(r.state, kStateNoSignal) << "R-PITCH-56: sin instrumento declarado, ausencia";
    EXPECT_TRUE(std::isnan(r.cents)) << "sin enganche no se publica una desviacion";

    EXPECT_EQ(r.porElUsuario, 1u);
    EXPECT_EQ(r.porElModoRapido, 0u)
        << "sin candidatos el bloque del modo rapido ni entra (candidateCount() == 0)";
}
