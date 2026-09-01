/**
 * REQ-027 S2 — cuando los parciales no se explican entre si, CONVERGIDO se apaga.
 *
 * QUE CAMBIA, Y POR QUE NO ALCANZABA CON LA MEDIA PONDERADA
 * ---------------------------------------------------------
 * Una cuerda REAL es inarmonica: sus parciales estan en
 * `f_n = n·f0·√(1+B·n²)`, o sea que discrepan **por fisica, no por error**.
 * Medido con B = 1e-3 en E2, los cuatro parciales leen 0,865 / 3,455 / 7,756 /
 * 13,740 cents. La media ponderada por 1/σ² los promedia como si discreparan por
 * ruido, y el peso lo decide σ, que es MENOR en los parciales altos: la lectura
 * quedaba dominada por un parcial agudo estirado en vez del fundamental.
 *
 * Ahora se ajusta el modelo `cents_n = C + 600·log2(1+B·n²)` y se publica **C**,
 * que es la desviacion del FUNDAMENTAL — lo que el musico esta afinando.
 *
 * 🔴 Y σ SALE DE LOS RESIDUOS DEL AJUSTE, no de las σ por parcial. La σ del
 * estimador de fase es una PRECISION (1e-7 a 1e-4 cents), no una exactitud: dice
 * cuan bien encaja una recta, no cuan cerca esta de la verdad. Publicar la
 * propagada seria repetir el defecto que REQ-027 arregla, con otra ropa.
 */

#include "../AnalysisSnapshot.h"
#include "../InharmonicityEstimator.h"
#include "../OfflineAnalysis.h"
#include "../StrobeTracker.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

using namespace wma::analysis;

constexpr int kRate = 44100;
constexpr int kFrames = kRate * 3;
constexpr double kE2 = 82.4069;

/// El presupuesto del producto. UNA constante para todo el archivo: un gemelo
/// que afirma contra su propio numero no vigila al otro.
constexpr double kBudgetCents = 0.1;

/// Cuerda inarmonica: parcial n en `n·f0·√(1+B·n²)`, desafinada `cents`.
std::vector<float> cuerda(double f0, double B, double cents, int nParciales = 4,
                          double ruido = 0.0, double loboCents = 0.0, int loboOrden = 0) {
    std::vector<float> m(static_cast<size_t>(kFrames));
    const double base = f0 * std::pow(2.0, cents / 1200.0);
    unsigned seed = 7u;
    for (int i = 0; i < kFrames; ++i) {
        double s = 0.0;
        for (int n = 1; n <= nParciales; ++n) {
            double fn = base * n * std::sqrt(1.0 + B * n * n);
            if (n == loboOrden) fn *= std::pow(2.0, loboCents / 1200.0);
            s += (0.3 / n) * std::sin(2.0 * M_PI * fn * i / kRate);
        }
        seed = seed * 1103515245u + 12345u;
        s += ruido * ((seed >> 16) / 32768.0 - 1.0);
        m[static_cast<size_t>(i)] = static_cast<float>(s);
    }
    return m;
}

std::vector<float> estereo(const std::vector<float>& m) {
    std::vector<float> b(m.size() * 2);
    for (size_t i = 0; i < m.size(); ++i) { b[i * 2] = m[i]; b[i * 2 + 1] = m[i]; }
    return b;
}

/// B tipicos de cuerda real: 1e-5 nylon, 1e-4 acero, 1e-3 acero agudo.
constexpr double kBs[] = {0.0, 1e-5, 1e-4, 1e-3};

// ---------------------------------------------------------------------------
// AC-027.5 — lo que el modelo NO explica apaga CONVERGIDO
// ---------------------------------------------------------------------------

/**
 * Un parcial "lobo" —corrido respecto de la serie armonica— no lo puede explicar
 * ningun par (C, B). σ tiene que decirlo.
 *
 * Medido: con el 3er parcial corrido +5 y +20 cents, σ da 1,51 y 6,05 contra un
 * umbral de convergencia de 0,1.
 *
 * 🔴 EL RANGO DE ESTE TEST ESTA ACOTADO POR ARRIBA, Y LO FIJA LA FISICA DEL
 * ESTIMADOR, no el gusto. El corrimiento tiene que caber en el RANGO DE CAPTURA
 * del parcial 3: a 44,1 kHz son `dfMax = 44100/8192 = 5,38 Hz` sobre un objetivo
 * de 247,2 Hz, o sea **37,3 cents**. Un corrimiento mayor no es un parcial lobo
 * sino un parcial ALIASADO, que es asunto de REQ-003 y de `canMeasureDeviation`,
 * no de esta etapa. Probar a 45 cents mezclaba los dos fenomenos: se midio, y el
 * motor convergia con la lectura correcta porque el parcial ya se estaba
 * descartando por otra razon.
 *
 * Y por arriba de `kMaxPartialDisagreementCents` (50) lo descarta ademas el
 * filtro de mediana. Ver el gemelo de abajo: las dos defensas se reparten el
 * trabajo y hay que afirmar las dos.
 */
TEST(ConvergenceHonesty, UnParcialQueElModeloNoExplicaApagaConvergido) {
    for (double lobo : {5.0, 20.0, 30.0}) {
        const auto buf = estereo(cuerda(kE2, 1e-4, 0.0, 4, 0.0, lobo, 3));
        float v[kSnapshotValueCount] = {};
        ASSERT_TRUE(analyzeBuffer(buf.data(), kFrames, kRate, kE2, v));
        EXPECT_NE(static_cast<int>(v[kSnapState]), kStateConverged)
            << "con el 3er parcial corrido " << lobo
            << " cents el motor dice CONVERGIDO con cents=" << v[kSnapCents]
            << " y sigma=" << v[kSnapUncertainty];
    }
}

/**
 * 🔴 EL REPARTO ENTRE LAS DOS DEFENSAS, y este test existe porque una expectativa
 * mia estaba mal y la MEDICION la corrigio.
 *
 * Con el parcial lobo a 60 cents esperaba que σ se inflara y se apagara
 * CONVERGIDO. No pasa, y esta BIEN que no pase: 60 > 50, asi que el filtro de
 * mediana descarta al lobo ANTES de que llegue al ajuste, y los tres parciales
 * que quedan describen la cuerda perfectamente. Medido: `cents = 5,2e-06` con
 * σ = 2,1e-05, o sea la respuesta correcta.
 *
 * Las dos defensas se reparten el trabajo por magnitud: el filtro de mediana se
 * lleva la basura evidente, y la σ de los residuos agarra la discrepancia SUTIL,
 * que es justamente la que pasaba el filtro sin despeinarse. Afirmar sólo la
 * primera mitad dejaria pasar un arreglo que rompe la otra.
 */
TEST(ConvergenceHonesty, ElParcialGroseramenteFueraDeSerieSeDescartaYLaLecturaSobrevive) {
    for (double lobo : {60.0}) {
        const auto buf = estereo(cuerda(kE2, 1e-4, 0.0, 4, 0.0, lobo, 3));
        float v[kSnapshotValueCount] = {};
        ASSERT_TRUE(analyzeBuffer(buf.data(), kFrames, kRate, kE2, v));
        EXPECT_EQ(static_cast<int>(v[kSnapState]), kStateConverged)
            << "lobo=" << lobo << ": por encima del filtro de mediana el parcial se"
            << " descarta y la lectura tiene que sobrevivir";
        EXPECT_LE(std::fabs(static_cast<double>(v[kSnapCents])), kBudgetCents)
            << "lobo=" << lobo << ": publica " << v[kSnapCents];
    }
}

/**
 * 🔴 EL GEMELO OBLIGATORIO. Sin el, AC-027.5 se satisface no convergiendo NUNCA.
 *
 * Una cuerda real e inarmonica, afinada, tiene que seguir convergiendo — que es
 * exactamente lo que un factor de escala χ² ingenuo rompia: medido, apagaba
 * CONVERGIDO desde B = 1e-4, o sea sobre cualquier cuerda de acero.
 */
TEST(ConvergenceHonesty, LaCuerdaInarmonicaRealSigueConvergiendo) {
    for (double B : kBs) {
        for (double ruido : {0.0, 0.002, 0.01}) {
            const auto buf = estereo(cuerda(kE2, B, 0.0, 4, ruido));
            float v[kSnapshotValueCount] = {};
            ASSERT_TRUE(analyzeBuffer(buf.data(), kFrames, kRate, kE2, v));
            EXPECT_EQ(static_cast<int>(v[kSnapState]), kStateConverged)
                << "B=" << B << " ruido=" << ruido
                << ": una cuerda real afinada tiene que converger";
        }
    }
}

/**
 * 🔴 σ TIENE QUE SER HONESTA EN LAS DOS DIRECCIONES, y este test existe porque un
 * MUTANTE sobrevivio y me corrigio una justificacion equivocada.
 *
 * El modelo se ajusta con la forma EXACTA `600·log2(1+B·n²)`. Yo habia
 * justificado eso diciendo que la linealizacion `K·n²` haria explotar los
 * residuos y apagaria CONVERGIDO sobre cuerdas sanas. **Es falso**, y el mutante
 * que cambiaba una por otra pasaba los cinco tests: dentro del ajuste B es libre
 * y absorbe casi toda la diferencia. Medido, con la cuerda a −12 cents:
 *
 *     B        C exacto / σ        C lineal / σ
 *     1e-04    -12,0000 / 0,00000  -11,9998 / 0,00007
 *     1e-03    -12,0000 / 0,00000  -11,9823 / 0,00722
 *
 * O sea que la lectura de la linealizacion sigue DENTRO del presupuesto. Lo que
 * NO sigue bien es σ: 0,0072 de σ sobre una cuerda que el modelo describe
 * perfectamente es **error de modelo disfrazado de discrepancia de medicion**,
 * 140x el del modelo exacto. Y la entrega de esta etapa es justamente que σ
 * signifique lo que dice.
 *
 * Una σ que se infla sola es tan deshonesta como una que miente para abajo: la de
 * abajo publica basura con cara de certeza, y la de arriba haria que el afinador
 * se declare inseguro sobre una cuerda que midio perfecto.
 */
TEST(ConvergenceHonesty, LaSigmaNoSeInflaPorElModelo) {
    for (double B : kBs) {
        const auto buf = estereo(cuerda(kE2, B, -12.0));
        float v[kSnapshotValueCount] = {};
        ASSERT_TRUE(analyzeBuffer(buf.data(), kFrames, kRate, kE2, v));
        EXPECT_LE(static_cast<double>(v[kSnapUncertainty]), 0.001)
            << "B=" << B << ": la cuerda encaja EXACTO en el modelo, asi que sigma"
            << " tiene que ser chica; salio " << v[kSnapUncertainty];
    }
}

// ---------------------------------------------------------------------------
// La lectura publicada es la del FUNDAMENTAL, no un promedio de parciales
// ---------------------------------------------------------------------------

/**
 * Con la cuerda desafinada `d` cents, el motor tiene que publicar `d` — no un
 * promedio corrido hacia arriba por el estiramiento de los parciales altos.
 *
 * Sobre la media ponderada anterior, con B = 1e-3 los parciales leian de 0,865 a
 * 13,740 y el combinado quedaba lejos del fundamental.
 */
TEST(ConvergenceHonesty, LaLecturaPublicadaEsLaDelFundamental) {
    for (double B : kBs) {
        for (double d : {0.0, -12.0, 7.5}) {
            const auto buf = estereo(cuerda(kE2, B, d));
            float v[kSnapshotValueCount] = {};
            ASSERT_TRUE(analyzeBuffer(buf.data(), kFrames, kRate, kE2, v));
            EXPECT_NEAR(static_cast<double>(v[kSnapCents]), d, kBudgetCents)
                << "B=" << B << " desafinacion real=" << d;
        }
    }
}

// ---------------------------------------------------------------------------
// Los dos estimadores de B no se contradicen
// ---------------------------------------------------------------------------

/**
 * Queda una redundancia declarada: el tracker ajusta B para combinar, y
 * `InharmonicityEstimator` lo estima aparte de los valores POR PARCIAL. No se
 * unifican (invertiria la dependencia), pero no pueden discrepar.
 */
TEST(ConvergenceHonesty, LosDosEstimadoresDeBNoSeContradicen) {
    for (double B : {1e-5, 1e-4, 1e-3}) {
        const auto m = cuerda(kE2, B, 0.0);
        StrobeTracker st;
        st.prepare(kRate);
        st.setTarget(kE2);
        st.setCoarseFrequencyHz(kE2);
        st.process(m.data(), static_cast<int>(m.size()));
        InharmonicityEstimator inh;
        ASSERT_TRUE(inh.estimateFrom(st)) << "B=" << B;
        EXPECT_NEAR(inh.b(), B, 0.25 * B)
            << "el estimador aparte dice " << inh.b() << " para un B real de " << B;
    }
}

}  // namespace
