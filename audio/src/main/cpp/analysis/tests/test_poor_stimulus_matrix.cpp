/**
 * REQ-027 S3 — el barrido deja de asumir cuatro parciales.
 *
 * EL PUNTO CIEGO NO ERA DEL MOTOR: ERA DEL CORPUS
 * -----------------------------------------------
 * Los ocho tests de extremo a extremo de este directorio generaban su estimulo
 * con `for (int n = 1; n <= 4; ++n)`: cuatro parciales EXACTAMENTE armonicos,
 * sostenidos, a un solo rate. Esa uniformidad escondio TRES cosas distintas, y
 * las tres salieron en REQ-027:
 *
 *   1. el defecto reportado — un parcial sin energia integraba fuga espectral y
 *      publicaba +38,70 cents con estado CONVERGIDO;
 *   2. la justificacion de haber sacado la zona muerta del arbitraje por signo
 *      ("los otros parciales sostienen la lectura igual"), medida sobre 42
 *      escenarios que TODOS tenian cuatro parciales;
 *   3. un factor de escala χ² sobre σ que habria apagado CONVERGIDO en toda
 *      cuerda de acero, y que con B = 0 parecia inofensivo.
 *
 * Un punto ciego, tres hallazgos. Este archivo lo cierra en el barrido: recorre
 * el catalogo COMPLETO x riqueza armonica 1..4 x los DOS rates x inarmonicidad
 * realista x fase inicial.
 */

#include "../AnalysisSnapshot.h"
#include "../OfflineAnalysis.h"
#include "../StrobeTracker.h"
#include "support/SyntheticSignal.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

using namespace wma::analysis;

/// El presupuesto del producto. UNA constante para todo el archivo.
constexpr double kBudgetCents = 0.1;

/// 2 s: deja 21 ventanas a 44,1 kHz, que alcanza hasta en B0.
constexpr double kSeconds = 2.0;

std::vector<float> estereo(const std::vector<float>& m) {
    std::vector<float> b(m.size() * 2);
    for (size_t i = 0; i < m.size(); ++i) { b[i * 2] = m[i]; b[i * 2 + 1] = m[i]; }
    return b;
}

// ---------------------------------------------------------------------------
// AC-027.4 — el veredicto no depende de la fase, en TODA la matriz
// ---------------------------------------------------------------------------

/**
 * La matriz completa: 14 cuerdas x riqueza 1..4 x 2 rates x 2 valores de B x
 * 2 fases = 448 casos, todos afinados EXACTO.
 *
 * Sobre `2.14.0` esto fallaba en las cuerdas graves con pocos parciales, y el
 * peor caso del catalogo —B0 a 48 kHz— no lo tocaba NINGUN test.
 */
TEST(PoorStimulusMatrix, LaMatrizCompletaSeMantieneEnPresupuesto) {
    int casos = 0;
    for (const auto& cuerda : wma_test::catalogStrings()) {
        for (int rate : {44100, 48000}) {
            const int frames = static_cast<int>(rate * kSeconds);
            for (int nPart = 1; nPart <= 4; ++nPart) {
                for (double B : {0.0, 1e-4}) {
                    for (double fase : {0.0, M_PI / 3.0}) {
                        const auto mono = wma_test::inharmonicString(
                            cuerda.hz, B, nPart, rate, frames, 0.3, fase);
                        const auto buf = estereo(mono);
                        float v[kSnapshotValueCount] = {};
                        ASSERT_TRUE(analyzeBuffer(buf.data(), frames, rate, cuerda.hz, v))
                            << cuerda.name << " rate=" << rate << " parciales=" << nPart;
                        ++casos;
                        if (static_cast<int>(v[kSnapState]) != kStateConverged) continue;
                        EXPECT_LE(std::fabs(static_cast<double>(v[kSnapCents])), kBudgetCents)
                            << cuerda.name << ", rate " << rate << ", " << nPart
                            << " parcial(es), B=" << B << ", fase " << fase
                            << ": publica " << v[kSnapCents] << " cents";
                    }
                }
            }
        }
    }
    EXPECT_EQ(casos, 448) << "la matriz encogio: alguien saco una dimension";
}

// ---------------------------------------------------------------------------
// El INSTRUMENTO del criterio de muerte de REQ-027
// ---------------------------------------------------------------------------

/**
 * 🔴 LA CUERDA QUE DECAE, QUE HASTA HOY NO EXISTIA EN NINGUN TEST.
 *
 * El riesgo declarado de REQ-027 es que el piso de energia —calibrado sobre
 * estimulo sintetico, sostenido y sin ruido— descarte un parcial LEGITIMO pero
 * debil tarde en el sustain, y que la lectura se degrade segundos despues del
 * punteo con todos los AC en verde.
 *
 * Se modela con `tau_n = tau/n`: los parciales altos se apagan ANTES, que es lo
 * que hace una cuerda real. (`applyDecay` no sirve: aplica el mismo decaimiento a
 * todo, asi que las amplitudes RELATIVAS nunca cambian y el caso no se ejerce.)
 *
 * Medido con tau = 3 s: `partialsUsed()` baja 4 -> 3 a los 3 s -> 2 a los 5 s, y
 * la lectura se mantiene en |cents| <= 0,007 y CONVERGIDA todo el tiempo. O sea
 * que los parciales se van cayendo a medida que MUEREN, que es correcto, y la
 * degradacion es suave.
 *
 * El umbral que vigila el criterio de muerte: `partialsUsed()` no puede bajar de
 * 2 antes de los 3 s que el contrato de exactitud pide.
 */
TEST(PoorStimulusMatrix, LaCuerdaQueDecaeSigueMidiendoALosTresSegundos) {
    constexpr int kRate = 44100;
    for (const auto& cuerda : wma_test::catalogStrings()) {
        const int frames = static_cast<int>(kRate * 3.0);
        const auto mono = wma_test::decayingString(cuerda.hz, 1e-4, 4, kRate, frames,
                                                   /*tauFundamental=*/3.0, 0.5);
        StrobeTracker st;
        st.prepare(kRate);
        st.setTarget(cuerda.hz);
        st.setCoarseFrequencyHz(cuerda.hz);
        st.process(mono.data(), static_cast<int>(mono.size()));

        // 🔴 CONTROL SOBRE EL INSTRUMENTO, no sobre el motor. Sin esto el test es
        // decorativo: un mutante que apagaba el decaimiento POR PARCIAL
        // (`tau_n = tau` en vez de `tau/n`) SOBREVIVIA, porque con decaimiento
        // uniforme los cuatro parciales siguen vivos y `>= 2` se cumple igual. O
        // sea que el test no estaba ejerciendo la muerte de parciales que dice
        // ejercer. Medido con `tau_n = tau/n`: a los 3 s quedan 3.
        EXPECT_LT(st.partialsUsed(), 4)
            << cuerda.name << ": a los 3 s siguen vivos los cuatro parciales, asi que"
            << " este estimulo NO esta ejerciendo la muerte de parciales — el test no"
            << " prueba lo que dice probar";
        EXPECT_GE(st.partialsUsed(), 2)
            << cuerda.name << ": a los 3 s quedan " << st.partialsUsed()
            << " parciales — el piso de energia esta descartando parciales vivos";
        EXPECT_TRUE(st.converged())
            << cuerda.name << ": una cuerda que decae normalmente tiene que seguir"
            << " convergiendo a los 3 s (sigma=" << st.uncertaintyCents() << ")";
        EXPECT_LE(std::fabs(st.cents()), kBudgetCents)
            << cuerda.name << ": publica " << st.cents() << " cents";
    }
}

}  // namespace
