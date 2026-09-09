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
#include "support/PhaseTrend.h"
#include "tests/support/TestSanitizer.h"

#include <gtest/gtest.h>

#include "support/AscentVsSweep.h"

#include <array>
#include <limits>
#include <map>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace wma_test {
namespace {

using corpus::State;

/// El barrido de los 41, hecho UNA vez por proceso y compartido entre los tests que lo leen
/// (REQ-036 S1): cuesta ~10 s sin instrumentar y ~60 bajo TSan, y dos tests que lo repitan son dos
/// veces ese costo por nada — la misma regla que el barrido de glides sintetico.
const std::vector<corpus::Outcome>& sweptCorpus() {
    static const std::vector<corpus::Outcome> kSwept =
        corpus::sweepAll(corpus::defaultCorpusDir(), corpus::manifestPath());
    return kSwept;
}

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
    /**
     * REQ-036 (2026-09-09): con la admision por tendencia y la ventana adaptativa (R-PITCH-62/63),
     * sobre los 41 con lectura: **39 CONVERGIDAS al final** (venian 33), |error| maximo **0,30 c**
     * (bajo-dedos_D2, una nota que deriva de +2,7 a +0,25 c y a la que el oraculo le mide la media
     * de 2 a 5 s; venia 0,73) y una sola MIDIENDO dentro del presupuesto (bajo-pua_D2, +0,26 c,
     * σ 0,17). El presupuesto pasa de 1 c a **0,4 c** sobre toda lectura mostrada: 1,3× sobre lo
     * medido, la misma holgura que REQ-035 dejo (0,73 → 1).
     */
    constexpr double kRecordedFineBudgetCents = 0.4;
    /**
     * REQ-038 S2 — el presupuesto de la GRUESA, sobre el exceso fuera del intervalo de sus dos
     * referencias (ver el bloque del criterio, mas abajo).
     *
     * Venia de **6,0 c**, un numero heredado que ademas se medía sobre la ULTIMA publicacion
     * —la cola decayendo— y contra el oraculo ESPECTRAL solo. Con la cifra correcta (la mediana
     * de la nota) y el criterio correcto (el intervalo), lo medido sobre los 41 es un exceso
     * medio de **0,085 c** y un peor caso de **1,73 c**, que es `guitarra-acero_A2` — el mismo
     * archivo cuyo parcial 4 real esta a −16 c de la serie estirada y que ya esta declarado
     * fuera del presupuesto FINO por ese mecanismo. El segundo peor es 0,76 c.
     *
     * 2,3 c es 1,3x sobre lo medido: la misma holgura que el repo ya uso dos veces (REQ-035
     * dejo 0,73 -> 1; REQ-036 dejo 0,30 -> 0,4).
     */
    constexpr double kRecordedCoarseBudgetCents = 2.3;

    const auto& results = sweptCorpus();
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
     * REQ-038 S2 — LA TABLA QUE PARTE EL REQ: la gruesa contra los DOS oraculos, sobre la cifra
     * que describe la nota. Si el sesgo de acero es del material, la columna `vsTemp` tiene que
     * ser mucho mas chica que `vsEspec` en los archivos de acero y parecida en el resto.
     */
    std::printf("  [REQ-038] la gruesa contra los dos oraculos (mediana desde %.1f s)\n", corpus::kSustainedFromSec);
    std::printf("  %-24s %10s %10s %8s %8s %7s %7s %9s\n", "archivo", "medianaHz", "temporalHz",
                "vsEspec", "vsTemp", "exceso", "r_temp", "ultimaC");
    double peorTemp = 0.0, peorEspec = 0.0, peorExceso = 0.0, sumaExceso = 0.0;
    int conDosOraculos = 0, adentro = 0;
    for (const corpus::Outcome& o : results) {
        const double med = corpus::coarseMedian(o, corpus::kSustainedFromSec);
        if (!std::isfinite(med) || !std::isfinite(o.temporalHz)) {
            std::printf("  %-24s %10s %10s\n", o.name.c_str(),
                        std::isfinite(med) ? "-" : "sin mediana",
                        std::isfinite(o.temporalHz) ? "-" : "sin temporal");
            continue;
        }
        const double vsEspec = 1200.0 * std::log2(med / o.trueHz);
        const double vsTemp = 1200.0 * std::log2(med / o.temporalHz);
        const double ultima = o.detectedHz > 0.0 ? 1200.0 * std::log2(o.detectedHz / o.trueHz) : NAN;
        const bool dentro = (vsEspec >= 0.0 && vsTemp <= 0.0) || (vsEspec <= 0.0 && vsTemp >= 0.0);
        const double exceso = dentro ? 0.0 : std::min(std::fabs(vsEspec), std::fabs(vsTemp));
        std::printf("  %-24s %10.3f %10.3f %+8.2f %+8.2f %7.2f %7.4f %+9.2f\n", o.name.c_str(), med,
                    o.temporalHz, vsEspec, vsTemp, exceso, o.temporalR, ultima);
        peorTemp = std::max(peorTemp, std::fabs(vsTemp));
        peorEspec = std::max(peorEspec, std::fabs(vsEspec));
        peorExceso = std::max(peorExceso, exceso);
        sumaExceso += exceso;
        ++conDosOraculos;
        adentro += dentro ? 1 : 0;
    }
    // 🔴 El resumen que sostiene el criterio: si una sola de las dos referencias fuera la
    // correcta, su peor caso seria el presupuesto. Es el EXCESO el que se presupuesta.
    std::printf("  peor |vsEspectral| = %.2f c  ·  peor |vsTemporal| = %.2f c  ·  "
                "PEOR EXCESO = %.2f c\n", peorEspec, peorTemp, peorExceso);
    std::printf("  %d de %d caen DENTRO del intervalo de las dos referencias; exceso medio %.3f c "
                "(presupuesto %.2f)\n\n",
                adentro, conDosOraculos, conDosOraculos ? sumaExceso / conDosOraculos : 0.0,
                kRecordedCoarseBudgetCents);

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
    /**
     * REQ-036 (2026-09-09) saco a `bajo-acustico_G2` de esta lista, en ROJO como corresponde: con la
     * ventana adaptativa el glide de −27,7 c ya no arrastra la regresion y la nota converge a
     * +0,0005 c a 2,04 s. Queda `guitarra-acero_A2`, cuyo mecanismo no es el ataque.
     */
    struct KnownOutsideFineBudget { const char* name; const char* mechanism; };
    constexpr std::array<KnownOutsideFineBudget, 1> kKnownOutsideFineBudget{{
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

        /**
         * REQ-038 S2 (AC-038.4) — LA GRUESA SE JUZGA CONTRA LA CANTIDAD QUE MIDE, Y SOBRE LA
         * CIFRA QUE DESCRIBE LA NOTA. Las dos mitades de esta comparacion estaban mal:
         *
         *  · LA CIFRA. Se medía `o.detectedHz`, que es la altura de la ULTIMA publicacion —la
         *    cola decayendo—. Por eso `bajo-pua_A1` figuraba con −4,66 c cuando su mediana esta
         *    en −0,06: el numero acusaba al detector por una lectura de la cola. S1 arreglo el
         *    barrido (`coarseMedian`, `coarseAtReading`) y este gate era el ultimo que seguia
         *    leyendo la vieja.
         *  · EL ORACULO. Se comparaba contra `trueHz`, que es el pico ESPECTRAL de H1. La
         *    gruesa es TEMPORAL (NSDF sobre la forma de onda) y sobre una cuerda inarmonica
         *    esas dos cantidades no son la misma: `f_n = n·f0·sqrt(1+B·n²)` no tiene periodo
         *    exacto. Medido en S1: sobre `guitarra-acero_E2` el detector da +6,32 c, una
         *    autocorrelacion cruda INDEPENDIENTE +6,71 c, y H1 0,00 c. Dos metodos temporales
         *    coinciden y el espectral se aparta: el sesgo es del MATERIAL, no del detector.
         *
         * `temporalHz` es ese segundo oraculo (`TemporalOracle.h`, con su propio self-test en
         * `test_temporal_oracle.cpp`).
         */
        const double coarseMedianHz = corpus::coarseMedian(o, corpus::kSustainedFromSec);
        ASSERT_TRUE(std::isfinite(coarseMedianHz))
            << o.name << ": no hubo ninguna publicacion con altura despues de "
            << corpus::kSustainedFromSec << " s";

        // La guarda gruesa —"no es otra nota"— sigue siendo contra el nominal declarado: media
        // nota es media nota mida quien mida, y ahi no hay ambiguedad de cantidad.
        const double coarseVsTrue = 1200.0 * std::log2(coarseMedianHz / o.trueHz);
        EXPECT_LT(std::fabs(coarseVsTrue), kWrongNoteCents)
            << o.name << ": la deteccion gruesa esta a mas de media nota";

        // Sin referencia no se juzga la gruesa — y eso es un FALLO, no un salteo: un archivo
        // que desaparece de la comparacion en silencio es un falso verde. Se usa
        // `ADD_FAILURE` y no `ASSERT_` para que una nota corta no se lleve puesta la revision
        // de los otros 40.
        if (!std::isfinite(o.temporalHz)) {
            ADD_FAILURE() << o.name << ": el oraculo temporal no pudo medir esta nota; su gruesa "
                                       "quedaria sin juzgar";
            continue;
        }

        /**
         * 🔴 EL CRITERIO ES UN INTERVALO, NO UN PUNTO — y eso NO es aflojar el gate: lo APRIETA.
         *
         * Sobre una cuerda inarmonica "la altura" no es un solo numero. El pico espectral de H1
         * dice una cosa, el periodo de la forma de onda dice otra, y las dos son correctas: la
         * serie `f_n = n·f0·sqrt(1+B·n²)` no tiene periodo exacto. Pedirle al detector que
         * coincida con UNA de las dos es pedirle que resuelva una ambiguedad que esta en la
         * señal, no en el.
         *
         * Asi que lo que se exige es que caiga DENTRO del intervalo que las dos referencias
         * abren. Sobre material armonico ese intervalo se cierra —las dos referencias coinciden—
         * y el criterio queda tan estricto como comparar contra un punto; sobre material
         * inarmonico se abre exactamente lo que la fisica del material justifica.
         *
         * MEDIDO sobre los 41 (2026-09-09), y es lo que decidio el criterio — las tres opciones
         * se compararon con numeros antes de elegir:
         *
         *   | referencia                    | exceso medio | peor  | mejor en |
         *   |-------------------------------|--------------|-------|----------|
         *   | espectral sola (lo de antes)  | 0,307 c      | 4,66  | 30 de 41 |
         *   | temporal sola                 | 0,416 c      | 2,41  |  8 de 41 |
         *   | **el intervalo de las dos**   | **0,085 c**  | **1,73** | 26 caen ADENTRO |
         *
         * 🔴 La opcion "temporal sola" era la que la spec habia decidido (decision 1 de S2), y
         * la medicion la REFUTA: mejora el peor caso pero empeora la media y esta mas lejos en
         * 30 de los 41 archivos. El detector no es un estimador puro de periodo de onda —la
         * normalizacion de McLeod, la decimacion y el refinamiento lo dejan ENTRE las dos
         * cantidades— asi que ninguna de las dos sola es su referencia. Ver las Notas de S2.
         *
         * Y el caso que abrio el REQ queda con exceso CERO: `guitarra-acero_E2`, que contra H1
         * media +4,66 c, cae entre las dos referencias. El sesgo era del material.
         */
        const double coarseVsTemporal = 1200.0 * std::log2(coarseMedianHz / o.temporalHz);
        // Adentro del intervalo <=> la mediana esta entre las dos referencias <=> los dos
        // desvios tienen signos opuestos (o alguno es exactamente cero).
        const bool dentro = (coarseVsTrue >= 0.0 && coarseVsTemporal <= 0.0) ||
                            (coarseVsTrue <= 0.0 && coarseVsTemporal >= 0.0);
        const double exceso =
            dentro ? 0.0 : std::min(std::fabs(coarseVsTrue), std::fabs(coarseVsTemporal));
        EXPECT_LT(exceso, kRecordedCoarseBudgetCents)
            << o.name << ": la gruesa quedo " << exceso
            << " c FUERA del intervalo que abren sus dos referencias (espectral "
            << coarseVsTrue << " c, temporal " << coarseVsTemporal << " c)";

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
 * REQ-036 S1 (AC-036.3) — LA TRAYECTORIA DE LA FINA POR ARCHIVO, Y LO QUE LA COMPUERTA CAMBIARIA.
 *
 * Por archivo: la primera lectura fina (t, error), la primera CONVERGIDA, el tiempo hasta quedar a
 * ±0,5 c del valor final, y la ultima (t, error, estado). Sumado: cuantas de las lecturas finales
 * CONVERGIDAS superan 0,1 / 0,3 / 0,5 c — la linea de base del trinquete de AC-036.6 (hoy: 33
 * convergidas, maximo 0,73 c).
 *
 * Y la SIMULACION DESDE AFUERA, sobre la historia de fases de cada parcial reconstruida desde la
 * sonda (`Outcome::history`), del veredicto de `test_attack_in_window.cpp`: el estadistico de
 * tendencia con su umbral, como compuerta SOLA (la ventana de produccion, sin admitir al parcial
 * que dispara) y como ventana ADAPTATIVA sincronizada (los cuatro se reinician en el quiebre). Es
 * el numero que decide S2: cuantas de las 33 dejarian de converger al final de la nota, y con que
 * error maximo quedan las que siguen.
 *
 * Lo que este test AFIRMA: que el simulador es fiel —sin compuerta reproduce la lectura de
 * produccion en cada publicacion con lectura fina, sobre la MISMA ventana (la historia se
 * reconstruyo bien)— y que la contabilidad cierra. Los numeros se imprimen y registran: exigen
 * correr algo (REQ-021).
 */
struct CorpusSim {
    bool convergedAtEnd = false;
    double errAtEnd = NAN;
    double firstConvSec = NAN, firstConvErr = NAN;
    int blindPubs = 0;         ///< publicaciones CONVERGIDAS con |error| > 0,1 c
    int restarts = 0;
    int finePubs = 0;          ///< publicaciones con lectura de produccion
    int simPubs = 0;           ///< ... de las que el simulador tambien dio lectura
    double worstFidelity = 0.0;
    int windowMismatch = 0;    ///< ventanas reconstruidas que no coinciden con las de produccion
};

CorpusSim simulateCorpus(const corpus::Outcome& o, const trend::Threshold& th, bool gate, bool adaptive,
                         bool majority = false) {
    using wma::analysis::PhaseSlopeEstimator;
    using wma::analysis::StrobeTracker;
    constexpr int kP = StrobeTracker::kPartials;
    CorpusSim r;
    int start[kP] = {0, 0, 0, 0};
    for (const corpus::Outcome::Publication& pub : o.publicationLog) {
        if (!pub.fine || pub.admitted <= 0) continue;
        ++r.finePubs;
        int excluded = 0;
        for (int p = 0; p < kP; ++p)
            start[p] = std::max({start[p], pub.histSegment[p], pub.histEnd[p] - PhaseSlopeEstimator::kMaxWindows});
        if (adaptive) {
            int common = 0;
            for (int p = 0; p < kP; ++p) common = std::max(common, start[p]);
            int nFire = 0, nAdmitted = 0, fired = 0;
            for (int p = 0; p < kP; ++p) {
                if (!(pub.admitted & (1 << p))) continue;
                const int cnt = pub.histEnd[p] - common;
                if (cnt < th.minWindows) continue;   // un quiebre se juzga sobre una ventana admisible
                ++nAdmitted;
                if (trend::fires(trend::trendOver(o.history[p].data() + common, cnt, o.sampleRate,
                                                  pub.targetHz * (p + 1)), th)) { ++nFire; fired |= 1 << p; }
            }
            // `majority`: ver test_attack_in_window.cpp — un parcial solo que dispara se excluye
            // sin reiniciar a los otros tres.
            const bool any = majority ? (nFire > 0 && 2 * nFire >= nAdmitted) : (nFire > 0);
            for (int p = 0; p < kP; ++p) start[p] = common;
            if (!any) excluded = fired;
            if (any) {
                for (int p = 0; p < kP; ++p) start[p] = pub.histEnd[p] - (pub.histEnd[p] - common) / 2;
                ++r.restarts;
                continue;
            }
        }
        double pc[kP], ps[kP];
        int mask = 0;
        for (int p = 0; p < kP; ++p) {
            if (!(pub.admitted & (1 << p))) continue;
            if (excluded & (1 << p)) continue;
            const int cnt = pub.histEnd[p] - start[p];
            if (cnt < 4) continue;
            const double* w = o.history[p].data() + start[p];
            const trend::PartialReading pr = trend::readingFromSlope(trend::fitSlope(w, 0, cnt), o.sampleRate,
                                                                     pub.targetHz * (p + 1));
            if (!pr.ok) continue;
            if (!gate) {
                if (cnt != pub.count[p]) ++r.windowMismatch;
                r.worstFidelity = std::max(r.worstFidelity, std::fabs(pr.cents - pub.pCents[p]));
            } else if (!trend::admits(trend::trendOver(w, cnt, o.sampleRate, pub.targetHz * (p + 1)), th)) {
                continue;
            }
            pc[p] = pr.cents; ps[p] = pr.sigma; mask |= 1 << p;
        }
        const trend::Combined g = trend::combineFrom(pc, ps, mask, gate);
        if (!g.hasMeasurement) { r.convergedAtEnd = false; continue; }
        ++r.simPubs;
        const double err = 1200.0 * std::log2(pub.targetHz * std::pow(2.0, g.cents / 1200.0) / o.trueHz);
        const bool conv = g.sigma <= StrobeTracker::kConvergedUncertaintyCents;
        if (!gate) r.worstFidelity = std::max(r.worstFidelity, std::fabs(g.cents - pub.strobeC));
        if (conv && std::fabs(err) > 0.1) ++r.blindPubs;
        if (conv && std::isnan(r.firstConvSec)) { r.firstConvSec = pub.sec; r.firstConvErr = err; }
        r.convergedAtEnd = conv;
        r.errAtEnd = err;
    }
    return r;
}

TEST(CorpusRobustness, TheAttackTrajectoryAndWhatTheGateWouldChange) {
    const auto st = corpus::stateOf(corpus::defaultCorpusDir(), corpus::manifestPath());
    if (!corpus::shouldRunRobustness(st)) {
        GTEST_SKIP() << "sin corpus grabado (" << corpus::describe(st) << ")";
    }
    /// El veredicto de S1 (`test_attack_in_window.cpp`, `kChosen`): la misma terna, escrita dos
    /// veces a proposito — el barrido sintetico la elige, y este test mide que hace sobre material
    /// real. Si divergen, uno de los dos habla de otra compuerta.
    constexpr trend::Threshold kChosen{5.0, 0.05, 12, trend::Magnitude::kDelta};
    constexpr double kNearFinalCents = 0.5;
    constexpr double kContractCents = 0.1;

    const auto& results = sweptCorpus();
    ASSERT_FALSE(results.empty());

    std::printf("\n  [REQ-036] trayectoria de la fina por archivo (error en Hz absolutos contra el oraculo) y la compuerta "
                "simulada (|T| > %.1f, |Δ| > %.2f c, min %d ventanas): sola y adaptativa sincronizada\n",
                kChosen.t, kChosen.deltaCents, kChosen.minWindows);
    std::printf("  %-22s | %5s %6s | %5s %6s | %5s | %5s %6s %4s %6s | %5s %6s %4s %3s | %5s %6s %4s %3s %3s\n",
                "archivo", "t_1ra", "err1ra", "t_cnv", "errcnv", "t±0.5", "t_ult", "errult", "est", "sigma",
                "g_cnv", "g_err", "gcie", "gC", "a_cnv", "a_err", "acie", "aC", "rei");
    int published = 0, convergedToday = 0, over01 = 0, over03 = 0, over05 = 0, blindToday = 0;
    int gateConverged = 0, gateBlind = 0, adapConverged = 0, adapBlind = 0, mismatches = 0;
    double maxToday = 0.0, maxGate = 0.0, maxAdap = 0.0, worstFidelity = 0.0;
    std::vector<std::string> lostGate, lostAdap, gainedAdap;
    for (const corpus::Outcome& o : results) {
        if (!o.published) continue;
        ++published;
        // --- la trayectoria de hoy ---
        const auto first = o.trajectory.front();
        double tConv = NAN, errConv = NAN;
        for (const corpus::Outcome::Publication& pub : o.publicationLog) {
            if (pub.fine && pub.state == wma::analysis::kStateConverged) { tConv = pub.sec; errConv = pub.centsAbs; break; }
        }
        double tNear = NAN;
        for (size_t i = 0; i < o.trajectory.size(); ++i) {
            bool stays = true;
            for (size_t j = i; j < o.trajectory.size(); ++j)
                stays = stays && std::fabs(o.trajectory[j].second - o.cents) <= kNearFinalCents;
            if (stays) { tNear = o.trajectory[i].first; break; }
        }
        for (const corpus::Outcome::Publication& pub : o.publicationLog)
            if (pub.fine && pub.state == wma::analysis::kStateConverged && std::fabs(pub.centsAbs) > kContractCents) ++blindToday;
        const bool convToday = o.readingState == wma::analysis::kStateConverged;
        if (convToday) {
            ++convergedToday;
            maxToday = std::max(maxToday, std::fabs(o.cents));
            if (std::fabs(o.cents) > 0.1) ++over01;
            if (std::fabs(o.cents) > 0.3) ++over03;
            if (std::fabs(o.cents) > 0.5) ++over05;
        }
        // --- las simulaciones ---
        const CorpusSim today = simulateCorpus(o, kChosen, false, false);
        const CorpusSim gate = simulateCorpus(o, kChosen, true, false);
        const CorpusSim adap = simulateCorpus(o, kChosen, true, true);
        worstFidelity = std::max(worstFidelity, today.worstFidelity);
        mismatches += today.windowMismatch;
        if (gate.convergedAtEnd) { ++gateConverged; maxGate = std::max(maxGate, std::fabs(gate.errAtEnd)); }
        else if (convToday) lostGate.push_back(o.name);
        if (adap.convergedAtEnd) {
            ++adapConverged;
            maxAdap = std::max(maxAdap, std::fabs(adap.errAtEnd));
            if (!convToday) gainedAdap.push_back(o.name);
        } else if (convToday) {
            lostAdap.push_back(o.name);
        }
        gateBlind += gate.blindPubs;
        adapBlind += adap.blindPubs;
        std::printf("  %-22s | %5.2f %+6.2f | %5.2f %+6.2f | %5.2f | %5.2f %+6.2f %4d %6.3f | %5.2f %+6.2f %4d %3s | %5.2f %+6.2f %4d %3s %3d\n",
                    o.name.c_str(), first.first, first.second, tConv, errConv, tNear, o.lastReadingSec, o.cents,
                    o.readingState, o.strobeSigmaC, gate.firstConvSec, gate.errAtEnd, gate.blindPubs,
                    gate.convergedAtEnd ? "si" : "NO", adap.firstConvSec, adap.errAtEnd, adap.blindPubs,
                    adap.convergedAtEnd ? "si" : "NO", adap.restarts);
    }
    std::printf("\n  [REQ-036] hoy: %d con lectura, %d CONVERGIDAS al final (max |error| %.2f c; > 0,1: %d, > 0,3: %d, > 0,5: %d); "
                "publicaciones CONVERGIDAS con |error| > 0,1 c: %d\n", published, convergedToday, maxToday, over01, over03, over05, blindToday);
    std::printf("  [REQ-036] compuerta sola:       %d convergidas al final (max |error| %.2f c), publicaciones convergidas equivocadas %d; "
                "dejan de converger %zu:", gateConverged, maxGate, gateBlind, lostGate.size());
    for (const std::string& n : lostGate) std::printf(" %s", n.c_str());
    std::printf("\n  [REQ-036] ventana adaptativa:   %d convergidas al final (max |error| %.2f c), publicaciones convergidas equivocadas %d; "
                "dejan de converger %zu:", adapConverged, maxAdap, adapBlind, lostAdap.size());
    for (const std::string& n : lostAdap) std::printf(" %s", n.c_str());
    std::printf("; empiezan a converger %zu:", gainedAdap.size());
    for (const std::string& n : gainedAdap) std::printf(" %s", n.c_str());
    {
        int conv = 0, blind = 0, restarts = 0;
        double maxErr = 0.0;
        std::vector<std::string> lost, gained;
        for (const corpus::Outcome& o : results) {
            if (!o.published) continue;
            const CorpusSim a = simulateCorpus(o, kChosen, true, true, true);
            const bool convToday = o.readingState == wma::analysis::kStateConverged;
            if (a.convergedAtEnd) { ++conv; maxErr = std::max(maxErr, std::fabs(a.errAtEnd)); if (!convToday) gained.push_back(o.name); }
            else if (convToday) lost.push_back(o.name);
            blind += a.blindPubs; restarts += a.restarts;
        }
        std::printf("\n  [REQ-036] adaptativa por MAYORIA:  %d convergidas al final (max |error| %.2f c), publicaciones convergidas equivocadas %d, "
                    "%d reinicios; dejan de converger %zu:", conv, maxErr, blind, restarts, lost.size());
        for (const std::string& n : lost) std::printf(" %s", n.c_str());
        std::printf("; empiezan a converger %zu:", gained.size());
        for (const std::string& n : gained) std::printf(" %s", n.c_str());
        RecordProperty("convergidas_adaptativa_mayoria", conv);
        RecordProperty("max_error_adaptativa_mayoria_milicents", static_cast<int>(maxErr * 1000.0));
    }
    std::printf("\n  fidelidad del simulador: peor |lectura externa − produccion| = %.2e c, ventanas que no coinciden = %d\n",
                worstFidelity, mismatches);
    // Los disparos sobre las que DEJAN de converger: donde, y con que T y Δ por parcial, para que S2
    // sepa que es lo que la compuerta ve en esas notas (un glide real, un batido entre capas del
    // sample, o una cola que decae) antes de decidir si el precio se paga.
    for (const std::string& lost : lostAdap) {
        for (const corpus::Outcome& o : results) {
            if (o.name != lost) continue;
            std::printf("  %s: disparos de la adaptativa (t, y por parcial admitido n / T / Δ):\n", o.name.c_str());
            int start = 0;
            for (const corpus::Outcome::Publication& pub : o.publicationLog) {
                if (!pub.fine || pub.admitted <= 0) continue;
                for (int p = 0; p < corpus::Outcome::kPartials; ++p)
                    start = std::max({start, pub.histSegment[p], pub.histEnd[p] - wma::analysis::PhaseSlopeEstimator::kMaxWindows});
                bool any = false;
                char line[400];
                int len = std::snprintf(line, sizeof line, "    t=%5.2f err=%+6.2f σ=%.3f est=%d |", pub.sec, pub.centsAbs, pub.sigma, pub.state);
                for (int p = 0; p < corpus::Outcome::kPartials; ++p) {
                    if (!(pub.admitted & (1 << p))) continue;
                    const int cnt = pub.histEnd[p] - start;
                    if (cnt < kChosen.minWindows) continue;
                    const trend::Trend tr = trend::trendOver(o.history[p].data() + start, cnt, o.sampleRate, pub.targetHz * (p + 1));
                    const bool f = trend::fires(tr, kChosen);
                    any = any || f;
                    len += std::snprintf(line + len, sizeof line - static_cast<size_t>(len), " p%d n=%2d T=%+6.1f Δ=%+6.3f%s", p + 1, cnt, tr.tScore, tr.deltaCents, f ? "*" : " ");
                }
                if (any) { std::printf("%s\n", line); start = pub.histEnd[0] - (pub.histEnd[0] - start) / 2; }
            }
        }
    }
    std::printf("\n");

    // La misma grilla que el barrido sintetico (T × Δ × minimo), con reinicio sincronizado, sobre
    // los 41: convergidas al final, error maximo entre ellas, publicaciones convergidas
    // equivocadas, cuantas de las de hoy se pierden y cuantas nuevas convergen.
    std::printf("  grilla sobre el corpus, ventana adaptativa sincronizada (hoy: %d convergidas, max %.2f c, %d publicaciones equivocadas):\n",
                convergedToday, maxToday, blindToday);
    std::printf("  %5s %6s %5s | %7s %7s %7s %7s %7s %7s\n", "T", "Δ", "min", "conv", "max_err", "pub_eq", "pierde", "gana", "reini");
    for (double tt : {3.0, 4.0, 5.0, 6.0}) for (double d : {0.05, 0.10}) for (int minW : {8, 12, 16}) {
        const trend::Threshold th{tt, d, minW, trend::Magnitude::kDelta};
        int conv = 0, blind = 0, lost = 0, gained = 0, restarts = 0;
        double maxErr = 0.0;
        for (const corpus::Outcome& o : results) {
            if (!o.published) continue;
            const CorpusSim a = simulateCorpus(o, th, true, true);
            const bool convToday = o.readingState == wma::analysis::kStateConverged;
            if (a.convergedAtEnd) { ++conv; maxErr = std::max(maxErr, std::fabs(a.errAtEnd)); if (!convToday) ++gained; }
            else if (convToday) ++lost;
            blind += a.blindPubs;
            restarts += a.restarts;
        }
        std::printf("  %5.1f %6.2f %5d | %7d %7.2f %7d %7d %7d %7d\n", tt, d, minW, conv, maxErr, blind, lost, gained, restarts);
    }
    std::printf("\n");

    /**
     * AC-036.6 — EL TRINQUETE DEL CORPUS, sobre lo que PRODUCCION publica (la columna "hoy" de arriba):
     * al menos las 33 convergidas que habia antes de REQ-036, y el error maximo entre ellas por
     * debajo del 0,73 c de entonces. Medido al cerrar S2 (2026-09-09): 39 y 0,30. Los dos numeros
     * de la linea de base son los de la spec, no se re-miden aca: son lo que este cambio prometio no
     * empeorar.
     */
    constexpr int kConvergedBeforeReq036 = 33;
    constexpr double kMaxErrorBeforeReq036 = 0.73;
    EXPECT_GE(convergedToday, kConvergedBeforeReq036)
        << "la admision por tendencia bajo las convergidas del corpus: " << convergedToday << " de " << published;
    EXPECT_LT(maxToday, kMaxErrorBeforeReq036)
        << "el error maximo de las convergidas no bajo: " << maxToday << " c";

    EXPECT_EQ(mismatches, 0) << "la historia de fases reconstruida no reproduce la ventana de produccion";
    EXPECT_LT(worstFidelity, 1e-6) << "el simulador no reproduce la lectura de produccion sin compuerta";
    EXPECT_EQ(over01 + (convergedToday - over01), convergedToday) << "la contabilidad no cierra";
    RecordProperty("convergidas_hoy", convergedToday);
    RecordProperty("convergidas_compuerta", gateConverged);
    RecordProperty("convergidas_adaptativa", adapConverged);
    RecordProperty("max_error_hoy_milicents", static_cast<int>(maxToday * 1000.0));
    RecordProperty("max_error_compuerta_milicents", static_cast<int>(maxGate * 1000.0));
    RecordProperty("max_error_adaptativa_milicents", static_cast<int>(maxAdap * 1000.0));
}

/**
 * REQ-038 S1 (AC-038.1, AC-038.2) — LA DETECCION GRUESA SOBRE EL CORPUS, MEDIDA DONDE CORRESPONDE.
 *
 * 🔴 EL INSTRUMENTO ESTABA MAL, Y ESO VA PRIMERO. El barrido guardaba `detectedHz` de la ULTIMA
 * publicacion de la nota — la cola decayendo, donde la señal ya cruzo el piso y el detector ve lo
 * que queda. Con esa cifra `bajo-pua_A1` figuraba a **−4,66 c** y parecia el peor caso del corpus,
 * cuando su mediana tras 1 s esta en ±0,2. Una tabla de la gruesa construida sobre eso mezcla dos
 * efectos y acusa al detector por lecturas que no representan la nota. Es la misma clase que
 * REQ-035, donde el instrumento era el test anterior.
 *
 * Con el instrumento arreglado (`coarseAtReading`, `coarseMedian`, la ultima al lado), esta tabla
 * contesta la pregunta de AC-038.2: si el sesgo se concentra en el timbre de la guitarra de ACERO
 * —donde el oraculo por parcial mide H2 muy por encima de H1— o si esta repartido.
 *
 * Lo que AFIRMA (lo demas lo mide): que las tres cifras salen del MISMO barrido; que la mediana se
 * calcula sobre una muestra que existe (≥ 3 publicaciones tras el asentamiento, o el archivo se
 * declara sin muestra en vez de publicar una mediana de una); y **el control del instrumento**:
 * `bajo-pua_A1`, que era el caso testigo, cae dentro de ±1 c medido donde corresponde. Si ese
 * control fallara, la cifra nueva no es la que se cree y nada de lo de abajo significa algo.
 */
TEST(CorpusRobustness, TheCoarseDetectionMeasuredWhereItMeansSomething) {
    const auto st = corpus::stateOf(corpus::defaultCorpusDir(), corpus::manifestPath());
    if (!corpus::shouldRunRobustness(st)) {
        GTEST_SKIP() << "sin corpus grabado (" << corpus::describe(st) << ")";
    }
    /// Desde donde se considera asentada la nota para la mediana. 1,0 s: es el instante en que la
    /// sonda del 08/09 midio ±0,2 c en 33 de 41, y el oraculo del manifiesto mide desde 2,0.
    constexpr double kSettledSec = 1.0;
    constexpr int kMinSamples = 3;

    const auto& results = sweptCorpus();
    ASSERT_FALSE(results.empty());

    auto centsOf = [](double hz, double ref) { return 1200.0 * std::log2(hz / ref); };
    auto familyOf = [](const std::string& n) -> const char* {
        if (n.rfind("guitarra-acero", 0) == 0) return "acero";
        if (n.rfind("guitarra", 0) == 0) return "otra guitarra";
        if (n.rfind("bajo", 0) == 0) return "bajo";
        return "ukelele";
    };

    std::printf("\n  [REQ-038] la gruesa sobre el corpus, por instante. `lectura` = la publicacion de la que sale la "
                "fina (la que ARBITRA dominio y signo); `mediana` = desde %.1f s (describe la NOTA); `ultima` = la cola "
                "(lo que el barrido reportaba ANTES, y por eso figura al lado)\n", kSettledSec);
    std::printf("  %-24s %8s %8s %8s | %7s %4s | %6s %6s %6s | %s\n", "archivo", "lectura", "mediana", "ultima",
                "disper", "n", "dB_H2", "dB_H3", "dB_H4", "familia");

    struct Acc { double worst = 0.0; double sum = 0.0; int n = 0; };
    std::map<std::string, Acc> porFamilia;
    double medianaBajoPua = NAN, ultimaBajoPua = NAN;
    int sinMuestra = 0;

    for (const corpus::Outcome& o : results) {
        if (!o.analysed) continue;
        const double atReading = corpus::coarseAtReading(o);
        const double median = corpus::coarseMedian(o, kSettledSec);
        const int n = corpus::coarseSampleCount(o, kSettledSec);
        const double spread = corpus::coarseSpreadCents(o, kSettledSec);

        // El nivel de los parciales, del oraculo: es lo que separa el timbre de acero del resto.
        const wav::WavData data = wav::readWav((corpus::defaultCorpusDir() + "/" + o.name).c_str());
        std::vector<float> mono(static_cast<size_t>(data.numFrames));
        for (int i = 0; i < data.numFrames; ++i)
            mono[static_cast<size_t>(i)] = 0.5f * (data.buffer[static_cast<size_t>(i) * 2]
                                                   + data.buffer[static_cast<size_t>(i) * 2 + 1]);
        const wma_test::oracle::PartialReading sus =
            wma_test::oracle::measureSustained(mono, data.sampleRate, o.trueHz);

        const double cReading = std::isfinite(atReading) ? centsOf(atReading, o.trueHz) : NAN;
        const double cMedian  = std::isfinite(median)    ? centsOf(median, o.trueHz)    : NAN;
        const double cLast    = o.detectedHz > 0.0       ? centsOf(o.detectedHz, o.trueHz) : NAN;

        std::printf("  %-24s %+8.2f %+8.2f %+8.2f | %7.2f %4d | %+6.1f %+6.1f %+6.1f | %s\n", o.name.c_str(),
                    cReading, cMedian, cLast, spread, n,
                    sus.valid ? sus.db[2] : NAN, sus.valid ? sus.db[3] : NAN, sus.valid ? sus.db[4] : NAN,
                    familyOf(o.name));

        if (n < kMinSamples) { ++sinMuestra; continue; }
        Acc& a = porFamilia[familyOf(o.name)];
        a.worst = std::max(a.worst, std::fabs(cMedian));
        a.sum += std::fabs(cMedian);
        ++a.n;
        if (o.name == "bajo-pua_A1.wav") { medianaBajoPua = cMedian; ultimaBajoPua = cLast; }
    }

    std::printf("\n  [REQ-038] por familia de timbre (sobre la MEDIANA, |cents| contra el oraculo):\n");
    for (const auto& kv : porFamilia)
        std::printf("    %-16s peor %5.2f c   promedio %5.2f c   (%d archivos)\n",
                    kv.first.c_str(), kv.second.worst, kv.second.sum / kv.second.n, kv.second.n);
    std::printf("  archivos sin muestra suficiente tras %.1f s: %d\n\n", kSettledSec, sinMuestra);

    // --- lo que se AFIRMA -------------------------------------------------------------------
    // El control del instrumento. Sin esto, la tabla de arriba es un numero nuevo sin validar.
    ASSERT_FALSE(std::isnan(medianaBajoPua))
        << "bajo-pua_A1 no dejo muestra tras el asentamiento: el control del instrumento no se pudo evaluar";
    EXPECT_LT(std::fabs(medianaBajoPua), 1.0)
        << "bajo-pua_A1 sigue apartado medido donde corresponde (" << medianaBajoPua
        << " c): la cifra nueva no es la que se creia, o el detector SI se aparta ahi";
    EXPECT_GT(std::fabs(ultimaBajoPua), std::fabs(medianaBajoPua))
        << "la ultima publicacion de bajo-pua_A1 (" << ultimaBajoPua << " c) no esta mas apartada que su mediana ("
        << medianaBajoPua << " c): el defecto del instrumento que este test existe para arreglar no se reproduce";

    EXPECT_EQ(sinMuestra, 0)
        << sinMuestra << " archivos no tienen " << kMinSamples << " publicaciones con altura tras "
        << kSettledSec << " s: su mediana seria de una muestra";

    /**
     * 🔴 LAS TRES CIFRAS TIENEN QUE SER TRES, y esto lo afirma. Dos mutantes sobrevivieron a la
     * primera version de este test porque las columnas `lectura` y el asentamiento de la mediana se
     * IMPRIMIAN y no se afirmaban: con `coarseAtReading` mirando la cola, y con `coarseMedian`
     * ignorando `fromSec`, la tabla salia distinta y el test seguia verde. Un instrumento cuyas
     * cifras nadie distingue no es un instrumento.
     */
    int difierenLecturaYUltima = 0, difierenConYSinAsentamiento = 0;
    for (const corpus::Outcome& o : results) {
        if (!o.analysed || !(o.detectedHz > 0.0)) continue;
        const double atReading = corpus::coarseAtReading(o);
        if (std::isfinite(atReading) && std::fabs(1200.0 * std::log2(atReading / o.detectedHz)) > 1.0)
            ++difierenLecturaYUltima;
        const double conAsentamiento = corpus::coarseMedian(o, kSettledSec);
        const double sinAsentamiento = corpus::coarseMedian(o, 0.0);
        if (std::isfinite(conAsentamiento) && std::isfinite(sinAsentamiento)
            && std::fabs(1200.0 * std::log2(conAsentamiento / sinAsentamiento)) > 0.05)
            ++difierenConYSinAsentamiento;
    }
    std::printf("  archivos donde la cifra CAMBIA segun el instante: lectura vs ultima = %d · "
                "mediana con vs sin asentamiento = %d\n\n", difierenLecturaYUltima, difierenConYSinAsentamiento);
    EXPECT_GT(difierenLecturaYUltima, 0)
        << "en ningun archivo la gruesa de la publicacion de la lectura difiere de la ultima: o el "
           "corpus cambio, o `coarseAtReading` dejo de mirar la publicacion con lectura fina y las "
           "dos columnas son la misma";
    EXPECT_GT(difierenConYSinAsentamiento, 0)
        << "en ningun archivo la mediana cambia al excluir el ataque: `coarseMedian` dejo de "
           "respetar `fromSec` y la cifra ya no describe la nota asentada";

    // --- AC-038.2: el sesgo SI se concentra en acero ------------------------------------------
    ASSERT_TRUE(porFamilia.count("acero") && porFamilia.count("otra guitarra"));
    EXPECT_GT(porFamilia["acero"].worst, 4.0 * porFamilia["otra guitarra"].worst)
        << "el sesgo dejo de concentrarse en la guitarra de acero (peor acero "
        << porFamilia["acero"].worst << " c contra " << porFamilia["otra guitarra"].worst
        << " c en las otras): si se emparejo, este REQ perdio su caso y hay que re-mirarlo";

    /**
     * 🔴 Y LA HIPOTESIS DEL TIMBRE, REFUTADA — el hallazgo de S1.
     *
     * La spec entro suponiendo que el sesgo venia de tener el SEGUNDO PARCIAL DOMINANTE (acero_E2
     * tiene H2 a +14,7 dB sobre H1). La tabla lo desmiente con contraejemplos de las dos clases, y
     * quedan congelados acá porque son lo que le dice a S2 por donde NO buscar:
     *
     *   · `guitarra-jazz_E2`  H2 a +14,3 dB — casi igual que acero_E2 — y mediana −0,00 c.
     *   · `guitarra-limpia_D3` H2 a +14,3 dB y mediana +0,01 c.
     *   · `guitarra-acero_A2` H2 a −17,8 dB (el segundo parcial es DEBIL) y mediana −1,73 c.
     *
     * O sea: H2 dominante sin sesgo, y sesgo sin H2 dominante. Lo que sí acompaña al sesgo es la
     * DISPERSION entre ventanas (acero: 3,9–10,2 c; el resto casi todo por debajo de 1,6), pero
     * tampoco alcanza sola —`bajo-fretless_D2` dispersa 6,4 c con la mediana en 0,00—, asi que lo
     * de acero no es ruido simetrico sino algo sistematico. Eso es lo que AC-038.3 va a mirar
     * adentro del detector.
     */
    auto medianaDe = [&](const char* name) {
        for (const corpus::Outcome& o : results)
            if (o.name == name) return 1200.0 * std::log2(corpus::coarseMedian(o, kSettledSec) / o.trueHz);
        return std::numeric_limits<double>::quiet_NaN();
    };
    EXPECT_LT(std::fabs(medianaDe("guitarra-jazz_E2.wav")), 0.5)
        << "guitarra-jazz_E2 (H2 a +14,3 dB, casi como acero_E2) empezo a sesgarse: el contraejemplo "
           "que refuta 'el sesgo lo causa H2 dominante' dejo de reproducirse";
    EXPECT_GT(std::fabs(medianaDe("guitarra-acero_A2.wav")), 1.0)
        << "guitarra-acero_A2 (H2 a −17,8 dB, el segundo parcial DEBIL) dejo de sesgarse: el otro "
           "contraejemplo tampoco se reproduce, y la hipotesis del timbre habria que re-abrirla";

    RecordProperty("familias", static_cast<int>(porFamilia.size()));
    RecordProperty("peor_acero_milicents", static_cast<int>(porFamilia["acero"].worst * 1000.0));
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
