/**
 * test_corpus_sweep.cpp — REQ-015 S3 · 3.6: el camino del corpus, EJERCIDO.
 *
 * 🔴 POR QUE HAY UN CORPUS SINTETICO Y NO UN `GTEST_SKIP()`
 * ---------------------------------------------------------
 * El material grabado todavia no existe. Hasta ahora eso dejaba el barrido de
 * REQ-001 10.7 escrito como un `FAIL()` que decia "necesita el material": codigo
 * que nadie ejecuta, esperando un dia que puede no llegar. Eso es el mecanismo
 * sin llamador, y esta misma semana costo caro — REQ-014 S3 entrego un contador
 * y no lo conecto al test que su plan decia que cerraba.
 *
 * Asi que el barrido se ejerce **hoy**, contra un corpus que este test fabrica:
 * WAVs de verdad escritos a disco, checksums de verdad, un manifiesto de verdad
 * y el mismo `stateOf()` que decide sobre el corpus de campo. Lo unico sintetico
 * es la señal. El dia que lleguen las 44 grabaciones, corre sobre ellas sin
 * cambiar una linea.
 *
 * 🔴 Y TRAE SU CONTROL NEGATIVO, QUE ES LA MITAD QUE IMPORTA
 * ----------------------------------------------------------
 * Un barrido que no compare nada pasa cualquier corpus. Por eso el segundo test
 * declara una frecuencia verdadera EQUIVOCADA sobre el mismo audio y exige que
 * el barrido lo note: si no lo nota, no esta midiendo.
 */

#include "support/Corpus.h"
#include "support/CorpusSweep.h"

#include "../../looper/WavFile.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace wma_test {
namespace {

using corpus::Entry;
using corpus::Outcome;
using corpus::State;

constexpr int kRate = 44100;      // NO 48000, por la razon de siempre
constexpr int kFrames = 52920;    // 1,2 s: alcanza para que la integracion converja

/// Presupuesto del barrido sintetico. Es estricto A PROPOSITO: la señal es
/// armonica y sin ruido, asi que cualquier desvio real seria del motor o del
/// camino de archivo, no del material. El corpus de campo —con inarmonicidad y
/// decaimiento— va a necesitar el suyo, y ese numero sale de MEDIRLO, no de
/// aflojar este.
constexpr double kSyntheticBudgetCents = 0.5;

/// Cuerda de 4 parciales armonicos, estereo intercalado.
std::vector<float> string(double f0, int frames, int rate) {
    std::vector<float> b(static_cast<size_t>(frames) * 2, 0.0f);
    for (int i = 0; i < frames; ++i) {
        double s = 0.0;
        for (int n = 1; n <= 4; ++n) {
            s += (0.5 / n) * std::sin(2.0 * M_PI * f0 * n * i / rate);
        }
        b[static_cast<size_t>(i) * 2]     = static_cast<float>(s);
        b[static_cast<size_t>(i) * 2 + 1] = static_cast<float>(s);
    }
    return b;
}

/// Un corpus de mentira hecho de archivos de verdad.
struct SyntheticCorpus {
    std::string dir;
    std::string manifest;

    ~SyntheticCorpus() { corpus::removeTempDir(dir); }

    /// Escribe un WAV y devuelve la linea de manifiesto que le corresponde, con
    /// su checksum REAL. `declaredHz` puede mentir: es lo que el control
    /// negativo necesita.
    std::string add(const std::string& name, double trueHz, double declaredHz) {
        const auto buf = string(trueHz, kFrames, kRate);
        const std::string path = dir + "/" + name;
        // FLOAT_32 y no PCM_16: el material sintetico no tiene por que cargar
        // ruido de cuantizacion, que le pondria un piso al presupuesto.
        if (!wav::writeWav(path.c_str(), buf.data(), kFrames, kRate,
                           wav::BitDepth::FLOAT_32)) {
            return {};
        }
        return name + "  " + corpus::sha256Of(path) + "  " + std::to_string(declaredHz)
               + "  sintetico\n";
    }

    void writeManifest(const std::string& lines) {
        manifest = dir + "/manifest.txt";
        std::FILE* f = std::fopen(manifest.c_str(), "wb");
        ASSERT_NE(f, nullptr);
        std::fputs("# corpus sintetico\n", f);
        std::fputs(lines.c_str(), f);
        std::fclose(f);
    }
};

// ---------------------------------------------------------------------------
// 3.6 — el control POSITIVO: el barrido mide, y mide bien
// ---------------------------------------------------------------------------

TEST(CorpusSweep, EveryDeclaredFileIsMeasuredAgainstItsTrueFrequency) {
    SyntheticCorpus c;
    c.dir = corpus::makeTempDir();
    ASSERT_FALSE(c.dir.empty());

    // Tres alturas de tres registros distintos, para que un barrido que midiera
    // siempre la misma nota no pueda pasar.
    std::string lines;
    lines += c.add("e2.wav",  82.4069,  82.4069);
    lines += c.add("a2.wav", 110.0000, 110.0000);
    lines += c.add("e4.wav", 329.6276, 329.6276);
    c.writeManifest(lines);

    // 🔴 La MISMA decision que gobierna el corpus de campo: si esto no dice
    // "verificado", el barrido de abajo no estaria autorizado a correr.
    ASSERT_EQ(corpus::stateOf(c.dir, c.manifest), State::kVerified)
        << "el corpus sintetico no paso su propio gate: " << corpus::describe(
               corpus::stateOf(c.dir, c.manifest));
    ASSERT_TRUE(corpus::shouldRunRobustness(corpus::stateOf(c.dir, c.manifest)));

    const auto results = corpus::sweepAll(c.dir, c.manifest);
    ASSERT_EQ(results.size(), 3u) << "el barrido no leyo las tres entradas del manifiesto";

    for (const Outcome& o : results) {
        EXPECT_EQ(o.sampleRate, kRate)
            << o.name << ": el rate salio del aire y no del archivo";
        ASSERT_TRUE(o.analysed) << o.name << ": el puerto no analizo el archivo";
        ASSERT_TRUE(o.published) << o.name << ": no hubo lectura de altura";
        EXPECT_NEAR(o.cents, 0.0, kSyntheticBudgetCents)
            << o.name << ": midio " << o.cents << " cents contra su propia frecuencia "
            << "verdadera (" << o.trueHz << " Hz)";
    }
}

// ---------------------------------------------------------------------------
// 3.6 — el control NEGATIVO: un f0 declarado que miente TIENE que notarse
// ---------------------------------------------------------------------------

/**
 * El mismo audio, con la frecuencia verdadera declarada 20 cents corrida.
 *
 * Sin este test, un barrido que devolviera 0 cents sin mirar el archivo —o que
 * ni siquiera lo abriera— pasaria el de arriba entero. Lo que se afirma aca no
 * es que el motor mida bien: es que **el barrido esta comparando**.
 */
TEST(CorpusSweep, AWrongDeclaredFrequencyIsNotSilentlyAccepted) {
    SyntheticCorpus c;
    c.dir = corpus::makeTempDir();
    ASSERT_FALSE(c.dir.empty());

    constexpr double kTrueHz = 110.0;
    constexpr double kLieCents = 20.0;
    const double declared = kTrueHz * std::pow(2.0, kLieCents / 1200.0);

    c.writeManifest(c.add("a2-mentida.wav", kTrueHz, declared));
    ASSERT_EQ(corpus::stateOf(c.dir, c.manifest), State::kVerified);

    const auto results = corpus::sweepAll(c.dir, c.manifest);
    ASSERT_EQ(results.size(), 1u);
    const Outcome& o = results.front();
    ASSERT_TRUE(o.analysed);
    ASSERT_TRUE(o.published) << "no publico: entonces el barrido no puede notar nada";

    // El audio esta 20 cents POR DEBAJO de lo declarado, asi que el barrido tiene
    // que reportar ~-20 y no ~0. Un barrido que no compare devolveria 0.
    EXPECT_LT(o.cents, -kLieCents / 2.0)
        << "el barrido reporto " << o.cents << " cents sobre un archivo cuya frecuencia "
           "declarada esta " << kLieCents << " cents arriba de la real: no esta comparando";
    EXPECT_NEAR(o.cents, -kLieCents, 1.0)
        << "reporto " << o.cents << " y la mentira es de " << kLieCents << " cents";
}

/**
 * Un archivo corrupto NO habilita el barrido — y esa decision es la de siempre.
 *
 * Se afirma aca ademas de en `test_corpus_gate.cpp` porque lo que se prueba es
 * distinto: alla, que `stateOf` clasifica; aca, que **el barrido respeta esa
 * clasificacion** en vez de leer los archivos igual.
 */
TEST(CorpusSweep, ACorruptCorpusDoesNotAuthoriseTheSweep) {
    SyntheticCorpus c;
    c.dir = corpus::makeTempDir();
    ASSERT_FALSE(c.dir.empty());

    c.writeManifest(c.add("a2.wav", 110.0, 110.0));
    ASSERT_EQ(corpus::stateOf(c.dir, c.manifest), State::kVerified);

    // Se corrompe el archivo DESPUES de anotar su checksum, que es exactamente lo
    // que pasa con una descarga a medias.
    std::FILE* f = std::fopen((c.dir + "/a2.wav").c_str(), "ab");
    ASSERT_NE(f, nullptr);
    std::fputs("basura", f);
    std::fclose(f);

    const State st = corpus::stateOf(c.dir, c.manifest);
    EXPECT_EQ(st, State::kCorrupt);
    EXPECT_FALSE(corpus::shouldRunRobustness(st))
        << "un corpus corrupto autorizo el barrido: los numeros saldrian de material "
           "que nadie verifico";
    EXPECT_FALSE(corpus::countsAsCoverage(st));
}

}  // namespace
}  // namespace wma_test
