/**
 * REQ-027 S1 — un parcial sin energia en su bin no entra en la combinacion.
 *
 * POR QUE ESTE ARCHIVO EXISTE
 * ---------------------------
 * `StrobeTracker` apunta cuatro estimadores a (i+1)*f0 y los combina por inverso
 * de la varianza. Cuando la señal NO tiene energia en 2f0/3f0/4f0, esos tres
 * integran FUGA ESPECTRAL: la fuga avanza de fase suave, o sea que da un ajuste
 * lineal bueno, o sea σ CHICA. La ponderacion sola no puede descartarla por mas
 * correcta que sea, y el filtro de mediana tampoco — medido sobre tono puro en
 * E2, la fuga queda a 41,96 cents de la mediana contra un umbral de 50, o sea el
 * 83,9 % del margen, y pasa.
 *
 * Resultado medido sobre v2.14.0, camino offline, 3 s, afinado EXACTO:
 *
 *     E2 con 1 parcial   ->  +38,70 cents   CONVERGIDO   σ = 0,0028
 *     D3 con 3 parciales ->  +11,46 cents   CONVERGIDO
 *
 * 🔴 POR QUE LOS TESTS VIENEN DE A PARES
 * --------------------------------------
 * El defecto tiene DOS modos de falla opuestos, y un test solo deja pasar al
 * otro. "No publiques un cents equivocado" se satisface no publicando nunca:
 * descartar los cuatro parciales apaga el afinador para el diapason y el tono de
 * referencia, que es material legitimo. Por eso AC-027.1 (no admitir al que no
 * mide) viaja siempre con AC-027.3 (seguir midiendo con el que si mide).
 */

#include "../AnalysisSnapshot.h"
#include "../OfflineAnalysis.h"
#include "../StrobeTracker.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

using namespace wma::analysis;

constexpr int kRate = 44100;

/// El presupuesto del producto, en cents. Lo declara docs/tuner/accuracy_contract.md.
///
/// 🔴 UNA SOLA CONSTANTE PARA TODOS LOS TESTS DE ESTE ARCHIVO. Es la leccion que
/// test_offline_analysis.cpp ya pago: con un `0.1` escrito a mano por test, el
/// mutante que afloja uno sobrevive porque los otros afirman contra su propio
/// numero y no vigilan nada.
constexpr double kBudgetCents = 0.1;

/// 3 s: la condicion exacta bajo la que el contrato declara su exactitud.
constexpr int kFrames = kRate * 3;

struct Cuerda { const char* nombre; double hz; };

/// Las seis de la guitarra. El catalogo entero de 14 es de S3: aca alcanza con
/// cubrir los registros donde el defecto se midio (E2 grave, D3 y G3 medias).
constexpr Cuerda kCuerdas[] = {
    {"E2", 82.4069}, {"A2", 110.0},     {"D3", 146.8324},
    {"G3", 195.9977}, {"B3", 246.9417}, {"E4", 329.6276},
};

/// Fases iniciales. NO es decoracion: la fase es una eleccion arbitraria de
/// quien graba, y es el eje sobre el que el defecto se hizo visible.
constexpr double kFases[] = {0.0, M_PI / 4, M_PI / 2, 3 * M_PI / 4};

/// Señal mono de `nParciales` armonicos, afinada EXACTO en `f0`.
std::vector<float> mono(double f0, int nParciales, double fase, int frames = kFrames,
                        int rate = kRate) {
    std::vector<float> m(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        double s = 0.0;
        for (int n = 1; n <= nParciales; ++n) {
            s += (0.3 / n) * std::sin(2.0 * M_PI * f0 * n * i / rate + fase * n);
        }
        m[static_cast<size_t>(i)] = static_cast<float>(s);
    }
    return m;
}

/// La misma señal, estereo intercalado, para el camino offline.
std::vector<float> estereo(double f0, int nParciales, double fase, int frames = kFrames,
                           int rate = kRate) {
    const auto m = mono(f0, nParciales, fase, frames, rate);
    std::vector<float> b(m.size() * 2);
    for (size_t i = 0; i < m.size(); ++i) { b[i * 2] = m[i]; b[i * 2 + 1] = m[i]; }
    return b;
}

// ---------------------------------------------------------------------------
// AC-027.2 — afinado exacto lee cero, para TODA riqueza armonica y TODA fase
// ---------------------------------------------------------------------------

/**
 * El test que reproduce el defecto reportado por Tunio.
 *
 * Recorre riqueza armonica 1..4 x 4 fases x 6 cuerdas = 96 casos. Sobre
 * v2.14.0 falla en E2/D3/G3 con pocos parciales.
 */
TEST(PartialAdmission, AfinadoExactoLeeCeroParaTodaRiquezaArmonica) {
    for (const auto& c : kCuerdas) {
        for (int nPart = 1; nPart <= 4; ++nPart) {
            for (double fase : kFases) {
                const auto buf = estereo(c.hz, nPart, fase);
                float v[kSnapshotValueCount] = {};
                ASSERT_TRUE(analyzeBuffer(buf.data(), kFrames, kRate, c.hz, v))
                    << c.nombre << " parciales=" << nPart << " fase=" << fase;
                EXPECT_LE(std::fabs(static_cast<double>(v[kSnapCents])), kBudgetCents)
                    << "cuerda " << c.nombre << ", " << nPart
                    << " parcial(es), fase " << fase << " rad: el motor publica "
                    << v[kSnapCents] << " cents con estado " << v[kSnapState];
            }
        }
    }
}

// ---------------------------------------------------------------------------
// AC-027.1 — el parcial que no mide no entra
// ---------------------------------------------------------------------------

/**
 * La observacion DIRECTA del mecanismo, no de su efecto.
 *
 * Con un tono puro solo p0 tiene energia; los otros tres ven fuga. Sobre
 * v2.14.0 `partialsUsed()` vale 4.
 */
TEST(PartialAdmission, ConTonoPuroSoloSeUsaElParcialQueTieneEnergia) {
    for (const auto& c : kCuerdas) {
        const auto m = mono(c.hz, 1, 0.0);
        StrobeTracker st;
        st.prepare(kRate);
        st.setTarget(c.hz);
        st.process(m.data(), static_cast<int>(m.size()));
        EXPECT_EQ(st.partialsUsed(), 1)
            << "cuerda " << c.nombre << ": se admitieron " << st.partialsUsed()
            << " parciales, pero solo uno tiene energia";
    }
}

/**
 * El gradiente completo: con `k` parciales en la señal se usan `k`.
 *
 * Un test que solo mirara el tono puro se satisface con una regla que cuente
 * mal en el medio — admitir siempre 1, por ejemplo, tira tres cuartos de la
 * evidencia en una cuerda real y la suite no lo veria.
 */
TEST(PartialAdmission, SeUsanExactamenteLosParcialesQueTienenEnergia) {
    for (const auto& c : kCuerdas) {
        for (int nPart = 1; nPart <= 4; ++nPart) {
            const auto m = mono(c.hz, nPart, 0.0);
            StrobeTracker st;
            st.prepare(kRate);
            st.setTarget(c.hz);
            st.process(m.data(), static_cast<int>(m.size()));
            EXPECT_EQ(st.partialsUsed(), nPart)
                << "cuerda " << c.nombre << " con " << nPart << " parcial(es)";
        }
    }
}

/**
 * 🔴 LA CUERDA MAS GRAVE A 48 kHz, QUE ES DONDE EL PISO DE ENERGIA SE GANA EL PAN.
 *
 * Este test existe porque un MUTANTE lo pidio. Con el piso de admision en 0 —o
 * sea el comportamiento de v2.14.0— los tests de arriba seguian VERDES: con la
 * zona muerta ya restaurada, el fundamental sobrevive y la ponderacion por 1/σ²
 * lo deja dominar, asi que la fuga admitida se diluye por debajo del presupuesto
 * en las seis cuerdas de guitarra a 44,1 kHz.
 *
 * O sea que la rejilla de arriba NO puede refutar el piso de energia, y sin este
 * test el piso seria una linea que ningun mutante mata — exactamente lo que le
 * paso a la zona muerta, que se saco por "no cambia ningun numero" medido sobre
 * una muestra que no contenia el caso.
 *
 * Medido sobre 2304 casos (2 rates x 3 duraciones x 8 cuerdas de B0 a C7 x
 * riqueza 1..4 x 6 fases x con y sin ruido):
 *
 *     con piso de energia   ->  peor |cents| = 0,0456
 *     sin piso de energia   ->  peor |cents| = 0,9375   (9,4x el presupuesto)
 *
 * y el peor caso es exactamente este: **B0 a 48 kHz, 2 parciales, 1 s**. La razon
 * es de resolucion: a 30,87 Hz y 48 kHz el bin del tercer parcial queda a pocos
 * bins del segundo, asi que la fuga trepa a 2,63e-02 — el doble larga del
 * catalogo entero, y lo que fija `kMinBinToRmsRatio`.
 */
TEST(PartialAdmission, EnLaCuerdaMasGraveA48kLaFugaTampocoEntra) {
    constexpr int kRate48 = 48000;
    constexpr double kB0 = 30.8677;
    for (double secs : {1.0, 2.0, 3.0}) {
        const int frames = static_cast<int>(kRate48 * secs);
        for (int nPart = 1; nPart <= 3; ++nPart) {
            for (double fase : kFases) {
                const auto buf = estereo(kB0, nPart, fase, frames, kRate48);
                float v[kSnapshotValueCount] = {};
                ASSERT_TRUE(analyzeBuffer(buf.data(), frames, kRate48, kB0, v))
                    << "B0 " << secs << " s, " << nPart << " parcial(es)";
                EXPECT_LE(std::fabs(static_cast<double>(v[kSnapCents])), kBudgetCents)
                    << "B0 a 48 kHz, " << secs << " s, " << nPart
                    << " parcial(es), fase " << fase << " rad: el motor publica "
                    << v[kSnapCents] << " cents con estado " << v[kSnapState];
            }
        }
    }
}

// ---------------------------------------------------------------------------
// AC-027.3 — EL GEMELO: descartar no puede volverse callarse
// ---------------------------------------------------------------------------

/**
 * 🔴 SIN ESTE TEST, EL REQ SE SATISFACE APAGANDO EL AFINADOR.
 *
 * Un tono puro es material legitimo y frecuente: diapason, tono de referencia,
 * flauta, bajo por DI. El unico parcial con energia mide EXACTO —medido, 0,000
 * cents con σ = 7e-05—, asi que la respuesta correcta no es callarse: es
 * publicar ese.
 */
TEST(PartialAdmission, ConTonoPuroElMotorSigueConvergiendo) {
    for (const auto& c : kCuerdas) {
        for (double fase : kFases) {
            const auto buf = estereo(c.hz, 1, fase);
            float v[kSnapshotValueCount] = {};
            ASSERT_TRUE(analyzeBuffer(buf.data(), kFrames, kRate, c.hz, v))
                << c.nombre << " fase=" << fase;
            EXPECT_EQ(static_cast<int>(v[kSnapState]), kStateConverged)
                << "cuerda " << c.nombre << ", fase " << fase
                << ": un tono puro exacto tiene que converger, no callarse";
            EXPECT_LE(std::fabs(static_cast<double>(v[kSnapCents])), kBudgetCents)
                << "cuerda " << c.nombre << ", fase " << fase;
        }
    }
}

// ---------------------------------------------------------------------------
// AC-027.6 — el caso rico no se paga con precision
// ---------------------------------------------------------------------------

/**
 * La admision no puede empeorar lo que ya andaba. El contrato declara 0,0011
 * cents como peor de 14 cuerdas a 3 s; aca se pide un orden de magnitud de
 * holgura sobre eso (0,01), que sigue siendo 10x mejor que el presupuesto.
 */
TEST(PartialAdmission, LaCuerdaRicaConservaSuExactitud) {
    for (const auto& c : kCuerdas) {
        for (double fase : kFases) {
            const auto buf = estereo(c.hz, 4, fase);
            float v[kSnapshotValueCount] = {};
            ASSERT_TRUE(analyzeBuffer(buf.data(), kFrames, kRate, c.hz, v));
            EXPECT_LE(std::fabs(static_cast<double>(v[kSnapCents])), 0.01)
                << "cuerda " << c.nombre << ", fase " << fase;
        }
    }
}

}  // namespace
