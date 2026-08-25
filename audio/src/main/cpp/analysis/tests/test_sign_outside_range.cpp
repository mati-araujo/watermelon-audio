/**
 * REQ-014 S2 — el signo invertido fuera del rango util (AC-014.3).
 *
 * 🔴 REINCIDENCIA, NO HALLAZGO NUEVO. REQ-003 AC-003.8 ataco esta clase, su
 * guarda `domainVerified()` esta en produccion desde 2.9.1, y **deja pasar este
 * caso**. Estos tests existen para que no vuelva una tercera vez.
 *
 * POR QUE LOS ESTIMULOS SON INARMONICOS, Y NO ES UN CAPRICHO
 * ----------------------------------------------------------
 * Con parciales ARMONICOS —`n·f0` exacto— el defecto **no se puede reproducir**:
 * medido en seis barridos (seno puro y cuerda de 4 parciales, a 44100 y 48000,
 * con y sin ruido, con y sin modo rapido) la guarda corta exactamente en su
 * rango declarado y no hay una sola inversion en 103 puntos.
 *
 * El ingrediente que falta es la INARMONICIDAD, que es lo que hace una cuerda
 * de verdad: `fn = n·f0·sqrt(1 + B·n²)`. Con ella la deteccion GRUESA queda
 * optimista —a −35,0 cents reales informa −28,56— y como |−28,56| cae dentro
 * del rango de 30,50 la guarda declara "en dominio". Pero el fundamental SI
 * esta afuera: aliasa y vuelve como **+27,53**.
 *
 * Ese numero es el del reporte de campo (+27,0 con la cuerda a −35), asi que un
 * test armonico habria quedado en verde con el defecto vivo. **El estimulo es
 * parte de la aserción.**
 */

#include "tests/support/TestWait.h"
#include "../AnalysisRing.h"
#include "../AnalysisSnapshot.h"
#include "../AnalysisThread.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

using namespace wma::analysis;

constexpr int kRate   = 48000;   // el rango de 30,50 c del comentario de REQ-003 sale de aca
constexpr int kFrames = 1024;
constexpr double kE4  = 329.6276;

/// Inarmonicidad de una cuerda de acero real. NO es un valor de laboratorio:
/// con B = 0 el defecto es INALCANZABLE, medido en 103 puntos de barrido.
constexpr double kSteelB = 1.0e-3;

double detune(double ref, double cents) { return ref * std::pow(2.0, cents / 1200.0); }

std::vector<float> inharmonicBlock(double f0, long startFrame, double B, double amp) {
    std::vector<float> b(static_cast<size_t>(kFrames) * 2, 0.0f);
    for (int i = 0; i < kFrames; ++i) {
        double s = 0.0;
        const double t = static_cast<double>(startFrame + i);
        for (int n = 1; n <= 4; ++n) {
            const double fn = f0 * n * std::sqrt(1.0 + B * static_cast<double>(n) * n);
            s += (amp / n) * std::sin(2.0 * M_PI * fn * t / kRate);
        }
        b[static_cast<size_t>(i) * 2]     = static_cast<float>(s);
        b[static_cast<size_t>(i) * 2 + 1] = static_cast<float>(s);
    }
    return b;
}

struct Reading {
    double cents = NAN;
    double coarseCents = NAN;
    bool published = false;
};

/// Corre el analisis sobre una cuerda a `realCents` del objetivo y devuelve lo
/// que el snapshot publica al final. Alimenta esperando LUGAR en el ring, para
/// no fabricar audio no contiguo (REQ-005 S3).
Reading measureAt(double realCents, double B) {
    AnalysisRing ring;
    AnalysisSnapshot snap;
    AnalysisThread th(ring, snap);
    ring.setCaptureRate(kRate);
    th.setTargetHz(kE4);
    th.start(kRate);

    const double f = detune(kE4, realCents);
    long cursor = 0;
    double seen = -1.0;
    // 50 bloques = ~1,07 s de audio a 48 kHz. Es lo que la integracion necesita
    // para converger con holgura (medido: converge cerca del bloque 24), y NO
    // mas: el presupuesto que aprieta es el de los SANITIZERS, donde cada
    // bloque cuesta varias veces mas. Ver "un test que no entra bajo
    // sanitizers" — un barrido uniforme y generoso es relleno caro.
    for (int k = 0; k < 50; ++k) {
        if (!wma_test::waitUntil([&] {
                return ring.availableFrames() + static_cast<uint32_t>(kFrames)
                       <= AnalysisRing::kCapacityFrames;
            }, std::chrono::seconds(5))) {
            break;
        }
        const auto blk = inharmonicBlock(f, cursor, B, 0.5);
        ring.writeStereo(blk.data(), kFrames);
        cursor += kFrames;
        wma_test::waitUntil([&] {
            float v[kSnapshotValueCount];
            if (!snap.read(v)) return false;
            if (static_cast<double>(v[kSnapFramesAnalyzed]) > seen) {
                seen = static_cast<double>(v[kSnapFramesAnalyzed]);
                return true;
            }
            return false;
        }, std::chrono::seconds(2));
    }

    Reading r;
    float v[kSnapshotValueCount];
    if (snap.read(v)) {
        r.cents = static_cast<double>(v[kSnapCents]);
        r.published = !std::isnan(r.cents);
        const double hz = static_cast<double>(v[kSnapDetectedHz]);
        if (hz > 0.0) r.coarseCents = 1200.0 * std::log2(hz / kE4);
    }
    th.stop();
    return r;
}

// ---------------------------------------------------------------------------
// AC-014.3 — el signo publicado no puede contradecir a la desviacion real
// ---------------------------------------------------------------------------

/**
 * El caso exacto del reporte: E4 con la cuerda 35 cents ABAJO. Antes de este
 * REQ el motor publicaba **+27,53** — le decia al musico que aflojara una
 * cuerda que hay que apretar.
 */
TEST(SignOutsideRange, TheReportedCaseNeverPublishesTheOppositeSign) {
    const Reading r = measureAt(-35.0, kSteelB);
    if (r.published) {
        EXPECT_LT(r.cents, 0.0)
            << "la cuerda esta 35 cents ABAJO y el motor publico " << r.cents
            << ": le dice al musico que afloje lo que hay que apretar";
    }
    SUCCEED() << "publicado=" << (r.published ? "si" : "no") << " cents=" << r.cents;
}

/**
 * El barrido: ninguna desviacion, adentro ni afuera del rango, puede publicar
 * un `cents` de signo contrario al real.
 *
 * Se afirma sobre el SIGNO y no sobre la magnitud a proposito: el sesgo por
 * inarmonicidad **esta fuera del alcance de este REQ** (medido y atribuido:
 * con seno puro las seis cuerdas leen dentro de ±0,2 cents). Lo que este REQ
 * prohibe es la INVERSION, que es la que manda al usuario en la direccion
 * equivocada.
 */
TEST(SignOutsideRange, NoDeviationEverPublishesTheOppositeSign) {
    int published = 0;
    // 🔴 EL BARRIDO ES DEL BORDE, NO UNIFORME, Y ESO NO ES AHORRO CIEGO.
    // La inversion solo puede nacer donde el sustituto se rompe: cerca del
    // rango de captura. Mas afuera (±60) la gruesa tambien queda afuera y el
    // motor publica NaN, o sea que esos puntos no evaluan una sola asercion y
    // cuestan lo mismo. Se conservan los positivos aunque las cuatro
    // inversiones medidas fueran todas del lado bemol: son el control de que la
    // guarda no se volvio "callate si es negativo".
    for (double real : {-45.0, -40.0, -35.0, -32.0, -31.5, -30.0, -25.0,
                         25.0,  30.0,  31.5,  32.0,  35.0}) {
        const Reading r = measureAt(real, kSteelB);
        if (!r.published) continue;
        ++published;
        EXPECT_EQ(r.cents > 0.0, real > 0.0)
            << "desviacion real " << real << " c, publicado " << r.cents
            << " c (gruesa " << r.coarseCents << " c): signo invertido";
    }
    // 2.5 — control positivo del propio barrido. Sin esto, un motor MUDO
    // pasaria el test de arriba sin una sola asercion evaluada.
    EXPECT_GT(published, 0)
        << "el barrido no publico ni una lectura: estaria midiendo un motor mudo";
}

/**
 * 🔴 EL GEMELO. Sin esto, apagar la lectura fina entera pasa AC-014.3.
 *
 * Dentro del rango util el motor tiene que SEGUIR publicando. Los puntos son
 * los que se midieron publicando antes del arreglo, asi que el test falla si el
 * arreglo se los come.
 */
TEST(SignOutsideRange, InsideTheUsableRangeTheEngineStillPublishes) {
    for (double real : {-31.5, -30.0, -25.0, 0.0}) {
        const Reading r = measureAt(real, kSteelB);
        EXPECT_TRUE(r.published)
            << "el motor dejo de publicar a " << real
            << " c, que es adentro del rango util: la guarda se comio la sensibilidad";
    }
}

/**
 * El gemelo del gemelo: con parciales ARMONICOS el motor no puede haber
 * cambiado de comportamiento, porque ahi el defecto no existia. Es el control
 * que separa "arregle el aliasing" de "apreté la guarda hasta que dejó de
 * publicar".
 */
TEST(SignOutsideRange, TheHarmonicCaseIsUntouched) {
    for (double real : {-30.0, -20.0, 0.0, 20.0, 30.0}) {
        const Reading r = measureAt(real, 0.0);
        EXPECT_TRUE(r.published)
            << "con parciales armonicos el motor dejo de publicar a " << real
            << " c, y ahi nunca hubo defecto que arreglar";
        if (r.published && real != 0.0) {
            EXPECT_EQ(r.cents > 0.0, real > 0.0) << "signo invertido en el caso armonico";
        }
    }
}

/**
 * 🔴 LA ZONA MUERTA DEL ARBITRAJE, Y POR QUE NO ES DECORATIVA.
 *
 * En "afinado" las dos magnitudes se van al ruido y sus signos dejan de
 * significar nada. Medido a 0,00 cents exactos con ruido: el control informa
 * **−0,0121** y la lectura fina **+0,0001**. Los signos difieren, con las dos
 * magnitudes en el orden de la milesima de cent.
 *
 * Sin la zona muerta, el arbitraje por signo descartaria parciales **justo
 * cuando el usuario termino de afinar**, que es el peor momento para apagar la
 * aguja. Este test es el unico que puede matar al mutante que la saca: aparece
 * en 2 de 42 escenarios de cuasi-afinacion, o sea que un barrido que no lo
 * busque no lo encuentra.
 */
TEST(SignOutsideRange, APerfectlyTunedStringWithNoiseStillPublishes) {
    for (double noise : {0.0, 0.02, 0.08}) {
        AnalysisRing ring;
        AnalysisSnapshot snap;
        AnalysisThread th(ring, snap);
        ring.setCaptureRate(kRate);
        th.setTargetHz(kE4);
        th.start(kRate);

        long cursor = 0;
        double seen = -1.0;
        unsigned st = 1234u;
        for (int k = 0; k < 50; ++k) {
            if (!wma_test::waitUntil([&] {
                    return ring.availableFrames() + static_cast<uint32_t>(kFrames)
                           <= AnalysisRing::kCapacityFrames;
                }, std::chrono::seconds(5))) {
                break;
            }
            auto blk = inharmonicBlock(kE4, cursor, 0.0, 0.5);
            if (noise > 0.0) {
                for (int i = 0; i < kFrames; ++i) {
                    st = st * 1664525u + 1013904223u;
                    const float n = static_cast<float>(
                        noise * (static_cast<double>(st >> 8) / 8388608.0 - 1.0));
                    blk[static_cast<size_t>(i) * 2]     += n;
                    blk[static_cast<size_t>(i) * 2 + 1] += n;
                }
            }
            ring.writeStereo(blk.data(), kFrames);
            cursor += kFrames;
            wma_test::waitUntil([&] {
                float v[kSnapshotValueCount];
                if (!snap.read(v)) return false;
                if (static_cast<double>(v[kSnapFramesAnalyzed]) > seen) {
                    seen = static_cast<double>(v[kSnapFramesAnalyzed]);
                    return true;
                }
                return false;
            }, std::chrono::seconds(2));
        }

        float v[kSnapshotValueCount];
        ASSERT_TRUE(snap.read(v));
        EXPECT_FALSE(std::isnan(v[kSnapCents]))
            << "el motor apago la aguja con la cuerda AFINADA (ruido " << noise
            << "): el arbitraje por signo se disparo sobre un empate tecnico";
        th.stop();
    }
}

}  // namespace
