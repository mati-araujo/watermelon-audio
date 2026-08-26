/**
 * test_corpus_gate.cpp — REQ-001 S10 · 10.5 y 10.6.
 *
 * **Una corrida que no verifico no puede pasar por una que si.** Es la misma
 * regla que gobierna `regen-golden.sh` —donde regenerar deja los tests SKIPPED y
 * no PASSED— y la atestacion del gate local.
 *
 * Sin corpus, los tests de robustez tienen que salir **SKIPPED** con su razon. La
 * diferencia no es cosmetica: un PASSED se cuenta como cobertura en el reporte y
 * un SKIPPED no, asi que la suite entera cambia de significado segun cual sea.
 *
 * Lo que se prueba aca es la DECISION, no la maquinaria de gtest: `corpusState()`
 * es la funcion que decide, y es la que un mutante rompería.
 */

#include "support/Corpus.h"
#include "support/CorpusSweep.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace wma_test {
namespace {

using corpus::State;

// ---------------------------------------------------------------------------
// 10.5 — sin corpus se SALTEA, no se aprueba
// ---------------------------------------------------------------------------
TEST(CorpusGate, WithoutTheCorpusTheDecisionIsSkipAndNeverPass) {
    const auto st = corpus::stateOf("/un/directorio/que/no/existe",
                                    corpus::manifestPath());

    EXPECT_EQ(st, State::kAbsent)
        << "con el corpus ausente la decision no fue 'saltear'";
    EXPECT_NE(st, State::kVerified)
        << "🔴 declaro el corpus VERIFICADO sin haberlo mirado: la suite entera "
           "pasaria a leerse como cobertura completa";
    EXPECT_FALSE(corpus::shouldRunRobustness(st))
        << "iba a correr los tests de robustez sin material que correr";
    EXPECT_FALSE(corpus::countsAsCoverage(st))
        << "una corrida sin corpus se estaria contando como cobertura";
}

/**
 * El manifiesto vacio es un caso APARTE del corpus ausente, y tiene que seguir
 * siendo "saltear". Es el estado real del repo hoy: el mecanismo existe y el
 * material no.
 */
TEST(CorpusGate, AnEmptyManifestIsStillASkipAndNotAQuietPass) {
    const auto st = corpus::stateOf(corpus::defaultCorpusDir(), corpus::manifestPath());
    EXPECT_FALSE(corpus::countsAsCoverage(st))
        << "el manifiesto no declara archivos y aun asi se contaba como cobertura";
}

// ---------------------------------------------------------------------------
// 10.6 — un archivo corrupto falla RUIDOSAMENTE
// ---------------------------------------------------------------------------
/**
 * Un checksum que no coincide no puede degradarse a "bueno, corramos igual". Un
 * archivo bajado a medias produce un resultado RARO en vez de un error, y un
 * afinador que falla raro es peor que uno que falla fuerte: el raro se publica.
 */
TEST(CorpusGate, AChecksumMismatchIsLoudInsteadOfProducingAStrangeResult) {
    const std::string dir = corpus::makeTempDir();
    ASSERT_FALSE(dir.empty());

    // Un manifiesto con un hash que no le corresponde a nada.
    const std::string manifest = dir + "/manifest.txt";
    {
        std::FILE* f = std::fopen(manifest.c_str(), "wb");
        ASSERT_NE(f, nullptr);
        std::fprintf(f, "# de prueba\nvoz.wav  %s  440.0  tono de prueba\n",
                     std::string(64, '0').c_str());
        std::fclose(f);
    }
    {
        std::FILE* f = std::fopen((dir + "/voz.wav").c_str(), "wb");
        ASSERT_NE(f, nullptr);
        std::fprintf(f, "no soy el archivo que el manifiesto declara");
        std::fclose(f);
    }

    const auto st = corpus::stateOf(dir, manifest);
    EXPECT_EQ(st, State::kCorrupt)
        << "un archivo cuyo checksum no coincide no fue reportado como corrupto";
    EXPECT_FALSE(corpus::shouldRunRobustness(st))
        << "iba a correr la robustez contra un archivo corrupto, y el resultado "
           "hubiera sido un numero raro en vez de un error";
    EXPECT_FALSE(corpus::countsAsCoverage(st));

    corpus::removeTempDir(dir);
}

// ---------------------------------------------------------------------------
// El test de robustez de verdad: SKIPPED mientras no haya corpus
// ---------------------------------------------------------------------------
/**
 * REQ-015 S3 · 3.6 — ESTE TEST DEJO DE SER UN `FAIL()` DORMIDO.
 *
 * Hasta esta etapa decia "hay corpus y todavia no se escribio el barrido": codigo
 * que esperaba un dia que puede no llegar. El barrido ahora existe
 * (`support/CorpusSweep.h`) y **se ejerce en cada corrida** contra un corpus
 * sintetico —ver `test_corpus_sweep.cpp`—, asi que lo unico que falta para el
 * material de campo es el material. Cuando aparezca, esto corre sobre el sin
 * cambiar una linea.
 *
 * 🔴 LO QUE ESTE TEST NO AFIRMA, Y POR QUE NO
 * --------------------------------------------
 * No hay un presupuesto de exactitud para el corpus grabado, y **no se inventa
 * uno**. El contrato declara `strobe_worst_error_cents = 0.001092`, pero eso sale
 * de material sintetico limpio: una grabacion real trae ruido, decaimiento e
 * inarmonicidad, y el numero que corresponda ahi sale de MEDIRLO sobre el
 * material — no de elegirlo a ojo hoy.
 *
 * Lo que si se puede afirmar sin el material es lo cualitativo, y es justo lo que
 * atrapa los fallos que importan: que cada archivo se lea, se analice y publique
 * una altura, y que ninguna lectura este a mas de media nota de su frecuencia
 * declarada. Un error de esa magnitud no es "el presupuesto es discutible": es el
 * rate leido mal, el archivo leido mal, o el motor midiendo otra cuerda.
 */
TEST(CorpusRobustness, TheRecordedCorpusSweepRunsOnlyWhenThereIsACorpus) {
    const auto st = corpus::stateOf(corpus::defaultCorpusDir(), corpus::manifestPath());
    if (!corpus::shouldRunRobustness(st)) {
        GTEST_SKIP() << "sin corpus grabado (" << corpus::describe(st)
                     << "). Se baja con: bash scripts/fetch-corpus.sh — y hasta "
                        "entonces esto NO cuenta como cobertura. El BARRIDO en si "
                        "no queda sin probar: corre contra un corpus sintetico en "
                        "test_corpus_sweep.cpp.";
    }

    /// Media nota. No es un presupuesto de exactitud: es la frontera entre "hay
    /// que calibrar el numero" y "esto midio otra cosa".
    constexpr double kWrongNoteCents = 50.0;

    const auto results = corpus::sweepAll(corpus::defaultCorpusDir(), corpus::manifestPath());
    ASSERT_FALSE(results.empty())
        << "el corpus esta verificado y el barrido no leyo una sola entrada";

    for (const corpus::Outcome& o : results) {
        EXPECT_TRUE(o.analysed) << o.name << ": no se pudo analizar";
        EXPECT_TRUE(o.published) << o.name << ": no publico altura";
        if (o.published) {
            EXPECT_LT(std::fabs(o.cents), kWrongNoteCents)
                << o.name << ": " << o.cents << " cents contra su frecuencia declarada ("
                << o.trueHz << " Hz). A mas de media nota no es un presupuesto discutible: "
                   "es el rate, el archivo, o la nota equivocada.";
        }
    }
}

}  // namespace
}  // namespace wma_test
