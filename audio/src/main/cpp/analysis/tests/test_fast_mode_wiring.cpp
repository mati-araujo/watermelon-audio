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

/**
 * Como `analizar`, pero el consumidor CAMBIA el objetivo a mitad de la señal (AC-030.3).
 * Devuelve tambien cuantos frames vio el analisis, para poder afirmar que el cambio no lo
 * dejo ciego.
 */
struct LecturaConCambio {
    Lectura antes, despues;
};

LecturaConCambio analizarConCambioDeObjetivo(const std::vector<float>& buf,
                                             double objetivoInicial, double objetivoFinal) {
    AnalysisRing ring;
    AnalysisSnapshot snapshot;
    AnalysisThread analysis(ring, snapshot);

    ring.setCaptureRate(kRate);
    analysis.setTargetHz(objetivoInicial);

    const int capacity = static_cast<int>(AnalysisRing::kCapacityFrames);
    LecturaConCambio r;
    int written = 0;
    bool cambiado = false;
    while (written < kFrames) {
        const int chunk = (kFrames - written) < capacity ? (kFrames - written) : capacity;
        ring.writeStereo(buf.data() + static_cast<size_t>(written) * 2, chunk);
        written += chunk;
        while (analysis.drainOnce() != AnalysisThread::DrainOutcome::kRingEmpty) {}
        if (!cambiado && written >= kFrames / 2) {
            float v[kSnapshotValueCount];
            if (snapshot.read(v)) {
                r.antes.ok = true;
                r.antes.state = static_cast<int>(v[kSnapState]);
                r.antes.cents = static_cast<double>(v[kSnapCents]);
            }
            analysis.setTargetHz(objetivoFinal);
            cambiado = true;
        }
    }

    float v[kSnapshotValueCount];
    r.despues.ok = snapshot.read(v);
    r.despues.porElUsuario    = analysis.targetAppliedByUser();
    r.despues.porElModoRapido = analysis.targetAppliedByFastMode();
    if (r.despues.ok) {
        r.despues.state      = static_cast<int>(v[kSnapState]);
        r.despues.cents      = static_cast<double>(v[kSnapCents]);
        r.despues.detectedHz = static_cast<double>(v[kSnapDetectedHz]);
        r.despues.clarity    = static_cast<double>(v[kSnapDetectionClarity]);
    }
    return r;
}

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


// ===========================================================================
// REQ-030 S2 — el defecto y su arreglo.
//
// Los cuatro de abajo nacen ROJOS sobre la base de S1: se escribieron desde los AC y se
// corrieron ANTES de tocar produccion, que es la disciplina que en REQ-029 dio vuelta dos
// decisiones de diseño ya aprobadas.
// ===========================================================================

/**
 * AC-030.1 — tras un reenganche, la lectura converge igual que si el objetivo hubiera sido
 * el correcto desde el arranque.
 *
 * 🔴 El ORACULO no es un numero elegido a mano: es el caso [1] de esta misma suite
 * (`WithTheRightTargetAndNoCandidatesTheTargetIsAppliedExactlyOnce`). Un umbral inventado
 * aca podria ser mas flojo que el que el motor ya cumple y dejar pasar una convergencia
 * degradada; el control positivo de al lado es el unico oraculo que no se puede aflojar sin
 * que se note.
 */
TEST(FastModeWiring, AfterARelatchTheReadingConvergesLikeItWasTheTargetAllAlong) {
    const auto reenganchado = analizar(cuerda(kA2), kE4, guitarraHz());
    const auto desdeElArranque = analizar(cuerda(kA2), kA2, {});

    ASSERT_TRUE(reenganchado.ok) << "el snapshot nunca se publico";
    ASSERT_TRUE(desdeElArranque.ok);
    ASSERT_EQ(desdeElArranque.state, kStateConverged)
        << "el ORACULO no converge: sin control positivo este test no dice nada";

    EXPECT_EQ(reenganchado.state, kStateConverged)
        << "el modo rapido engancho A2 y despues no midio: un afinador que sigue la cuerda "
           "correcta y nunca converge es PEOR que uno que declara ausencia, porque parece "
           "que esta midiendo";
    EXPECT_NEAR(reenganchado.detectedHz, kA2, 0.05);
    EXPECT_NEAR(reenganchado.cents, desdeElArranque.cents, 0.1)
        << "la exactitud tras reenganchar tiene que ser la del objetivo correcto de entrada";
    EXPECT_NEAR(reenganchado.clarity, desdeElArranque.clarity, 0.02)
        << "una claridad degradada delata que al detector le esta llegando señal picada: es "
           "la firma de que el ring se descarta una vez por tick";
}

/**
 * AC-030.2 — EL MECANISMO. Con la señal y los candidatos quietos, el objetivo se aplica una
 * sola vez por lado.
 *
 * 🔴 No es redundante con el test de arriba, y la diferencia es la razon de ser de S1: un
 * test que solo mire el desenlace da verde si mañana converge POR OTRA RAZON. Este defecto
 * vivio desde agosto justamente porque nadie miraba el mecanismo.
 */
TEST(FastModeWiring, AStableSignalRelatchesTheTargetExactlyOnce) {
    const auto r = analizar(cuerda(kA2), kE4, guitarraHz());

    ASSERT_TRUE(r.ok) << "el snapshot nunca se publico";

    EXPECT_EQ(r.porElModoRapido, 1u)
        << "el modo rapido elige A2 una vez y no tiene por que volver a elegirla: la señal "
           "no cambia";
    EXPECT_EQ(r.porElUsuario, 1u)
        << "el consumidor pidio E4 UNA vez y nunca lo cambio. Que este contador suba es el "
           "defecto entero: la rama del usuario reacciona a que lo aplicado DIFIERA de lo "
           "pedido, y el modo rapido las hace diferir para siempre";
}

/**
 * AC-030.4 — sin objetivo declarado, el analisis sigue viendo la señal.
 *
 * 🔴 Este test nace de un ROJO PROPIO, no de una hipotesis. La primera version del arreglo
 * usaba un centinela -1,0 para "el ultimo pedido del usuario"; con `targetHz == 0` la rama
 * disparaba en el primer tick y llamaba a `skipToNewest()` ANTES de analizar nada, y se
 * caian tres tests de `AnalysisThread` que en master estaban VERDES. Descartar el ring
 * cuando no hay objetivo contra el que integrar deja al analisis ciego.
 */
TEST(FastModeWiring, WithNoTargetTheAnalysisStillSeesTheSignal) {
    const auto r = analizar(cuerda(kA2), 0.0, {});

    ASSERT_TRUE(r.ok) << "el snapshot nunca se publico: el analisis no vio un solo frame";
    EXPECT_GT(r.clarity, 0.99)
        << "sin objetivo no hay nada contra que integrar, pero la deteccion GRUESA sigue "
           "corriendo: si esto es 0, el ring se esta descartando antes de leerlo";
    EXPECT_NEAR(r.detectedHz, kA2, 0.5)
        << "el motor sabe QUE nota suena aunque no tenga objetivo (R-PITCH-5)";
}

/**
 * AC-030.3 — cuando el objetivo lo cambia EL CONSUMIDOR, lo viejo del ring se sigue tirando.
 *
 * Es la razon original de `skipToNewest()` (REQ-001.6: la lectura salia 4,55 cents contra 2,0
 * reales) y el arreglo no la puede perder. Sin este test, "no descartes nunca" pasaria
 * AC-030.1 y AC-030.2 sin problema.
 */
TEST(FastModeWiring, AConsumerRetargetStillDropsThePreviousStringFromTheRing) {
    const auto r = analizarConCambioDeObjetivo(cuerda(kA2), kE4, kA2);

    ASSERT_TRUE(r.despues.ok) << "el snapshot nunca se publico";
    EXPECT_EQ(r.despues.state, kStateConverged)
        << "tras apuntar al tono correcto, converge";
    EXPECT_NEAR(r.despues.cents, 0.0, 0.1)
        << "si lo que quedo de la cuerda anterior entrara a la integracion nueva, la lectura "
           "se corre: 4,55 cents contra 2,0 reales, medido en REQ-001.6";
    EXPECT_EQ(r.despues.porElUsuario, 2u)
        << "dos pedidos del consumidor, dos aplicaciones: E4 al arrancar y A2 al cambiar";
}
