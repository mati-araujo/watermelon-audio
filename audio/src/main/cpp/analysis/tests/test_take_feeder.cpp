/**
 * REQ-055 S0 · 0.3 (D37-D42) — EL ALIMENTADOR de las tomas reales: pasa el archivo por el motor como lo vio
 * produccion, con sus cortes y sus cambios de objetivo (`feedTakeWithCuts`).
 *
 * AC-055.1 — los cortes que declara la toma llegan al motor en el frame exacto, y lo que el motor no vio
 * (una costura) no se le da de comer. AC-055.2 — la serie de publicaciones que mide el instrumento esta en
 * la MISMA coordenada que los cortes, los objetivos y los t0 del oraculo.
 *
 * Tomas SINTETICAS: un seno con un salto de fase real en cada corte (lo que deja en el archivo una perdida o
 * una costura de verdad), a 48 kHz, con las alturas de las cuerdas de `catalogStrings()`. Todo corre
 * sincrono (`drainOnce`, sin hilos): no hay nada que esperar y por eso no hay un solo sleep.
 *
 * LO QUE ESTOS TESTS NO FIJAN: los tiempos del estimador (REQ-057 los mueve). Afirman contadores, marca,
 * posiciones del ring y frames analizados, que son del alimentador y del contrato del ring; de la altura
 * publicada, sólo que sigue al objetivo declarado con una tolerancia amplia.
 *
 * La posicion de una publicacion es `ring.readPosition()` cuando `drainOnce()` cierra un tick. Desde REQ-057 (D31)
 * el motor publica cada `AnalysisThread::kTickFrames` frames analizados, contados desde el ultimo ORIGEN del tick:
 * el objetivo aplicado, el aviso cruzado de una perdida (en su frame) o el FIN de una costura (lo que sigue al
 * descarte). Por eso estos tests derivan la serie esperada de esos origenes y de la constante del motor
 * (`takeTickGrid`), y la comparan ENTERA: un aviso corrido un frame corre todas las publicaciones que siguen.
 * Lo que el motor analizo de un tick que un origen corto no se publica nunca: los frames analizados se afirman
 * en cada publicacion (`pos` menos lo descartado antes de `pos`), no contra el largo del archivo.
 */

#include "support/SyntheticTake.h"
#include "support/TakeFeeder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace wma_test::take {
namespace {

namespace corpus_ns = wma_test::corpus;
using wma::analysis::kSnapDiscontinuityCount;
using wma::analysis::kSnapFramesAnalyzed;
using wma::analysis::kSnapInputDiscontinuity;

constexpr int kTakeFeedRate = 48000;

/// La cadencia de publicacion del motor (REQ-057, D31): de la constante, nunca un numero escrito aca.
constexpr int64_t kTakeTick = wma::analysis::AnalysisThread::kTickFrames;

/// La altura de la cuerda `name` del catalogo compartido ("guitarra A2").
double takeStringHz(const std::string& name) {
    for (const auto& s : catalogStrings())
        if (name == s.name) return s.hz;
    return 0.0;
}

/// El archivo como lo deja `readWav`: estereo con el mismo mono en los dos canales.
wav::WavData takeStereoOf(const std::vector<float>& mono, int rate) {
    wav::WavData d;
    d.numFrames = static_cast<int>(mono.size());
    d.sampleRate = rate;
    d.numChannels = 2;
    d.buffer.resize(mono.size() * 2);
    for (size_t i = 0; i < mono.size(); ++i) {
        d.buffer[2 * i] = mono[i];
        d.buffer[2 * i + 1] = mono[i];
    }
    return d;
}

/// Mono de `frames` frames, en silencio digital, con un seno de `hz` (amplitud 0,5) en [from, to).
void takeAddTone(std::vector<float>* mono, int64_t from, int64_t to, double hz) {
    const std::vector<float> t = sineWithPhaseJumps(static_cast<int>(to - from), kTakeFeedRate, hz, 0.5, {});
    for (int64_t i = from; i < to; ++i) (*mono)[static_cast<size_t>(i)] = t[static_cast<size_t>(i - from)];
}

/// Un bloque de +-`amp` alternado en [from, to): su RMS es exactamente `amp` (sirve para fijar un nivel en dBFS).
void takeAddLevel(std::vector<float>* mono, int64_t from, int64_t to, double amp) {
    for (int64_t i = from; i < to; ++i)
        (*mono)[static_cast<size_t>(i)] = static_cast<float>((i & 1) ? amp : -amp);
}

double takeAmpOfDbfs(double dbfs) { return std::pow(10.0, dbfs / 20.0); }

/// La toma base: A2 todo el archivo con un salto de fase en cada frame de `jumps`, y el objetivo A2 en el
/// frame 0 (la ventana de silencio de un objetivo en 0 esta vacia: pasa).
struct TakeBase {
    int64_t frames = 100000;
    std::vector<float> mono;
    std::vector<TakeTargetLine> targets;
    explicit TakeBase(const std::vector<int64_t>& jumps, int64_t n = 100000) : frames(n) {
        const double hz = takeStringHz("guitarra A2");
        mono = sineWithPhaseJumps(static_cast<int>(frames), kTakeFeedRate, hz, 0.5, jumps);
        targets = {{0, hz}};
    }
};

TakeFeedRun takeRun(const TakeBase& b, const std::vector<TakeCutLine>& cuts) {
    return feedTakeWithCuts(takeStereoOf(b.mono, kTakeFeedRate), cuts, b.targets);
}

/// El indice de la primera publicacion con pos > `pos`; -1 si no hay.
int takeFirstPubAfter(const TakeFeedRun& run, int64_t pos) {
    for (size_t i = 0; i < run.pubs.size(); ++i)
        if (run.pubs[i].pos > pos) return static_cast<int>(i);
    return -1;
}

int64_t takeCountAt(const TakeFeedRun& run, size_t i) { return static_cast<int64_t>(run.snaps[i][kSnapDiscontinuityCount]); }

/// Un tramo que el motor integra de corrido: el tick arranca en `from` (un objetivo, el cruce de una perdida, el FIN de
/// una costura) y lo corta `to` (el origen siguiente, el INI de una costura o el final del archivo).
struct TakeTickSpan {
    int64_t from = 0;
    int64_t to = 0;
};

/// Las posiciones de las publicaciones: en cada tramo, `from + kTakeTick·k` (k >= 1) mientras no pase de `to`. El
/// tick que `to` deja a medias no publica.
std::vector<int64_t> takeTickGrid(const std::vector<TakeTickSpan>& spans) {
    std::vector<int64_t> grid;
    for (const TakeTickSpan& sp : spans)
        for (int64_t p = sp.from + kTakeTick; p <= sp.to; p += kTakeTick) grid.push_back(p);
    return grid;
}

std::vector<int64_t> takePositionsOf(const TakeFeedRun& run) {
    std::vector<int64_t> pos;
    for (const latch::Publication& p : run.pubs) pos.push_back(p.pos);
    return pos;
}

/// Que cada publicacion traiga como frames analizados EXACTAMENTE su `pos` menos lo descartado antes de `pos`
/// (`skipIni`/`skipFin`: el tramo de una costura; vacio si skipIni == skipFin). Atrapa descartar de mas o de menos
/// en cualquier punto de la toma, no solo al final.
void takeExpectAnalyzedIsPosMinusSkipped(const TakeFeedRun& run, int64_t skipIni, int64_t skipFin) {
    for (size_t i = 0; i < run.pubs.size(); ++i) {
        const int64_t pos = run.pubs[i].pos;
        const int64_t skipped = pos > skipIni ? std::min(pos, skipFin) - skipIni : 0;
        EXPECT_EQ(pos - std::max<int64_t>(0, skipped), static_cast<int64_t>(run.snaps[i][kSnapFramesAnalyzed]))
            << "frames analizados de la publicacion " << i << " (pos " << pos << ")";
    }
}

/// El aviso de discontinuidad: todas las publicaciones con pos <= `at` traen el contador en 0 y la marca baja; la
/// PRIMERA con pos > `at` lo trae en 1 y la marca alta; todas las que siguen, en 1 (un solo evento).
void takeExpectOneEventAfter(const TakeFeedRun& run, int64_t at) {
    const int first = takeFirstPubAfter(run, at);
    ASSERT_GT(first, 0) << "no hay publicaciones a los dos lados de " << at;
    for (size_t i = 0; i < run.pubs.size(); ++i) {
        const bool after = static_cast<int>(i) >= first;
        EXPECT_EQ(after ? 1 : 0, takeCountAt(run, i)) << "contador de la publicacion " << i << " (pos " << run.pubs[i].pos << ")";
    }
    EXPECT_EQ(0.0f, run.snaps[static_cast<size_t>(first) - 1][kSnapInputDiscontinuity]) << "la marca se levanto antes de " << at;
    EXPECT_EQ(1.0f, run.snaps[static_cast<size_t>(first)][kSnapInputDiscontinuity]) << "la marca viva de la lectura que arrastra el hueco";
}

// ============================ las constantes del contrato ============================

// D37 / D40 / D42. atrapa: una constante del contrato movida sin que lo vea nadie (la tanda de la grilla, la
// tolerancia del aplauso, el umbral de silencio). Los otros tests usan los numeros literales del diseno.
TEST(TakeFeeder, TheContractConstantsAreTheDesignOnes) {
    EXPECT_EQ(256, kTakeChunkFrames);
    EXPECT_EQ(256, kTakeClapToleranceFrames);
    EXPECT_DOUBLE_EQ(-40.0, kTakeTargetSilenceDbfs);
}

// ============================ el flujo alimentado es el archivo ============================

// AC-055.2 / D37 (invariante: todo frame del archivo se escribe al ring, una vez y en orden). La toma pasa POR
// ARCHIVO: header de produccion con una perdida y una costura, `readWav` y el alimentador.
// atrapa: saltear la costura al escribir (el ring se salta el tramo y `pub.pos` queda corrido contra los cortes),
// perder o duplicar un frame al partir una tanda en un corte u objetivo.
TEST(TakeFeeder, TheFedStreamIsTheFileBitForBit) {
    const std::vector<int64_t> jumps = {30000, 60500};
    const TakeBase base(jumps);
    std::vector<SyntheticTakeCut> sc(2);
    sc[0].fileFrame = 30000; sc[0].lost = 12;
    sc[1].fileFrame = 60000; sc[1].span = 1000; sc[1].seam = true;
    const std::string dir = corpus_ns::makeTempDir();
    ASSERT_FALSE(dir.empty());
    const std::string path = dir + "/toma-sintetica.wav";
    ASSERT_TRUE(writeSyntheticTake(path, syntheticTakeBytes(base.mono, syntheticTakeStats(static_cast<uint64_t>(base.frames), kTakeFeedRate, sc))));
    const wav::WavData data = wav::readWav(path.c_str());
    corpus_ns::removeTempDir(dir);
    ASSERT_EQ(base.frames, data.numFrames) << "readWav no leyo la toma";
    ASSERT_EQ(kTakeFeedRate, data.sampleRate);

    // Los cortes parten la grilla; el silencio previo a un objetivo se prueba en otro test (aca el nivel no
    // importa, asi que el objetivo va en el frame 0). `fedMono` se arma del mismo `src` que se
    // escribe: un error SOLO en el argumento de `writeStereo` no lo ve este test (lo ven los de avisos y descarte).
    const std::vector<TakeCutLine> cuts = {{30000, 30000, 12}, {60000, 61000, -1}};
    const TakeFeedRun run = feedTakeWithCuts(data, cuts, base.targets);
    ASSERT_EQ("", run.error);
    EXPECT_EQ(base.frames, run.frames);
    EXPECT_EQ(base.frames, run.ringWritePosition);
    ASSERT_EQ(base.mono.size(), run.fedMono.size());
    EXPECT_EQ(0, std::memcmp(run.fedMono.data(), base.mono.data(), base.mono.size() * sizeof(float)))
        << "lo que se escribio al ring no es el archivo bit a bit";
    ASSERT_FALSE(run.pieceEnds.empty());
    EXPECT_EQ(base.frames, run.pieceEnds.back());
}

// AC-055.2 / D37. atrapa: un aviso o un objetivo que se dispara solo, sin que la toma lo declare (el contador
// subiria o se descartaria algo en una toma sin cortes); y el aviso de un corte en el frame 0 (no hay
// integracion que cortar: una costura en 0 descartaria lo primero de la toma).
TEST(TakeFeeder, ATakeWithoutCutsAndACutAtFrameZeroAreNeverAnnounced) {
    const TakeBase base({});
    const TakeFeedRun none = takeRun(base, {});
    ASSERT_EQ("", none.error);
    ASSERT_FALSE(none.pubs.empty());
    for (size_t i = 0; i < none.pubs.size(); ++i) { EXPECT_EQ(0, takeCountAt(none, i)) << "publicacion " << i; }
    // Sin cortes el unico origen del tick es el objetivo del frame 0: si algo descartara o reiniciara, la serie se
    // correria (y los frames analizados de cada publicacion dejarian de ser su pos).
    EXPECT_EQ(takeTickGrid({{0, base.frames}}), takePositionsOf(none));
    takeExpectAnalyzedIsPosMinusSkipped(none, 0, 0);

    const TakeFeedRun zero = takeRun(base, {{0, 1000, -1}});
    ASSERT_EQ("", zero.error);
    ASSERT_FALSE(zero.pubs.empty());
    for (size_t i = 0; i < zero.pubs.size(); ++i) { EXPECT_EQ(0, takeCountAt(zero, i)) << "publicacion " << i; }
    EXPECT_EQ(takeTickGrid({{0, base.frames}}), takePositionsOf(zero)) << "una costura en el frame 0 no se avisa: no corre el tick";
    takeExpectAnalyzedIsPosMinusSkipped(zero, 0, 0);
    EXPECT_EQ(base.frames, zero.ringWritePosition);
}

// ============================ la perdida ============================

// AC-055.1 / D38. Una perdida en F reinicia la integracion SIN descartar nada, en el frame exacto: el tick vuelve a
// arrancar en F (las publicaciones siguen en F + tick·k), la primera publicacion despues de F trae el contador +1 y
// ninguna de antes lo trae; cada publicacion analizo todos los frames hasta su pos.
// atrapa: sin aviso (el contador no sube y el tick sigue la serie desde 0), aviso en F±1 frame o en F±1 tanda (la
// serie despues de F queda corrida), y la perdida avisada como una costura con descarte (se pierde lo que sigue a F:
// la serie arranca despues y los frames analizados quedan por debajo de pos).
TEST(TakeFeeder, ALossResetsAtItsExactFrameWithoutDiscarding) {
    const int64_t F = 30000;   // no es multiplo de 256 ni de la cadencia: la grilla se parte ahi
    ASSERT_NE(0, F % kTakeChunkFrames);
    ASSERT_NE(0, F % kTakeTick);
    const TakeBase base({F});
    const TakeFeedRun run = takeRun(base, {{F, F, 12}});
    ASSERT_EQ("", run.error);
    ASSERT_EQ(run.pubs.size(), run.snaps.size());

    EXPECT_EQ(takeTickGrid({{0, F}, {F, base.frames}}), takePositionsOf(run)) << "el tick tiene que volver a arrancar en F";
    takeExpectOneEventAfter(run, F);
    takeExpectAnalyzedIsPosMinusSkipped(run, 0, 0);
}

// ============================ la costura ============================

// AC-055.1 / D38. Una costura [INI, FIN] se descarta EXACTAMENTE: el motor no analiza [INI, FIN), el tick vuelve a
// arrancar en FIN (ninguna publicacion cae en (INI, FIN]), la primera despues de FIN ya trae el evento y ninguna de
// antes lo trae, y cada publicacion analizo su pos menos lo descartado antes de ella.
// atrapa: `report(0)` (descarta una pieza y no el tramo), el aviso en FIN (integra a traves del salto y descarta
// de mas), la tanda sin partir en FIN (descarta hasta el final de la tanda, de mas), sin aviso.
TEST(TakeFeeder, ASeamDiscardsExactlyItsSpan) {
    const int64_t INI = 60000, FIN = 61000;   // ni INI ni FIN caen en la grilla ni en la cadencia
    ASSERT_NE(0, FIN % kTakeChunkFrames);
    ASSERT_NE(0, INI % kTakeTick);
    ASSERT_NE(0, FIN % kTakeTick);
    const TakeBase base({60500});
    const TakeFeedRun run = takeRun(base, {{INI, FIN, -1}});
    ASSERT_EQ("", run.error);
    ASSERT_EQ(run.pubs.size(), run.snaps.size());

    EXPECT_EQ(base.frames, run.ringWritePosition) << "todo frame del archivo se escribe al ring";
    for (const latch::Publication& p : run.pubs) {
        EXPECT_FALSE(p.pos > INI && p.pos <= FIN) << "publicacion en pos " << p.pos << " dentro del tramo descartado";
    }
    EXPECT_EQ(takeTickGrid({{0, INI}, {FIN, base.frames}}), takePositionsOf(run)) << "el tick tiene que volver a arrancar en FIN";
    takeExpectAnalyzedIsPosMinusSkipped(run, INI, FIN);
    takeExpectOneEventAfter(run, INI);
}

// AC-055.1 / D38 («pérdida dentro de una costura pendiente: el máximo del ring la absorbe, no adelanta FIN»).
// atrapa: una perdida dentro de la costura que adelanta (o atrasa) su FIN, que la cuenta como un evento mas, o que
// la deja a medias.
TEST(TakeFeeder, ALossInsideASeamDoesNotMoveItsEnd) {
    const int64_t INI = 60000, FIN = 61000, LOSS = 60500;
    const TakeBase base({LOSS});
    const TakeFeedRun run = takeRun(base, {{INI, FIN, -1}, {LOSS, LOSS, 3}});
    ASSERT_EQ("", run.error);
    ASSERT_EQ(run.pubs.size(), run.snaps.size());

    EXPECT_EQ(base.frames, run.ringWritePosition);
    for (const latch::Publication& p : run.pubs) {
        EXPECT_FALSE(p.pos > INI && p.pos <= FIN) << "publicacion en pos " << p.pos << " dentro de la costura";
    }
    EXPECT_EQ(takeTickGrid({{0, INI}, {FIN, base.frames}}), takePositionsOf(run)) << "el descarte sigue siendo exactamente [INI, FIN)";
    takeExpectAnalyzedIsPosMinusSkipped(run, INI, FIN);
    takeExpectOneEventAfter(run, INI);   // la costura y la perdida de adentro son UN evento
}

// ============================ los objetivos ============================

/// La toma de dos cuerdas: un golpe de A2 antes del primer objetivo, silencio, A2 con su objetivo, silencio y E2
/// con el suyo. Cada objetivo tiene su silencio de >= 0,5 s antes.
struct TakeTwoStrings {
    int64_t F1 = 40000, F2 = 130000, frames = 200000;
    double hzA = takeStringHz("guitarra A2");
    double hzE = takeStringHz("guitarra E2");
    std::vector<float> mono = std::vector<float>(200000, 0.0f);
    std::vector<TakeTargetLine> targets;
    TakeTwoStrings() {
        takeAddTone(&mono, 0, 9600, hzA);        // antes del primer objetivo: no hay lectura posible
        takeAddTone(&mono, F1, 100000, hzA);
        takeAddTone(&mono, F2, frames, hzE);
        targets = {{F1, hzA}, {F2, hzE}};
    }
};

/// El ultimo valor finito de `hz` entre las publicaciones con pos en (from, to]; NaN si no hay ninguno.
double takeLastHzIn(const TakeFeedRun& run, int64_t from, int64_t to) {
    double last = std::numeric_limits<double>::quiet_NaN();
    for (const latch::Publication& p : run.pubs)
        if (p.pos > from && p.pos <= to && !std::isnan(p.hz)) last = p.hz;
    return last;
}

double takeCentsBetween(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }

// AC-055.2 / D39. Los objetivos se aplican con el ring VACIO: antes del primer objetivo no hay lectura (con un
// golpe de A2 sonando), cada objetivo queda registrado en su frame y el tick vuelve a arrancar ahi, el motor analiza
// TODOS los frames (el `skipToNewest` no descarta nada: cada publicacion analizo su pos) y la altura publicada sigue
// a la cuerda declarada, no a la anterior.
// atrapa: aplicar el objetivo sin drenar (el primer drain despues descarta la pieza siguiente: el tick arranca tarde y
// los frames analizados quedan por debajo de pos), un objetivo aplicado antes o despues de su frame, una lectura con
// hz sin objetivo, y un objetivo nuevo que no llega al motor (la lectura se queda en la cuerda anterior).
TEST(TakeFeeder, TargetsApplyWithTheRingEmpty) {
    const TakeTwoStrings t;
    const TakeFeedRun run = feedTakeWithCuts(takeStereoOf(t.mono, kTakeFeedRate), {}, t.targets);
    ASSERT_EQ("", run.error);
    EXPECT_EQ(t.frames, run.ringWritePosition);
    ASSERT_EQ(run.pubs.size(), run.snaps.size());
    EXPECT_EQ(takeTickGrid({{0, t.F1}, {t.F1, t.F2}, {t.F2, t.frames}}), takePositionsOf(run)) << "el tick arranca en cada objetivo";
    takeExpectAnalyzedIsPosMinusSkipped(run, 0, 0);   // el objetivo con el ring vacio no descarta ni un frame

    ASSERT_EQ(2u, run.events.size());
    EXPECT_EQ(t.F1, run.events[0].pos);
    EXPECT_EQ(t.F2, run.events[1].pos);
    EXPECT_TRUE(run.events[0].hasTarget);
    EXPECT_TRUE(run.events[1].hasTarget);

    int before = 0;
    for (const latch::Publication& p : run.pubs) {
        if (p.pos > t.F1) break;
        ++before;
        EXPECT_TRUE(std::isnan(p.hz)) << "lectura con hz en pos " << p.pos << " antes del primer objetivo";
    }
    EXPECT_EQ(t.F1 / kTakeTick, before) << "el golpe de antes del objetivo se alimento (si no, la ausencia de lecturas no prueba nada)";

    const double a = takeLastHzIn(run, t.F1, t.F2);
    ASSERT_FALSE(std::isnan(a)) << "ninguna lectura de A2 con su objetivo";
    EXPECT_LT(std::fabs(takeCentsBetween(a, t.hzA)), 50.0) << "la ultima lectura de la primera cuerda: " << a << " Hz";
    const double e = takeLastHzIn(run, t.F2, t.frames);
    ASSERT_FALSE(std::isnan(e)) << "ninguna lectura de E2 con su objetivo";
    EXPECT_LT(std::fabs(takeCentsBetween(e, t.hzE)), 50.0) << "la ultima lectura de la segunda cuerda: " << e << " Hz";
}

// ============================ la grilla ============================

// AC-055.2 / D37. Las piezas escritas terminan EXACTAMENTE en los multiplos de 256 desde el frame 0, mas los
// cortes (INI de cada uno y FIN de cada costura), mas los objetivos, mas el final.
// atrapa: re-alinear la grilla en el corte (las tandas siguientes quedarian en INI + 256·k), partir de mas o no
// partir en un FIN o en un objetivo.
TEST(TakeFeeder, TheGridStaysAlignedToFrameZero) {
    const int64_t frames = 100123;
    ASSERT_NE(0, frames % kTakeChunkFrames);
    const double hz = takeStringHz("guitarra A2");
    std::vector<float> mono(static_cast<size_t>(frames), 0.0f);
    takeAddTone(&mono, 0, 66000, hz);
    const std::vector<TakeCutLine> cuts = {{30000, 30000, 12}, {60000, 61000, -1}, {61100, 61100, 4}};
    const std::vector<TakeTargetLine> targets = {{0, hz}, {90000, hz}};   // el de 90000: silencio desde 66000
    const TakeFeedRun run = feedTakeWithCuts(takeStereoOf(mono, kTakeFeedRate), cuts, targets);
    ASSERT_EQ("", run.error);

    std::set<int64_t> want;
    for (int64_t m = kTakeChunkFrames; m < frames; m += kTakeChunkFrames) want.insert(m);
    for (const TakeCutLine& c : cuts) {
        want.insert(c.ini);
        want.insert(c.fin);
    }
    for (const TakeTargetLine& t : targets)
        if (t.frame > 0) want.insert(t.frame);
    want.insert(frames);
    const std::vector<int64_t> expected(want.begin(), want.end());
    EXPECT_EQ(expected, run.pieceEnds);
}

// ============================ lo que no es punteo ============================

// D40. Un ataque antes del primer objetivo, o a <= 256 frames del aplauso, no es un punteo: sigue siendo limite de
// tEnd pero no se mide. Bordes de los dos lados del aplauso (256 si, 257 no) y del primer objetivo (== no, -1 si).
// atrapa: `<` por `<=` (o al reves) en la tolerancia, mirar un solo lado del aplauso, tomar el primer objetivo
// como no punteo, no mirar todos los aplausos, y una toma sin objetivos con ataques que si se miden.
TEST(TakeFeeder, NonPluckAttacksAreCountedApart) {
    const std::vector<TakeTargetLine> targets = {{20000, 82.407}, {150000, 110.0}};
    const std::vector<int64_t> claps = {50000};
    EXPECT_TRUE(isNonPluckAttack(50000, targets, claps)) << "el aplauso";
    EXPECT_TRUE(isNonPluckAttack(50000 - 256, targets, claps)) << "borde 256 antes";
    EXPECT_FALSE(isNonPluckAttack(50000 - 257, targets, claps)) << "257 antes ya es un punteo";
    EXPECT_TRUE(isNonPluckAttack(50000 + 256, targets, claps)) << "borde 256 despues";
    EXPECT_FALSE(isNonPluckAttack(50000 + 257, targets, claps)) << "257 despues ya es un punteo";

    EXPECT_FALSE(isNonPluckAttack(20000, targets, claps)) << "t0 == primer objetivo: ya hay lectura posible";
    EXPECT_TRUE(isNonPluckAttack(19999, targets, claps)) << "un frame antes del primer objetivo";
    EXPECT_TRUE(isNonPluckAttack(0, targets, claps));
    EXPECT_FALSE(isNonPluckAttack(120000, targets, claps)) << "un punteo comun, entre objetivos";

    const std::vector<int64_t> two = {50000, 90000};
    EXPECT_TRUE(isNonPluckAttack(90000 + 100, targets, two)) << "se miran todos los aplausos";
    EXPECT_TRUE(isNonPluckAttack(30000, {}, {})) << "sin objetivos no hay lectura posible";
    EXPECT_FALSE(isNonPluckAttack(30000, targets, {})) << "sin aplauso declarado solo cuenta el primer objetivo";
}

// ============================ el silencio de un objetivo ============================

// D42. La ventana de silencio de un objetivo en `frame`: medio segundo (en segundos, no en frames: a 44,1 kHz son
// 22050), sin pasar de 0, y sin cruzar un corte (del corte, solo el lado de FRAME). Una perdida EN el frame del
// objetivo no cruza la ventana.
// atrapa: la ventana en frames fijos de 48 kHz, el negativo sin recortar, cruzar un corte, correrse al INI en
// vez de al FIN de la costura, o mover la ventana por una perdida que esta justo en el frame.
TEST(TakeFeeder, TheSilenceWindowIsHalfASecondAndNeverCrossesACut) {
    EXPECT_EQ(76000, takeSilenceWindowStart(100000, 48000, {}));
    EXPECT_EQ(77950, takeSilenceWindowStart(100000, 44100, {}));
    EXPECT_EQ(0, takeSilenceWindowStart(10000, 48000, {})) << "no antes de 0";
    EXPECT_EQ(0, takeSilenceWindowStart(0, 48000, {}));
    EXPECT_EQ(90000, takeSilenceWindowStart(100000, 48000, {{90000, 90000, 5}})) << "una perdida adentro: desde ella";
    EXPECT_EQ(98000, takeSilenceWindowStart(100000, 48000, {{95000, 98000, -1}})) << "una costura adentro: desde su FIN";
    EXPECT_EQ(98000, takeSilenceWindowStart(100000, 48000, {{90000, 90000, 5}, {95000, 98000, -1}})) << "el ultimo corte de adentro";
    EXPECT_EQ(76000, takeSilenceWindowStart(100000, 48000, {{100000, 100000, 5}})) << "una perdida en el frame no cruza la ventana";
    EXPECT_EQ(76000, takeSilenceWindowStart(100000, 48000, {{70000, 70000, 5}, {72000, 75000, -1}})) << "cortes de antes de la ventana no la mueven";
}

/// Corre el alimentador sobre un archivo en memoria de `frames` frames a `rate`, con los bloques de nivel dados y
/// UN objetivo en `at`.
struct TakeLevelBlock {
    int64_t from = 0;
    int64_t to = 0;
    double amp = 0.0;
};

TakeFeedRun takeRunWithLevels(int64_t frames, int rate, const std::vector<TakeLevelBlock>& blocks, int64_t at,
                              const std::vector<TakeCutLine>& cuts = {}) {
    std::vector<float> mono(static_cast<size_t>(frames), 0.0f);
    for (const TakeLevelBlock& b : blocks) takeAddLevel(&mono, b.from, b.to, b.amp);
    return feedTakeWithCuts(takeStereoOf(mono, rate), cuts, {{at, takeStringHz("guitarra A2")}});
}

/// El objetivo se aplico (hay un evento y no hay error) / se rechazo (error y ningun evento, la corrida corta ahi).
void takeExpectApplied(const TakeFeedRun& run, int64_t at, const std::string& why) {
    EXPECT_EQ("", run.error) << why;
    ASSERT_EQ(1u, run.events.size()) << why;
    EXPECT_EQ(at, run.events[0].pos) << why;
}

void takeExpectRejected(const TakeFeedRun& run, int64_t at, const std::string& why) {
    EXPECT_NE("", run.error) << why << ": la corrida no fallo";
    EXPECT_TRUE(run.events.empty()) << why << ": el objetivo se aplico igual";
    EXPECT_LE(run.frames, at) << why << ": la corrida siguio despues del frame del objetivo";
    EXPECT_NE(std::string::npos, run.error.find(std::to_string(static_cast<long long>(at)))) << why << ": el error no dice el frame: " << run.error;
}

// D42 / D39. Un objetivo que cae sobre sonido se RECHAZA: la corrida falla diciendo el FRAME y el objetivo no se
// aplica. Los bordes son -40,1 dBFS (pasa) y -39,9 dBFS (falla) en [FRAME - 0,5 s, FRAME).
// atrapa: aplicar igual (y avisar despues), medir DESPUES de FRAME (el sonido que sigue al objetivo, que es lo
// normal, lo rechazaria; y el que lo precede pasaria), una ventana que cruza el corte (sonido de antes de un corte,
// con silencio entre el corte y FRAME, rechazaria una toma buena), el umbral movido, y la ventana en frames de 48 kHz
// a otro rate.
TEST(TakeFeeder, ATargetOutsideSilenceIsRejected) {
    const int64_t F = 60000, frames = 100000, w = 24000;   // 0,5 s a 48 kHz

    // Los bordes del umbral.
    takeExpectApplied(takeRunWithLevels(frames, 48000, {{F - w, F, takeAmpOfDbfs(-40.1)}}, F), F, "-40,1 dBFS");
    takeExpectRejected(takeRunWithLevels(frames, 48000, {{F - w, F, takeAmpOfDbfs(-39.9)}}, F), F, "-39,9 dBFS");

    // Sonido en cualquier parte de la ventana la rechaza; en silencio digital pasa.
    takeExpectRejected(takeRunWithLevels(frames, 48000, {{F - 12000, F - 11000, 0.5}}, F), F, "un golpe a mitad de la ventana");
    takeExpectApplied(takeRunWithLevels(frames, 48000, {}, F), F, "silencio digital");

    // Medir DESPUES de FRAME: el sonido que sigue al objetivo (la cuerda tocada) no lo rechaza...
    takeExpectApplied(takeRunWithLevels(frames, 48000, {{F, F + w, 0.5}}, F), F, "sonido fuerte justo despues de FRAME");
    // ... y el que lo precede si.
    takeExpectRejected(takeRunWithLevels(frames, 48000, {{F - w, F, 0.5}}, F), F, "sonido fuerte justo antes de FRAME");

    // Ventana que cruza un corte: de un corte solo cuenta el lado de FRAME. El sonido de ANTES del corte no cuenta...
    const int64_t c = F - 1000;
    takeExpectApplied(takeRunWithLevels(frames, 48000, {{F - w, c, 0.5}}, F, {{c, c, 5}}), F, "sonido antes de una perdida, silencio entre ella y FRAME");
    // ... el de DESPUES del corte (el lado de FRAME) si.
    takeExpectRejected(takeRunWithLevels(frames, 48000, {{c, F, 0.5}}, F, {{c, c, 5}}), F, "sonido entre la perdida y FRAME");
    // Una costura: la ventana empieza en su FIN (el INI y el tramo de adentro no cuentan).
    takeExpectApplied(takeRunWithLevels(frames, 48000, {{F - w, F - 1000, 0.5}}, F, {{F - 3000, F - 1000, -1}}), F, "sonido hasta el FIN de una costura");

    // A 44,1 kHz la ventana son 22050 frames: el sonido pegado a su inicio, afuera, no cuenta.
    takeExpectApplied(takeRunWithLevels(frames, 44100, {{F - 30000, F - 22050, 0.5}}, F), F, "a 44100 Hz, sonido justo antes de la ventana");
    takeExpectRejected(takeRunWithLevels(frames, 44100, {{F - 22050, F, 0.5}}, F), F, "a 44100 Hz, sonido en toda la ventana");
}

}  // namespace
}  // namespace wma_test::take
