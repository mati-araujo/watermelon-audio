/**
 * test_latch_instrument.cpp — REQ-055 S0. La vara del afinador: tiempo y error POR ENGANCHE.
 *
 * Suites:
 *   · `AttackInstrument`       el nucleo POR ATAQUE (D11): t0 del oraculo, sobre secuencias a mano.
 *   · `LatchInstrumentRealTake` la toma del g42: SKIPPED mientras no exista.
 *   · `LatchInstrument`        el nucleo (`support/LatchInstrument.h`) sobre secuencias a mano,
 *                              SIN corpus. Un instrumento que no puede fallar no mide nada: cada
 *                              test dice que bug del instrumento atrapa.
 *   · `LatchInstrumentCorpus`  el corrido sobre corpus-v1 contra la LINEA DE BASE commiteada
 *                              (`baseline/enganche-corpus-v1.tsv`). Sin corpus verificado: SKIPPED,
 *                              nunca passed (`bash scripts/fetch-corpus.sh`).
 *
 * LA LINEA DE BASE ES UN TRINQUETE Y SU DIFF ES LA REVISION. Los tiempos se comparan en frames
 * EXACTOS; los errores, σ y cobertura con tolerancia 0,002. Si cualquier cambio mueve el analisis,
 * este test se pone rojo, y la unica salida es mirar el diff del TSV y decidir si el cambio es el
 * que se buscaba. Se regenera con
 *
 *     WMA_LATCH_BASELINE_UPDATE=1 bash scripts/run-cpp-tests.sh -R LatchInstrumentCorpus
 *
 * que ESCRIBE el archivo en vez de comparar y deja el test SKIPPED (regenerar no es verificar,
 * igual que `regen-golden.sh`).
 */

#include "support/Corpus.h"
#include "support/CorpusSweep.h"
#include "support/LatchInstrument.h"
#include "tests/support/TestSanitizer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace wma_test {
namespace {

using latch::Latch;
using latch::OracleSegment;
using latch::Params;
using latch::Publication;
using latch::TargetEvent;
using latch::Window;

// =====================================================================================
// El nucleo, sin corpus
// =====================================================================================

// Rate de 1000 frames/s en los tests del nucleo: un frame es un milisegundo y las cuentas se
// hacen a mano.
constexpr int kRate = 1000;
constexpr int kConv = wma::analysis::kStateConverged;
constexpr int kMeas = wma::analysis::kStateMeasuring;
constexpr int kNoLock = wma::analysis::kStateNoLock;
constexpr double kLoud = 0.5;
constexpr double kQuiet = 0.0005;
constexpr double kNan = latch::kNaN;

Params params() {
    Params p;
    p.sampleRate = kRate;
    return p;
}

/// Ventana de los tests del nucleo: 100 frames. La de produccion (4096) es la misma regla a otra escala.
constexpr int kWin = 100;

/// Una serie de ventanas de 100 frames desde el frame 0, con el rms dado a cada una.
std::vector<Window> wins(const std::vector<double>& rms) {
    std::vector<Window> out;
    int64_t start = 0;
    for (double r : rms) {
        out.push_back({start, r});
        start += kWin;
    }
    return out;
}

std::vector<Window> loudWins(int n) { return wins(std::vector<double>(static_cast<size_t>(n), kLoud)); }

/// Una publicacion fechada en `pos`.
Publication pub(int64_t pos, double hz = kNan, int state = kNoLock, double sigma = 1.0) {
    Publication p;
    p.pos = pos;
    p.hz = hz;
    p.state = state;
    p.sigmaCents = sigma;
    return p;
}

double hzAt(double base, double cents) { return base * std::pow(2.0, cents / 1200.0); }

const std::vector<OracleSegment> kNoOracle{};

TEST(LatchInstrument, SignalAfterSilenceOpensALatchAtTheStartOfThatWindow) {
    // Atrapa: no abrir enganche tras el silencio, o fechar t0 al FINAL de la ventana en vez de a
    // su inicio (400 en lugar de 300).
    const std::vector<Publication> pubs = {pub(100), pub(200), pub(300), pub(400)};
    const auto l = latch::measure(pubs, wins({kLoud, kQuiet, kQuiet, kLoud}), {{0, true}}, kNoOracle, params());
    ASSERT_EQ(l.size(), 2u);
    EXPECT_EQ(l[0].t0Frames, 0);
    EXPECT_EQ(l[1].t0Frames, 300);
    EXPECT_EQ(l[1].cause, Latch::Cause::kSignalAfterSilence);
    EXPECT_DOUBLE_EQ(l[1].t0Sec, 0.3);
}

TEST(LatchInstrument, TheFirstWindowWithSignalAtTheTargetChangeIsOneLatchNotTwo) {
    // Atrapa: contar dos veces el arranque (el evento de objetivo Y la primera ventana con senal
    // sobre el ring vacio) y duplicar el enganche inicial.
    const std::vector<Publication> pubs = {pub(100), pub(200)};
    const auto l = latch::measure(pubs, loudWins(2), {{0, true}}, kNoOracle, params());
    ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l[0].cause, Latch::Cause::kTargetChange);
    EXPECT_EQ(l[0].publications, 2);
}

TEST(LatchInstrument, SignalFloorIsInclusiveAtOneThousandth) {
    // Atrapa: `>` en vez de `>=`, o un piso distinto de 0,001. rms == 0,001 ES senal;
    // 0,00099 no lo es. Un piso mal puesto mueve t0 de todos los enganches por senal.
    const std::vector<Publication> atFloor = {pub(100), pub(200)};
    auto l = latch::measure(atFloor, wins({0.0, 0.001}), {{0, true}}, kNoOracle, params());
    ASSERT_EQ(l.size(), 2u);
    EXPECT_EQ(l[1].t0Frames, 100);

    const std::vector<Publication> below = {pub(100), pub(200)};
    l = latch::measure(below, wins({0.0, 0.00099}), {{0, true}}, kNoOracle, params());
    EXPECT_EQ(l.size(), 1u) << "0,00099 es silencio: no abre enganche";
}

TEST(LatchInstrument, TargetChangeOpensALatchEvenWhileTheStringKeepsSounding) {
    // Atrapa: ignorar los cambios de objetivo cuando no hubo silencio. Las publicaciones
    // posteriores al evento se atribuyen al enganche NUEVO.
    const std::vector<Publication> pubs = {pub(100, 100.0), pub(200, 100.0), pub(300),
                                           pub(400, 200.0)};
    const auto l = latch::measure(pubs, loudWins(4), {{0, true}, {200, true}}, kNoOracle, params());
    ASSERT_EQ(l.size(), 2u);
    EXPECT_EQ(l[1].t0Frames, 200);
    EXPECT_EQ(l[0].publications, 2);
    EXPECT_EQ(l[1].publications, 2);
    EXPECT_EQ(l[1].firstReadingFrames, 200) << "desde el cambio de objetivo, no desde el 0";
}

TEST(LatchInstrument, ARepluckWithoutSilenceInBetweenOpensNoLatch) {
    // Atrapa: abrir un enganche en cada subida de nivel. El segundo ataque llega con la cuerda
    // aun sonando (nunca bajo 0,001): es el MISMO enganche.
    const std::vector<Publication> pubs = {pub(100), pub(200), pub(300), pub(400),
                                           pub(500)};
    const auto l = latch::measure(pubs, wins({0.5, 0.01, 0.002, 0.9, 0.9}), {{0, true}}, kNoOracle, params());
    ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l[0].publications, 5);
}

TEST(LatchInstrument, TargetSetDuringSilenceThenSignalAreTwoLatches) {
    // Atrapa: fusionar el evento con una ventana con senal que llega MUCHO despues. Son dos
    // t_reenganche distintos; el primero queda sin lecturas.
    const std::vector<Publication> pubs = {pub(100), pub(200), pub(300, 100.0)};
    const auto l = latch::measure(pubs, wins({kQuiet, kQuiet, kLoud}), {{0, true}}, kNoOracle, params());
    ASSERT_EQ(l.size(), 2u);
    EXPECT_EQ(l[0].t0Frames, 0);
    EXPECT_EQ(l[0].readings, 0);
    EXPECT_EQ(l[1].t0Frames, 200);
    EXPECT_EQ(l[1].firstReadingFrames, 100);
}

TEST(LatchInstrument, WithoutATargetNothingIsAttributed) {
    // Atrapa: abrir enganches por senal "con objetivo puesto" cuando NO hay objetivo; y que
    // quitar el objetivo (hasTarget=false) deje el enganche anterior abierto.
    const std::vector<Publication> pubs = {pub(100, 100.0), pub(200), pub(300, 100.0),
                                           pub(400), pub(500, 100.0)};
    EXPECT_TRUE(latch::measure(pubs, wins({kLoud, kLoud, kQuiet, kLoud, kQuiet, kLoud}), {}, kNoOracle, params()).empty());
    const auto l = latch::measure(pubs, wins({kLoud, kLoud, kQuiet, kLoud, kQuiet, kLoud}), {{0, true}, {250, false}}, kNoOracle, params());
    ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l[0].publications, 2) << "lo posterior a quitar el objetivo no cuenta";
}

TEST(LatchInstrument, FirstReadingAndConvergedAreMeasuredFromT0Exactly) {
    // Atrapa: medir desde 0 en vez de desde t0, contar por indice en vez de frames, tomar el
    // primer CONVERGED sin esperar al primero, o contar una publicacion sin cents como lectura.
    const std::vector<Publication> pubs = {
        pub(100), pub(200), pub(300, kNan, kMeas), pub(400, 100.0, kMeas),
        pub(500, 100.0, kConv), pub(600, 100.0, kConv)};
    const auto l = latch::measure(pubs, wins({kQuiet, kLoud, kLoud, kLoud, kLoud, kLoud}), {{0, true}}, kNoOracle, params());
    ASSERT_EQ(l.size(), 2u);
    const Latch& b = l[1];
    EXPECT_EQ(b.t0Frames, 100);
    EXPECT_EQ(b.firstReadingFrames, 300);
    EXPECT_DOUBLE_EQ(b.firstReadingSec, 0.3);
    EXPECT_EQ(b.convergedFrames, 400);
    EXPECT_DOUBLE_EQ(b.convergedSec, 0.4);
    EXPECT_EQ(b.readings, 3);
    EXPECT_EQ(b.publications, 5);
}

TEST(LatchInstrument, NoReadingOrNoConvergenceIsMissingNotZero) {
    // Atrapa: devolver 0 donde no hubo lectura (un cero se lee como "instantaneo").
    const std::vector<Publication> pubs = {pub(100), pub(200)};
    const auto l = latch::measure(pubs, loudWins(2), {{0, true}}, kNoOracle, params());
    ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l[0].firstReadingFrames, -1);
    EXPECT_TRUE(std::isnan(l[0].firstReadingSec));
    EXPECT_EQ(l[0].convergedFrames, -1);
    EXPECT_TRUE(std::isnan(l[0].medianAbsCents));
    EXPECT_TRUE(std::isnan(l[0].coverage));
}

TEST(LatchInstrument, OracleSegmentChosenIsTheLatestEndingAtOrBeforeThePublication) {
    // Atrapa: elegir el tramo POSTERIOR a la publicacion (premia un adelanto imposible) o el
    // primero de la lista. Segmentos desordenados a proposito. La publicacion en 250 ms usa el
    // tramo que termino en 200 (110 Hz), no el de 300 (120 Hz); y una publicacion justo en
    // t_fin usa ESE tramo (el limite es inclusivo).
    const std::vector<OracleSegment> oracle = {{0.2, 0.3, 120.0}, {0.0, 0.1, 100.0}, {0.1, 0.2, 110.0}};
    const std::vector<Publication> pubs = {pub(100), pub(200, 110.0, kMeas), pub(250, 110.0, kMeas)};
    const auto l = latch::measure(pubs, loudWins(3), {{0, true}}, oracle, params());
    ASSERT_EQ(l.size(), 1u);
    ASSERT_EQ(l[0].withError, 2);
    EXPECT_NEAR(l[0].maxAbsCents, 0.0, 1e-9) << "contra el tramo previo el error es 0; contra el posterior seria -151 c";
}

TEST(LatchInstrument, ReadingsWithoutOracleAreCountedApartNotAsError) {
    // Atrapa: tomar una lectura sin tramo previo (o con hz NaN) como error 0 o como error enorme.
    // t = 0,1 s: ningun tramo termino todavia. t >= 0,25 s: el tramo previo (0,15-0,25) no tiene hz.
    const std::vector<OracleSegment> oracle = {{0.0, 0.15, 100.0}, {0.15, 0.25, kNan}};
    const std::vector<Publication> pubs = {
        pub(100, 500.0, kMeas),   // sin tramo previo
        pub(300, 500.0, kMeas),   // el tramo previo es NaN
        pub(400, hzAt(100.0, 3.0), kMeas)};   // idem: el de t_fin mas cercano manda, aunque haya uno valido antes
    auto l = latch::measure(pubs, loudWins(4), {{0, true}}, oracle, params());
    ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l[0].readings, 3);
    EXPECT_EQ(l[0].noOracle, 3);
    EXPECT_EQ(l[0].withError, 0);
    EXPECT_TRUE(std::isnan(l[0].maxAbsCents));
    EXPECT_TRUE(std::isnan(l[0].firstErrorCents));

    const std::vector<OracleSegment> oracle2 = {{0.0, 0.15, 100.0}};
    // σ = 2: 2σ = 4 c cubre el error de 3 c. Cubierta 1 de 1 con oraculo; si la lectura sin oraculo entrara al
    // denominador seria 1/2.
    const std::vector<Publication> pubs2 = {pub(100, 500.0, kMeas, 2.0), pub(200, hzAt(100.0, 3.0), kMeas, 2.0)};
    l = latch::measure(pubs2, loudWins(2), {{0, true}}, oracle2, params());
    ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l[0].noOracle, 1);
    EXPECT_EQ(l[0].withError, 1);
    EXPECT_NEAR(l[0].maxAbsCents, 3.0, 1e-9) << "la lectura sin oraculo no entra a los percentiles";
    EXPECT_EQ(l[0].covered, 1);
    EXPECT_NEAR(l[0].coverage, 1.0, 1e-12) << "y tampoco al denominador de la cobertura";
}

TEST(LatchInstrument, ErrorHasThePublishedOverOracleSign) {
    // Atrapa: la razon invertida (oraculo/publicado) o el error de la primera lectura tomado de
    // otra. Publicado +7 c arriba del oraculo = +7; la segunda lectura (−2 c) no pisa la primera.
    const std::vector<OracleSegment> oracle = {{0.0, 0.1, 200.0}};
    const std::vector<Publication> pubs = {pub(100), pub(200, hzAt(200.0, 7.0), kMeas, 3.5),
                                           pub(300, hzAt(200.0, -2.0), kMeas, 0.5)};
    const auto l = latch::measure(pubs, loudWins(3), {{0, true}}, oracle, params());
    ASSERT_EQ(l.size(), 1u);
    EXPECT_NEAR(l[0].firstErrorCents, 7.0, 1e-9);
    EXPECT_DOUBLE_EQ(l[0].firstSigmaCents, 3.5);
}

TEST(LatchInstrument, PercentilesAndCoverageOnAHandMadeCase) {
    // Errores con signo alternado de 1..10 c contra un oraculo fijo, σ = 4,95 (2σ = 9,9).
    // |error|: mediana de 10 valores = (5+6)/2 = 5,5; p90 por rango mas cercano = el 9º = 9; max 10;
    // cobertura = 9/10 (el 10 queda fuera de 2σ).
    // Atrapa: mediana "alta" (6), p90 = max, error con signo en vez de |error| (el max seria 9),
    // y la cobertura contra σ en vez de 2σ (daria 4/10).
    const std::vector<OracleSegment> oracle = {{0.0, 0.05, 100.0}};
    std::vector<Publication> pubs = {pub(100)};
    for (int k = 1; k <= 10; ++k)
        pubs.push_back(pub(100 + 100 * k, hzAt(100.0, (k % 2 ? 1.0 : -1.0) * k), kMeas, 4.95));
    const auto l = latch::measure(pubs, loudWins(11), {{0, true}}, oracle, params());
    ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l[0].withError, 10);
    EXPECT_NEAR(l[0].medianAbsCents, 5.5, 1e-9);
    EXPECT_NEAR(l[0].p90AbsCents, 9.0, 1e-9);
    EXPECT_NEAR(l[0].maxAbsCents, 10.0, 1e-9);
    EXPECT_NEAR(l[0].coverage, 0.9, 1e-12);
    EXPECT_EQ(l[0].covered, 9);
}

TEST(LatchInstrument, ANaNSigmaIsNeverCovered) {
    // Atrapa: tratar la σ NaN como "sin limite" y cubrir todo. Una lectura sin incertidumbre no
    // esta calibrada: no cuenta como cubierta.
    const std::vector<OracleSegment> oracle = {{0.0, 0.05, 100.0}};
    const std::vector<Publication> pubs = {pub(100), pub(200, 100.0, kMeas, kNan), pub(300, 100.0, kMeas, 1.0)};
    const auto l = latch::measure(pubs, loudWins(3), {{0, true}}, oracle, params());
    ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l[0].withError, 2);
    EXPECT_EQ(l[0].covered, 1);
}

TEST(LatchInstrument, PooledStatisticsUseAllPublicationsNotTheMeanOfMedians) {
    // Un enganche con 1 error de 10 c y otro con 3 errores de 1 c: mediana del conjunto = 1, no
    // el promedio de medianas (5,5). Atrapa agregar entre enganches promediando resumenes.
    const std::vector<OracleSegment> oracle = {{0.0, 0.05, 100.0}};
    const std::vector<Publication> pubs = {pub(100, hzAt(100.0, 10.0), kMeas, 1.0), pub(200),
                                           pub(300, hzAt(100.0, 1.0), kMeas, 1.0),
                                           pub(400, hzAt(100.0, -1.0), kMeas, 1.0),
                                           pub(500, hzAt(100.0, 1.0), kMeas, 1.0)};
    const auto l = latch::measure(pubs, loudWins(5), {{0, true}, {250, true}}, oracle, params());
    ASSERT_EQ(l.size(), 2u);
    const auto pooled = latch::poolLatchErrors(l);
    EXPECT_EQ(pooled.withError, 4);
    EXPECT_NEAR(pooled.medianAbsCents, 1.0, 1e-9);
    EXPECT_NEAR(pooled.maxAbsCents, 10.0, 1e-9);
    EXPECT_NEAR(pooled.coverage, 3.0 / 4.0, 1e-12) << "el 10 c queda fuera de 2σ = 2";
}

/// `n` frames de senal constante (0,5 en ambos canales: mono 0,5) o de silencio digital, a mono.
std::vector<float> monoRun(int n, bool loud) { return std::vector<float>(static_cast<size_t>(n), loud ? 0.5f : 0.0f); }

void append(std::vector<float>* dst, const std::vector<float>& src) { dst->insert(dst->end(), src.begin(), src.end()); }

TEST(LatchInstrument, ShortSilenceBeforeTheAttackInsideAWindowOpensNoSecondLatch) {
    // Atrapa: que un silencio digital MAS CORTO que la ventana al comienzo del archivo cuente como
    // ventana silenciosa y abra un enganche vacio ademas del de la nota (lo que pasaba con el rms del
    // bloque drenado de 256 frames: 55 enganches sobre 41 archivos).
    std::vector<float> mono = monoRun(40, false);
    append(&mono, monoRun(260, true));
    const auto levels = latch::windowLevels(mono, kWin);
    ASSERT_EQ(levels.size(), 3u);
    EXPECT_GE(levels[0].rms, 0.001) << "40 frames mudos + 60 con senal: la ventana tiene senal";
    const auto l = latch::measure({pub(150), pub(250)}, levels, {{0, true}}, kNoOracle, params());
    ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l[0].t0Frames, 0);
}

TEST(LatchInstrument, AFullSilentWindowBetweenTwoNotesOpensALatchAtTheNextWindow) {
    // Atrapa: no abrir el enganche (o fecharlo en el final de la ventana) cuando el silencio ocupa una
    // ventana ENTERA de la grilla.
    std::vector<float> mono = monoRun(200, true);
    append(&mono, monoRun(100, false));
    append(&mono, monoRun(200, true));
    const auto levels = latch::windowLevels(mono, kWin);
    const auto l = latch::measure({pub(150), pub(450)}, levels, {{0, true}}, kNoOracle, params());
    ASSERT_EQ(l.size(), 2u);
    EXPECT_EQ(l[1].t0Frames, 300);
    EXPECT_EQ(l[1].publications, 1);
}

TEST(LatchInstrument, SilenceStraddlingTwoWindowsWithoutFillingOneOpensNoLatch) {
    // Atrapa: una grilla desalineada o que mida el silencio por duracion: 120 frames de silencio que
    // cruzan el limite 200 no llenan ninguna ventana de 100 alineada en 100..199 y 200..299.
    std::vector<float> mono = monoRun(140, true);
    append(&mono, monoRun(120, false));
    append(&mono, monoRun(140, true));
    const auto levels = latch::windowLevels(mono, kWin);
    ASSERT_EQ(levels.size(), 4u);
    for (const auto& w : levels) EXPECT_GE(w.rms, 0.001);
    EXPECT_EQ(latch::measure({pub(150)}, levels, {{0, true}}, kNoOracle, params()).size(), 1u);
}

/// Sub-bloque de los tests del nucleo: 20 frames (el de produccion, 256, es la misma regla a otra escala).
constexpr int kSub = 20;

TEST(LatchInstrument, AnAttackMidWindowDatesT0AtItsSubBlockNotAtTheWindowEdge) {
    // Atrapa: fechar t0 en el borde de la ventana con senal. Ventana 0 muda, ventana 1 (100..199) con el ataque
    // en el frame 165: t0 es 160 (inicio del sub-bloque 160..179), no 100. Con t0 = 100 la primera lectura
    // saldria 60 ms mas larga de lo que tardo el motor. La DECISION silencio/senal sigue siendo la ventana.
    std::vector<float> mono = monoRun(165, false);
    append(&mono, monoRun(35, true));
    const auto levels = latch::windowLevels(mono, kWin, kSub);
    ASSERT_EQ(levels.size(), 2u);
    EXPECT_EQ(levels[1].start, 100);
    EXPECT_EQ(levels[1].onset, 160);
    const auto l = latch::measure({pub(100), pub(200, 100.0, kMeas)}, levels, {{0, true}}, kNoOracle, params());
    ASSERT_EQ(l.size(), 2u);
    EXPECT_EQ(l[1].cause, Latch::Cause::kSignalAfterSilence);
    EXPECT_EQ(l[1].t0Frames, 160);
    EXPECT_EQ(l[1].firstReadingFrames, 40);
}

TEST(LatchInstrument, TheOnsetIsTheFirstSubBlockAtTheFloorNotTheFirstLoudFrame) {
    // Atrapa: un onset por muestra (165) en lugar de por sub-bloque de la grilla alineada al frame 0 (160), y
    // un sub-bloque bajo el piso tomado como senal. 0,0005 en 160..179 es silencio; el ataque real llega en 180.
    std::vector<float> mono = monoRun(100, false);
    append(&mono, std::vector<float>(80, 0.0005f));
    append(&mono, monoRun(20, true));
    const auto levels = latch::windowLevels(mono, kWin, kSub);
    ASSERT_EQ(levels.size(), 2u);
    EXPECT_EQ(levels[1].onset, 180);
}

TEST(LatchInstrument, AnEventBetweenTheWindowStartAndTheAttackKeepsTheLatchesOrdered) {
    // Atrapa: un enganche por senal fechado en el ataque (160) que queda ANTES en la lista que un evento
    // anterior (130) y atribuye mal las publicaciones. El evento va primero y su enganche termina en el ataque.
    std::vector<float> mono = monoRun(165, false);
    append(&mono, monoRun(35, true));
    const auto levels = latch::windowLevels(mono, kWin, kSub);
    const auto l = latch::measure({pub(140, 100.0, kMeas), pub(200, 100.0, kMeas)}, levels, {{0, true}, {130, true}},
                                  kNoOracle, params());
    ASSERT_EQ(l.size(), 3u);
    EXPECT_EQ(l[1].t0Frames, 130);
    EXPECT_EQ(l[2].t0Frames, 160);
    EXPECT_EQ(l[1].publications, 1);
    EXPECT_EQ(l[2].publications, 1);
}

TEST(LatchInstrument, FrameRangeKeepsMinMaxOfThoseThatArrivedAndCountsTheRest) {
    // Atrapa: tomar el -1 de "no llego" como el minimo (rango que arranca en -1), perder cuantas no llegaron, o
    // devolver 0 cuando ninguna llego.
    auto r = latch::frameRange({300, -1, 100, 200, -1});
    EXPECT_EQ(r.min, 100);
    EXPECT_EQ(r.max, 300);
    EXPECT_EQ(r.missing, 2);
    r = latch::frameRange({-1, -1});
    EXPECT_EQ(r.min, -1);
    EXPECT_EQ(r.max, -1);
    EXPECT_EQ(r.missing, 2);
    r = latch::frameRange({7});
    EXPECT_EQ(r.min, 7);
    EXPECT_EQ(r.max, 7);
    EXPECT_EQ(r.missing, 0);
}

TEST(LatchInstrument, WindowLevelsIsTheRmsOfCompleteWindowsOnAGridFromFrameZero) {
    // Atrapa: rms sin raiz o dividido por otro largo, una cola incompleta tratada como ventana, o
    // una grilla que no arranca en 0. Una ventana 0,5 constante tiene rms 0,5; mitad 0,5 y mitad 0 =
    // 0,5/sqrt(2).
    std::vector<float> mono = monoRun(100, true);
    append(&mono, monoRun(50, true));
    append(&mono, monoRun(50, false));
    append(&mono, monoRun(30, true));
    const auto levels = latch::windowLevels(mono, kWin);
    ASSERT_EQ(levels.size(), 2u) << "los 30 frames de cola no son una ventana";
    EXPECT_EQ(levels[0].start, 0);
    EXPECT_NEAR(levels[0].rms, 0.5, 1e-9);
    EXPECT_EQ(levels[1].start, 100);
    EXPECT_NEAR(levels[1].rms, 0.5 / std::sqrt(2.0), 1e-6);
}

// =====================================================================================
// Formato y comparacion de la linea de base (puros: se prueban sin corpus)
// =====================================================================================

struct Col {
    const char* name;
    bool exact;   ///< frames y conteos: EXACTOS; el resto, con tolerancia
};
constexpr Col kCols[] = {
    {"enganche", true},        {"t0_frames", true},       {"t_primera_frames", true}, {"t_primera_s", false},
    {"t_conv_frames", true},   {"t_conv_s", false},       {"n_lecturas", true},       {"n_sin_oraculo", true},
    {"err_primera_c", false},  {"sigma_primera_c", false}, {"mediana_abs_c", false},  {"p90_abs_c", false},
    {"max_abs_c", false},      {"cobertura", false},      {"t_primera2_frames", true}, {"t_primera2_s", false},
    {"t_conv2_frames", true},  {"t_conv2_s", false},      {"sueltas2", true},
    {"t_primera_min_frames", true}, {"t_primera_max_frames", true}, {"t_primera_sin_lectura", true},
    {"t_conv_min_frames", true},    {"t_conv_max_frames", true},    {"t_conv_sin_lectura", true}};
constexpr size_t kNumCols = sizeof(kCols) / sizeof(kCols[0]);
constexpr double kTolerance = 0.002;

struct Row {
    std::string name;
    double v[kNumCols];
};

std::string keyOf(const std::string& name, double engage) {
    return name + "#" + std::to_string(static_cast<int>(engage));
}

std::string formatNumber(double x, bool exact) {
    if (std::isnan(x)) return "nan";
    char buf[64];
    std::snprintf(buf, sizeof(buf), exact ? "%.0f" : "%.3f", x);
    return buf;
}

std::string formatRow(const Row& r) {
    std::string out = r.name;
    for (size_t i = 0; i < kNumCols; ++i) out += "\t" + formatNumber(r.v[i], kCols[i].exact);
    return out;
}

/// Lee las filas de datos (las `#` se saltean). false si una fila no tiene sus columnas.
bool parseBaseline(const std::string& text, std::map<std::string, Row>* out) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty() || line[0] == '#') continue;
        Row r{};
        size_t cur = 0;
        size_t tab = line.find('\t');
        if (tab == std::string::npos) return false;
        r.name = line.substr(0, tab);
        cur = tab + 1;
        for (size_t i = 0; i < kNumCols; ++i) {
            const size_t next = line.find('\t', cur);
            const std::string cell = line.substr(cur, next == std::string::npos ? std::string::npos : next - cur);
            if (cell.empty()) return false;
            r.v[i] = std::strtod(cell.c_str(), nullptr);
            if ((i + 1 < kNumCols) != (next != std::string::npos)) return false;
            cur = next + 1;
        }
        (*out)[keyOf(r.name, r.v[0])] = r;
    }
    return true;
}

bool sameValue(double want, double got, bool exact) {
    if (std::isnan(want) || std::isnan(got)) return std::isnan(want) && std::isnan(got);
    return exact ? want == got : std::fabs(want - got) <= kTolerance;
}

/// Una linea por diferencia. Vacio = coincide.
std::vector<std::string> diffRows(const std::map<std::string, Row>& want, const std::map<std::string, Row>& got,
                                  bool requireSameSet) {
    std::vector<std::string> out;
    for (const auto& [key, g] : got) {
        const auto it = want.find(key);
        if (it == want.end()) {
            out.push_back(key + ": no esta en la linea de base");
            continue;
        }
        for (size_t i = 0; i < kNumCols; ++i) {
            if (!sameValue(it->second.v[i], g.v[i], kCols[i].exact))
                out.push_back(key + " " + kCols[i].name + ": base " + formatNumber(it->second.v[i], kCols[i].exact) +
                              ", ahora " + formatNumber(g.v[i], kCols[i].exact));
        }
    }
    if (requireSameSet)
        for (const auto& kv : want)
            if (got.find(kv.first) == got.end()) out.push_back(kv.first + ": esta en la linea de base y no se midio");
    return out;
}

Row sampleRow() {
    Row r{};
    r.name = "a.wav";
    const double base[kNumCols] = {1, 0, 4096, 0.093, 8192, 0.186, 30, 2, 1.5, 0.4, 0.8, 2.1, 3.3, 0.9, 100, 0.002, 200, 0.004, 0, 4096, 4352, 0, 8192, 8448, 1};
    for (size_t i = 0; i < kNumCols; ++i) r.v[i] = base[i];
    return r;
}

TEST(LatchInstrument, BaselineComparisonIsExactOnFramesAndTolerantOnCents) {
    // Atrapa: una comparacion con tolerancia en los frames (un cambio de un solo tick pasaria
    // inadvertido), una sin tolerancia en los cents (ruido de libm entre plataformas), y el NaN
    // tratado como distinto de si mismo (rojo permanente) o como igual a un numero.
    std::map<std::string, Row> want;
    want[keyOf("a.wav", 1)] = sampleRow();
    Row got = sampleRow();
    std::map<std::string, Row> gotMap;
    gotMap[keyOf("a.wav", 1)] = got;
    EXPECT_TRUE(diffRows(want, gotMap, true).empty());

    got.v[2] += 1;   // un frame de t_primera
    gotMap[keyOf("a.wav", 1)] = got;
    EXPECT_EQ(diffRows(want, gotMap, true).size(), 1u);

    got = sampleRow();
    got.v[10] += 0.0015;   // mediana dentro de la tolerancia
    gotMap[keyOf("a.wav", 1)] = got;
    EXPECT_TRUE(diffRows(want, gotMap, true).empty());
    got.v[10] += 0.001;    // 0,0025 en total: fuera
    gotMap[keyOf("a.wav", 1)] = got;
    EXPECT_EQ(diffRows(want, gotMap, true).size(), 1u);

    got = sampleRow();
    got.v[4] = kNan;       // t_conv deja de llegar
    gotMap[keyOf("a.wav", 1)] = got;
    EXPECT_EQ(diffRows(want, gotMap, true).size(), 1u);
    want[keyOf("a.wav", 1)].v[4] = kNan;
    EXPECT_TRUE(diffRows(want, gotMap, true).empty()) << "NaN contra NaN coincide";
}

TEST(LatchInstrument, BaselineComparisonFlagsMissingAndExtraRows) {
    // Atrapa: una fila que desaparece de la base (o aparece sin base) y no hace ruido.
    std::map<std::string, Row> want, got;
    want[keyOf("a.wav", 1)] = sampleRow();
    EXPECT_EQ(diffRows(want, got, true).size(), 1u);
    EXPECT_TRUE(diffRows(want, got, false).empty()) << "un subconjunto medido (sanitizer) no exige el resto";
    got[keyOf("a.wav", 1)] = sampleRow();
    got[keyOf("b.wav", 1)] = sampleRow();
    EXPECT_EQ(diffRows(want, got, true).size(), 1u);
}

TEST(LatchInstrument, BaselineRowsRoundTripThroughTheirTextForm) {
    // Atrapa: una columna que se escribe y no se lee (o al reves) y desplaza todas las siguientes.
    Row r = sampleRow();
    r.v[6] = kNan;
    std::map<std::string, Row> parsed;
    ASSERT_TRUE(parseBaseline("# cabecera\n" + formatRow(r) + "\n", &parsed));
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_TRUE(diffRows(parsed, {{keyOf("a.wav", 1), r}}, true).empty());
    EXPECT_FALSE(parseBaseline("a.wav\t1\t2\n", &parsed)) << "una fila corta es un error, no se rellena";
}

// =====================================================================================
// El corrido sobre corpus-v1
// =====================================================================================

std::string oraclePath() { return std::string(WMA_ANALYSIS_GOLDEN_DIR) + "/../corpus-v1-oraculo-corto.tsv"; }
std::string baselinePath() { return std::string(WMA_ANALYSIS_GOLDEN_DIR) + "/../baseline/enganche-corpus-v1.tsv"; }

std::string slurp(const std::string& path, bool* ok) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    *ok = f != nullptr;
    std::string out;
    if (f == nullptr) return out;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

/// sha256 por archivo, del manifiesto (que `entriesOf` descarta).
std::map<std::string, std::string> manifestShas(const std::string& manifest) {
    std::map<std::string, std::string> out;
    bool ok = false;
    const std::string text = slurp(manifest, &ok);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty() || line[0] == '#') continue;
        char name[512] = {0}, sha[256] = {0};
        if (std::sscanf(line.c_str(), "%511s %255s", name, sha) == 2) out[name] = sha;
    }
    return out;
}

struct OracleFile {
    std::string sha;
    int rate = 0;
    long long frames = 0;
    std::vector<OracleSegment> segments;
    /// Los ataques del archivo, en frames, como los detecto el ORACULO (`# ataque NOMBRE frame N`).
    std::vector<int64_t> attacks;
};

/// El oraculo corto: `# archivo NOMBRE sha256 HEX rate HZ frames N` antes de las filas de cada
/// archivo, sus ataques como `# ataque NOMBRE frame N` (REQ-055 D11), y por fila `nombre t_ini t_fin hz
/// cents nivel`. Devuelve el motivo si no se pudo leer.
std::string loadOracle(const std::string& path, std::map<std::string, OracleFile>* out) {
    bool ok = false;
    const std::string text = slurp(path, &ok);
    if (!ok) return "no existe " + path;
    std::string current;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty()) continue;
        if (line[0] == '#') {
            char name[512] = {0}, sha[256] = {0};
            int rate = 0;
            long long frames = 0;
            if (std::sscanf(line.c_str(), "# archivo %511s sha256 %255s rate %d frames %lld", name, sha, &rate, &frames) == 4) {
                current = name;
                OracleFile& f = (*out)[current];
                f.sha = sha;
                f.rate = rate;
                f.frames = frames;
            } else if (std::sscanf(line.c_str(), "# ataque %511s frame %lld", name, &frames) == 2) {
                if (current.empty() || current != name) return std::string("ataque de ") + name + " fuera de su cabecera";
                (*out)[current].attacks.push_back(static_cast<int64_t>(frames));
            }
            continue;
        }
        char name[512] = {0};
        double tIni = 0, tFin = 0, hz = 0;
        if (std::sscanf(line.c_str(), "%511s %lf %lf %lf", name, &tIni, &tFin, &hz) != 4) return "fila ilegible: " + line;
        if (current.empty() || current != name) return std::string("fila de ") + name + " fuera de su cabecera";
        (*out)[current].segments.push_back({tIni, tFin, hz});
    }
    return "";
}

std::string missingCorpusMessage(corpus::State st) {
    return std::string("sin corpus grabado (") + corpus::describe(st) + "). Se baja con: bash scripts/fetch-corpus.sh — hasta "
           "entonces esto NO cuenta como cobertura";
}

/// La corrida de UN archivo: la NOTA alimentada DOS veces seguidas sin tocar el objetivo, en tandas
/// de `chunkFrames` drenadas hasta ring vacio, con cada publicacion registrada. La primera pasada es la que se mide por enganche; la segunda, el
/// re-punteo, que por definicion no abre enganche nuevo.
struct FedRun {
    int rate = 0;
    int64_t frames1 = 0;   ///< donde termina la primera pasada (= donde empieza la segunda)
    std::vector<Publication> pubs;
    std::vector<TargetEvent> events;
    /// rms por ventana fija de la senal alimentada (las DOS pasadas, en una sola grilla desde 0).
    std::vector<latch::Window> windows;
};

// 🔴 EL TAMANO DE TANDA ES PARTE DE LO QUE SE MIDE, Y EL MOTOR DE HOY DEPENDE DE LA PARTICION DEL FLUJO.
// La cifra canonica es la del motor alimentado en tandas de 256 (determinista, comparable); la linea de
// base trae ademas el RANGO sobre `kRangeChunks`. Medido con el motor del commit 1351b7e: con tandas que
// dividen a 4096 (64..4096) los 41 archivos dan la misma posicion de ring, pero con 96/192/240/480/960/1536
// (bursts tipicos de Android) t_primera se mueve en 37-40 de los 41 (bajo-fretless_D2: 2,51 s con 256 contra
// ~1,21 s con otra; bajo-pua_A1: 4,64 contra ~4,09 s) y algun archivo converge con una tanda y no con otra.
// Es una propiedad del motor, no del instrumento: lo que se mide queda declarado en el TSV y cualquier
// cambio del motor (S1/S2) aparece en su diff. Escribir el ring entero (8192 frames) antes de drenar
// ademas hace que el modo rapido, al re-enganchar, tire el resto de la tanda: no es una tanda de captura.
constexpr int kProductionChunkFrames = 256;
constexpr int kRangeChunks[] = {96, 192, 240, kProductionChunkFrames, 480, 960, 1536};
constexpr size_t kNumRangeChunks = sizeof(kRangeChunks) / sizeof(kRangeChunks[0]);

FedRun feedTwice(const wav::WavData& data, const corpus::Entry& e, int chunkFrames = kProductionChunkFrames) {
    FedRun run;
    run.rate = data.sampleRate;
    const int frames = corpus::noteEndFrames(data);
    // El flujo mono con que se miden las ventanas se arma tanda a tanda, a medida que se escribe al ring:
    // si el alimentador perdiera o duplicara frames al partir, la serie de ventanas cambiaria con la tanda.
    std::vector<float> fedMono;
    wma::analysis::AnalysisRing ring;
    wma::analysis::AnalysisSnapshot snapshot;
    wma::analysis::AnalysisThread analysis(ring, snapshot);
    ring.setCaptureRate(data.sampleRate);
    analysis.setTargetHz(e.trueHz);
    run.events.push_back({static_cast<int64_t>(ring.readPosition()), true});
    analysis.setCandidates(e.candidatesHz.empty() ? nullptr : e.candidatesHz.data(),
                           static_cast<int>(e.candidatesHz.size()));
    // 🔴 EL OBJETIVO SE APLICA CON EL RING VACIO, ANTES DE ESCRIBIR. El primer `drainOnce()` tras
    // `setTargetHz` hace `skipToNewest()` (lo que hubiera en el ring es de la cuerda anterior): si
    // ya se escribio el primer bloque, lo tira ENTERO. `sweepFile` y el scratch de REQ-044 lo hacian
    // —escriben 8192 frames y recien despues drenan— y el motor nunca veia los primeros 0,186 s de
    // cada nota: justo el ataque, que es lo que esta vara mide. Con el ring vacio el salto no
    // descarta nada, y es la situacion de la definicion: "al empezar a alimentar con objetivo
    // puesto y ring vacio".
    (void)analysis.drainOnce();
    for (int pass = 0; pass < 2; ++pass) {
        int written = 0;
        while (written < frames) {
            const int chunk = std::min(frames - written, chunkFrames);
            const float* src = data.buffer.data() + static_cast<size_t>(written) * 2;
            latch::appendMonoMix(&fedMono, src, chunk);
            ring.writeStereo(src, chunk);
            written += chunk;
            for (;;) {
                if (analysis.drainOnce() == wma::analysis::AnalysisThread::DrainOutcome::kRingEmpty) break;
                float v[wma::analysis::kSnapshotValueCount];
                if (!snapshot.read(v)) continue;
                Publication p;
                p.pos = static_cast<int64_t>(ring.readPosition());
                p.state = static_cast<int>(v[wma::analysis::kSnapState]);
                p.sigmaCents = static_cast<double>(v[wma::analysis::kSnapUncertainty]);
                const float cents = v[wma::analysis::kSnapCents];
                // kSnapCents es relativo al OBJETIVO del strobe, que el modo rapido puede mover:
                // la altura absoluta sale del objetivo de ESTA publicacion.
                if (!std::isnan(cents))
                    p.hz = analysis.strobe().targetHz() * std::pow(2.0, static_cast<double>(cents) / 1200.0);
                run.pubs.push_back(p);
            }
        }
        if (pass == 0) run.frames1 = static_cast<int64_t>(ring.readPosition());
    }
    run.windows = latch::windowLevels(fedMono, latch::kLatchWindowFrames);
    return run;
}

/// Lo de la PRIMERA pasada: las publicaciones hasta `frames1` y las ventanas que empiezan antes.
struct FirstPassOfRun {
    std::vector<Publication> pubs;
    std::vector<latch::Window> windows;
};

FirstPassOfRun firstPassOfRun(const FedRun& run) {
    FirstPassOfRun out;
    for (const Publication& p : run.pubs)
        if (p.pos <= run.frames1) out.pubs.push_back(p);
    for (const latch::Window& w : run.windows)
        if (w.start < run.frames1) out.windows.push_back(w);
    return out;
}

struct Pass2 {
    int64_t firstFrames = -1;
    int64_t convFrames = -1;
    int dropped = 0;   ///< veces que la lectura fina paso de presente a NaN durante la segunda pasada
};

/// El re-punteo, medido desde el inicio de la segunda pasada. `dropped` arranca con la ultima
/// lectura de la primera: si el enganche se solto justo en el segundo ataque, cuenta.
Pass2 measurePass2(const FedRun& run) {
    Pass2 out;
    bool hadReading = false;
    for (const Publication& p : run.pubs) {
        if (p.pos <= run.frames1) {
            hadReading = !std::isnan(p.hz);
            continue;
        }
        const bool reading = !std::isnan(p.hz);
        if (reading && out.firstFrames < 0) out.firstFrames = p.pos - run.frames1;
        if (p.state == wma::analysis::kStateConverged && out.convFrames < 0) out.convFrames = p.pos - run.frames1;
        if (!reading && hadReading) ++out.dropped;
        hadReading = reading;
    }
    return out;
}

double secOf(int64_t frames, int rate) { return frames < 0 ? kNan : static_cast<double>(frames) / rate; }
double framesOrNan(int64_t frames) { return frames < 0 ? kNan : static_cast<double>(frames); }

Row rowOf(const std::string& name, int engage, const Latch& l, const Pass2& p2, int rate, const latch::FrameRange& first,
          const latch::FrameRange& conv) {
    Row r{};
    r.name = name;
    const double v[kNumCols] = {static_cast<double>(engage),
                                static_cast<double>(l.t0Frames),
                                framesOrNan(l.firstReadingFrames),
                                secOf(l.firstReadingFrames, rate),
                                framesOrNan(l.convergedFrames),
                                secOf(l.convergedFrames, rate),
                                static_cast<double>(l.readings),
                                static_cast<double>(l.noOracle),
                                l.firstErrorCents,
                                l.firstSigmaCents,
                                l.medianAbsCents,
                                l.p90AbsCents,
                                l.maxAbsCents,
                                l.coverage,
                                framesOrNan(p2.firstFrames),
                                secOf(p2.firstFrames, rate),
                                framesOrNan(p2.convFrames),
                                secOf(p2.convFrames, rate),
                                static_cast<double>(p2.dropped),
                                framesOrNan(first.min),
                                framesOrNan(first.max),
                                static_cast<double>(first.missing),
                                framesOrNan(conv.min),
                                framesOrNan(conv.max),
                                static_cast<double>(conv.missing)};
    for (size_t i = 0; i < kNumCols; ++i) r.v[i] = v[i];
    return r;
}

std::string baselineHeader() {
    std::string h =
        "# Linea de base del ENGANCHE del afinador sobre corpus-v1 (REQ-055 S0). Una fila por archivo y enganche.\n"
        "# Motor medido: commit 1351b7e (el de hoy, sin cambio de produccion). Al regenerar tras un cambio de motor\n"
        "# INTENCIONAL, actualizar este commit en `baselineHeader()` (test_latch_instrument.cpp).\n"
        "#\n"
        "# 🔴 ESTA CIFRA ES DEL MOTOR ALIMENTADO EN TANDAS DE 256 FRAMES, Y EL MOTOR DE HOY DEPENDE DE LA PARTICION DEL\n"
        "# FLUJO. Con tandas que dividen a 4096 (64..4096) los 41 archivos dan la misma posicion de ring; con tandas\n"
        "# que no (96, 192, 240, 480, 960, 1536: bursts tipicos de Android) t_primera se mueve en 37-40 de 41 archivos\n"
        "# (bajo-fretless_D2: 2,51 s con 256 contra ~1,21 s con otra; bajo-pua_A1: 4,64 contra ~4,09 s) y algun archivo\n"
        "# converge con una tanda y no con otra. Por eso la fila trae el RANGO sobre el conjunto fijo de tandas\n"
        "# {96, 192, 240, 256, 480, 960, 1536}: t_*_min_frames / t_*_max_frames son el minimo y el maximo de t_primera /\n"
        "# t_conv (frames desde t0) sobre las tandas donde LLEGO; t_*_sin_lectura, en cuantas de las 7 no llego (si no\n"
        "# llego en ninguna, min y max son nan). 256 esta en el conjunto: min <= t_*_frames <= max cuando llega. El rango\n"
        "# se compara EXACTO, como el resto: cualquier cambio del motor que mueva la dependencia de la tanda aparece\n"
        "# en el diff. Es una propiedad del motor; este instrumento la mide y la declara, no la arregla.\n"
        "#\n"
        "# Definicion de enganche (acordada con el consumidor). La ventana es FIJA: 4096 frames (la del estimador fino)\n"
        "# sobre la senal mono alimentada 0,5*(L+R), en una grilla alineada al frame 0 del flujo e independiente de la\n"
        "# tanda; la DECISION silencio/senal se toma con esa ventana (antes se usaba el rms del bloque drenado y un\n"
        "# bloque de silencio digital al comienzo de cada pasada abria enganches espurios: 55 sobre 41 archivos).\n"
        "# t_reenganche = cambio de objetivo (el frame exacto del evento), o (con objetivo puesto) el ATAQUE de la\n"
        "# primera ventana con rms >= 0,001 que sigue a una ventana con rms < 0,001, fechado en el inicio del primer\n"
        "# SUB-BLOQUE de 256 frames (grilla desde el frame 0) con rms >= 0,001 dentro de esa ventana, no en el borde de\n"
        "# la ventana; al empezar a alimentar con objetivo puesto, la primera ventana con senal (sin ventana previa)\n"
        "# es t_reenganche. El enganche dura hasta el siguiente t_reenganche. Un re-punteo con la cuerda sonando NO es\n"
        "# un enganche (no hay ventana < 0,001 en el medio).\n"
        "#\n"
        "# Se alimenta la NOTA (noteEndFrames) por AnalysisThread::drainOnce() sin thread, en TANDAS DE 256 FRAMES\n"
        "# (kProductionChunkFrames) drenadas hasta ring vacio tras cada una, como un callback de captura: con tandas de\n"
        "# 8192 el modo rapido tiraba el resto de la tanda al re-enganchar y la primera lectura salia hasta 93 ms tarde\n"
        "# (la primera linea de base, descartada). Con setTargetHz(hz del manifiesto) y los candidatos del\n"
        "# instrumento, aplicados con el ring VACIO (un drainOnce antes de escribir: si no, skipToNewest tira el primer\n"
        "# bloque, el ataque, como hacia sweepFile). Tiempos en frames del ring (exactos) y en segundos; errores en\n"
        "# cents contra el ORACULO INSTANTANEO corpus-v1-oraculo-corto.tsv (tramo con t_fin mas cercano <= t de la\n"
        "# publicacion); cobertura = fraccion de lecturas con oraculo y |error| <= 2 sigma. n_lecturas = publicaciones\n"
        "# con cents; n_sin_oraculo = de esas, las que no tuvieron tramo con hz. Las columnas *2 son el RE-PUNTEO: la\n"
        "# misma nota alimentada una segunda vez sin tocar el objetivo, medida desde el segundo ataque; sueltas2 = veces\n"
        "# que la lectura fina volvio a NaN en esa pasada. No abre enganche nuevo. `nan` = no llego / no aplica.\n"
        "#\n"
        "# EL DIFF DE ESTE ARCHIVO ES LA REVISION. Se compara con frames exactos y tolerancia 0,002 en lo demas.\n"
        "# Regenerar (corre la suite y ESCRIBE en vez de comparar; el test queda SKIPPED):\n"
        "#   WMA_LATCH_BASELINE_UPDATE=1 bash scripts/run-cpp-tests.sh -R LatchInstrumentCorpus\n"
        "# archivo";
    for (const Col& c : kCols) h += std::string("\t") + c.name;
    return h + "\n";
}

double medianOf(std::vector<double> v) { return latch::median(std::move(v)); }
double maxOf(const std::vector<double>& v) { return v.empty() ? kNan : *std::max_element(v.begin(), v.end()); }

TEST(LatchInstrumentCorpus, OracleDescribesTheSameFilesAsTheManifest) {
    // Atrapa: un oraculo regenerado sobre OTRO archivo (otra toma, otro render) bajo el mismo
    // nombre: sus tramos no describirian el audio que se alimenta. Se ata por sha256, no por nombre.
    const corpus::State st = corpus::stateOf(corpus::defaultCorpusDir(), corpus::manifestPath());
    if (st != corpus::State::kVerified) GTEST_SKIP() << missingCorpusMessage(st);

    std::map<std::string, OracleFile> oracle;
    const std::string why = loadOracle(oraclePath(), &oracle);
    ASSERT_EQ(why, "") << "el corpus esta y el oraculo corto no: regenerarlo con scripts/corpus-reference-pitch.py --short "
                          "(ver analysis/CLAUDE.md). Con corpus presente esto FALLA, no se saltea";
    const auto shas = manifestShas(corpus::manifestPath());
    int checked = 0;
    for (const corpus::Entry& e : corpus::entriesOf(corpus::manifestPath())) {
        const auto o = oracle.find(e.name);
        ASSERT_NE(o, oracle.end()) << e.name << " no esta en el oraculo corto";
        EXPECT_EQ(o->second.sha, shas.at(e.name)) << e.name << ": el oraculo describe otro archivo que el del manifiesto";
        EXPECT_FALSE(o->second.segments.empty()) << e.name;
        ++checked;
    }
    EXPECT_GT(checked, 0);
}

/// Una nota sintetica: silencio hasta `attackFrame` y despues un tono de 440 Hz. `noteEndFrames` la corta
/// en un multiplo de 2048, asi que sobra cola muda para que haya al menos dos ventanas completas.
wav::WavData syntheticNote(int attackFrame, int totalFrames) {
    wav::WavData d;
    d.sampleRate = 48000;
    d.numChannels = 2;
    d.numFrames = totalFrames;
    d.buffer.assign(static_cast<size_t>(totalFrames) * 2, 0.0f);
    for (int i = attackFrame; i < totalFrames; ++i) {
        const float v = 0.3f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 440.0 * i / d.sampleRate));
        d.buffer[static_cast<size_t>(i) * 2] = v;
        d.buffer[static_cast<size_t>(i) * 2 + 1] = v;
    }
    return d;
}

bool sameWindows(const std::vector<latch::Window>& a, const std::vector<latch::Window>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].start != b[i].start || a[i].rms != b[i].rms || a[i].onset != b[i].onset) return false;
    return true;
}

TEST(LatchInstrument, TheWindowsTheFeederBuildsDoNotDependOnTheChunkSize) {
    // Atrapa: un alimentador que pierde, duplica o parte mal el flujo al escribirlo en tandas (la cola de la
    // ultima tanda, p. ej.), de modo que la serie de ventanas, y con ella los t0, cambie con la tanda. Las
    // tandas incluyen una que NO divide a 4096 (960) y otra que no divide ni a 2048 (240). Con el ataque en el
    // frame 8000 (a mitad de la ventana 4096..8191) t0 sale 7936 con todas.
    corpus::Entry e;
    e.trueHz = 440.0;
    const wav::WavData note = syntheticNote(8000, 40960 + 4096);
    const FedRun ref = feedTwice(note, e, kProductionChunkFrames);
    ASSERT_GE(ref.windows.size(), 4u);
    EXPECT_EQ(ref.windows[1].start, 4096);
    EXPECT_EQ(ref.windows[1].onset, 7936);
    for (int chunk : {64, 240, 960, 2048}) {
        const FedRun other = feedTwice(note, e, chunk);
        EXPECT_TRUE(sameWindows(ref.windows, other.windows)) << "tanda " << chunk;
        EXPECT_EQ(ref.frames1, other.frames1) << "tanda " << chunk;
    }
    Params prm;
    prm.sampleRate = ref.rate;
    const FirstPassOfRun first = firstPassOfRun(ref);
    const auto l = latch::measure(first.pubs, first.windows, ref.events, kNoOracle, prm);
    ASSERT_EQ(l.size(), 2u) << "el evento del objetivo (silencio) y el ataque";
    EXPECT_EQ(l[1].t0Frames, 7936) << "t0 en el sub-bloque del ataque, no en el borde 4096 de la ventana";
}

/// Posiciones ABSOLUTAS del ring (t0 + t_primera, t0 + t_conv) de la primera lectura y de la primera
/// convergencia de la PRIMERA pasada, alimentada en tandas de `chunkFrames`; −1 si no llegaron.
struct AbsoluteLatchTimes {
    int64_t firstFrames = -1;
    int64_t convFrames = -1;
    size_t latchCount = 0;
};

/// Un archivo alimentado en UNA tanda: la corrida y los enganches de su primera pasada.
struct ChunkRun {
    int chunk = 0;
    FedRun run;
    std::vector<Latch> latches;
};

ChunkRun runChunk(const wav::WavData& data, const corpus::Entry& e, const std::vector<OracleSegment>& segments, int chunk) {
    ChunkRun out;
    out.chunk = chunk;
    out.run = feedTwice(data, e, chunk);
    const FirstPassOfRun first = firstPassOfRun(out.run);
    Params prm;
    prm.sampleRate = out.run.rate;
    out.latches = latch::measure(first.pubs, first.windows, out.run.events, segments, prm);
    return out;
}

/// El archivo en cada tanda de `kRangeChunks`, una por thread: cada corrida tiene su propio ring, snapshot y
/// motor, y nada se comparte mas que los datos de entrada, de solo lectura. Los resultados se leen despues del join.
std::vector<ChunkRun> runAllRangeChunks(const wav::WavData& data, const corpus::Entry& e,
                                        const std::vector<OracleSegment>& segments) {
    std::vector<ChunkRun> out(kNumRangeChunks);
    std::vector<std::thread> workers;
    for (size_t i = 0; i < kNumRangeChunks; ++i)
        workers.emplace_back([&, i] { out[i] = runChunk(data, e, segments, kRangeChunks[i]); });
    for (std::thread& t : workers) t.join();
    return out;
}

size_t canonicalIndex() {
    for (size_t i = 0; i < kNumRangeChunks; ++i)
        if (kRangeChunks[i] == kProductionChunkFrames) return i;
    return 0;
}

/// Para el enganche `idx`, el rango de t_primera / t_conv sobre las tandas.
std::pair<latch::FrameRange, latch::FrameRange> rangesOf(const std::vector<ChunkRun>& runs, size_t idx) {
    std::vector<int64_t> first, conv;
    for (const ChunkRun& r : runs) {
        first.push_back(r.latches[idx].firstReadingFrames);
        conv.push_back(r.latches[idx].convergedFrames);
    }
    return {latch::frameRange(first), latch::frameRange(conv)};
}

AbsoluteLatchTimes absoluteTimesAtChunk(const wav::WavData& data, const corpus::Entry& e,
                                        const std::vector<OracleSegment>& segments, int chunkFrames) {
    const ChunkRun r = runChunk(data, e, segments, chunkFrames);
    AbsoluteLatchTimes out;
    out.latchCount = r.latches.size();
    for (const Latch& l : r.latches) {
        if (l.firstReadingFrames >= 0 && (out.firstFrames < 0 || l.t0Frames + l.firstReadingFrames < out.firstFrames))
            out.firstFrames = l.t0Frames + l.firstReadingFrames;
        if (l.convergedFrames >= 0 && (out.convFrames < 0 || l.t0Frames + l.convergedFrames < out.convFrames))
            out.convFrames = l.t0Frames + l.convergedFrames;
    }
    return out;
}

/// Bajo sanitizer cada archivo cuesta varias veces mas: se mide 1 de cada 4; el conjunto completo lo exigen
/// los jobs sin sanitizer.
std::vector<corpus::Entry> corpusEntriesForThisBuild(bool* subset) {
    std::vector<corpus::Entry> entries = corpus::entriesOf(corpus::manifestPath());
#ifdef WMA_TEST_UNDER_SANITIZER
    *subset = true;
    std::vector<corpus::Entry> kept;
    for (size_t i = 0; i < entries.size(); i += 4) kept.push_back(entries[i]);
    return kept;
#else
    *subset = false;
    return entries;
#endif
}

TEST(LatchInstrumentCorpus, ChunkSizesThatDivideTheWindowDoNotMoveTheMeasurement) {
    // Atrapa: un instrumento que mide el arnes y no el motor. Con tandas que dividen a 4096 (256 y 1024) la
    // posicion de ring de la primera lectura y de la primera convergencia, y la cantidad de enganches, es la
    // MISMA en los 41 archivos (EXACTA). Se mide asi, no esta demostrado: la hipotesis es que el motor
    // publica en multiplos de su ventana y una tanda que la divide cae justo en ellos. Vale SOLO para
    // divisores de 4096: con 96/192/240/480/960/1536 el motor SI se mueve, y eso no se afirma aca sino que lo
    // fija el rango de la linea de base y lo imprime `MeasuresTheLatchesAgainstTheCommittedBaseline`.
    const std::string dir = corpus::defaultCorpusDir();
    const corpus::State st = corpus::stateOf(dir, corpus::manifestPath());
    if (st != corpus::State::kVerified) GTEST_SKIP() << missingCorpusMessage(st);
    std::map<std::string, OracleFile> oracle;
    ASSERT_EQ(loadOracle(oraclePath(), &oracle), "");

    bool subset = false;
    const std::vector<corpus::Entry> entries = corpusEntriesForThisBuild(&subset);
    ASSERT_EQ(4096 % kProductionChunkFrames, 0);
    ASSERT_EQ(4096 % 1024, 0);
    for (const corpus::Entry& e : entries) {
        const wav::WavData data = wav::readWav((dir + "/" + e.name).c_str());
        ASSERT_GT(data.sampleRate, 0) << e.name;
        const auto& segs = oracle.at(e.name).segments;
        const AbsoluteLatchTimes small = absoluteTimesAtChunk(data, e, segs, kProductionChunkFrames);
        const AbsoluteLatchTimes wide = absoluteTimesAtChunk(data, e, segs, 1024);
        EXPECT_EQ(small.latchCount, wide.latchCount) << e.name << ": la cantidad de enganches no depende de la tanda";
        EXPECT_EQ(small.firstFrames, wide.firstFrames) << e.name << ": primera lectura, 256 vs 1024";
        EXPECT_EQ(small.convFrames, wide.convFrames) << e.name << ": primera convergencia, 256 vs 1024";
    }
}

TEST(LatchInstrumentCorpus, MeasuresTheLatchesAgainstTheCommittedBaseline) {
    const std::string dir = corpus::defaultCorpusDir();
    const corpus::State st = corpus::stateOf(dir, corpus::manifestPath());
    if (st != corpus::State::kVerified) GTEST_SKIP() << missingCorpusMessage(st);

    std::map<std::string, OracleFile> oracle;
    const std::string why = loadOracle(oraclePath(), &oracle);
    ASSERT_EQ(why, "") << "el corpus esta y el oraculo corto no: esto FALLA, no se saltea";

    bool kSubset = false;
    const std::vector<corpus::Entry> entries = corpusEntriesForThisBuild(&kSubset);

    std::map<std::string, Row> rows;
    std::vector<Latch> allLatches;
    std::vector<double> tFirst, tConv, tFirst2, tFirstWorst, tConvWorst;
    int filesMoved = 0, filesMovedFirst = 0;
    double widestSpreadSec = 0.0;
    std::string widestName;
    std::printf("\n%-24s %7s %2s | %7s %7s %5s %7s %7s %7s %7s %5s | %7s %7s %3s | %7s %7s\n", "archivo", "hz", "n", "t_prim",
                "t_conv", "lect", "e_prim", "med|e|", "p90|e|", "max|e|", "cob", "t_prim2", "t_conv2", "sue", "prim_mx", "conv_mx");
    for (const corpus::Entry& e : entries) {
        const wav::WavData data = wav::readWav((dir + "/" + e.name).c_str());
        ASSERT_GT(data.sampleRate, 0) << e.name;
        const OracleFile& of = oracle.at(e.name);
        // El oraculo tiene que describir ESTE audio: mismo rate y mismo largo.
        ASSERT_EQ(of.rate, data.sampleRate) << e.name;
        ASSERT_EQ(of.frames, static_cast<long long>(data.numFrames)) << e.name;

        const std::vector<ChunkRun> runs = runAllRangeChunks(data, e, of.segments);
        const ChunkRun& canon = runs[canonicalIndex()];
        const FedRun& run = canon.run;
        const std::vector<Latch>& latches = canon.latches;
        ASSERT_FALSE(latches.empty()) << e.name << ": la nota no abrio ningun enganche";
        // Los enganches salen de eventos y ventanas: ninguna tanda cambia su cantidad.
        for (const ChunkRun& r : runs)
            ASSERT_EQ(r.latches.size(), latches.size()) << e.name << ": tanda " << r.chunk << " cambio la cantidad de enganches";
        const Params prm = [&] { Params q; q.sampleRate = run.rate; return q; }();
        // La definicion, verificada sobre material real: el segundo ataque llega con la cuerda
        // sonando (ninguna ventana de 4096 de las dos pasadas queda bajo 0,001 en la costura), asi
        // que NO puede abrir enganche.
        EXPECT_EQ(latch::measure(run.pubs, run.windows, run.events, of.segments, prm).size(), latches.size())
            << e.name << ": el re-punteo abrio un enganche nuevo";

        const Pass2 p2 = measurePass2(run);
        bool moved = false, movedFirst = false;
        for (size_t i = 0; i < latches.size(); ++i) {
            const Latch& l = latches[i];
            const auto [rFirst, rConv] = rangesOf(runs, i);
            const Row r = rowOf(e.name, static_cast<int>(i) + 1, l, p2, run.rate, rFirst, rConv);
            rows[keyOf(r.name, r.v[0])] = r;
            std::printf("%-24s %7.2f %2zu | %7.3f %7.3f %5d %+7.2f %7.2f %7.2f %7.2f %5.2f | %7.3f %7.3f %3d | %7.3f %7.3f\n",
                        e.name.c_str(), e.trueHz, i + 1, l.firstReadingSec, l.convergedSec, l.readings, l.firstErrorCents,
                        l.medianAbsCents, l.p90AbsCents, l.maxAbsCents, l.coverage, secOf(p2.firstFrames, run.rate),
                        secOf(p2.convFrames, run.rate), p2.dropped, secOf(rFirst.max, run.rate), secOf(rConv.max, run.rate));
            allLatches.push_back(l);
            if (!std::isnan(l.firstReadingSec)) tFirst.push_back(l.firstReadingSec);
            if (!std::isnan(l.convergedSec)) tConv.push_back(l.convergedSec);
            if (rFirst.max >= 0) tFirstWorst.push_back(secOf(rFirst.max, run.rate));
            if (rConv.max >= 0) tConvWorst.push_back(secOf(rConv.max, run.rate));
            for (const ChunkRun& other : runs) {
                const Latch& o = other.latches[i];
                if (o.firstReadingFrames != l.firstReadingFrames) movedFirst = moved = true;
                if (o.convergedFrames != l.convergedFrames) moved = true;
            }
            if (rFirst.max >= 0 && secOf(rFirst.max - rFirst.min, run.rate) > widestSpreadSec) {
                widestSpreadSec = secOf(rFirst.max - rFirst.min, run.rate);
                widestName = e.name;
            }
        }
        filesMoved += moved;
        filesMovedFirst += movedFirst;
        if (p2.firstFrames >= 0) tFirst2.push_back(secOf(p2.firstFrames, run.rate));
    }
    const latch::Pooled pooled = latch::poolLatchErrors(allLatches);
    std::printf("\nenganches=%zu archivos=%zu\n", allLatches.size(), entries.size());
    std::printf("t_primera (tanda %d): n=%zu mediana %.3f max %.3f | peor de la particion: n=%zu mediana %.3f max %.3f\n",
                kProductionChunkFrames, tFirst.size(), medianOf(tFirst), maxOf(tFirst), tFirstWorst.size(), medianOf(tFirstWorst),
                maxOf(tFirstWorst));
    std::printf("t_conv    (tanda %d): n=%zu mediana %.3f max %.3f | peor de la particion: n=%zu mediana %.3f max %.3f\n",
                kProductionChunkFrames, tConv.size(), medianOf(tConv), maxOf(tConv), tConvWorst.size(), medianOf(tConvWorst),
                maxOf(tConvWorst));
    std::printf("t_primera2: n=%zu mediana %.3f max %.3f\n", tFirst2.size(), medianOf(tFirst2), maxOf(tFirst2));
    std::printf("tandas que NO dividen a 4096 (96,192,240,480,960,1536) vs %d: %d de %zu archivos mueven t_primera, %d mueven t_primera "
                "o t_conv; rango mas ancho de t_primera %.3f s (%s)\n",
                kProductionChunkFrames, filesMovedFirst, entries.size(), filesMoved, widestSpreadSec, widestName.c_str());
    std::printf("|error| de %d lecturas con oraculo: mediana %.3f p90 %.3f max %.3f c | cobertura 2sigma %.3f\n", pooled.withError,
                pooled.medianAbsCents, pooled.p90AbsCents, pooled.maxAbsCents, pooled.coverage);

    if (const char* upd = std::getenv("WMA_LATCH_BASELINE_UPDATE"); upd != nullptr && std::string(upd) == "1") {
        if (kSubset) GTEST_SKIP() << "no se regenera la linea de base bajo sanitizer: solo se midio un subconjunto";
        std::string text = baselineHeader();
        for (const auto& kv : rows) text += formatRow(kv.second) + "\n";
        std::FILE* f = std::fopen(baselinePath().c_str(), "wb");
        ASSERT_NE(f, nullptr) << "no se pudo escribir " << baselinePath();
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
        GTEST_SKIP() << "linea de base REGENERADA en " << baselinePath() << ": regenerar no es verificar. Revisar el diff.";
    }

    bool ok = false;
    const std::string text = slurp(baselinePath(), &ok);
    ASSERT_TRUE(ok) << "falta " << baselinePath() << ". Con corpus presente esto FALLA. Se genera con "
                       "WMA_LATCH_BASELINE_UPDATE=1 (ver la cabecera de este test)";
    std::map<std::string, Row> want;
    ASSERT_TRUE(parseBaseline(text, &want)) << baselinePath() << " tiene filas mal formadas";
    const std::vector<std::string> diffs = diffRows(want, rows, !kSubset);
    for (const std::string& d : diffs) ADD_FAILURE() << d;
    EXPECT_TRUE(diffs.empty()) << diffs.size() << " diferencias contra la linea de base: si el cambio es el buscado, "
                                  "regenerar con WMA_LATCH_BASELINE_UPDATE=1 y revisar el diff del TSV";
}

// =====================================================================================
// POR ATAQUE (AC-055.2, D11): el nucleo sin corpus
// =====================================================================================
//
// Rate de 1000 frames/s como arriba: Δ de 250 ms son 250 frames.

std::vector<Publication> readingsEvery(int64_t from, int64_t to, int64_t step, double hz, int state = kMeas,
                                       double sigma = 1.0) {
    std::vector<Publication> out;
    for (int64_t p = from; p <= to; p += step) out.push_back(pub(p, hz, state, sigma));
    return out;
}

TEST(AttackInstrument, T0IsTheOraclesFrameNotAPublicationOfTheEngine) {
    // AC-055.2. Atrapa: t0 tomado del motor (la primera publicacion despues del ataque, o la primera
    // con lectura) en vez del frame que dio el oraculo. Con t0 = 1037 fuera de la grilla de
    // publicaciones, t_primera tiene que salir 1300 - 1037 = 263 exactos, y t_conv 1500 - 1037.
    const std::vector<Publication> pubs{pub(1100), pub(1200), pub(1300, 440.0), pub(1500, 440.0, kConv)};
    const auto a = latch::measureAttacks(pubs, {1037}, kNoOracle, 5000, params());
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].t0Frames, 1037);
    EXPECT_EQ(a[0].firstReadingFrames, 263);
    EXPECT_NEAR(a[0].firstReadingSec, 0.263, 1e-12);
    EXPECT_EQ(a[0].convergedFrames, 463);
    EXPECT_EQ(a[0].publications, 4);
    EXPECT_EQ(a[0].readings, 2);
    EXPECT_FALSE(a[0].readingCarried);
}

TEST(AttackInstrument, ARepluckWithTheStringSoundingIsAnAttackAndItsReadingComesCarried) {
    // AC-055.2. Atrapa: un instrumento que solo abre ataques tras silencio (la definicion de enganche),
    // que en el g42 no ve ningun re-punteo; y uno que no marca la lectura arrastrada, que reportaria
    // t_primera = 50 ms como si el motor hubiera leido el re-punteo en 50 ms.
    std::vector<Publication> pubs = readingsEvery(100, 3000, 100, 440.0, kConv);
    const auto a = latch::measureAttacks(pubs, {50, 2020}, kNoOracle, 3000, params());
    ASSERT_EQ(a.size(), 2u);
    EXPECT_FALSE(a[0].readingCarried);
    EXPECT_FALSE(a[0].convergedCarried);
    EXPECT_TRUE(a[1].readingCarried);
    EXPECT_TRUE(a[1].convergedCarried);
    EXPECT_EQ(a[1].firstReadingFrames, 80) << "2100 - 2020: la primera publicacion DESPUES de t0";
    EXPECT_EQ(a[0].endFrames, 2020);
    EXPECT_EQ(a[1].endFrames, 3000);
}

TEST(AttackInstrument, APublicationAtOrBeforeT0IsNeverTheFirstReading) {
    // AC-055.2. Atrapa: contar como primera lectura una publicacion anterior a t0 (o en t0 mismo:
    // `pos` es la posicion del ring DESPUES de drenar, y con pos == t0 el motor no vio ninguna muestra
    // del ataque). Con `<` en vez de `<=` t_primera sale 0.
    const std::vector<Publication> pubs{pub(900, 440.0), pub(1000, 440.0), pub(1040), pub(1090, 441.0)};
    const auto a = latch::measureAttacks(pubs, {1000}, kNoOracle, 2000, params());
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].firstReadingFrames, 90);
    EXPECT_EQ(a[0].publications, 2) << "solo las de (t0, fin]";
    EXPECT_TRUE(a[0].readingCarried) << "la de pos == t0 traia cents";
    EXPECT_FALSE(a[0].convergedCarried);
}

TEST(AttackInstrument, APublicationAtTheNextT0BelongsToThePreviousAttack) {
    // AC-055.2. Atrapa: el borde entre ataques corrido un frame (una publicacion contada en los dos,
    // o en ninguno).
    const std::vector<Publication> pubs{pub(1500, 440.0), pub(2000, 440.0), pub(2001, 440.0)};
    const auto a = latch::measureAttacks(pubs, {1000, 2000}, kNoOracle, 3000, params());
    ASSERT_EQ(a.size(), 2u);
    EXPECT_EQ(a[0].publications, 2);
    EXPECT_EQ(a[1].publications, 1);
    EXPECT_EQ(a[1].firstReadingFrames, 1);
}

TEST(AttackInstrument, NoReadingOrNoConvergenceIsMissingNotZero) {
    // AC-055.2. Atrapa: un ataque sin lectura que se agrega como t = 0 (bajaria la mediana).
    const auto a = latch::measureAttacks({pub(1100), pub(1200, kNan, kMeas)}, {1000}, kNoOracle, 2000, params());
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].firstReadingFrames, -1);
    EXPECT_TRUE(std::isnan(a[0].firstReadingSec));
    EXPECT_EQ(a[0].convergedFrames, -1);
    EXPECT_TRUE(std::isnan(a[0].medianAbsCents));
    EXPECT_TRUE(std::isnan(a[0].coverage));
    EXPECT_FALSE(a[0].early.reading);
    EXPECT_TRUE(a[0].early.applies);
}

TEST(AttackInstrument, EachPublicationIsJudgedByTheSegmentAlreadyFinishedNeverALaterOne) {
    // AC-055.2. Atrapa: comparar contra un tramo del oraculo POSTERIOR a la publicacion (o el de t_ini
    // mas cercano): premiaria un adelanto que el motor no puede tener. La publicacion de t = 1,30 s
    // tiene que medirse contra el tramo que termino en 1,20 (440 Hz), no contra el de 1,35 (hzAt +10 c).
    const std::vector<OracleSegment> oracle{{1.0, 1.2, 440.0}, {1.15, 1.35, hzAt(440.0, 10.0)}};
    const auto a = latch::measureAttacks({pub(1300, hzAt(440.0, 3.0), kMeas, 2.0)}, {1000}, oracle, 2000, params());
    ASSERT_EQ(a.size(), 1u);
    ASSERT_EQ(a[0].withError, 1);
    EXPECT_NEAR(a[0].firstErrorCents, 3.0, 1e-9);
    EXPECT_EQ(a[0].covered, 1) << "3 c <= 2σ = 4 c";
}

TEST(AttackInstrument, TheEarlyAndLateProbesReadTheVisiblePublicationAgainstTheFinishedSegment) {
    // AC-055.2. Atrapa: el error «a 250 ms» tomado de la publicacion MAS CERCANA (incluso posterior) en vez
    // de la visible en t0 + 250, o contra el oraculo de despues de ese instante. t0 = 1000: visible a
    // 1250 es la de 1240 (+4 c); la de 1260 (+20 c) todavia no existia. El tramo terminado en 1,25 s es
    // el de [1,10, 1,25] a 440; el de [1,15, 1,30] (+30 c) no habia terminado.
    const std::vector<OracleSegment> oracle{{1.10, 1.25, 440.0}, {1.15, 1.30, hzAt(440.0, 30.0)},
                                            {1.85, 2.00, hzAt(440.0, -1.0)}, {1.90, 2.05, hzAt(440.0, 50.0)}};
    const std::vector<Publication> pubs{pub(1240, hzAt(440.0, 4.0), kMeas, 1.5), pub(1260, hzAt(440.0, 20.0)),
                                        pub(1990, hzAt(440.0, -0.5), kConv, 0.3), pub(2001, hzAt(440.0, 9.0))};
    const auto a = latch::measureAttacks(pubs, {1000}, oracle, 3000, params());
    ASSERT_EQ(a.size(), 1u);
    ASSERT_TRUE(a[0].early.applies && a[0].early.reading);
    EXPECT_NEAR(a[0].early.errorCents, 4.0, 1e-9);
    EXPECT_NEAR(a[0].early.sigmaCents, 1.5, 1e-12);
    EXPECT_FALSE(a[0].early.covered) << "4 c > 2σ = 3 c";
    ASSERT_TRUE(a[0].late.applies && a[0].late.reading);
    EXPECT_NEAR(a[0].late.errorCents, 0.5, 1e-9) << "-0,5 c publicado contra -1 c del tramo terminado en 2,00 s";
    EXPECT_TRUE(a[0].late.covered) << "0,5 c <= 2σ = 0,6 c";
}

TEST(AttackInstrument, AProbeNeverReadsACarriedPublicationAndDoesNotApplyPastTheAttack) {
    // AC-055.2. Atrapa: la lectura arrastrada de antes de t0 contada como lectura «a 250 ms» del ataque
    // nuevo; y una sonda que mira pasado el final del ataque (lo que ya es del siguiente).
    const std::vector<OracleSegment> oracle{{0.0, 0.5, 440.0}};
    const std::vector<Publication> pubs{pub(900, 440.0), pub(1300, 440.0)};
    const auto a = latch::measureAttacks(pubs, {1000, 1600}, oracle, 2000, params());
    ASSERT_EQ(a.size(), 2u);
    EXPECT_TRUE(a[0].early.applies);
    EXPECT_FALSE(a[0].early.reading) << "en (1000, 1250] no hay publicacion propia; la de 900 es arrastrada";
    EXPECT_TRUE(std::isnan(a[0].early.errorCents));
    EXPECT_FALSE(a[0].late.applies) << "1000 + 1000 > 1600, el final del ataque";
    EXPECT_FALSE(a[1].late.applies) << "1600 + 1000 > 2000, el final del flujo";
    EXPECT_TRUE(a[1].early.applies);
}

TEST(AttackInstrument, PercentilesAndCoveragePerAttack) {
    // AC-055.2. Atrapa: percentiles o cobertura que mezclan ataques (los del segundo en el primero), y la
    // lectura sin oraculo contada como error. Ataque 1: errores 1..4 c con σ 1,6 (2σ = 3,2: cubre 1, 2, 3);
    // ataque 2: una lectura de 10 c y una sin oraculo.
    std::vector<Publication> pubs;
    for (int c = 1; c <= 4; ++c) pubs.push_back(pub(1000 + 100 * c, hzAt(440.0, c), kMeas, 1.6));
    pubs.push_back(pub(2100, hzAt(440.0, 10.0), kMeas, 1.0));
    const std::vector<OracleSegment> withHole{{0.0, 1.0, 440.0}, {1.0, 2.15, kNan}};
    pubs.push_back(pub(2200, 440.0, kMeas, 1.0));
    const auto a = latch::measureAttacks(pubs, {1000, 2000}, withHole, 3000, params());
    ASSERT_EQ(a.size(), 2u);
    EXPECT_EQ(a[0].withError, 4);
    EXPECT_NEAR(a[0].medianAbsCents, 2.5, 1e-9);
    EXPECT_NEAR(a[0].p90AbsCents, 4.0, 1e-9);
    EXPECT_NEAR(a[0].coverage, 0.75, 1e-12);
    EXPECT_EQ(a[1].withError, 1);
    EXPECT_EQ(a[1].noOracle, 1);
    EXPECT_NEAR(a[1].maxAbsCents, 10.0, 1e-9);
}

TEST(AttackInstrument, OracleAttackLinesAreReadUnderTheirFile) {
    // AC-055.2. Atrapa: un `# ataque` atribuido al archivo equivocado (o leido como comentario y
    // perdido): los t0 de un archivo serian los de otro.
    const std::string tmp = corpus::makeTempDir();
    const std::string path = tmp + "/oraculo.tsv";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    const std::string text = "# comando: x\n# archivo a.wav sha256 aa rate 48000 frames 100\n# ataque a.wav frame 7\n"
                             "a.wav\t0.0\t0.1\t440\t0\t-20\n# archivo b.wav sha256 bb rate 48000 frames 100\n"
                             "# ataque b.wav frame 0\n# ataque b.wav frame 4096\n";
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    std::map<std::string, OracleFile> oracle;
    const std::string why = loadOracle(path, &oracle);
    std::remove(path.c_str());
    ASSERT_EQ(why, "");
    EXPECT_EQ(oracle["a.wav"].attacks, (std::vector<int64_t>{7}));
    EXPECT_EQ(oracle["b.wav"].attacks, (std::vector<int64_t>{0, 4096}));
    std::map<std::string, OracleFile> bad;
    const std::string badPath = path + ".mal";
    f = std::fopen(badPath.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    const std::string badText = "# archivo a.wav sha256 aa rate 48000 frames 100\n# ataque b.wav frame 7\n";
    std::fwrite(badText.data(), 1, badText.size(), f);
    std::fclose(f);
    EXPECT_NE(loadOracle(badPath, &bad), "") << "un ataque de otro archivo es un error, no se descarta";
    std::remove(badPath.c_str());
    corpus::removeTempDir(tmp);
}

// =====================================================================================
// POR ATAQUE: la linea de base sobre corpus-v1 y la toma real
// =====================================================================================

constexpr Col kAttackCols[] = {
    {"ataque", true},          {"pasada", true},          {"t0_frames", true},       {"arrastrada_lectura", true},
    {"arrastrada_conv", true}, {"t_primera_frames", true}, {"t_primera_s", false},   {"t_conv_frames", true},
    {"t_conv_s", false},       {"n_publicaciones", true}, {"n_lecturas", true},      {"n_sin_oraculo", true},
    {"err_primera_c", false},  {"sigma_primera_c", false}, {"mediana_abs_c", false}, {"p90_abs_c", false},
    {"max_abs_c", false},      {"cobertura", false},      {"lect_250ms", true},      {"err_250ms_c", false},
    {"sigma_250ms_c", false},  {"cub_250ms", true},       {"lect_1s", true},         {"err_1s_c", false},
    {"sigma_1s_c", false},     {"cub_1s", true}};
constexpr size_t kNumAttackCols = sizeof(kAttackCols) / sizeof(kAttackCols[0]);

struct AttackRow {
    std::string name;
    double v[kNumAttackCols];
};

std::string formatAttackRow(const AttackRow& r) {
    std::string out = r.name;
    for (size_t i = 0; i < kNumAttackCols; ++i) out += "\t" + formatNumber(r.v[i], kAttackCols[i].exact);
    return out;
}

bool parseAttackBaseline(const std::string& text, std::map<std::string, AttackRow>* out) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty() || line[0] == '#') continue;
        AttackRow r{};
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) return false;
        r.name = line.substr(0, tab);
        size_t cur = tab + 1;
        for (size_t i = 0; i < kNumAttackCols; ++i) {
            const size_t next = line.find('\t', cur);
            const std::string cell = line.substr(cur, next == std::string::npos ? std::string::npos : next - cur);
            if (cell.empty()) return false;
            r.v[i] = std::strtod(cell.c_str(), nullptr);
            if ((i + 1 < kNumAttackCols) != (next != std::string::npos)) return false;
            cur = next + 1;
        }
        (*out)[keyOf(r.name, r.v[0])] = r;
    }
    return true;
}

std::vector<std::string> diffAttackRows(const std::map<std::string, AttackRow>& want,
                                        const std::map<std::string, AttackRow>& got, bool requireSameSet) {
    std::vector<std::string> out;
    for (const auto& [key, g] : got) {
        const auto it = want.find(key);
        if (it == want.end()) {
            out.push_back(key + ": no esta en la linea de base por ataque");
            continue;
        }
        for (size_t i = 0; i < kNumAttackCols; ++i)
            if (!sameValue(it->second.v[i], g.v[i], kAttackCols[i].exact))
                out.push_back(key + " " + kAttackCols[i].name + ": base " +
                              formatNumber(it->second.v[i], kAttackCols[i].exact) + ", ahora " +
                              formatNumber(g.v[i], kAttackCols[i].exact));
    }
    if (requireSameSet)
        for (const auto& kv : want)
            if (got.find(kv.first) == got.end()) out.push_back(kv.first + ": esta en la linea de base por ataque y no se midio");
    return out;
}

double flag(bool b) { return b ? 1.0 : 0.0; }
double probeFlag(const latch::Probe& p, bool value) { return p.applies ? flag(value) : kNan; }

AttackRow attackRowOf(const std::string& name, int index, int pass, const latch::Attack& a, int rate) {
    AttackRow r{};
    r.name = name;
    const double v[kNumAttackCols] = {static_cast<double>(index),
                                      static_cast<double>(pass),
                                      static_cast<double>(a.t0Frames),
                                      flag(a.readingCarried),
                                      flag(a.convergedCarried),
                                      framesOrNan(a.firstReadingFrames),
                                      secOf(a.firstReadingFrames, rate),
                                      framesOrNan(a.convergedFrames),
                                      secOf(a.convergedFrames, rate),
                                      static_cast<double>(a.publications),
                                      static_cast<double>(a.readings),
                                      static_cast<double>(a.noOracle),
                                      a.firstErrorCents,
                                      a.firstSigmaCents,
                                      a.medianAbsCents,
                                      a.p90AbsCents,
                                      a.maxAbsCents,
                                      a.coverage,
                                      probeFlag(a.early, a.early.reading),
                                      a.early.errorCents,
                                      a.early.sigmaCents,
                                      probeFlag(a.early, a.early.covered),
                                      probeFlag(a.late, a.late.reading),
                                      a.late.errorCents,
                                      a.late.sigmaCents,
                                      probeFlag(a.late, a.late.covered)};
    for (size_t i = 0; i < kNumAttackCols; ++i) r.v[i] = v[i];
    return r;
}

TEST(AttackInstrument, AttackBaselineRowsRoundTripAndCompareExactOnFrames) {
    // AC-055.2. Atrapa: una columna de la linea de base por ataque que se escribe y no se lee (desplaza
    // las siguientes), y una comparacion con tolerancia en los frames.
    latch::Attack a;
    a.t0Frames = 4096;
    a.firstReadingFrames = 2048;
    a.early.applies = true;
    a.early.reading = true;
    a.early.errorCents = 1.25;
    const AttackRow r = attackRowOf("a.wav", 3, 2, a, 48000);
    std::map<std::string, AttackRow> parsed;
    ASSERT_TRUE(parseAttackBaseline("# cabecera\n" + formatAttackRow(r) + "\n", &parsed));
    std::map<std::string, AttackRow> got{{keyOf("a.wav", 3), r}};
    EXPECT_TRUE(diffAttackRows(parsed, got, true).empty());
    got.begin()->second.v[5] += 1;   // un frame de t_primera
    EXPECT_EQ(diffAttackRows(parsed, got, true).size(), 1u);
    EXPECT_TRUE(std::isnan(parsed.begin()->second.v[22])) << "la sonda de 1 s no aplica: nan, no 0";
    EXPECT_FALSE(parseAttackBaseline("a.wav\t1\t2\n", &parsed));
}

std::string attackBaselinePath() { return std::string(WMA_ANALYSIS_GOLDEN_DIR) + "/../baseline/ataque-corpus-v1.tsv"; }

std::string attackBaselineHeader() {
    std::string h =
        "# Linea de base POR ATAQUE del afinador sobre corpus-v1 (REQ-055 S0, D11, AC-055.2). Una fila por ataque.\n"
        "# Motor medido: el de 1351b7e (sin cambio de produccion en S0). Al regenerar tras un cambio de motor\n"
        "# INTENCIONAL, actualizar este commit en `attackBaselineHeader()` (test_latch_instrument.cpp).\n"
        "#\n"
        "# t0 de cada ataque lo da el ORACULO (lineas `# ataque` de corpus-v1-oraculo-corto.tsv, detector de\n"
        "# scripts/corpus-reference-pitch.py: subida >= 6 dB de la energia cruda o de la primera diferencia en 2048\n"
        "# frames contra los 2048 previos, fechada en la grilla de 128, refractario 0,15 s; tolerancia 256 frames),\n"
        "# nunca el motor. Se alimenta como la linea de base del enganche (feedTwice): la NOTA dos veces seguidas sin\n"
        "# tocar el objetivo, en TANDAS FIJAS DE 256 FRAMES drenadas hasta ring vacio. pasada 1 = los ataques del\n"
        "# archivo; pasada 2 = los mismos, corridos noteEndFrames: el RE-PUNTEO con la cuerda sonando. El oraculo de\n"
        "# la pasada 2 son los tramos del archivo corridos igual (los que terminan pasado noteEndFrames no se usan).\n"
        "#\n"
        "# El ataque dura (t0, siguiente t0] (o hasta el final del flujo). t_primera / t_conv = primera publicacion\n"
        "# con cents / con CONVERGED despues de t0, en frames del ring desde t0. arrastrada_* = 1 si la ultima\n"
        "# publicacion con pos <= t0 ya traia cents / CONVERGED: el motor no solto la lectura de antes, y entonces\n"
        "# t_primera no es una latencia del ataque. Errores en cents contra el ORACULO INSTANTANEO (tramo de t_fin\n"
        "# mas cercano <= t de la publicacion); cobertura = fraccion con |error| <= 2 sigma. *_250ms / *_1s = la\n"
        "# lectura VISIBLE en t0 + 0,25 s / t0 + 1 s (ultima publicacion en (t0, t0 + Δ]) contra el tramo ya\n"
        "# terminado en ese instante; lect = 1 si habia lectura propia del ataque, cub = 1 si |error| <= 2 sigma;\n"
        "# nan = no aplica (t0 + Δ pasado el final del ataque). `nan` en el resto = no llego / no aplica.\n"
        "#\n"
        "# EL DIFF DE ESTE ARCHIVO ES LA REVISION. Frames y banderas exactos; lo demas con tolerancia 0,002.\n"
        "# Regenerar (corre la suite y ESCRIBE en vez de comparar; el test queda SKIPPED):\n"
        "#   WMA_LATCH_BASELINE_UPDATE=1 bash scripts/run-cpp-tests.sh -R LatchInstrumentCorpus\n"
        "# archivo";
    for (const Col& c : kAttackCols) h += std::string("\t") + c.name;
    return h + "\n";
}

/// Resumen de un conjunto de ataques para imprimir: mediana / p90 / max.
std::string summary(const char* label, std::vector<double> v) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s n=%zu mediana %.3f p90 %.3f max %.3f", label, v.size(), latch::median(v),
                  latch::percentile(v, 0.9), maxOf(v));
    return buf;
}

TEST(LatchInstrumentCorpus, MeasuresTheAttacksAgainstTheCommittedBaseline) {
    // AC-055.2 (D11). La linea de base POR ATAQUE del motor de hoy. Atrapa: cualquier cambio del analisis
    // que mueva un t_primera / t_conv por ataque en un frame, o el error / σ / cobertura en mas de 0,002.
    const std::string dir = corpus::defaultCorpusDir();
    const corpus::State st = corpus::stateOf(dir, corpus::manifestPath());
    if (st != corpus::State::kVerified) GTEST_SKIP() << missingCorpusMessage(st);
    std::map<std::string, OracleFile> oracle;
    ASSERT_EQ(loadOracle(oraclePath(), &oracle), "") << "el corpus esta y el oraculo corto no: esto FALLA";

    bool subset = false;
    const std::vector<corpus::Entry> entries = corpusEntriesForThisBuild(&subset);
    std::map<std::string, AttackRow> rows;
    std::vector<double> first[3], conv[3], e250[3], e1s[3];   // [0] pasada 1, [1] pasada 2, [2] todas
    std::vector<latch::Attack> all[3];
    int carried = 0, noRead250 = 0, applies250 = 0, cov250 = 0, with250 = 0;
    std::printf("\n%-24s %2s %1s %7s | %1s %7s %7s | %7s %7s %5s %7s %7s\n", "archivo", "n", "p", "t0_s", "a", "t_prim",
                "t_conv", "e_250", "e_1s", "cob", "med|e|", "max|e|");
    for (const corpus::Entry& e : entries) {
        const wav::WavData data = wav::readWav((dir + "/" + e.name).c_str());
        ASSERT_GT(data.sampleRate, 0) << e.name;
        const OracleFile& of = oracle.at(e.name);
        ASSERT_EQ(of.rate, data.sampleRate) << e.name;
        ASSERT_EQ(of.frames, static_cast<long long>(data.numFrames)) << e.name;
        ASSERT_FALSE(of.attacks.empty()) << e.name << ": el oraculo no declara ningun ataque (regenerarlo con --short)";

        // t0 y el corte de la nota salen del ARCHIVO y del oraculo, no del ring: noteEndFrames es lo que
        // se alimento en cada pasada.
        const int64_t noteEnd = corpus::noteEndFrames(data);
        const FedRun run = feedTwice(data, e, kProductionChunkFrames);
        ASSERT_EQ(run.frames1, noteEnd) << e.name << ": el alimentador no escribio la nota entera";
        std::vector<int64_t> t0s;
        std::vector<int> passOf;
        for (int pass = 0; pass < 2; ++pass)
            for (int64_t a : of.attacks)
                if (a < noteEnd) {
                    t0s.push_back(a + pass * noteEnd);
                    passOf.push_back(pass + 1);
                }
        const double cut = static_cast<double>(noteEnd) / data.sampleRate;
        std::vector<OracleSegment> segs;
        for (int pass = 0; pass < 2; ++pass)
            for (const OracleSegment& sg : of.segments)
                if (sg.tFin <= cut) segs.push_back({sg.tIni + pass * cut, sg.tFin + pass * cut, sg.hz});
        Params prm;
        prm.sampleRate = run.rate;
        const std::vector<latch::Attack> attacks = latch::measureAttacks(run.pubs, t0s, segs, 2 * noteEnd, prm);
        ASSERT_EQ(attacks.size(), t0s.size());
        for (size_t i = 0; i < attacks.size(); ++i) {
            const latch::Attack& a = attacks[i];
            EXPECT_EQ(a.t0Frames, t0s[i]) << "t0 es el del oraculo";
            const int p = passOf[i] - 1;
            const AttackRow r = attackRowOf(e.name, static_cast<int>(i) + 1, passOf[i], a, run.rate);
            rows[keyOf(r.name, r.v[0])] = r;
            std::printf("%-24s %2zu %1d %7.3f | %1d %7.3f %7.3f | %+7.2f %+7.2f %5.2f %7.2f %7.2f\n", e.name.c_str(), i + 1,
                        passOf[i], a.t0Sec, a.readingCarried ? 1 : 0, a.firstReadingSec, a.convergedSec, a.early.errorCents,
                        a.late.errorCents, a.coverage, a.medianAbsCents, a.maxAbsCents);
            for (int k : {p, 2}) {
                all[k].push_back(a);
                if (a.firstReadingFrames >= 0) first[k].push_back(a.firstReadingSec);
                if (a.convergedFrames >= 0) conv[k].push_back(a.convergedSec);
                if (!std::isnan(a.early.errorCents)) e250[k].push_back(std::fabs(a.early.errorCents));
                if (!std::isnan(a.late.errorCents)) e1s[k].push_back(std::fabs(a.late.errorCents));
            }
            carried += a.readingCarried;
            if (a.early.applies) {
                ++applies250;
                noRead250 += !a.early.reading;
                if (!std::isnan(a.early.errorCents)) {
                    ++with250;
                    cov250 += a.early.covered;
                }
            }
        }
    }
    const char* names[3] = {"pasada 1 (ataque)", "pasada 2 (re-punteo)", "todas"};
    for (int k = 0; k < 3; ++k) {
        const latch::Pooled pooled = [&] {
            std::vector<Latch> asLatches;
            for (const latch::Attack& a : all[k]) {
                Latch l;
                l.absErrors = a.absErrors;
                l.withError = a.withError;
                l.covered = a.covered;
                asLatches.push_back(l);
            }
            return latch::poolLatchErrors(asLatches);
        }();
        std::printf("\n[%s] ataques=%zu\n  %s\n  %s\n  %s\n  %s\n  |error| de %d publicaciones: mediana %.3f p90 %.3f max %.3f c | "
                    "cobertura 2sigma %.3f\n",
                    names[k], all[k].size(), summary("t_primera s:", first[k]).c_str(), summary("t_conv s:   ", conv[k]).c_str(),
                    summary("|err 250ms| c:", e250[k]).c_str(), summary("|err 1s| c:   ", e1s[k]).c_str(), pooled.withError,
                    pooled.medianAbsCents, pooled.p90AbsCents, pooled.maxAbsCents, pooled.coverage);
    }
    std::printf("\narrastradas=%d | a 250 ms: aplica %d, sin lectura propia %d, con error %d, cubiertas a 2sigma %d\n", carried,
                applies250, noRead250, with250, cov250);

    if (const char* upd = std::getenv("WMA_LATCH_BASELINE_UPDATE"); upd != nullptr && std::string(upd) == "1") {
        if (subset) GTEST_SKIP() << "no se regenera la linea de base bajo sanitizer: solo se midio un subconjunto";
        std::string text = attackBaselineHeader();
        for (const auto& kv : rows) text += formatAttackRow(kv.second) + "\n";
        std::FILE* f = std::fopen(attackBaselinePath().c_str(), "wb");
        ASSERT_NE(f, nullptr) << "no se pudo escribir " << attackBaselinePath();
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
        GTEST_SKIP() << "linea de base por ataque REGENERADA en " << attackBaselinePath() << ": revisar el diff.";
    }
    bool ok = false;
    const std::string text = slurp(attackBaselinePath(), &ok);
    ASSERT_TRUE(ok) << "falta " << attackBaselinePath() << ". Con corpus presente esto FALLA";
    std::map<std::string, AttackRow> want;
    ASSERT_TRUE(parseAttackBaseline(text, &want)) << attackBaselinePath() << " tiene filas mal formadas";
    const std::vector<std::string> diffs = diffAttackRows(want, rows, !subset);
    for (const std::string& d : diffs) ADD_FAILURE() << d;
    EXPECT_TRUE(diffs.empty()) << diffs.size() << " diferencias contra la linea de base por ataque";
}

TEST(LatchInstrumentRealTake, MeasuresTheAttacksOfTheG42Take) {
    // AC-055.1 / AC-055.2. La toma real del g42 (S0, tareas 0.2/0.3) todavia no existe: SKIPPED, nunca
    // passed. Cuando su manifiesto aparezca, esto FALLA hasta que el alimentador por ataque de la toma
    // (objetivo por cuerda, su oraculo y su linea de base) exista: un SKIPPED no se vuelve PASSED sin medir.
    const std::string manifest = std::string(WMA_ANALYSIS_GOLDEN_DIR) + "/../corpus-g42-manifest.txt";
    if (!corpus::fileExists(manifest))
        GTEST_SKIP() << "sin la toma real del g42 (" << manifest << "): S0 tareas 0.2/0.3. Esto NO cuenta como cobertura";
    FAIL() << "aparecio " << manifest << " y el instrumento por ataque todavia no la alimenta (REQ-055 S0, tarea 0.3)";
}

}  // namespace
}  // namespace wma_test
