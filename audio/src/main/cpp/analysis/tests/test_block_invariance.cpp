/**
 * test_block_invariance.cpp — REQ-057 S1 · AC-057.5 («el bloque no cambia la lectura») y el
 * trinquete de REQ-036 de AC-057.1 medido con el motor independiente del bloque (D27).
 *
 * POR QUE EXISTE. Hoy la serie de publicaciones depende del tamaño de bloque con que llega el
 * audio: con bloques que no dividen 4096, `t_primera` cambia en 40 de 41 archivos de `corpus-v1`
 * (medido por REQ-055 S0; `bajo-fretless_D2` 2,51 s contra ≈ 1,21 s). La dependencia esta en
 * `AnalysisThread` (el reenganche del modo rapido se aplica al final del drenado y su
 * `skipToNewest()` tira lo que quede en el ring; el modo rapido y el nivel se evaluan por drenado),
 * no en el estimador. El arreglo (D-D, D31) es un reloj de tick de 1024 frames absolutos.
 *
 * EL ARNES. Un `AnalysisThread` sin `start()`, conducido con `drainOnce()`; el objetivo y los
 * candidatos se aplican CON EL RING VACIO (un `drainOnce()` antes de escribir nada); se escribe de a
 * `block` frames (16, 256, 1000 y 8192 —la capacidad del ring—) y se drena hasta `kRingEmpty` tras
 * cada escritura. `snapshot.read()` devuelve el ULTIMO publicado aunque no haya uno nuevo, asi que
 * cada publicacion nueva se deduplica por `kSnapFramesAnalyzed`.
 */

#include "support/Corpus.h"
#include "support/CorpusSweep.h"
#include "support/SyntheticSignal.h"

#include "tests/support/TestSanitizer.h"

#include "AnalysisRing.h"
#include "AnalysisSnapshot.h"
#include "AnalysisThread.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace wma_test {
namespace {

using wma::analysis::AnalysisRing;
using wma::analysis::AnalysisSnapshot;
using wma::analysis::AnalysisThread;
using Outcome = AnalysisThread::DrainOutcome;

/// Los cuatro bloques de AC-057.5. 8192 es la capacidad del ring: llena el ring de un golpe.
constexpr int kBlocks[] = {16, 256, 1000, 8192};
constexpr int kBlockCount = 4;

struct Pub {
    float frames = 0.0f;       ///< kSnapFramesAnalyzed
    int state = -1;            ///< kSnapState
    uint32_t centsBits = 0;    ///< kSnapCents, BIT A BIT (NaN incluido)
};

bool operator==(const Pub& a, const Pub& b) {
    return a.frames == b.frames && a.state == b.state && a.centsBits == b.centsBits;
}

/// Lo que un consumidor ve de un archivo (o de una señal) con un tamaño de bloque dado.
struct BlockRun {
    bool setupOk = false;                  ///< el drainOnce() con el ring vacio devolvio kRingEmpty
    std::vector<Pub> series;
    double firstFineFrames = NAN;          ///< t_primera: primera publicacion con kSnapCents no NaN
    double convergedFrames = NAN;          ///< t_conv: primera con kStateConverged
    int accumulated = 0;                   ///< drainOnce() == kAccumulated
    int published = 0;                     ///< drainOnce() == kPublished
    int contractViolations = 0;            ///< kPublished sin publicacion nueva, o kAccumulated con una
    uint64_t fastModeApplied = 0;          ///< targetAppliedByFastMode()
    // La ULTIMA lectura fina, como `sweepFile` (REQ-032): estado de esa publicacion y su error
    // en Hz absolutos contra el hz verdadero, con el objetivo VIGENTE del strobe.
    bool hasFine = false;
    int lastFineState = -1;
    double lastFineErrCents = NAN;
};

/**
 * Corre el motor sobre `frames` cuadros estereo intercalados con tamaño de bloque `block`.
 * `trueHz` (si > 0) es contra lo que se mide el error de la ultima lectura fina.
 */
BlockRun runAtBlock(const float* stereo, int frames, int rate, double target,
               const std::vector<double>& candidates, double trueHz, int block) {
    BlockRun r;
    auto ring = std::make_unique<AnalysisRing>();
    auto snap = std::make_unique<AnalysisSnapshot>();
    auto th = std::make_unique<AnalysisThread>(*ring, *snap);
    ring->setCaptureRate(rate);
    th->setTargetHz(target);
    th->setCandidates(candidates.empty() ? nullptr : candidates.data(), static_cast<int>(candidates.size()));
    // Objetivo y candidatos con el ring VACIO: no hay nada que descartar al aplicarlos.
    if (th->drainOnce() != Outcome::kRingEmpty) return r;
    r.setupOk = true;

    float lastFrames = -1.0f;
    for (int off = 0; off < frames; off += block) {
        ring->writeStereo(stereo + static_cast<size_t>(off) * 2, std::min(block, frames - off));
        for (;;) {
            const Outcome o = th->drainOnce();
            if (o == Outcome::kRingEmpty) break;
            float v[wma::analysis::kSnapshotValueCount];
            const bool have = snap->read(v);
            const bool isNew = have && v[wma::analysis::kSnapFramesAnalyzed] != lastFrames;
            if (o == Outcome::kPublished) {
                ++r.published;
                if (!isNew) ++r.contractViolations;
            } else if (o == Outcome::kAccumulated) {
                ++r.accumulated;
                if (isNew) ++r.contractViolations;
            }
            if (!isNew) continue;

            lastFrames = v[wma::analysis::kSnapFramesAnalyzed];
            Pub p;
            p.frames = lastFrames;
            p.state = static_cast<int>(v[wma::analysis::kSnapState]);
            std::memcpy(&p.centsBits, &v[wma::analysis::kSnapCents], sizeof p.centsBits);
            r.series.push_back(p);

            const float cents = v[wma::analysis::kSnapCents];
            if (!std::isnan(cents)) {
                if (std::isnan(r.firstFineFrames)) r.firstFineFrames = p.frames;
                r.hasFine = true;
                r.lastFineState = p.state;
                if (trueHz > 0.0) {
                    const double hz = th->strobe().targetHz() * std::pow(2.0, static_cast<double>(cents) / 1200.0);
                    r.lastFineErrCents = 1200.0 * std::log2(hz / trueHz);
                }
            }
            if (p.state == wma::analysis::kStateConverged && std::isnan(r.convergedFrames))
                r.convergedFrames = p.frames;
        }
    }
    r.fastModeApplied = th->targetAppliedByFastMode();
    return r;
}

/// `NaN == NaN` es false: «nunca llego» tiene que contar como igual a «nunca llego».
bool sameOrBothNaN(double a, double b) { return a == b || (std::isnan(a) && std::isnan(b)); }

std::vector<float> toStereo(const std::vector<float>& mono) {
    std::vector<float> s(mono.size() * 2);
    for (size_t i = 0; i < mono.size(); ++i) s[2 * i] = s[2 * i + 1] = mono[i];
    return s;
}

/// Primera diferencia entre dos series, o "" si son identicas.
std::string firstDifference(const std::vector<Pub>& ref, const std::vector<Pub>& other) {
    char buf[200];
    if (ref.size() != other.size()) {
        std::snprintf(buf, sizeof buf, "largo %zu contra %zu", ref.size(), other.size());
        return buf;
    }
    for (size_t i = 0; i < ref.size(); ++i) {
        if (!(ref[i] == other[i])) {
            float a, b;
            std::memcpy(&a, &ref[i].centsBits, sizeof a);
            std::memcpy(&b, &other[i].centsBits, sizeof b);
            std::snprintf(buf, sizeof buf, "publicacion %zu: frames %.0f/%.0f estado %d/%d cents %g/%g", i,
                          ref[i].frames, other[i].frames, ref[i].state, other[i].state, a, b);
            return buf;
        }
    }
    return "";
}

std::vector<double> guitarCandidates() {
    std::vector<double> hz;
    for (const CatalogString& s : catalogStrings())
        if (std::string(s.name).rfind("guitarra", 0) == 0) hz.push_back(s.hz);
    return hz;
}

// ---------------------------------------------------------------------------
// AC-057.5 — sintetico, corre siempre (sin corpus)
// ---------------------------------------------------------------------------

/**
 * AC-057.5 («la misma serie de publicaciones sobre el mismo audio, cualquiera sea el tamaño de
 * bloque: 16, 256, 1000 y 8192 frames») y D31 («sin publicación antes de 1024 frames; snapshot y rms
 * cada 1024»).
 *
 * Tres señales de 4 s a 48 kHz con candidatos de guitarra (el camino donde el consumidor mide):
 *   · E2 a +30 c con objetivo E2 (dentro del rango de hoy y del nuevo);
 *   · E4 a +40 c con objetivo E4 (FUERA del rango de hoy, 30,5 c; dentro del nuevo, 107,4 c);
 *   · A2 a +10 c con el objetivo del usuario en E2: el modo rapido tiene que REENGANCHAR a A2 —el
 *     caso que en el motor de hoy depende del bloque, porque el reenganche tira el ring—.
 *
 * Las cuatro series (frames analizados, estado, cents bit a bit) son identicas; ninguna publicacion
 * con menos de 1024 frames; dos publicaciones consecutivas distan exactamente un tick; y el
 * contrato de `kAccumulated`: «consumio sin publicar» (un `kPublished` trae una publicacion nueva,
 * un `kAccumulated` no).
 *
 * Mutante: drenar `kDrainFrames` (2048) sin reloj de tick ⇒ la cadencia depende de cuanto haya en
 * el ring y de donde caiga el corte (rojo en la serie y en «exactamente un tick»); o el reenganche
 * del modo rapido con `skipToNewest()` ⇒ con bloque 8192 se tiran frames de la cuerda nueva y la
 * serie del caso A2 cambia.
 */
TEST(BlockSize, ThePublicationSeriesDoesNotDependOnTheBlockSize) {
    // AC-057.5
    constexpr int kRate = 48000;
    constexpr int kFrames = 4 * kRate;
    struct Signal { const char* label; double f0; double dev; double target; bool mustRetarget; };
    const Signal kSignals[] = {{"E2 +30 c, objetivo E2", 82.407, +30.0, 82.407, false},
                               {"E4 +40 c, objetivo E4 (fuera del rango de hoy)", 329.628, +40.0, 329.628, false},
                               {"A2 +10 c, objetivo del usuario E2 (reenganche del modo rapido)", 110.0, +10.0, 82.407, true}};
    const std::vector<double> cands = guitarCandidates();

    for (const Signal& sg : kSignals) {
        SCOPED_TRACE(sg.label);
        const std::vector<float> stereo = toStereo(inharmonicString(detune(sg.f0, sg.dev), 1.0e-4, 4, kRate, kFrames));
        BlockRun runs[kBlockCount];
        for (int b = 0; b < kBlockCount; ++b) {
            runs[b] = runAtBlock(stereo.data(), kFrames, kRate, sg.target, cands, detune(sg.f0, sg.dev), kBlocks[b]);
            ASSERT_TRUE(runs[b].setupOk) << "bloque " << kBlocks[b] << ": el drainOnce() con el ring vacio no devolvio kRingEmpty";
            ASSERT_FALSE(runs[b].series.empty()) << "bloque " << kBlocks[b] << ": no publico nada";
        }

        const BlockRun& ref = runs[1];   // 256
        ASSERT_FALSE(std::isnan(ref.firstFineFrames)) << "la señal no produjo ninguna lectura fina: el test no mide nada";
        if (sg.mustRetarget) {
            EXPECT_GE(ref.fastModeApplied, 1u) << "el modo rapido no reengancho: el escenario no ejerce el caso";
        }
        for (int b = 0; b < kBlockCount; ++b) {
            const BlockRun& r = runs[b];
            const std::string diff = firstDifference(ref.series, r.series);
            EXPECT_TRUE(diff.empty())
                << "con bloque " << kBlocks[b] << " la serie difiere de la de 256 — " << diff
                << " (AC-057.5: el bloque del llamador no puede cambiar la lectura)";
            EXPECT_TRUE(sameOrBothNaN(r.firstFineFrames, ref.firstFineFrames))
                << "t_primera depende del bloque " << kBlocks[b] << ": " << r.firstFineFrames << " contra " << ref.firstFineFrames;
            EXPECT_TRUE(sameOrBothNaN(r.convergedFrames, ref.convergedFrames))
                << "t_conv depende del bloque " << kBlocks[b] << ": " << r.convergedFrames << " contra " << ref.convergedFrames;
            EXPECT_EQ(r.fastModeApplied, ref.fastModeApplied) << "bloque " << kBlocks[b] << ": cuantas veces reengancho el modo rapido";

            // D31
            EXPECT_GE(r.series.front().frames, static_cast<float>(AnalysisThread::kTickFrames))
                << "bloque " << kBlocks[b] << ": publico con menos de un tick (1024 frames) analizado";
            for (size_t i = 1; i < r.series.size(); ++i) {
                ASSERT_EQ(r.series[i].frames - r.series[i - 1].frames, static_cast<float>(AnalysisThread::kTickFrames))
                    << "bloque " << kBlocks[b] << ": las publicaciones " << i - 1 << " y " << i
                    << " no distan exactamente un tick (D31: snapshot cada 1024 frames)";
            }
            // kAccumulated = «consumio sin publicar»
            EXPECT_EQ(r.contractViolations, 0)
                << "bloque " << kBlocks[b] << ": un drainOnce() dijo kPublished sin publicacion nueva, o kAccumulated con una";
        }
        // Con bloques mas chicos que un tick tiene que haber vueltas que acumulan sin publicar.
        EXPECT_GT(runs[0].accumulated, 0) << "con bloque 16 ningun drainOnce() devolvio kAccumulated";
        EXPECT_GT(runs[1].accumulated, 0) << "con bloque 256 ningun drainOnce() devolvio kAccumulated";
    }
}

// ---------------------------------------------------------------------------
// AC-057.1 / AC-057.5 — el trinquete de REQ-036 sobre corpus-v1, igual en los cuatro bloques
// ---------------------------------------------------------------------------

#ifdef WMA_TEST_UNDER_SANITIZER
/// Bajo sanitizer 4 bloques × 41 archivos exceden el techo del job: se barre el MISMO subconjunto
/// declarado que `test_corpus_gate.cpp` (los archivos con hallazgo + uno de cada tres) y el
/// trinquete de conteo NO se afirma (necesita los 41): la invariancia al bloque si.
std::vector<corpus::Entry> entriesForThisBuild() {
    const std::vector<corpus::Entry> all = corpus::entriesOf(corpus::manifestPath());
    const char* const kAlways[] = {"ukelele_C4.wav",    "guitarra-nylon_E4.wav", "guitarra-jazz_E4.wav",
                                   "ukelele_G4.wav",    "guitarra-acero_A2.wav", "bajo-acustico_G2.wav",
                                   "bajo-pua_A1.wav",   "guitarra-jazz_E2.wav"};
    std::vector<corpus::Entry> kept;
    for (size_t i = 0; i < all.size(); ++i) {
        bool keep = (i % 3 == 0);
        for (const char* k : kAlways) keep = keep || all[i].name == k;
        if (keep) kept.push_back(all[i]);
    }
    return kept;
}
#else
std::vector<corpus::Entry> entriesForThisBuild() { return corpus::entriesOf(corpus::manifestPath()); }
#endif

/**
 * AC-057.1 («≥ 37 convergidas de 41 en `corpus-v1` y error máximo ≤ 0,30 c, con el objetivo
 * aplicado con el ring vacío y el mismo resultado con bloques de 16, 256, 1000 y 8192») y AC-057.5
 * («t_primera, t_conv y los cents» iguales) sobre el corpus real.
 *
 * 🔴 D27 (humano, 2026-10-28): EL 39/41 DE ANTES NO LO DABA EL MOTOR, LO DABA EL ARNES. `sweepFile`
 * alimenta tandas de 8192 y descarta el ring al aplicar el objetivo (`skipToNewest()`), asi que la
 * cuerda arranca a 0,37 s y el glide del ataque queda afuera. Con el motor independiente del bloque
 * y el objetivo aplicado con el ring vacio, el prototipo del architect midio **37/41 y 0,2997 c
 * (`bajo-dedos_D2`, el mismo de hoy)** para los cuatro bloques; salen `ukelele_A4` y
 * `bajo-acustico_G2` (reinicio por tendencia en el glide del ataque que el arnes descartaba).
 * Recuperarlos es de REQ-058 (D14). El trinquete de `test_corpus_gate.cpp`
 * (`kConvergedAfterReq036 = 39`) se MUDO aca: ver el comentario que dejo en su lugar.
 *
 * «Termina CONVERGED» = la ultima publicacion con lectura fina esta en `kStateConverged` (la misma
 * definicion que `sweepFile`/`readingState`); su error es el de esa lectura en Hz absolutos contra
 * `trueHz`, con el objetivo vigente de `strobe().targetHz()`.
 *
 * Mutante: la rejilla alineada al bloque del llamador (la clase de defecto de hoy) ⇒ `t_primera`
 * cambia con el bloque y el test lista los archivos; el ratchet baja de 37 si una rejilla nueva
 * empeora la fina, o sube el error maximo sobre 0,30 c.
 */
TEST(CorpusRobustness, TheRatchetHoldsForEveryBlockSize) {
    // AC-057.1, AC-057.5
    const auto st = corpus::stateOf(corpus::defaultCorpusDir(), corpus::manifestPath());
    if (!corpus::shouldRunRobustness(st)) {
        GTEST_SKIP() << "sin corpus grabado (" << corpus::describe(st) << ")";
    }
    constexpr int kRatchetConverged = 37;      // D27
    constexpr double kRatchetMaxErrorCents = 0.30;

    const std::vector<corpus::Entry> all = corpus::entriesOf(corpus::manifestPath());
    const std::vector<corpus::Entry> entries = entriesForThisBuild();
    ASSERT_FALSE(entries.empty());
    const bool fullCorpus = entries.size() == all.size();

    std::vector<BlockRun> runs[kBlockCount];
    for (const corpus::Entry& e : entries) {
        const wav::WavData data =
            wav::readWav((corpus::defaultCorpusDir() + "/" + e.name).c_str());
        ASSERT_TRUE(data.numFrames > 0 && data.sampleRate > 0) << e.name << ": no se pudo leer";
        const int frames = corpus::noteEndFrames(data);   // se alimenta LA NOTA, como el barrido
        ASSERT_GT(frames, 0) << e.name;
        for (int b = 0; b < kBlockCount; ++b) {
            runs[b].push_back(runAtBlock(data.buffer.data(), frames, data.sampleRate, e.trueHz,
                                         e.candidatesHz, e.trueHz, kBlocks[b]));
            ASSERT_TRUE(runs[b].back().setupOk) << e.name << " bloque " << kBlocks[b];
        }
    }

    // --- el trinquete, por bloque --------------------------------------------------------------
    std::printf("\n  [REQ-057 · D27] trinquete de REQ-036 por tamaño de bloque (%zu de %zu archivos%s)\n",
                entries.size(), all.size(), fullCorpus ? "" : ", subconjunto bajo sanitizer");
    for (int b = 0; b < kBlockCount; ++b) {
        int converged = 0;
        double maxErr = 0.0;
        std::string worst = "-";
        for (size_t i = 0; i < entries.size(); ++i) {
            const BlockRun& r = runs[b][i];
            if (!r.hasFine || r.lastFineState != wma::analysis::kStateConverged) continue;
            ++converged;
            if (std::fabs(r.lastFineErrCents) > maxErr) {
                maxErr = std::fabs(r.lastFineErrCents);
                worst = entries[i].name;
            }
        }
        std::printf("    bloque %5d: %2d convergidas, error maximo %.4f c (%s)\n", kBlocks[b], converged, maxErr,
                    worst.c_str());
        RecordProperty("convergidas_bloque_" + std::to_string(kBlocks[b]), converged);
        RecordProperty("max_error_milicents_bloque_" + std::to_string(kBlocks[b]), static_cast<int>(maxErr * 1000.0));

        if (fullCorpus) {
            EXPECT_GE(converged, kRatchetConverged)
                << "bloque " << kBlocks[b] << ": bajaron las convergidas de corpus-v1 (" << converged
                << " de " << entries.size() << "; D27 fija ≥ " << kRatchetConverged << ")";
        }
        EXPECT_LE(maxErr, kRatchetMaxErrorCents)
            << "bloque " << kBlocks[b] << ": subio el error maximo de las convergidas (" << maxErr << " c en "
            << worst << "; D27 fija ≤ " << kRatchetMaxErrorCents << " c)";
    }
    std::printf("\n");

    // --- la serie, igual en los cuatro bloques (AC-057.5) ---------------------------------------
    std::vector<std::string> differing;
    for (size_t i = 0; i < entries.size(); ++i) {
        const BlockRun& ref = runs[0][i];
        for (int b = 1; b < kBlockCount; ++b) {
            const BlockRun& r = runs[b][i];
            std::string why = firstDifference(ref.series, r.series);
            if (why.empty() && !sameOrBothNaN(ref.firstFineFrames, r.firstFineFrames)) why = "t_primera";
            if (why.empty() && !sameOrBothNaN(ref.convergedFrames, r.convergedFrames)) why = "t_conv";
            if (!why.empty()) {
                char line[300];
                std::snprintf(line, sizeof line, "%s: bloque %d contra %d — %s", entries[i].name.c_str(),
                              kBlocks[b], kBlocks[0], why.c_str());
                differing.push_back(line);
            }
            EXPECT_EQ(r.contractViolations, 0) << entries[i].name << " bloque " << kBlocks[b];
            if (!r.series.empty()) {
                EXPECT_GE(r.series.front().frames, static_cast<float>(AnalysisThread::kTickFrames))
                    << entries[i].name << " bloque " << kBlocks[b] << ": publico antes de un tick (D31)";
            }
        }
    }
    std::string msg;
    for (size_t k = 0; k < differing.size() && k < 12; ++k) msg += "\n  " + differing[k];
    EXPECT_TRUE(differing.empty())
        << differing.size() << " diferencias entre bloques (AC-057.5: t_primera, t_conv y los cents no pueden "
        << "depender del bloque del llamador):" << msg;
}

}  // namespace
}  // namespace wma_test
