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
#include "tests/support/TestSanitizer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
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

/// Un punto de la matriz. La cuerda va por indice en `catalogStrings()`.
struct Caso {
    size_t cuerda;
    int rate;
    int nPart;
    double B;
    double fase;
    bool operator==(const Caso& o) const {
        return cuerda == o.cuerda && rate == o.rate && nPart == o.nPart && B == o.B && fase == o.fase;
    }
};

constexpr int kRates[] = {44100, 48000};
constexpr double kBs[] = {0.0, 1e-4};
constexpr double kFases[] = {0.0, M_PI / 3.0};

/**
 * 🔴 EL PEOR CASO DEL CATALOGO VA SIEMPRE, ESTE O NO EL SANITIZER (MINI-020, AC-M020.2).
 * Es la razon de que este archivo exista: sobre `2.14.0` "B0 a 48 kHz no lo tocaba NINGUN
 * test", y el piso de energia de REQ-027 salio de medir ahi (la fuga del segundo parcial a 2,63
 * bins del fundamental). La cuerda mas grave se ELIGE del catalogo, no se nombra: si alguien
 * agrega una mas grave, el peor caso la sigue.
 */
std::vector<Caso> peorCaso() {
    const auto& cat = wma_test::catalogStrings();
    size_t grave = 0;
    for (size_t i = 1; i < cat.size(); ++i) if (cat[i].hz < cat[grave].hz) grave = i;
    std::vector<Caso> out;
    for (double B : kBs) for (double fase : kFases) out.push_back({grave, 48000, 1, B, fase});
    return out;
}

/**
 * 🔴 LOS CENTINELAS: los casos que MATAN al mutante de REQ-027, medidos (MINI-020, AC-M020.5).
 *
 * Con `kMinBinToRmsRatio` 0,05 -> 0 (el piso de admision por energia apagado) la matriz
 * COMPLETA de 448 se pone roja en exactamente DOS casos, y son estos dos: ukelele A4 a
 * 44,1 kHz con tres parciales y B = 1e-4, en sus dos fases (−0,145 y −0,142 cents contra un
 * presupuesto de 0,1). El resto de la matriz no lo ve. O sea que la fuerza de este test contra
 * el defecto que lo hizo nacer vive en UN punto de la geometria —el bin del cuarto parcial,
 * vacio, a la distancia justa del tercero—, y un subconjunto que lo deje afuera es verde con
 * el defecto adentro. Por eso van SIEMPRE, y por nombre: no salen de ninguna regla del catalogo
 * (no es la mas grave ni la mas aguda), salen de la medicion. Si el catalogo cambia y el
 * ukelele A4 desaparece, esto tiene que ponerse rojo, no callarse.
 */
std::vector<Caso> centinelas() {
    const auto& cat = wma_test::catalogStrings();
    size_t idx = cat.size();
    for (size_t i = 0; i < cat.size(); ++i) if (std::string(cat[i].name) == "ukelele A4") idx = i;
    EXPECT_LT(idx, cat.size()) << "el catalogo ya no tiene 'ukelele A4': el centinela de REQ-027 "
                                  "se quedo sin cuerda — re-medir el mutante antes de tocar esto";
    std::vector<Caso> out;
    if (idx < cat.size())
        for (double fase : kFases) out.push_back({idx, 44100, 3, 1e-4, fase});
    return out;
}

/**
 * LA MATRIZ, Y CUANTA SE RECORRE (MINI-020).
 *
 * Sin sanitizer: las cinco dimensiones cruzadas enteras, 14 x 4 x 2 x 2 x 2 = 448 casos.
 *
 * Bajo sanitizer NO. Medido el 2026-09-08, TSan local en serie y sin carga: 118 s antes de
 * REQ-033 y 282 s despues —el detector grueso refina cada candidato y cuesta 2,4x—, contra un
 * techo de 180 s con CTEST_JOBS=4 en el gate local. Los 448 casos x 2 s ya no entran, y la
 * salida no es subir el techo ni saltear el test (es de EXACTITUD, no de costo: bajo TSan
 * significa lo mismo) ni achicar los 2 s que B0 necesita para converger. Es recorrer un
 * SUBCONJUNTO ROTATIVO que conserva las cinco dimensiones: por cada par (cuerda, riqueza)
 * —56— una de las 8 combinaciones de (rate, B, fase), elegida por el indice del par, asi que
 * cada combinacion aparece siete veces, con siete cuerdas distintas, y cada cuerda ve las
 * cuatro riquezas. Mas el peor caso y los centinelas, siempre: 56 + 4 + 2, menos el centinela
 * que la rotacion ya trae (el par 54 cae en la combinacion 6, que ES ese caso): 61.
 *
 * Que el recorte no le saco fuerza esta MEDIDO con el mutante que este test existe para
 * matar (`kMinBinToRmsRatio` 0,05 -> 0 sobre el binario instrumentado): muere en los dos
 * centinelas, igual que la matriz completa. "Rotativo" y no "los primeros N": los primeros N
 * serian todos de guitarra a 44,1 kHz — y sin centinelas, la rotacion traia UNO de los dos
 * casos que matan por casualidad del indice, que es una fuerza que no se puede declarar.
 */
std::vector<Caso> matriz() {
    const auto& cat = wma_test::catalogStrings();
    std::vector<Caso> out;
#ifdef WMA_TEST_UNDER_SANITIZER
    size_t par = 0;
    for (size_t c = 0; c < cat.size(); ++c) {
        for (int nPart = 1; nPart <= 4; ++nPart, ++par) {
            const size_t k = par % 8;
            out.push_back({c, kRates[k & 1], nPart, kBs[(k >> 1) & 1], kFases[(k >> 2) & 1]});
        }
    }
#else
    for (size_t c = 0; c < cat.size(); ++c)
        for (int rate : kRates)
            for (int nPart = 1; nPart <= 4; ++nPart)
                for (double B : kBs)
                    for (double fase : kFases) out.push_back({c, rate, nPart, B, fase});
#endif
    for (const Caso& p : peorCaso())
        if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    for (const Caso& p : centinelas())
        if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    return out;
}

#ifdef WMA_TEST_UNDER_SANITIZER
constexpr int kCasosEsperados = 61;
#else
constexpr int kCasosEsperados = 448;
#endif

/**
 * La matriz completa: 14 cuerdas x riqueza 1..4 x 2 rates x 2 valores de B x
 * 2 fases = 448 casos, todos afinados EXACTO (60 bajo sanitizer, ver `matriz()`).
 *
 * Sobre `2.14.0` esto fallaba en las cuerdas graves con pocos parciales, y el
 * peor caso del catalogo —B0 a 48 kHz— no lo tocaba NINGUN test.
 */
TEST(PoorStimulusMatrix, LaMatrizCompletaSeMantieneEnPresupuesto) {
    const auto& cat = wma_test::catalogStrings();
    int casos = 0;
    for (const Caso& c : matriz()) {
        const auto& cuerda = cat[c.cuerda];
        const int frames = static_cast<int>(c.rate * kSeconds);
        const auto mono = wma_test::inharmonicString(
            cuerda.hz, c.B, c.nPart, c.rate, frames, 0.3, c.fase);
        const auto buf = estereo(mono);
        float v[kSnapshotValueCount] = {};
        ASSERT_TRUE(analyzeBuffer(buf.data(), frames, c.rate, cuerda.hz, v))
            << cuerda.name << " rate=" << c.rate << " parciales=" << c.nPart;
        ++casos;
        if (static_cast<int>(v[kSnapState]) != kStateConverged) continue;
        EXPECT_LE(std::fabs(static_cast<double>(v[kSnapCents])), kBudgetCents)
            << cuerda.name << ", rate " << c.rate << ", " << c.nPart
            << " parcial(es), B=" << c.B << ", fase " << c.fase
            << ": publica " << v[kSnapCents] << " cents";
    }
    EXPECT_EQ(casos, kCasosEsperados) << "la matriz encogio: alguien saco una dimension";

    // Las cinco dimensiones estan, instrumentado o no: cada valor de cada eje aparece.
    const auto m = matriz();
    for (size_t c = 0; c < cat.size(); ++c)
        EXPECT_TRUE(std::any_of(m.begin(), m.end(), [&](const Caso& x) { return x.cuerda == c; }))
            << "la cuerda " << cat[c].name << " no esta en la matriz";
    for (int nPart = 1; nPart <= 4; ++nPart)
        EXPECT_TRUE(std::any_of(m.begin(), m.end(), [&](const Caso& x) { return x.nPart == nPart; }));
    for (int rate : kRates)
        EXPECT_TRUE(std::any_of(m.begin(), m.end(), [&](const Caso& x) { return x.rate == rate; }));
    for (double B : kBs)
        EXPECT_TRUE(std::any_of(m.begin(), m.end(), [&](const Caso& x) { return x.B == B; }));
    for (double fase : kFases)
        EXPECT_TRUE(std::any_of(m.begin(), m.end(), [&](const Caso& x) { return x.fase == fase; }));
    for (const Caso& p : peorCaso())
        EXPECT_TRUE(std::find(m.begin(), m.end(), p) != m.end())
            << "el peor caso del catalogo (la mas grave a 48 kHz, 1 parcial) no esta";
    for (const Caso& p : centinelas())
        EXPECT_TRUE(std::find(m.begin(), m.end(), p) != m.end())
            << "un centinela de REQ-027 (ukelele A4, 44,1 kHz, 3 parciales, B=1e-4) no esta";
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
