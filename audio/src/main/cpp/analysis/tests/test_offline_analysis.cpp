/**
 * REQ-015 S1 — el nucleo de analisis offline.
 *
 * Cubre AC-015.2 (determinismo) y AC-015.3 (paridad con el camino de tiempo
 * real). La superficie publica es de S2; aca se prueba el nucleo.
 *
 * 🔴 POR QUE ESTOS TESTS NO ESPERAN NADA
 * ---------------------------------------
 * No hay un solo `waitUntil` en este archivo, y esa ausencia es la entrega. El
 * puerto corre el analisis sincronicamente, asi que no hay a que esperarle: si
 * mañana alguien mete un thread adentro, estos tests van a necesitar esperas y
 * eso es la señal de que el nucleo dejo de estar separado.
 */

#include "../AnalysisRing.h"
#include "../AnalysisSnapshot.h"
#include "../AnalysisThread.h"
#include "../OfflineAnalysis.h"
#include "tests/support/TestWait.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

using namespace wma::analysis;

constexpr int kRate = 44100;   // NO 48000: es la constante que el motor tuvo
                               // hardcodeada, y usarla aca haria que un bug de
                               // propagacion pase inadvertido.
constexpr double kE4 = 329.6276;

double detune(double ref, double cents) { return ref * std::pow(2.0, cents / 1200.0); }

/// Cuerda de 4 parciales, estereo intercalado, fase continua por construccion.
std::vector<float> string(double f0, int frames, double amp = 0.5) {
    std::vector<float> b(static_cast<size_t>(frames) * 2, 0.0f);
    for (int i = 0; i < frames; ++i) {
        double s = 0.0;
        for (int n = 1; n <= 4; ++n) {
            s += (amp / n) * std::sin(2.0 * M_PI * f0 * n * i / kRate);
        }
        b[static_cast<size_t>(i) * 2]     = static_cast<float>(s);
        b[static_cast<size_t>(i) * 2 + 1] = static_cast<float>(s);
    }
    return b;
}

/// ~1,5 s: de sobra para que la integracion converja.
constexpr int kFrames = 66150;

/**
 * El presupuesto de la paridad, en cents. Es el del producto (0,1).
 *
 * 🔴 UNA SOLA CONSTANTE PARA LOS DOS TESTS, Y ESO SALIO DE LA MUTACION.
 * Antes cada test tenia su `0.1` escrito a mano, y el mutante que aflojaba el de
 * la paridad **sobrevivia**: el gemelo afirmaba contra SU propio numero, asi que
 * no vigilaba nada del otro. Un gemelo que no comparte el umbral que dice
 * proteger es decorativo — afirma sobre una constante que nadie mas usa.
 *
 * Compartiendola, aflojarla rompe el gemelo por construccion: dos señales
 * separadas por 1 cent tienen que diferir por MAS que el presupuesto, y con un
 * presupuesto grande dejan de hacerlo.
 */
constexpr double kParityBudgetCents = 0.1;

// ---------------------------------------------------------------------------
// AC-015.2 — determinismo
// ---------------------------------------------------------------------------

/**
 * El mismo buffer dos veces da EXACTAMENTE el mismo snapshot.
 *
 * Igualdad bit a bit, no `EXPECT_NEAR`: el determinismo es la unica razon de ser
 * de este puerto, y un "casi igual" no sirve para regresion — un cambio de 0,001
 * cents entre corridas ya obliga a elegir tolerancias, y elegir tolerancias es
 * como se pierde la capacidad de detectar deriva chica.
 */
TEST(OfflineAnalysis, TheSameBufferTwiceGivesTheIdenticalSnapshot) {
    const auto buf = string(detune(kE4, -12.0), kFrames);

    float a[kSnapshotValueCount];
    float b[kSnapshotValueCount];
    ASSERT_TRUE(analyzeBuffer(buf.data(), kFrames, kRate, kE4, a));
    ASSERT_TRUE(analyzeBuffer(buf.data(), kFrames, kRate, kE4, b));

    for (int i = 0; i < kSnapshotValueCount; ++i) {
        if (std::isnan(a[i])) {
            EXPECT_TRUE(std::isnan(b[i])) << "el valor " << i << " fue NaN una vez y no la otra";
        } else {
            EXPECT_EQ(a[i], b[i]) << "el valor " << i << " cambio entre dos corridas del "
                                     "MISMO buffer: el puerto no es determinista";
        }
    }
}

/**
 * Y no arrastra estado: dos llamadas separadas por OTRO analisis distinto siguen
 * dando lo mismo.
 *
 * Sin esto, "determinista" podria significar apenas "no se resetea entre
 * corridas seguidas" — y el estimador integra fase a lo largo de segundos, asi
 * que un ring reusado haria que el resultado dependa de que se analizo antes.
 */
TEST(OfflineAnalysis, AnInterveningAnalysisDoesNotChangeTheResult) {
    const auto mine  = string(detune(kE4, -12.0), kFrames);
    const auto other = string(detune(kE4, +40.0), kFrames);

    float first[kSnapshotValueCount];
    float second[kSnapshotValueCount];
    float scratch[kSnapshotValueCount];
    ASSERT_TRUE(analyzeBuffer(mine.data(), kFrames, kRate, kE4, first));
    ASSERT_TRUE(analyzeBuffer(other.data(), kFrames, kRate, kE4, scratch));
    ASSERT_TRUE(analyzeBuffer(mine.data(), kFrames, kRate, kE4, second));

    for (int i = 0; i < kSnapshotValueCount; ++i) {
        if (std::isnan(first[i])) continue;
        EXPECT_EQ(first[i], second[i])
            << "el valor " << i << " cambio porque en el medio se analizo otra cosa: "
               "el puerto arrastra estado";
    }
}

// ---------------------------------------------------------------------------
// AC-015.3 — paridad con el camino de tiempo real
// ---------------------------------------------------------------------------

/// Corre el MISMO audio por el camino vivo: thread, ring y esperas por condicion.
double realtimeCents(const std::vector<float>& buf, int frames) {
    AnalysisRing ring;
    AnalysisSnapshot snap;
    AnalysisThread th(ring, snap);
    ring.setCaptureRate(kRate);
    th.setTargetHz(kE4);
    th.start(kRate);

    constexpr int kBlock = 1024;
    int written = 0;
    while (written < frames) {
        const int chunk = (frames - written) < kBlock ? (frames - written) : kBlock;
        if (!wma_test::waitUntil([&] {
                return ring.availableFrames() + static_cast<uint32_t>(chunk)
                       <= AnalysisRing::kCapacityFrames;
            }, std::chrono::seconds(5))) {
            break;
        }
        ring.writeStereo(buf.data() + static_cast<size_t>(written) * 2, chunk);
        written += chunk;
    }
    // Esperar a que el analisis alcance lo escrito.
    wma_test::waitUntil([&] {
        float v[kSnapshotValueCount];
        return snap.read(v) && v[kSnapFramesAnalyzed] >= static_cast<float>(frames) * 0.9f;
    }, std::chrono::seconds(10));

    float v[kSnapshotValueCount];
    const bool ok = snap.read(v);
    th.stop();
    return ok ? static_cast<double>(v[kSnapCents]) : NAN;
}

/**
 * 🔴 EL FALSADOR DEL REQ ENTERO. Si el puerto mide otro motor, su verde no dice
 * nada del producto — y todo lo que se construya encima seria teatro.
 */
TEST(OfflineAnalysis, TheOfflinePortAgreesWithTheRealtimePath) {
    for (double cents : {-12.0, 0.0, +7.5}) {
        const auto buf = string(detune(kE4, cents), kFrames);

        float off[kSnapshotValueCount];
        ASSERT_TRUE(analyzeBuffer(buf.data(), kFrames, kRate, kE4, off));
        ASSERT_FALSE(std::isnan(off[kSnapCents]))
            << "el puerto no publico lectura a " << cents << " cents";

        const double live = realtimeCents(buf, kFrames);
        ASSERT_FALSE(std::isnan(live))
            << "premisa rota: el camino vivo no publico lectura a " << cents << " cents";

        // El presupuesto del producto es 0,1 cents. Los dos caminos corren el
        // MISMO analisis sobre el MISMO audio, asi que lo unico que los puede
        // separar es cuanto alcanzo a integrar cada uno.
        EXPECT_NEAR(off[kSnapCents], live, kParityBudgetCents)
            << "offline y tiempo real discrepan a " << cents << " cents: "
            << "offline=" << off[kSnapCents] << " vivo=" << live;
    }
}

/**
 * 🔴 EL GEMELO, y sin el la paridad no afirma nada.
 *
 * Un puerto que devolviera SIEMPRE lo mismo —o el valor del camino vivo— pasaria
 * el test de arriba. Lo que hace que la comparacion signifique algo es que el
 * presupuesto **distinga**: dos señales distintas NO pueden caer adentro.
 */
TEST(OfflineAnalysis, TheParityBudgetActuallyDiscriminates) {
    const auto a = string(detune(kE4, -12.0), kFrames);
    const auto b = string(detune(kE4, -11.0), kFrames);   // 1 cent, 10x el presupuesto

    float va[kSnapshotValueCount];
    float vb[kSnapshotValueCount];
    ASSERT_TRUE(analyzeBuffer(a.data(), kFrames, kRate, kE4, va));
    ASSERT_TRUE(analyzeBuffer(b.data(), kFrames, kRate, kE4, vb));
    ASSERT_FALSE(std::isnan(va[kSnapCents]));
    ASSERT_FALSE(std::isnan(vb[kSnapCents]));

    EXPECT_GT(std::fabs(va[kSnapCents] - vb[kSnapCents]), kParityBudgetCents)
        << "dos señales separadas por 1 cent salieron indistinguibles bajo el presupuesto "
           "de paridad: ese presupuesto no puede falsar nada";
}

/// Sin objetivo el puerto se comporta como el camino vivo: NaN, no cero.
TEST(OfflineAnalysis, WithoutATargetThereIsNoReadingButThereIsASnapshot) {
    const auto buf = string(kE4, kFrames);
    float v[kSnapshotValueCount];
    ASSERT_TRUE(analyzeBuffer(buf.data(), kFrames, kRate, 0.0, v));
    EXPECT_TRUE(std::isnan(v[kSnapCents]))
        << "publico un cents sin objetivo contra el cual medir";
    EXPECT_GT(v[kSnapFramesAnalyzed], 0.0f) << "no analizo nada";
}

/// Argumentos que no describen audio: false, y `out` intacto.
TEST(OfflineAnalysis, BadArgumentsReturnFalseWithoutTouchingTheOutput) {
    const auto buf = string(kE4, 1024);
    float v[kSnapshotValueCount];
    for (int i = 0; i < kSnapshotValueCount; ++i) v[i] = -12345.0f;

    EXPECT_FALSE(analyzeBuffer(nullptr, 1024, kRate, kE4, v));
    EXPECT_FALSE(analyzeBuffer(buf.data(), 0, kRate, kE4, v));
    EXPECT_FALSE(analyzeBuffer(buf.data(), 1024, 0, kE4, v));
    EXPECT_FALSE(analyzeBuffer(buf.data(), 1024, kRate, kE4, nullptr));

    for (int i = 0; i < kSnapshotValueCount; ++i) {
        EXPECT_EQ(v[i], -12345.0f) << "toco el valor " << i << " en un caso de error: "
                                      "devolver datos a medias es peor que no devolver nada";
    }
}

}  // namespace
