/**
 * test_latch_instrument.cpp — REQ-055 S0. La vara del afinador: tiempo y error POR ENGANCHE.
 *
 * Dos suites:
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

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace wma_test {
namespace {

using latch::Latch;
using latch::OracleSegment;
using latch::Params;
using latch::Publication;
using latch::TargetEvent;

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

/// Una publicacion al final del bloque [pos − 100, pos).
Publication pub(int64_t pos, double rms, double hz = kNan, int state = kNoLock, double sigma = 1.0) {
    Publication p;
    p.posStart = pos - 100;
    p.pos = pos;
    p.rms = rms;
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
    const std::vector<Publication> pubs = {pub(100, kLoud), pub(200, kQuiet), pub(300, kQuiet), pub(400, kLoud)};
    const auto l = latch::measure(pubs, {{0, true}}, kNoOracle, params());
    ASSERT_EQ(l.size(), 2u);
    EXPECT_EQ(l[0].t0Frames, 0);
    EXPECT_EQ(l[1].t0Frames, 300);
    EXPECT_EQ(l[1].cause, Latch::Cause::kSignalAfterSilence);
    EXPECT_DOUBLE_EQ(l[1].t0Sec, 0.3);
}

TEST(LatchInstrument, TheFirstWindowWithSignalAtTheTargetChangeIsOneLatchNotTwo) {
    // Atrapa: contar dos veces el arranque (el evento de objetivo Y la primera ventana con senal
    // sobre el ring vacio) y duplicar el enganche inicial.
    const std::vector<Publication> pubs = {pub(100, kLoud), pub(200, kLoud)};
    const auto l = latch::measure(pubs, {{0, true}}, kNoOracle, params());
    ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l[0].cause, Latch::Cause::kTargetChange);
    EXPECT_EQ(l[0].publications, 2);
}

TEST(LatchInstrument, SignalFloorIsInclusiveAtOneThousandth) {
    // Atrapa: `>` en vez de `>=`, o un piso distinto de 0,001. rms == 0,001 ES senal;
    // 0,00099 no lo es. Un piso mal puesto mueve t0 de todos los enganches por senal.
    const std::vector<Publication> atFloor = {pub(100, 0.0), pub(200, 0.001)};
    auto l = latch::measure(atFloor, {{0, true}}, kNoOracle, params());
    ASSERT_EQ(l.size(), 2u);
    EXPECT_EQ(l[1].t0Frames, 100);

    const std::vector<Publication> below = {pub(100, 0.0), pub(200, 0.00099)};
    l = latch::measure(below, {{0, true}}, kNoOracle, params());
    EXPECT_EQ(l.size(), 1u) << "0,00099 es silencio: no abre enganche";
}

TEST(LatchInstrument, TargetChangeOpensALatchEvenWhileTheStringKeepsSounding) {
    // Atrapa: ignorar los cambios de objetivo cuando no hubo silencio. Las publicaciones
    // posteriores al evento se atribuyen al enganche NUEVO.
    const std::vector<Publication> pubs = {pub(100, kLoud, 100.0), pub(200, kLoud, 100.0), pub(300, kLoud),
                                           pub(400, kLoud, 200.0)};
    const auto l = latch::measure(pubs, {{0, true}, {200, true}}, kNoOracle, params());
    ASSERT_EQ(l.size(), 2u);
    EXPECT_EQ(l[1].t0Frames, 200);
    EXPECT_EQ(l[0].publications, 2);
    EXPECT_EQ(l[1].publications, 2);
    EXPECT_EQ(l[1].firstReadingFrames, 200) << "desde el cambio de objetivo, no desde el 0";
}

TEST(LatchInstrument, ARepluckWithoutSilenceInBetweenOpensNoLatch) {
    // Atrapa: abrir un enganche en cada subida de nivel. El segundo ataque llega con la cuerda
    // aun sonando (nunca bajo 0,001): es el MISMO enganche.
    const std::vector<Publication> pubs = {pub(100, 0.5), pub(200, 0.01), pub(300, 0.002), pub(400, 0.9),
                                           pub(500, 0.9)};
    const auto l = latch::measure(pubs, {{0, true}}, kNoOracle, params());
    ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l[0].publications, 5);
}

TEST(LatchInstrument, TargetSetDuringSilenceThenSignalAreTwoLatches) {
    // Atrapa: fusionar el evento con una ventana con senal que llega MUCHO despues. Son dos
    // t_reenganche distintos; el primero queda sin lecturas.
    const std::vector<Publication> pubs = {pub(100, kQuiet), pub(200, kQuiet), pub(300, kLoud, 100.0)};
    const auto l = latch::measure(pubs, {{0, true}}, kNoOracle, params());
    ASSERT_EQ(l.size(), 2u);
    EXPECT_EQ(l[0].t0Frames, 0);
    EXPECT_EQ(l[0].readings, 0);
    EXPECT_EQ(l[1].t0Frames, 200);
    EXPECT_EQ(l[1].firstReadingFrames, 100);
}

TEST(LatchInstrument, WithoutATargetNothingIsAttributed) {
    // Atrapa: abrir enganches por senal "con objetivo puesto" cuando NO hay objetivo; y que
    // quitar el objetivo (hasTarget=false) deje el enganche anterior abierto.
    const std::vector<Publication> pubs = {pub(100, kLoud, 100.0), pub(200, kQuiet), pub(300, kLoud, 100.0),
                                           pub(400, kQuiet), pub(500, kLoud, 100.0)};
    EXPECT_TRUE(latch::measure(pubs, {}, kNoOracle, params()).empty());
    const auto l = latch::measure(pubs, {{0, true}, {250, false}}, kNoOracle, params());
    ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l[0].publications, 2) << "lo posterior a quitar el objetivo no cuenta";
}

TEST(LatchInstrument, FirstReadingAndConvergedAreMeasuredFromT0Exactly) {
    // Atrapa: medir desde 0 en vez de desde t0, contar por indice en vez de frames, tomar el
    // primer CONVERGED sin esperar al primero, o contar una publicacion sin cents como lectura.
    const std::vector<Publication> pubs = {
        pub(100, kQuiet), pub(200, kLoud), pub(300, kLoud, kNan, kMeas), pub(400, kLoud, 100.0, kMeas),
        pub(500, kLoud, 100.0, kConv), pub(600, kLoud, 100.0, kConv)};
    const auto l = latch::measure(pubs, {{0, true}}, kNoOracle, params());
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
    const std::vector<Publication> pubs = {pub(100, kLoud), pub(200, kLoud)};
    const auto l = latch::measure(pubs, {{0, true}}, kNoOracle, params());
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
    const std::vector<Publication> pubs = {pub(100, kLoud), pub(200, kLoud, 110.0, kMeas), pub(250, kLoud, 110.0, kMeas)};
    const auto l = latch::measure(pubs, {{0, true}}, oracle, params());
    ASSERT_EQ(l.size(), 1u);
    ASSERT_EQ(l[0].withError, 2);
    EXPECT_NEAR(l[0].maxAbsCents, 0.0, 1e-9) << "contra el tramo previo el error es 0; contra el posterior seria -151 c";
}

TEST(LatchInstrument, ReadingsWithoutOracleAreCountedApartNotAsError) {
    // Atrapa: tomar una lectura sin tramo previo (o con hz NaN) como error 0 o como error enorme.
    // t = 0,1 s: ningun tramo termino todavia. t >= 0,25 s: el tramo previo (0,15-0,25) no tiene hz.
    const std::vector<OracleSegment> oracle = {{0.0, 0.15, 100.0}, {0.15, 0.25, kNan}};
    const std::vector<Publication> pubs = {
        pub(100, kLoud, 500.0, kMeas),   // sin tramo previo
        pub(300, kLoud, 500.0, kMeas),   // el tramo previo es NaN
        pub(400, kLoud, hzAt(100.0, 3.0), kMeas)};   // idem: el de t_fin mas cercano manda, aunque haya uno valido antes
    auto l = latch::measure(pubs, {{0, true}}, oracle, params());
    ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l[0].readings, 3);
    EXPECT_EQ(l[0].noOracle, 3);
    EXPECT_EQ(l[0].withError, 0);
    EXPECT_TRUE(std::isnan(l[0].maxAbsCents));
    EXPECT_TRUE(std::isnan(l[0].firstErrorCents));

    const std::vector<OracleSegment> oracle2 = {{0.0, 0.15, 100.0}};
    const std::vector<Publication> pubs2 = {pub(100, kLoud, 500.0, kMeas), pub(200, kLoud, hzAt(100.0, 3.0), kMeas)};
    l = latch::measure(pubs2, {{0, true}}, oracle2, params());
    ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l[0].noOracle, 1);
    EXPECT_EQ(l[0].withError, 1);
    EXPECT_NEAR(l[0].maxAbsCents, 3.0, 1e-9) << "la lectura sin oraculo no entra a los percentiles";
    EXPECT_NEAR(l[0].coverage, 0.0, 1e-12) << "y tampoco al denominador de la cobertura";
}

TEST(LatchInstrument, ErrorHasThePublishedOverOracleSign) {
    // Atrapa: la razon invertida (oraculo/publicado) o el error de la primera lectura tomado de
    // otra. Publicado +7 c arriba del oraculo = +7; la segunda lectura (−2 c) no pisa la primera.
    const std::vector<OracleSegment> oracle = {{0.0, 0.1, 200.0}};
    const std::vector<Publication> pubs = {pub(100, kLoud), pub(200, kLoud, hzAt(200.0, 7.0), kMeas, 3.5),
                                           pub(300, kLoud, hzAt(200.0, -2.0), kMeas, 0.5)};
    const auto l = latch::measure(pubs, {{0, true}}, oracle, params());
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
    std::vector<Publication> pubs = {pub(100, kLoud)};
    for (int k = 1; k <= 10; ++k)
        pubs.push_back(pub(100 + 100 * k, kLoud, hzAt(100.0, (k % 2 ? 1.0 : -1.0) * k), kMeas, 4.95));
    const auto l = latch::measure(pubs, {{0, true}}, oracle, params());
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
    const std::vector<Publication> pubs = {pub(100, kLoud), pub(200, kLoud, 100.0, kMeas, kNan), pub(300, kLoud, 100.0, kMeas, 1.0)};
    const auto l = latch::measure(pubs, {{0, true}}, oracle, params());
    ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l[0].withError, 2);
    EXPECT_EQ(l[0].covered, 1);
}

TEST(LatchInstrument, PooledStatisticsUseAllPublicationsNotTheMeanOfMedians) {
    // Un enganche con 1 error de 10 c y otro con 3 errores de 1 c: mediana del conjunto = 1, no
    // el promedio de medianas (5,5). Atrapa agregar entre enganches promediando resumenes.
    const std::vector<OracleSegment> oracle = {{0.0, 0.05, 100.0}};
    const std::vector<Publication> pubs = {pub(100, kLoud, hzAt(100.0, 10.0), kMeas, 1.0), pub(200, kLoud),
                                           pub(300, kLoud, hzAt(100.0, 1.0), kMeas, 1.0),
                                           pub(400, kLoud, hzAt(100.0, -1.0), kMeas, 1.0),
                                           pub(500, kLoud, hzAt(100.0, 1.0), kMeas, 1.0)};
    const auto l = latch::measure(pubs, {{0, true}, {250, true}}, oracle, params());
    ASSERT_EQ(l.size(), 2u);
    const auto pooled = latch::poolLatchErrors(l);
    EXPECT_EQ(pooled.withError, 4);
    EXPECT_NEAR(pooled.medianAbsCents, 1.0, 1e-9);
    EXPECT_NEAR(pooled.maxAbsCents, 10.0, 1e-9);
    EXPECT_NEAR(pooled.coverage, 3.0 / 4.0, 1e-12) << "el 10 c queda fuera de 2σ = 2";
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
    {"t_conv2_frames", true},  {"t_conv2_s", false},      {"sueltas2", true}};
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
    const double base[kNumCols] = {1, 0, 4096, 0.093, 8192, 0.186, 30, 2, 1.5, 0.4, 0.8, 2.1, 3.3, 0.9, 100, 0.002, 200, 0.004, 0};
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
};

/// El oraculo corto: `# archivo NOMBRE sha256 HEX rate HZ frames N` antes de las filas de cada
/// archivo, y por fila `nombre t_ini t_fin hz cents nivel`. Devuelve el motivo si no se pudo leer.
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

/// La corrida de UN archivo: la NOTA alimentada DOS veces seguidas sin tocar el objetivo, con cada
/// publicacion registrada. La primera pasada es la que se mide por enganche; la segunda, el
/// re-punteo, que por definicion no abre enganche nuevo.
struct FedRun {
    int rate = 0;
    int64_t frames1 = 0;   ///< donde termina la primera pasada (= donde empieza la segunda)
    std::vector<Publication> pubs;
    std::vector<TargetEvent> events;
};

FedRun feedTwice(const wav::WavData& data, const corpus::Entry& e) {
    FedRun run;
    run.rate = data.sampleRate;
    const int frames = corpus::noteEndFrames(data);
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
    const int capacity = static_cast<int>(wma::analysis::AnalysisRing::kCapacityFrames);
    for (int pass = 0; pass < 2; ++pass) {
        int written = 0;
        while (written < frames) {
            const int chunk = std::min(frames - written, capacity);
            ring.writeStereo(data.buffer.data() + static_cast<size_t>(written) * 2, chunk);
            written += chunk;
            for (;;) {
                const int64_t before = static_cast<int64_t>(ring.readPosition());
                if (analysis.drainOnce() == wma::analysis::AnalysisThread::DrainOutcome::kRingEmpty) break;
                float v[wma::analysis::kSnapshotValueCount];
                if (!snapshot.read(v)) continue;
                Publication p;
                p.posStart = before;
                p.pos = static_cast<int64_t>(ring.readPosition());
                p.state = static_cast<int>(v[wma::analysis::kSnapState]);
                p.rms = static_cast<double>(v[wma::analysis::kSnapLevelRms]);
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
    return run;
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

Row rowOf(const std::string& name, int engage, const Latch& l, const Pass2& p2, int rate) {
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
                                static_cast<double>(p2.dropped)};
    for (size_t i = 0; i < kNumCols; ++i) r.v[i] = v[i];
    return r;
}

std::string baselineHeader() {
    std::string h =
        "# Linea de base del ENGANCHE del afinador sobre corpus-v1 (REQ-055 S0). Una fila por archivo y enganche.\n"
        "# Motor medido: commit 1351b7e (el de hoy, sin cambio de produccion). Al regenerar tras un cambio de motor\n"
        "# INTENCIONAL, actualizar este commit en `baselineHeader()` (test_latch_instrument.cpp).\n"
        "#\n"
        "# Definicion de enganche (acordada con el consumidor): t_reenganche = cambio de objetivo, o (con objetivo\n"
        "# puesto) la primera ventana con senal (rms >= 0,001 del bloque mono drenado) despues de una ventana con\n"
        "# rms < 0,001; el enganche dura hasta el siguiente t_reenganche. Un re-punteo con la cuerda sonando NO es un\n"
        "# enganche. Al empezar a alimentar con objetivo puesto y ring vacio, la primera ventana con senal es t_reenganche.\n"
        "#\n"
        "# Se alimenta la NOTA (noteEndFrames) por AnalysisThread::drainOnce() sin thread, con setTargetHz(hz del\n"
        "# manifiesto) y los candidatos del instrumento, aplicados con el ring VACIO (un drainOnce antes de escribir:\n"
        "# si no, skipToNewest tira el primer bloque, el ataque, como hacia sweepFile). Tiempos en frames del ring (exactos) y en segundos; errores en\n"
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

TEST(LatchInstrumentCorpus, MeasuresTheLatchesAgainstTheCommittedBaseline) {
    const std::string dir = corpus::defaultCorpusDir();
    const corpus::State st = corpus::stateOf(dir, corpus::manifestPath());
    if (st != corpus::State::kVerified) GTEST_SKIP() << missingCorpusMessage(st);

    std::map<std::string, OracleFile> oracle;
    const std::string why = loadOracle(oraclePath(), &oracle);
    ASSERT_EQ(why, "") << "el corpus esta y el oraculo corto no: esto FALLA, no se saltea";

    // Bajo sanitizer cada archivo cuesta varias veces mas: se mide un subconjunto (1 de cada 4) y se
    // compara contra SUS filas; el conjunto completo lo exigen los jobs sin sanitizer.
    std::vector<corpus::Entry> entries = corpus::entriesOf(corpus::manifestPath());
#ifdef WMA_TEST_UNDER_SANITIZER
    constexpr bool kSubset = true;
    {
        std::vector<corpus::Entry> kept;
        for (size_t i = 0; i < entries.size(); i += 4) kept.push_back(entries[i]);
        entries = kept;
    }
#else
    constexpr bool kSubset = false;
#endif

    std::map<std::string, Row> rows;
    std::vector<Latch> allLatches;
    std::vector<double> tFirst, tConv, tFirst2;
    std::printf("\n%-24s %7s %2s | %7s %7s %5s %7s %7s %7s %7s %5s | %7s %7s %3s\n", "archivo", "hz", "n", "t_prim", "t_conv",
                "lect", "e_prim", "med|e|", "p90|e|", "max|e|", "cob", "t_prim2", "t_conv2", "sue");
    for (const corpus::Entry& e : entries) {
        const wav::WavData data = wav::readWav((dir + "/" + e.name).c_str());
        ASSERT_GT(data.sampleRate, 0) << e.name;
        const OracleFile& of = oracle.at(e.name);
        // El oraculo tiene que describir ESTE audio: mismo rate y mismo largo.
        ASSERT_EQ(of.rate, data.sampleRate) << e.name;
        ASSERT_EQ(of.frames, static_cast<long long>(data.numFrames)) << e.name;

        const FedRun run = feedTwice(data, e);
        std::vector<Publication> firstPass;
        for (const Publication& p : run.pubs)
            if (p.pos <= run.frames1) firstPass.push_back(p);
        const Params prm = [&] { Params q; q.sampleRate = run.rate; return q; }();
        const std::vector<Latch> latches = latch::measure(firstPass, run.events, of.segments, prm);
        ASSERT_FALSE(latches.empty()) << e.name << ": la nota no abrio ningun enganche";
        // La definicion, verificada sobre material real: el segundo ataque llega con la cuerda
        // sonando (la nota se corta en un bloque >= 0,002), asi que NO puede abrir enganche.
        EXPECT_EQ(latch::measure(run.pubs, run.events, of.segments, prm).size(), latches.size())
            << e.name << ": el re-punteo abrio un enganche nuevo";

        const Pass2 p2 = measurePass2(run);
        for (size_t i = 0; i < latches.size(); ++i) {
            const Latch& l = latches[i];
            const Row r = rowOf(e.name, static_cast<int>(i) + 1, l, p2, run.rate);
            rows[keyOf(r.name, r.v[0])] = r;
            std::printf("%-24s %7.2f %2zu | %7.3f %7.3f %5d %+7.2f %7.2f %7.2f %7.2f %5.2f | %7.3f %7.3f %3d\n", e.name.c_str(),
                        e.trueHz, i + 1, l.firstReadingSec, l.convergedSec, l.readings, l.firstErrorCents, l.medianAbsCents,
                        l.p90AbsCents, l.maxAbsCents, l.coverage, secOf(p2.firstFrames, run.rate), secOf(p2.convFrames, run.rate),
                        p2.dropped);
            allLatches.push_back(l);
            if (!std::isnan(l.firstReadingSec)) tFirst.push_back(l.firstReadingSec);
            if (!std::isnan(l.convergedSec)) tConv.push_back(l.convergedSec);
        }
        if (p2.firstFrames >= 0) tFirst2.push_back(secOf(p2.firstFrames, run.rate));
    }
    const latch::Pooled pooled = latch::poolLatchErrors(allLatches);
    std::printf("\nenganches=%zu archivos=%zu\n", allLatches.size(), entries.size());
    std::printf("t_primera: n=%zu mediana %.3f max %.3f | t_conv: n=%zu mediana %.3f max %.3f | t_primera2: n=%zu mediana %.3f max %.3f\n",
                tFirst.size(), medianOf(tFirst), maxOf(tFirst), tConv.size(), medianOf(tConv), maxOf(tConv), tFirst2.size(),
                medianOf(tFirst2), maxOf(tFirst2));
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

}  // namespace
}  // namespace wma_test
