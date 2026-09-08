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
#include "support/PartialOracle.h"
#include "tests/support/TestSanitizer.h"

#include <gtest/gtest.h>

#include "support/AscentVsSweep.h"

#include <array>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

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
     * 🔴 EL PRESUPUESTO DEL CORPUS GRABADO, MEDIDO — y NO es el del contrato (0,1 cents).
     *
     * REQ-032 S2 (2026-09-07) lo fijo en 6 c porque la "lectura fina" se apartaba del oraculo hasta
     * +5,40 c (ukelele_C4) mientras la gruesa coincidia. REQ-035 S1 (2026-09-08) midio DONDE nacia
     * ese error y no era del strobe: **`kSnapCents` es relativo al OBJETIVO del strobe**, y con el
     * instrumento declarado el modo rapido reengancha ese objetivo a la cuerda del CATALOGO (el
     * nominal temperado: 261,626 para C4), no al `trueHz` que `sweepFile` fijo. Este test comparaba
     * cents-vs-nominal contra un oraculo ABSOLUTO, y el "+5,40" era la desafinacion del propio
     * sample (+5,78 c en el SoundFont) leida correctamente. Ver `WhereTheFineReadingErrorIsBorn`.
     *
     * Con la fina en Hz absolutos (`Outcome::cents` desde REQ-035), sobre los 41 con lectura:
     *
     *     33 CONVERGIDAS (σ ≤ 0,1):  |max| = 0,73 c (limpia_G3) · 0,69 (limpia_E4) · 0,45 (fretless_D2)
     *      8 MIDIENDO   (σ > 0,1):   6 dentro de 0,52 c; y DOS afuera, declaradas abajo con su mecanismo
     *
     * El presupuesto es **1 cent** para toda lectura mostrada, convergida o no: es lo que el
     * material real alcanza hoy con el motor tal cual esta, y una regresion de la fina se ve. El
     * residuo que queda (≤ 0,73 c) tiene mecanismo —el ataque de la nota dentro de la ventana de
     * regresion del estimador de fase (48 × 4096 frames ≈ 4,5 s): la guitarra limpia trae un glide
     * de > 1 s— y es un REQ aparte si alguna vez importa. La GRUESA no se toca aca (REQ-033/034):
     * bajo-pua_A1 lee −4,66 c y acero_A2 −4,59, y su presupuesto sigue en 6.
     */
    constexpr double kRecordedFineBudgetCents = 1.0;
    constexpr double kRecordedCoarseBudgetCents = 6.0;

    const auto results = corpus::sweepAll(corpus::defaultCorpusDir(), corpus::manifestPath());
    ASSERT_FALSE(results.empty())
        << "el corpus esta verificado y el barrido no leyo una sola entrada";

    // REQ-032 S2 — el reporte se IMPRIME, no se escribe en CLAUDE.md: es un numero que exige
    // correr algo, y la regla de REQ-021 es que esos no se afirman a mano.
    std::printf("\n  [REQ-032] corpus grabado: %zu archivos con altura declarada\n", results.size());
    std::printf("  %-24s %9s %9s %7s %8s %6s %6s %6s %5s %6s %4s %6s %6s\n", "archivo", "hz_verd", "detectHz",
                "gruesoC", "objHz", "finPub", "finAbs", "sigma", "estLe", "estado", "sop", "lect_s", "nota_s");
    for (const corpus::Outcome& o : results) {
        const double coarse = o.detectedHz > 0.0 ? 1200.0 * std::log2(o.detectedHz / o.trueHz) : NAN;
        std::printf("  %-24s %9.3f %9.3f %+7.2f %8.3f %+6.2f %+6.2f %6.3f %5d %6d %4.0f %6.2f %6.2f\n", o.name.c_str(),
                    o.trueHz, o.detectedHz, coarse, o.strobeTargetHz, o.strobeC, o.cents, o.strobeSigmaC,
                    o.readingState, o.state, static_cast<double>(o.spectralSupport), o.lastReadingSec, o.noteEndSec);
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

    /**
     * 🔴 TRINQUETE BIDIRECCIONAL: las lecturas que terminan la nota FUERA del presupuesto fino, con
     * su mecanismo medido (REQ-035 S1). Las dos estan SIN CONVERGER —σ 0,91 y 4,88— o sea que el
     * motor no las afirma; se declaran para que el presupuesto de 1 c sea de verdad 1 c y no "1 c
     * salvo lo que no miro". Si una converge o entra en el presupuesto, esto se pone ROJO para
     * sacarla de la lista.
     *
     *  · bajo-acustico_G2: el preset trae un GLIDE de ataque de −27,7 c a 0,1 s que se estabiliza
     *    recien a 1,0 s (oraculo por tramos), y la regresion de 48 ventanas del estimador de fase
     *    lo arrastra al parcial 1 (−2,73 c contra ~0 en los parciales 2..4). σ lo ve: 0,91.
     *  · guitarra-acero_A2: su parcial 4 REAL esta a −16 c de la serie estirada (oraculo por
     *    parcial, −16 dB), y el ajuste se niega a converger: σ 4,88. Es REQ-027 haciendo su trabajo;
     *    "acertar" ese sample pediria doblar el modelo fisico, y eso no entra (AC-035.6).
     */
    struct KnownOutsideFineBudget { const char* name; const char* mechanism; };
    constexpr std::array<KnownOutsideFineBudget, 2> kKnownOutsideFineBudget{{
        {"bajo-acustico_G2.wav", "glide de ataque de -27,7 c dentro de la ventana de regresion"},
        {"guitarra-acero_A2.wav", "parcial 4 real a -16 c de la serie estirada: el ajuste no converge"},
    }};
    auto knownOutside = [&](const std::string& name) -> const KnownOutsideFineBudget* {
        for (const auto& k : kKnownOutsideFineBudget) if (name == k.name) return &k;
        return nullptr;
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
            if (const KnownOutsideFineBudget* k = knownOutside(o.name)) {
                EXPECT_NE(o.readingState, wma::analysis::kStateConverged)
                    << o.name << ": CONVERGIO con " << o.cents << " c (" << k->mechanism
                    << ") — si el mecanismo se arreglo, sacalo de la lista";
                EXPECT_GE(std::fabs(o.cents), kRecordedFineBudgetCents)
                    << o.name << ": ya esta dentro del presupuesto (" << o.cents << " c, "
                    << k->mechanism << ") — sacalo de la lista";
                continue;
            }
            EXPECT_LT(std::fabs(o.cents), kRecordedFineBudgetCents)
                << o.name << ": la lectura fina (en Hz absolutos, objetivo " << o.strobeTargetHz
                << " Hz) se aparto " << o.cents << " c del oraculo, σ " << o.strobeSigmaC;
        }
    }
}

/**
 * REQ-035 S1 (AC-035.1) — DONDE NACE EL ERROR DE LA LECTURA FINA, archivo por archivo.
 *
 * Sobre el corpus la "lectura fina" se apartaba del oraculo hasta +5,40 c mientras la gruesa y el
 * oraculo coincidian. Tres hipotesis: (a) el rastreo de fase del parcial 1 ya se aparta; (b) el
 * ajuste de la serie estirada corre C por parciales 2..4 que no la siguen; (c) el sample no sigue
 * ninguna serie. Esta tabla las separa: por archivo, los cuatro `partialCents(i)` del strobe en la
 * publicacion de la que salio la fina, contra un oraculo POR PARCIAL (Goertzel + Hann, otro metodo)
 * en dos ventanas —la sostenida del manifiesto (desde 2,0 s) y la que TERMINA donde el strobe leyo—,
 * el subconjunto que entro al ajuste (reconstruido contra la funcion de produccion), el B del
 * snapshot y el del ajuste, y la trayectoria de la fina en el tiempo.
 *
 * 🔴 LO QUE LA TABLA ENCONTRO (2026-09-08): NINGUNA DE LAS TRES. El error nacia en el INSTRUMENTO.
 * ------------------------------------------------------------------------------------------
 * Los cuatro parciales del strobe coincidian entre si (ukelele_C4: +5,48 / +5,33 / +5,23 / +5,40)
 * y el oraculo los ponia a 0,00 en la MISMA ventana — o sea que el ajuste no corria nada (b) y el
 * sample era una serie armonica exacta (c: B_fit ≈ 0, residuo ≤ 0,1 c en 39 de 41). Y el rastreo
 * (a) tampoco: el incremento de fase de Goertzel a 262,500 Hz, ventana a ventana y a mano, da
 * 0,00 c desde 0,84 s. Lo que pasaba es que **el strobe no medía contra `trueHz`**: con el
 * instrumento declarado, el modo rapido reengancha el objetivo a la CUERDA DEL CATALOGO
 * (`objHz` = 261,626 para ukelele C4, el nominal temperado), asi que `kSnapCents` es relativo a
 * ese nominal, y el barrido de REQ-032 lo comparaba con un oraculo ABSOLUTO. El "+5,40" era la
 * desafinacion del propio sample respecto del temperamento igual (+5,78 c en el SoundFont), leida
 * por un afinador que hace exactamente lo que tiene que hacer. Convertida a Hz absolutos
 * (`finAbs` = objetivo · 2^(cents/1200), contra `trueHz`), la fina esta a ≤ 0,73 c del oraculo en
 * los 39 archivos convergidos; los dos restantes no estan convergidos y lo declaran (σ 0,91 y 4,88).
 *
 * Es la tercera vez seguida que la hipotesis de entrada se refuta al medir, y esta vez el que
 * estaba mal era el instrumento: ver [[un-rojo-que-era-del-instrumento]]. La columna `finoC`
 * (cents publicados, relativos al objetivo) se conserva al lado de `finAbs` justamente para que se
 * vea la diferencia; la clasificacion por mecanismo se hace sobre `finAbs`, que es el error real.
 *
 * Los mecanismos se cuentan con los umbrales de la etapa: RASTREO si |p1 − oraculo_1| ≥ 0,5 c (los
 * dos en la MISMA referencia); AJUSTE si p1 coincide y |C − oraculo_1| > 2; SAMPLE si los
 * parciales del oraculo no siguen la serie estirada con ningun B por mas de 2 c. El numero se
 * IMPRIME: exige correr algo, asi que no se escribe a mano en ningun lado (REQ-021).
 *
 * Lo que este test AFIRMA (lo demas lo mide): que la reconstruccion del subconjunto admitido es
 * UNICA en cada archivo con lectura —si no, la tabla habla de un ajuste que no es el que corrio—,
 * que el H1 sostenido de este oraculo coincide con el `hz_verdadero` del manifiesto a 0,1 c —los dos
 * oraculos son el mismo metodo en dos lenguajes, y si divergen uno esta roto—, y que cada archivo
 * con lectura cae en exactamente una fila de la contabilidad.
 */
TEST(CorpusRobustness, WhereTheFineReadingErrorIsBorn) {
    const auto st = corpus::stateOf(corpus::defaultCorpusDir(), corpus::manifestPath());
    if (!corpus::shouldRunRobustness(st)) {
        GTEST_SKIP() << "sin corpus grabado (" << corpus::describe(st) << ")";
    }
    namespace orc = wma_test::oracle;
    constexpr double kSameAsOracleCents = 0.5;   // p1 "coincide" con el oraculo
    constexpr double kErrorCents = 2.0;          // la fina "se aparta" (umbral de la spec)
    constexpr double kWindowSec = 0.75;

    /**
     * Bajo sanitizer, un SUBCONJUNTO declarado (MINI-020, REQ-034 S1): la tabla entera cuesta ~17 s
     * sin instrumentar y el techo del gate local es 180 s por test. Se conservan SIEMPRE los seis
     * archivos con hallazgo —los cuatro de la tabla de la spec, el de SAMPLE y el del glide de
     * ataque— y uno de cada tres del resto, por indice del manifiesto: cada instrumento aparece. Sin
     * sanitizer se barren los 41. Cuantos se barrieron se imprime y se registra.
     */
    std::vector<corpus::Entry> entries;
    {
        const std::vector<corpus::Entry> all = corpus::entriesOf(corpus::manifestPath());
#ifdef WMA_TEST_UNDER_SANITIZER
        const char* const kAlways[] = {"ukelele_C4.wav", "guitarra-nylon_E4.wav", "guitarra-jazz_E4.wav",
                                       "ukelele_G4.wav", "guitarra-acero_A2.wav", "bajo-acustico_G2.wav"};
        for (size_t i = 0; i < all.size(); ++i) {
            bool keep = (i % 3 == 0);
            for (const char* k : kAlways) keep = keep || all[i].name == k;
            if (keep) entries.push_back(all[i]);
        }
#else
        entries = all;
#endif
    }
    std::vector<corpus::Outcome> results;
    for (const corpus::Entry& e : entries)
        results.push_back(corpus::sweepFile(corpus::defaultCorpusDir() + "/" + e.name, e));
    ASSERT_FALSE(results.empty());
    RecordProperty("archivos_barridos", static_cast<int>(results.size()));

    int published = 0, within = 0, tracking = 0, fit = 0, sample = 0, uniqueMasks = 0;
    std::printf("\n  [REQ-035] donde nace el error de la fina — strobe (p1..p4) contra el oraculo por parcial "
                "(%zu de %zu archivos%s)\n", results.size(), corpus::entriesOf(corpus::manifestPath()).size(),
#ifdef WMA_TEST_UNDER_SANITIZER
                ", subconjunto bajo sanitizer"
#else
                ""
#endif
    );
    std::printf("  %-22s %8s %6s %6s %6s %2s %4s | %6s %6s %6s %6s | %6s %6s %6s %6s | %6s | %5s %5s %5s | %8s %8s | %5s %6s | %s\n",
                "archivo", "objHz", "finoC", "finAbs", "sigC", "k", "mask", "p1", "p2", "p3", "p4",
                "o1@t", "o2", "o3", "o4", "o1@2s", "dB2", "dB3", "dB4", "B_snap", "B_fit",
                "t_1ra", "c_1ra", "mec");
    for (const corpus::Outcome& o : results) {
        if (!o.published) continue;
        ++published;
        const wav::WavData data = wav::readWav((corpus::defaultCorpusDir() + "/" + o.name).c_str());
        std::vector<float> mono(static_cast<size_t>(data.numFrames));
        for (int i = 0; i < data.numFrames; ++i)
            mono[static_cast<size_t>(i)] = 0.5f * (data.buffer[static_cast<size_t>(i) * 2]
                                                   + data.buffer[static_cast<size_t>(i) * 2 + 1]);
        const orc::PartialReading sus = orc::measureSustained(mono, data.sampleRate, o.trueHz);
        const double t0 = std::max(0.0, o.lastReadingSec - kWindowSec);
        const orc::PartialReading at = orc::measureAt(mono, data.sampleRate, o.trueHz, t0, kWindowSec);

        // Los dos oraculos (script y C++) son el mismo metodo: tienen que coincidir.
        ASSERT_TRUE(sus.valid) << o.name << ": el oraculo sostenido no entro en la señal";
        const double o1Sustained = orc::centsOf(sus.hz[1], o.trueHz);
        EXPECT_NEAR(o1Sustained, 0.0, 0.1)
            << o.name << ": el H1 del oraculo en C++ no coincide con el hz_verdadero del manifiesto";

        // La reconstruccion del subconjunto admitido tiene que ser unica.
        EXPECT_NE(o.admittedMask, -1)
            << o.name << ": ningun subconjunto de " << o.partialsUsed
            << " parciales reproduce C = " << o.strobeC << " (o mas de uno lo hace)";
        if (o.admittedMask != -1) ++uniqueMasks;

        // El oraculo sostenido contra la serie estirada: ?sigue el sample alguna serie?
        double oc[orc::kPartials], oB = NAN, oC = NAN;
        int orders[orc::kPartials];
        for (int n = 1; n <= orc::kPartials; ++n) { oc[n - 1] = sus.cents[n]; orders[n - 1] = n; }
        double residMax = NAN;
        if (corpus::fitStretchedSeriesWithB(oc, orders, orc::kPartials, &oC, &oB)) {
            residMax = 0.0;
            for (int n = 1; n <= orc::kPartials; ++n)
                residMax = std::max(residMax, std::fabs(oc[n - 1] - oC
                                                        - wma::analysis::StrobeTracker::stretchCents(oB, n)));
        }

        const double o1At = at.valid ? orc::centsOf(at.hz[1], o.trueHz) : NAN;
        const double ref = at.valid ? o1At : o1Sustained;    // el oraculo en la ventana del strobe
        // p1 y C vienen relativos al OBJETIVO del strobe (el nominal del catalogo bajo el modo
        // rapido); se los lleva a la referencia del oraculo (trueHz) antes de comparar.
        const double targetVsTrue = 1200.0 * std::log2(o.strobeTargetHz / o.trueHz);
        const double p1VsTrue = o.partialCents[0] + targetVsTrue;
        const double err = o.fineVsTrueCents - ref;
        const bool p1Off = std::isfinite(p1VsTrue) && std::fabs(p1VsTrue - ref) >= kSameAsOracleCents;
        const bool isErr = std::fabs(err) > kErrorCents;
        const bool sampleOff = std::isfinite(residMax) && residMax > kErrorCents;
        std::string mec;
        if (!isErr) { ++within; mec = "-"; }
        else if (p1Off) { ++tracking; mec = "RASTREO"; }
        else { ++fit; mec = "AJUSTE"; }
        if (sampleOff) { ++sample; mec += "+SAMPLE"; }

        char maskStr[5] = "----";
        for (int i = 0; i < 4; ++i) if (o.admittedMask > 0 && (o.admittedMask & (1 << i))) maskStr[i] = static_cast<char>('1' + i);
        const auto first = o.trajectory.front();
        std::printf("  %-22s %8.3f %+6.2f %+6.2f %6.3f %2d %4s | %+6.2f %+6.2f %+6.2f %+6.2f | %+6.2f %+6.2f %+6.2f %+6.2f | %+6.2f | %+5.0f %+5.0f %+5.0f | %8.1e %8.1e | %5.2f %+6.2f | %s\n",
                    o.name.c_str(), o.strobeTargetHz, o.strobeC, o.fineVsTrueCents, o.strobeSigmaC, o.partialsUsed, maskStr,
                    o.partialCents[0], o.partialCents[1], o.partialCents[2], o.partialCents[3],
                    o1At, at.cents[2], at.cents[3], at.cents[4], o1Sustained,
                    sus.db[2], sus.db[3], sus.db[4], o.snapshotB, o.fitB,
                    first.first, first.second, mec.c_str());
    }
    std::printf("\n  [REQ-035] resumen sobre %d archivos con lectura fina (error = finAbs, en Hz absolutos "
                "contra el oraculo): dentro de %.0f c = %d · RASTREO (p1 ya se aparta) = %d · AJUSTE (p1 "
                "coincide, C no) = %d · SAMPLE (el oraculo no sigue ninguna serie) = %d\n\n",
                published, kErrorCents, within, tracking, fit, sample);
    EXPECT_EQ(within + tracking + fit, published) << "la contabilidad no cierra";
    EXPECT_EQ(uniqueMasks, published) << "hay archivos cuyo ajuste no se pudo reconstruir";
    RecordProperty("archivos_con_lectura", published);
    RecordProperty("mecanismo_rastreo", tracking);
    RecordProperty("mecanismo_ajuste", fit);
    RecordProperty("mecanismo_sample", sample);
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
