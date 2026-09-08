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
#include "../AnalysisSnapshot.h"
#include "support/CorpusSweep.h"

#include <gtest/gtest.h>

#include "support/AscentVsSweep.h"

#include <array>

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
 * siendo "saltear".
 *
 * Hasta REQ-032 este test miraba el manifiesto REAL del repo, porque estaba
 * vacio a proposito. Ya no: el corpus existe (release `corpus-v1`), asi que el
 * caso se construye con un manifiesto temporal que solo tiene comentarios. La
 * regla que fija no cambio — no haber encontrado nada que verificar no es haber
 * verificado — y un mutante que devuelva `kVerified` sobre cero entradas sigue
 * muriendo aca.
 */
TEST(CorpusGate, AnEmptyManifestIsStillASkipAndNotAQuietPass) {
    const std::string dir = corpus::makeTempDir();
    ASSERT_FALSE(dir.empty());
    const std::string manifest = dir + "/manifest.txt";
    {
        std::FILE* f = std::fopen(manifest.c_str(), "wb");
        ASSERT_NE(f, nullptr);
        std::fputs("# solo comentarios: ninguna entrada\n\n", f);
        std::fclose(f);
    }
    const auto st = corpus::stateOf(dir, manifest);
    EXPECT_EQ(st, State::kAbsent)
        << "un manifiesto sin entradas no describe un corpus: es ausencia, no verificacion";
    EXPECT_FALSE(corpus::countsAsCoverage(st))
        << "el manifiesto no declara archivos y aun asi se contaba como cobertura";
    corpus::removeTempDir(dir);
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

    /**
     * 🔴 EL PRESUPUESTO DEL CORPUS GRABADO, MEDIDO (REQ-032 S2, 2026-09-07) — y NO es el del
     * contrato (0,1 cents). Sobre los 38 archivos con lectura, con el instrumento declarado:
     *
     *     lectura FINA (strobe) vs hz_verdadero:  |max| = 5,40 c (ukelele_C4) · 4,28 (nylon_E4)
     *                                             · 2,99 (jazz_E4) · 2,75 (ukelele_G4)
     *     deteccion GRUESA vs hz_verdadero:       |max| = 4,66 c (bajo-pua_A1) · 4,59 (acero_A2)
     *                                             y ±0,3 c en 30 de 38
     *
     * El hallazgo que esto deja para un REQ propio: donde la fina se aparta, la GRUESA y el
     * oraculo coinciden entre si (ukelele_C4: oraculo 262,500, gruesa 262,483, fina +5,40 c =
     * 263,32). Dos metodos independientes contra uno: el que se aparta es el strobe, sobre
     * cuerdas sampleadas INARMONICAS y decayendo. No se concluye la causa aca; se mide y se
     * fija el techo para que una regresion se vea. Bajar este numero es trabajo del REQ que
     * lo explique, y el comentario del corpus sintetico ya lo anticipaba: "ese numero sale de
     * MEDIRLO, no de aflojar este".
     */
    constexpr double kRecordedFineBudgetCents = 6.0;
    constexpr double kRecordedCoarseBudgetCents = 6.0;

    const auto results = corpus::sweepAll(corpus::defaultCorpusDir(), corpus::manifestPath());
    ASSERT_FALSE(results.empty())
        << "el corpus esta verificado y el barrido no leyo una sola entrada";

    // REQ-032 S2 — el reporte se IMPRIME, no se escribe en CLAUDE.md: es un numero que exige
    // correr algo, y la regla de REQ-021 es que esos no se afirman a mano.
    std::printf("\n  [REQ-032] corpus grabado: %zu archivos con altura declarada\n", results.size());
    std::printf("  %-24s %9s %9s %7s %6s %5s %4s %6s %6s\n", "archivo", "hz_verd", "detectHz",
                "gruesoC", "finoC", "estado", "sop", "lect_s", "nota_s");
    for (const corpus::Outcome& o : results) {
        const double coarse = o.detectedHz > 0.0 ? 1200.0 * std::log2(o.detectedHz / o.trueHz) : NAN;
        std::printf("  %-24s %9.3f %9.3f %+7.2f %+6.2f %5d %4.0f %6.2f %6.2f\n", o.name.c_str(),
                    o.trueHz, o.detectedHz, coarse, o.cents, o.state,
                    static_cast<double>(o.spectralSupport), o.lastReadingSec, o.noteEndSec);
    }
    std::printf("\n");

    /**
     * 🔴 TRINQUETE BIDIRECCIONAL: los archivos donde el detector lee un SUBMULTIPLO. Se declara
     * el desenlace exacto: altura en f0/k, bandera 0, NUNCA convergido. Si empiezan a leerse
     * bien, esto se pone ROJO para que se los saque de la lista — igual que
     * `rt-safety-baseline.txt`. No es escribir el defecto en el contrato: es decir que se sabe,
     * con nombre, y que cambiarlo tiene que verse en el diff.
     *
     * VACIO desde REQ-033 (2026-09-07), y asi fue: tuvo `guitarra-limpia_E4` (f0/3) y
     * `guitarra-limpia_G3` (f0/5) —los dos casos de REQ-031 sobre el render del propio repo—,
     * el arreglo del detector grueso los puso ROJOS y salieron. Hoy leen −0,66 y −1,76 c
     * convergidas con soporte. La lista queda armada por si vuelve la clase.
     */
    struct KnownSubmultiple { const char* name; int divisor; };
    constexpr std::array<KnownSubmultiple, 0> kKnownSubmultiples{};
    auto knownDivisor = [&](const std::string& name) {
        for (const auto& k : kKnownSubmultiples) if (name == k.name) return k.divisor;
        return 0;
    };

    /**
     * 🔴 Y el tercer caso conocido, de la misma clase pero con otro desenlace: el ATAQUE lee un
     * submultiplo y despues se corrige, y la nota es corta. `guitarra-acero_E4` (2,1 s por encima
     * del piso) leia f0/3 de 0,33 a 1,30 s, volvia a E4, caia otra vez a 1,58 y volvia; con el
     * instrumento declarado el modo rapido reenganchaba SEIS veces siguiendo eso y el strobe
     * nunca juntaba sus 0,5 s antes de que la nota muriera: ninguna publicacion traia lectura
     * fina. Medido publicacion por publicacion; el consumidor vio lo mismo (nota del 07/09 b, §2).
     *
     * VACIO desde REQ-033 (2026-09-07): el arreglo del detector grueso lo puso ROJO —"ahora SI
     * trae lectura fina (0,92 c)"— y salio. El ataque era la misma clase (el pico de τ mal
     * muestreado por el barrido), no un timbre distinto. La lista queda armada.
     */
    constexpr std::array<const char*, 0> kKnownNoFineReadingWithCandidates{};
    auto knownNoFineReading = [&](const std::string& name) {
        for (const char* k : kKnownNoFineReadingWithCandidates) if (name == k) return true;
        return false;
    };

    for (const corpus::Outcome& o : results) {
        EXPECT_TRUE(o.analysed) << o.name << ": no se pudo analizar";
        // R-PITCH-37 sobre TODAS las publicaciones de la nota, no solo la ultima.
        EXPECT_EQ(o.convergedWithoutSupport, 0)
            << o.name << ": publico CONVERGED con la bandera en 0 " << o.convergedWithoutSupport
            << " veces de " << o.publications;

        if (const int k = knownDivisor(o.name)) {
            EXPECT_NEAR(o.detectedHz, o.trueHz / k, 1.0)
                << o.name << ": ya no lee f0/" << k << " — si se arreglo, sacalo de la lista";
            EXPECT_EQ(o.spectralSupport, 0.0f) << o.name << ": la bandera dejo de decir 0";
            EXPECT_NE(o.state, wma::analysis::kStateConverged) << o.name << ": convergio en el submultiplo";
            continue;
        }

        EXPECT_EQ(o.spectralSupport, 1.0f) << o.name << ": la altura publicada no tiene soporte";
        EXPECT_NE(o.state, wma::analysis::kStateNoSignal) << o.name << ": termino en ausencia";
        ASSERT_GT(o.detectedHz, 0.0) << o.name << ": termino sin altura gruesa";
        const double coarseCents = 1200.0 * std::log2(o.detectedHz / o.trueHz);
        EXPECT_LT(std::fabs(coarseCents), kWrongNoteCents)
            << o.name << ": la deteccion gruesa esta a mas de media nota";
        EXPECT_LT(std::fabs(coarseCents), kRecordedCoarseBudgetCents)
            << o.name << ": la gruesa se aparto " << coarseCents << " c del oraculo";

        if (knownNoFineReading(o.name)) {
            EXPECT_FALSE(o.published)
                << o.name << ": ahora SI trae lectura fina (" << o.cents
                << " c) — si el ataque se arreglo, sacalo de la lista";
            continue;
        }
        EXPECT_TRUE(o.published) << o.name << ": no mostro una lectura fina en toda la nota";
        if (o.published) {
            EXPECT_LT(std::fabs(o.cents), kWrongNoteCents)
                << o.name << ": " << o.cents << " cents contra su frecuencia declarada ("
                << o.trueHz << " Hz). A mas de media nota no es un presupuesto discutible: "
                   "es el rate, el archivo, o la nota equivocada.";
            EXPECT_LT(std::fabs(o.cents), kRecordedFineBudgetCents)
                << o.name << ": la lectura fina se aparto " << o.cents << " c del oraculo";
        }
    }
}

/**
 * REQ-034 S1 (AC-034.2) — el ascenso contra el barrido entero sobre los 41 archivos REALES.
 *
 * Los sinteticos son armonicos exactos o estirados con B chico; una cuerda sampleada trae
 * ruido, decaimiento y parciales que la sintesis no tiene, y la unimodalidad de la NSDF en la
 * vecindad de un candidato es una propiedad de la SEÑAL. Se alimenta la misma mezcla a mono
 * (0,5·(L+R)) y la misma nota (hasta `noteEndFrames`) que el barrido del corpus, a un
 * `McLeodPitch` solo, en ventanas enteras. Sin corpus verificado: SKIPPED, nunca PASSED.
 */
TEST(CorpusRobustness, AscentVersusFullRefinementOnTheRecordedCorpus) {
    const auto st = corpus::stateOf(corpus::defaultCorpusDir(), corpus::manifestPath());
    if (!corpus::shouldRunRobustness(st)) {
        GTEST_SKIP() << "sin corpus grabado (" << corpus::describe(st) << ")";
    }
    using namespace wma_test::ascent;
    Tally total;
    int files = 0;
    for (const corpus::Entry& e : corpus::entriesOf(corpus::manifestPath())) {
        if (e.trueHz <= 0.0) continue;
        const wav::WavData data = wav::readWav((corpus::defaultCorpusDir() + "/" + e.name).c_str());
        if (data.numFrames <= 0 || data.sampleRate <= 0) continue;
        const int frames = corpus::noteEndFrames(data);
        if (frames <= 0) continue;
        std::vector<float> mono(static_cast<size_t>(frames));
        for (int i = 0; i < frames; ++i)
            mono[static_cast<size_t>(i)] = 0.5f * (data.buffer[static_cast<size_t>(i) * 2]
                                                   + data.buffer[static_cast<size_t>(i) * 2 + 1]);
        Tally t;
        compare(data.sampleRate, mono, t, e.name);
        add(total, t);
        ++files;
    }
    EXPECT_GT(files, 0);
    EXPECT_GT(total.windows, 0);
    std::printf("\n");
    print(("corpus grabado (" + std::to_string(files) + " archivos)").c_str(), total);
    std::printf("\n");
    RecordProperty("candidatos_lag_distinto", std::to_string(total.differLag));
    RecordProperty("ventanas_eleccion_distinta", std::to_string(total.chosenDiffer));
}

}  // namespace
}  // namespace wma_test
