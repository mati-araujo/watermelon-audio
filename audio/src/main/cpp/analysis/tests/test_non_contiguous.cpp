/**
 * REQ-005 S4 — el trinquete de la clase: el eje C, reproducido a voluntad.
 *
 * QUE ES EL "EJE C" Y POR QUE NECESITA UN TEST PROPIO
 * ---------------------------------------------------
 * REQ-005 persigue una familia: **un veredicto de test que depende del azar**. Su tercer eje
 * son los tests que afirman sobre una cantidad que todavia esta convergiendo, y fue el que
 * puso `master` en rojo el 21/08 — en el TSan del CI, verde en la maquina del autor.
 *
 * La causa se diagnostico MAL tres veces seguidas leyendo el codigo, y las tres las tumbo
 * medir el mecanismo. Lo que NO era: ni una lectura a medio converger (la transicion es un
 * escalon), ni el ring desbordado por capacidad (pico de 1024 sobre 8192), ni el troceado del
 * drenaje (`StrobeTracker` es bit-invariante de 113 a 4096 frames). Lo que SI es: **audio no
 * contiguo sostenido**. Cuando el ring se pisa, el estimador integra fase sobre muestras que
 * no son consecutivas, la fase salta, y la lectura sale plausible y equivocada.
 *
 * POR QUE SE PUEDE ESCRIBIR ESTE FRENO, Y POR QUE OTROS NO
 * --------------------------------------------------------
 * Un guardrail que no reproduce la falla no es un guardrail: queda registrado como cobertura
 * y no cubre nada. Por eso REQ-005 exige medir ANTES de escribirlo. Lo medido:
 *
 *   - ahogar la maquina (40 quemadores sobre 10 nucleos)  ->  0/10. No reproduce.
 *   - `taskpolicy -c background` + carga                  ->  1/10, y ese uno es un timeout.
 *   - fabricar el hueco a mano, sostenido                 ->  reproduce SIEMPRE.
 *
 * La contencion no sirve porque un `sleep` es tiempo ABSOLUTO: ahogar la maquina no achica la
 * ventana, y encima el render tarda mas, o sea que le da MAS margen. El hueco fabricado no
 * depende de ganar ninguna carrera, y por eso este test es determinista.
 *
 * 🔴 ESTE TEST DOCUMENTA UN DEFECTO DE PRODUCCION QUE NO ARREGLA, Y ESO ES DELIBERADO.
 * La tercera asercion afirma que la incertidumbre publicada **no ve** la discontinuidad. Eso
 * es un defecto real del motor y tiene su propio requisito (REQ-009); REQ-005 declara cero
 * archivos de produccion, y un test que exigiera lo correcto —que el motor NO declare
 * convergida una lectura equivocada— naceria ROJO, que es introducir un fallo y no un freno.
 *
 * 🔴 REQ-009 S3 YA PASO, Y ESTE TRINQUETE NO SE INVIERTE. Se reviso a proposito, porque el
 * doc de S2 habia escrito que actualizarlo era trabajo de S3. Leyendo `analyzeWithGap()`:
 * es falso. Ese helper maneja `StrobeTracker` **en aislamiento** —sin `AnalysisRing`, sin
 * `AnalysisThread`, sin backend— y le inyecta el hueco corriendo el indice de la señal. El
 * cable que S3 construyo (xrun del backend -> `InputNode` -> costura posicionada en el ring
 * -> `noteInputDiscontinuity()`) **no corre aca ni una linea**.
 *
 * Y no es una laguna: lo que la tercera asercion afirma es una propiedad del **estimador
 * solo, sin que nadie le avise**, y esa propiedad no cambio — es justamente la premisa de
 * REQ-009, medida en S1 (σ ANTI-correlacionada con el error: en la peor fila, 2,15 cents con
 * σ = 0,00098). El tracker sigue sin poder verlo; lo nuevo es que ahora alguien se lo dice.
 *
 * DONDE VIVE LA COBERTURA DEL EJE DE CAPTURA, para que esto no se lea como deuda sin pagar:
 *   · `core/tests/test_capture_discontinuity.cpp`  — el camino entero, las tres topologias
 *   · `core/tests/test_capture_gap_mailbox.cpp`    — el cruce de threads de iOS y USB
 */

#include "../StrobeTracker.h"
#include "../AnalysisThread.h"
#include "../../dsp/McLeodPitch.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using namespace wma::analysis;

/// El rate que el motor tenia hardcodeado. Usar 48000 aca haria que un bug de
/// propagacion de rate pase inadvertido — la leccion de WD-3.4.
constexpr int kRate = 44100;

constexpr double kTargetHz = 440.0;

/// Cuanto esta desafinada la cuerda de verdad. Es lo que el motor DEBERIA publicar.
constexpr double kRealCents = -5.0;

/**
 * El presupuesto de exactitud, en cents.
 *
 * Sale del contrato del motor y no de este test: `AnalysisThread::kConvergedUncertaintyCents`
 * es 0,1, o sea que por debajo de eso la medicion ya no es lo que limita. Un test que eligiera
 * su propio numero estaria midiendo contra si mismo.
 */
constexpr double kBudgetCents = AnalysisThread::kConvergedUncertaintyCents;

struct Reading {
    double cents;
    double sigma;
    bool converged;
};

/**
 * Analiza una cuerda de 4 parciales, saltandose `gapFrames` en la segunda mitad.
 *
 * El hueco se mete en CADA trozo a partir de la mitad: eso modela un ring que se pisa de forma
 * SOSTENIDA, que es el caso real, y no un tropiezo aislado del que el estimador se recupera.
 */
Reading analyzeWithGap(int gapFrames) {
    const double realHz = kTargetHz * std::pow(2.0, kRealCents / 1200.0);
    constexpr int kTotal = 200 * 1024;
    constexpr int kChunk = 1024;

    auto sampleAt = [&](int64_t i) {
        double s = 0.0;
        for (int n = 1; n <= 4; ++n) {
            s += (0.5 / n) *
                 std::sin(2.0 * M_PI * realHz * n * static_cast<double>(i) / kRate);
        }
        return static_cast<float>(s);
    };

    wma::dsp::McLeodPitch det;
    StrobeTracker strobe;
    det.prepare(kRate);
    strobe.prepare(kRate);
    strobe.setTarget(kTargetHz);

    std::vector<float> buf(kChunk);
    int64_t src = 0;   // indice en la señal REAL: avanza de mas cuando hay hueco
    for (int fed = 0; fed < kTotal; fed += kChunk) {
        if (gapFrames > 0 && fed >= kTotal / 2) src += gapFrames;
        for (int k = 0; k < kChunk; ++k) {
            buf[static_cast<size_t>(k)] = sampleAt(src + k);
        }
        src += kChunk;
        det.process(buf.data(), kChunk);
        strobe.setCoarseFrequencyHz(det.hasPitch() ? det.frequencyHz() : 0.0);
        strobe.process(buf.data(), kChunk);
    }
    return {strobe.cents(), strobe.uncertaintyCents(), strobe.converged()};
}

}  // namespace

/**
 * El eje C, reproducido a voluntad — y sus TRES mitades.
 *
 * La tercera es la que hace de esto un trinquete y no un test mas: sin ella, el dia que
 * alguien "arregle" el sintoma bajando un umbral, este test seguiria verde.
 */
TEST(NonContiguousAudio, SustainedGapsProduceAPlausibleButWrongReading) {
    // ---- 1 · control positivo -------------------------------------------
    // Sin esto, un test que "detecta" el hueco podria estar detectando cualquier otra cosa
    // —una señal mal construida, un estimador que nunca engancha— y nadie se enteraria.
    const Reading contiguous = analyzeWithGap(0);
    ASSERT_TRUE(contiguous.converged)
        << "control positivo roto: con audio CONTIGUO el motor ni siquiera convergio. "
        << "Este test no puede afirmar nada sobre el hueco hasta que el caso sano ande.";
    EXPECT_NEAR(contiguous.cents, kRealCents, kBudgetCents)
        << "con audio contiguo la lectura tiene que caer dentro de presupuesto";

    // ---- 2 · el daño, a voluntad ----------------------------------------
    constexpr int kGapFrames = 64;
    const Reading gapped = analyzeWithGap(kGapFrames);
    const double error = std::abs(gapped.cents - kRealCents);
    EXPECT_GT(error, kBudgetCents)
        << "el eje C dejo de reproducirse: con huecos de " << kGapFrames
        << " frames sostenidos, la lectura salio DENTRO de presupuesto (error " << error
        << " cents).\n"
        << "  Si el estimador se volvio robusto a la discontinuidad, esto es una BUENA "
        << "noticia y el trinquete hay que actualizarlo — no borrarlo.\n"
        << "  Si no, este test dejo de reproducir la falla que existe para congelar, y "
        << "cualquier freno que se apoye en el es decorativo.";

    // ---- 3 · y sigma SI lo ve, desde REQ-027 ----------------------------
    //
    // 🔴 ESTA MITAD ESTABA AL REVES HASTA EL 2026-09-01, y darla vuelta es una
    // BUENA noticia que hay que dejar congelada en la direccion nueva.
    //
    // Decia: "sigma NO lo ve — el motor mide MAL y dice que midio BIEN, con la
    // misma cara que cuando mide bien de verdad". Era cierto mientras σ se
    // PROPAGABA de las σ por parcial, que son una PRECISION (cuan bien encaja una
    // recta) y no una exactitud.
    //
    // REQ-027 S2 la cambio por la σ de los RESIDUOS del ajuste
    // `cents_n = C + 600·log2(1+B·n²)`, y eso arreglo esto de rebote: un hueco
    // sostenido rompe la coherencia entre parciales, los residuos crecen y σ sube.
    // Medido con huecos de 64 frames: **σ = 0,341 con un error de 1,359 cents**,
    // contra un presupuesto de 0,1. El motor ya no dice que midio bien.
    //
    // ⚠️ LO QUE **NO** SE ARREGLO, Y POR ESO LA MITAD 2 SIGUE EN PIE: la lectura
    // SIGUE SIENDO MALA (1,36 cents de error). REQ-027 no hizo al estimador
    // robusto a la discontinuidad — lo hizo HONESTO sobre ella. La deuda del eje C
    // sigue abierta y es de REQ-009; lo que se cerro es que saliera disfrazada de
    // medicion buena.
    EXPECT_GT(gapped.sigma, kBudgetCents)
        << "sigma dejo de ver la discontinuidad (sigma = " << gapped.sigma
        << " con un error de " << error << " cents).\n"
        << "  🔴 Esto es una REGRESION de REQ-027: la σ volvio a ser una precision "
        << "propagada en vez del residuo del ajuste, y el motor volvio a publicar "
        << "una lectura mala con cara de buena.\n"
        << "  El trinquete es bidireccional: avisa tanto si aparece deuda nueva "
        << "como si una mejora declarada deja de reproducirse.";
    EXPECT_FALSE(gapped.converged)
        << "el motor volvio a declarar CONVERGIDA una lectura equivocada (error "
        << error << " cents). Misma causa que el EXPECT anterior: ver REQ-027 S2.";
}
