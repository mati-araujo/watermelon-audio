/**
 * REQ-055 S0 · 0.3 (D35, D36, D41, D42) — EL MANIFIESTO de las tomas reales y la validacion de sus
 * cortes en TRES VIAS (manifiesto == `wmad` del WAV == `# corte` del TSV del oraculo).
 *
 * AC-055.1 — una toma con perdidas ubicadas sale sana y las declara; quien la mide corta ahi.
 * Estos tests protegen que la DECLARACION no mienta: si un `# corte` queda un frame corrido (o el
 * parser lee el span de la costura como un punto), el instrumento de 0.3 mide a traves de un hueco con
 * la toma "verificada", que es exactamente el WAV con huecos que parece sano.
 *
 * COMO ESTA ARMADO. Los archivos son tomas SINTETICAS con la forma exacta de un volcado: el header lo
 * escribe `writeInputDumpHeader` de PRODUCCION (`syntheticTakeBytes`) y se lee con `parseWmadV2Chunk`, un
 * parser propio. Para corromperlos se escriben bytes en los OFFSETS LITERALES del archivo:
 *
 *   RIFF 12 + fmt (8 + 18) + fact (8 + 4) + cabecera del chunk wmad (8)  =>  el CUERPO del wmad empieza
 *   en el byte 58 (`kTakeWmad`). Dentro del cuerpo (TunerInputDump.h): u32 version @0, u64 framesWritten
 *   @4, u64 framesLost @12, u64 discontinuities @20, i32 sampleRate @28, i32 inputSource @32, u32 valid
 *   @36, u64 unlocatedFrames @40, u64 unlocatedDiscontinuities @48, u32 cutCount @56, u32 cutCapacity
 *   @60, y desde @64 los cortes de 24 bytes {u64 fileFrame, u64 lostFrames, u32 fileSpan, u32 kind}.
 *   Despues del wmad (58 + 6208) viene la cabecera de `data` (id @6266, tamano u32 @6270, muestras @6274).
 *   El rate de `fmt` es un u32 en el byte 24. `TheCorruptionOffsetsMatchTheLayout` ata estos numeros al
 *   archivo que escribe produccion: si el layout se mueve, ese test se pone rojo ANTES que los otros den
 *   un verde por haber corrompido bytes que ya no son esos.
 *
 * Cada rechazo se prueba con UNA sola falta (si no, el rechazo podria venir de otro chequeo y quitar el
 * chequeo no se veria), y cada caso comprueba primero que el archivo SANO del mismo armado pasa.
 *
 * Los temporales van SIEMPRE a `corpus::makeTempDir()` (/tmp/wma-corpus-*); nunca se crea
 * `tests/corpus-g42-manifest.txt`.
 */

#include "support/SyntheticTake.h"
#include "support/TakeManifest.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace wma_test::take {
namespace {

namespace corpus_ns = wma_test::corpus;

constexpr int kTakeTestRate = 48000;
constexpr uint64_t kTakeTestFrames = 1000;
constexpr size_t kTakeWmad = 58;
constexpr size_t kTakeWmadCuts = kTakeWmad + 64;
constexpr size_t kTakeWmadBodyBytes = 64 + 24 * 256;
constexpr size_t kTakeDataHeader = kTakeWmad + kTakeWmadBodyBytes;
constexpr size_t kTakeFmtRate = 24;
static_assert(kTakeDataHeader + 8 == wma::analysis::kInputDumpHeaderBytes, "el layout del header v2");

/// Escribe `v` little-endian en `bytes[off, off + n)`.
void takePut(std::vector<uint8_t>* bytes, size_t off, uint64_t v, int n) {
    for (int i = 0; i < n; ++i) (*bytes)[off + static_cast<size_t>(i)] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

/// Un corte en el mapa del archivo, en el orden de los cortes: `kTakeWmadCuts + 24·i`.
size_t takeCutOffset(int i) { return kTakeWmadCuts + 24 * static_cast<size_t>(i); }

/// Los tres cortes de la toma base: una perdida de 7, una costura de 100 y una perdida de 11. Todos los
/// campos con valores DISTINTOS entre si: un offset vecino leido por otro da un numero que no coincide.
std::vector<SyntheticTakeCut> takeBaseCuts() {
    std::vector<SyntheticTakeCut> c(3);
    c[0].fileFrame = 300; c[0].lost = 7;  c[0].span = 0;   c[0].seam = false;
    c[1].fileFrame = 600; c[1].lost = 0;  c[1].span = 100; c[1].seam = true;
    c[2].fileFrame = 900; c[2].lost = 11; c[2].span = 0;   c[2].seam = false;
    return c;
}

/// Lo que `takeBaseCuts` tiene que decir como lineas `# corte`.
std::vector<TakeCutLine> takeBaseLines() {
    return {{300, 300, 7}, {600, 700, -1}, {900, 900, 11}};
}

/// El mono de una toma de `frames` frames: una rampa de senoide, sin ceros que se confundan con relleno.
std::vector<float> takeTestMono(uint64_t frames) {
    std::vector<float> m(static_cast<size_t>(frames));
    for (size_t i = 0; i < m.size(); ++i) m[i] = static_cast<float>(0.25 * std::sin(0.1 * static_cast<double>(i)));
    return m;
}

std::vector<uint8_t> takeBytesOf(const std::vector<SyntheticTakeCut>& cuts, uint64_t frames = kTakeTestFrames,
                                 int rate = kTakeTestRate) {
    return syntheticTakeBytes(takeTestMono(frames), syntheticTakeStats(frames, rate, cuts));
}

std::string takeParse(const std::vector<uint8_t>& bytes, const std::string& name = "toma.wav", WmadV2* out = nullptr) {
    WmadV2 scratch;
    return parseWmadV2Chunk(bytes, name, out != nullptr ? out : &scratch);
}

// ============================ parseWmadV2Chunk: lo sano ============================

// AC-055.1 / D36. atrapa: el span de una costura leido como un punto (INI == FIN), el lostFrames de un
// offset vecino, un corte que se pierde o se reordena, y un parser que acepta lo que produccion escribe.
TEST(TakeManifest, AProductionV2HeaderParsesToExactlyItsCuts) {
    WmadV2 w;
    ASSERT_EQ("", takeParse(takeBytesOf(takeBaseCuts()), "toma.wav", &w));
    const std::vector<TakeCutLine> want = takeBaseLines();
    ASSERT_EQ(want.size(), w.cuts.size());
    for (size_t i = 0; i < want.size(); ++i) {
        EXPECT_EQ(want[i].ini, w.cuts[i].ini) << "corte " << i;
        EXPECT_EQ(want[i].fin, w.cuts[i].fin) << "corte " << i;
        EXPECT_EQ(want[i].lost, w.cuts[i].lost) << "corte " << i;
    }
    EXPECT_EQ(kTakeTestFrames, w.framesWritten);
    EXPECT_EQ(18u, w.framesLost) << "7 + 11";
    EXPECT_EQ(1u, w.discontinuities) << "la costura";
    EXPECT_EQ(3u, w.cutCount);
    EXPECT_EQ(256u, w.cutCapacity);
    EXPECT_EQ(1u, w.valid);
    EXPECT_EQ(kTakeTestRate, w.fmtRate);
    EXPECT_EQ(kTakeTestRate, w.sampleRate);
    EXPECT_EQ(kTakeTestFrames * 4, w.dataBytes);
}

// AC-055.1. atrapa: un parser que exige al menos un corte (la toma sin perdidas es la sana por excelencia).
TEST(TakeManifest, ASoundTakeWithoutCutsParsesToAnEmptyMap) {
    WmadV2 w;
    ASSERT_EQ("", takeParse(takeBytesOf({}), "toma.wav", &w));
    EXPECT_TRUE(w.cuts.empty());
    EXPECT_EQ(0u, w.cutCount);
}

// AC-055.1. atrapa: `>` por `>=` en «fuera de [0, framesWritten]»: una perdida AL FINAL de la toma (los
// frames que faltan van justo antes de framesWritten) y una costura que termina exactamente en
// framesWritten son legales; framesWritten + 1 no (eso esta en `EachRejectionComesFromItsOwnCheck`).
TEST(TakeManifest, ACutRightAtTheEndOfTheTakeIsSound) {
    std::vector<SyntheticTakeCut> c(2);
    c[0].fileFrame = 600; c[0].span = 400; c[0].seam = true;
    c[1].fileFrame = kTakeTestFrames; c[1].lost = 5; c[1].seam = false;
    WmadV2 w;
    ASSERT_EQ("", takeParse(takeBytesOf(c), "toma.wav", &w));
    ASSERT_EQ(2u, w.cuts.size());
    EXPECT_TRUE((w.cuts[0] == TakeCutLine{600, 1000, -1}));
    EXPECT_TRUE((w.cuts[1] == TakeCutLine{1000, 1000, 5}));
}

// D36. atrapa: corromper bytes en offsets que ya no son los del layout de produccion y creer que se probo
// un rechazo (el verde por haber tocado otra cosa). Ata los numeros de la cabecera del archivo al escritor.
TEST(TakeManifest, TheCorruptionOffsetsMatchTheLayout) {
    const std::vector<uint8_t> b = takeBytesOf(takeBaseCuts());
    ASSERT_EQ(wma::analysis::kInputDumpHeaderBytes + kTakeTestFrames * 4, b.size());
    EXPECT_EQ(0, std::memcmp(b.data() + 50, "wmad", 4)) << "el chunk wmad empieza en 50: su cuerpo en 58";
    EXPECT_EQ(0, std::memcmp(b.data() + kTakeDataHeader, "data", 4));
    auto u32At = [&b](size_t off) {
        return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
               (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
    };
    EXPECT_EQ(static_cast<uint32_t>(kTakeTestRate), u32At(kTakeFmtRate)) << "rate de fmt";
    EXPECT_EQ(2u, u32At(kTakeWmad)) << "version";
    EXPECT_EQ(1u, u32At(kTakeWmad + 36)) << "valid";
    EXPECT_EQ(3u, u32At(kTakeWmad + 56)) << "cutCount";
    EXPECT_EQ(256u, u32At(kTakeWmad + 60)) << "cutCapacity";
    EXPECT_EQ(300u, u32At(takeCutOffset(0))) << "fileFrame del corte 0";
    EXPECT_EQ(7u, u32At(takeCutOffset(0) + 8)) << "lostFrames del corte 0";
    EXPECT_EQ(100u, u32At(takeCutOffset(1) + 16)) << "fileSpan del corte 1";
    EXPECT_EQ(2u, u32At(takeCutOffset(1) + 20)) << "kind (costura) del corte 1";
    EXPECT_EQ(kTakeTestFrames * 4, u32At(kTakeDataHeader + 4)) << "tamano de data";
}

// ============================ parseWmadV2Chunk: los rechazos ============================

struct TakeRejectCase {
    std::string what;                              ///< el chequeo que tiene que saltar
    std::vector<SyntheticTakeCut> cuts;            ///< el armado sano
    std::function<void(std::vector<uint8_t>*)> corrupt;  ///< UNA falta sobre el archivo sano
    std::string name = "toma.wav";                 ///< el nombre con el que se lee la version corrupta
    bool truncate = false;                         ///< el archivo se corta 4 bytes (no hay campo que escribir)
};

std::vector<TakeRejectCase> takeRejectCases() {
    std::vector<TakeRejectCase> v;
    auto add = [&v](const std::string& what, std::function<void(std::vector<uint8_t>*)> fn,
                    std::vector<SyntheticTakeCut> cuts = takeBaseCuts()) {
        TakeRejectCase c;
        c.what = what;
        c.cuts = std::move(cuts);
        c.corrupt = std::move(fn);
        v.push_back(std::move(c));
    };
    add("version 1 (la v1 no lleva mapa de cortes)", [](std::vector<uint8_t>* b) { takePut(b, kTakeWmad, 1, 4); });
    add("version 3 (una que este codigo no conoce)", [](std::vector<uint8_t>* b) { takePut(b, kTakeWmad, 3, 4); });
    add("valid = 0", [](std::vector<uint8_t>* b) { takePut(b, kTakeWmad + 36, 0, 4); });
    // unlocatedFrames > 0 con framesLost coherente (18 + 5): lo unico que falla es la perdida NO ubicada.
    add("unlocatedFrames > 0", [](std::vector<uint8_t>* b) {
        takePut(b, kTakeWmad + 40, 5, 8);
        takePut(b, kTakeWmad + 12, 18 + 5, 8);
    });
    add("unlocatedDiscontinuities > 0", [](std::vector<uint8_t>* b) { takePut(b, kTakeWmad + 48, 1, 8); });
    add("chunk != 64 + 24*cap (cap 255 con el chunk de 256)", [](std::vector<uint8_t>* b) { takePut(b, kTakeWmad + 60, 255, 4); });
    add("chunk != 64 + 24*cap (cap 257 con el chunk de 256)", [](std::vector<uint8_t>* b) { takePut(b, kTakeWmad + 60, 257, 4); });
    // Con cutCount = 257 el corte 257 caeria en la cabecera de `data`: otro chequeo podria atraparlo igual,
    // asi que esta es la unica falta que no se puede aislar del todo (el parser no debe llegar a leerlo).
    add("cutCount > cutCapacity", [](std::vector<uint8_t>* b) { takePut(b, kTakeWmad + 56, 257, 4); });
    add("kind = 0 en un corte", [](std::vector<uint8_t>* b) { takePut(b, takeCutOffset(1) + 20, 0, 4); });
    add("kind = 3 en un corte", [](std::vector<uint8_t>* b) { takePut(b, takeCutOffset(0) + 20, 3, 4); });
    // El corte 0 pasa a 700 (< 900 y fuera de orden con el 600): lo unico que falla es el desorden.
    add("cortes desordenados", [](std::vector<uint8_t>* b) { takePut(b, takeCutOffset(0), 700, 8); });
    add("costura que termina pasado framesWritten", [](std::vector<uint8_t>* b) { takePut(b, takeCutOffset(1) + 16, 2000, 4); });
    {
        std::vector<SyntheticTakeCut> one(1);
        one[0].fileFrame = kTakeTestFrames; one[0].lost = 5;
        add("perdida pasado framesWritten (framesWritten + 1)", [](std::vector<uint8_t>* b) { takePut(b, takeCutOffset(0), kTakeTestFrames + 1, 8); }, one);
    }
    add("perdida con fileSpan", [](std::vector<uint8_t>* b) { takePut(b, takeCutOffset(0) + 16, 5, 4); });
    // lostFrames de una costura con framesLost coherente (18 + 5): lo unico que falla es la costura con perdidos.
    add("costura con lostFrames", [](std::vector<uint8_t>* b) {
        takePut(b, takeCutOffset(1) + 8, 5, 8);
        takePut(b, kTakeWmad + 12, 18 + 5, 8);
    });
    add("framesLost != suma de las perdidas", [](std::vector<uint8_t>* b) { takePut(b, kTakeWmad + 12, 999, 8); });
    // El volcado es float32 mono: un fmt de otra cosa con el mismo `data` miente sobre los frames.
    add("fmt PCM (tag 1)", [](std::vector<uint8_t>* b) { takePut(b, 20, 1, 2); });
    add("fmt estereo", [](std::vector<uint8_t>* b) { takePut(b, 22, 2, 2); });
    add("fmt de 16 bits", [](std::vector<uint8_t>* b) { takePut(b, 34, 16, 2); });
    // Desbordes de u64 (security-auditor): hoy el oraculo Python los rechaza (enteros sin limite).
    // La ultima perdida pasa a costura que empieza cerca de 2^64: `frame + span` da la vuelta a 10.
    add("costura cuyo fin da la vuelta en u64", [](std::vector<uint8_t>* b) {
        takePut(b, takeCutOffset(2), UINT64_MAX - 9, 8);
        takePut(b, takeCutOffset(2) + 8, 0, 8);
        takePut(b, takeCutOffset(2) + 16, 20, 4);
        takePut(b, takeCutOffset(2) + 20, 2, 4);
        takePut(b, kTakeWmad + 12, 7, 8);
    });
    // lostFrames de 2^64 - 1 en el primer corte: la suma (2^64 - 1) + 11 da 10, y framesLost dice 10.
    add("suma de perdidas que da la vuelta en u64", [](std::vector<uint8_t>* b) {
        takePut(b, takeCutOffset(0) + 8, UINT64_MAX, 8);
        takePut(b, kTakeWmad + 12, 10, 8);
    });
    // framesWritten = 2^62 + N: por 4 da la vuelta a 4·N, que es justo el `data` del archivo.
    add("framesWritten·4 que da la vuelta en u64", [](std::vector<uint8_t>* b) {
        takePut(b, kTakeWmad + 4, (uint64_t{1} << 62) + kTakeTestFrames, 8);
    });
    add("rate del wmad != rate de fmt", [](std::vector<uint8_t>* b) { takePut(b, kTakeFmtRate, 44100, 4); });
    // `data` mas corto que framesWritten·4 (el archivo conserva los bytes: no es un archivo cortado).
    add("data != framesWritten*4", [](std::vector<uint8_t>* b) { takePut(b, kTakeDataHeader + 4, kTakeTestFrames * 4 - 4, 4); });
    add("no es un RIFF", [](std::vector<uint8_t>* b) { (*b)[0] = 'X'; });
    {
        TakeRejectCase t;
        t.what = "archivo cortado (data declara mas de lo que hay)";
        t.cuts = takeBaseCuts();
        t.truncate = true;
        v.push_back(std::move(t));
    }
    {
        TakeRejectCase t;
        t.what = "nombre .invalid.wav";
        t.cuts = takeBaseCuts();
        t.corrupt = [](std::vector<uint8_t>*) {};
        t.name = "toma.invalid.wav";
        v.push_back(std::move(t));
    }
    return v;
}

// AC-055.1 / D36. atrapa: CADA chequeo del oraculo quitado del parser C++ (version, valid, unlocated*,
// chunk, cutCount, kind, orden, rango, span de perdida, perdidos de costura, suma, rate, data, nombre,
// archivo cortado). Cada caso: el sano pasa, el corrupto no.
TEST(TakeManifest, EachRejectionComesFromItsOwnCheck) {
    for (const TakeRejectCase& c : takeRejectCases()) {
        SCOPED_TRACE(c.what);
        std::vector<uint8_t> bytes = takeBytesOf(c.cuts);
        EXPECT_EQ("", takeParse(bytes, "toma.wav")) << "el archivo sano del mismo armado tiene que pasar";
        if (c.truncate) {
            bytes.resize(bytes.size() - 4);
        } else {
            c.corrupt(&bytes);
        }
        EXPECT_NE("", takeParse(bytes, c.name)) << "el corrupto se acepto";
    }
}

// AC-055.1 / D36. atrapa: el chequeo `cutCount > cutCapacity` quitado. Es el unico rechazo que otro chequeo
// tapa (el corte 257 cae sobre la cabecera de `data` y sale «fuera de rango»), pero sin el el parser LEE
// pasado el chunk: si el `wmad` es el ultimo chunk, pasado el fin del archivo. Por eso se exige el MOTIVO.
TEST(TakeManifest, ACutCountOverTheCapacityIsRejectedByItsOwnCheck) {
    std::vector<uint8_t> bytes = takeBytesOf(takeBaseCuts());
    takePut(&bytes, kTakeWmad + 56, 257, 4);
    const std::string why = takeParse(bytes, "toma.wav");
    EXPECT_NE(std::string::npos, why.find("cutCount")) << why;
}

// ============================ takeCutsAgree / checkTakeFile: las tres vias ============================

/// Una variante de una secuencia de cortes (lo que un `# corte` mal copiado, un TSV viejo o un wmad de otra
/// toma podrian traer), con su nombre.
struct TakeLineVariant {
    std::string what;
    std::vector<TakeCutLine> cuts;
};

/// Todas las formas de desviar `base` por UNA diferencia: ±1 en INI, en FIN y en PERDIDOS de cada corte, `?`
/// contra un numero (en los dos sentidos), un corte faltante, uno sobrante y dos cortes en otro orden.
std::vector<TakeLineVariant> takeLineVariants(const std::vector<TakeCutLine>& base) {
    std::vector<TakeLineVariant> v;
    for (size_t i = 0; i < base.size(); ++i) {
        const std::string at = "corte " + std::to_string(i) + ": ";
        for (int d : {-1, 1}) {
            const std::string sg = d < 0 ? "-1" : "+1";
            auto ini = base; ini[i].ini += d; v.push_back({at + "INI " + sg, ini});
            auto fin = base; fin[i].fin += d; v.push_back({at + "FIN " + sg, fin});
            if (base[i].lost >= 0) {
                auto lost = base; lost[i].lost += d; v.push_back({at + "PERDIDOS " + sg, lost});
            }
        }
        auto q = base;
        q[i].lost = base[i].lost < 0 ? 5 : -1;
        v.push_back({at + (base[i].lost < 0 ? "'?' declarado como un numero" : "un numero declarado como '?'"), q});
        auto miss = base; miss.erase(miss.begin() + static_cast<std::ptrdiff_t>(i)); v.push_back({at + "faltante", miss});
    }
    auto extra = base; extra.push_back({950, 950, 1}); v.push_back({"corte sobrante", extra});
    auto swapped = base; std::swap(swapped[0], swapped[1]); v.push_back({"dos cortes en otro orden", swapped});
    return v;
}

// D36. atrapa: comparar SOLO el INI (o solo el FIN), tomar `?` por comodin, comparar el manifiesto con el wmad
// sin mirar el TSV (o al reves), aceptar un corte faltante o sobrante. Se desvia CADA via, de a una.
TEST(TakeManifest, AnyDeviationInOneOfTheThreeWaysIsDetected) {
    const std::vector<TakeCutLine> base = takeBaseLines();
    EXPECT_EQ("", takeCutsAgree(base, base, base)) << "las tres iguales: coinciden";
    for (const TakeLineVariant& v : takeLineVariants(base)) {
        SCOPED_TRACE(v.what);
        EXPECT_NE("", takeCutsAgree(v.cuts, base, base)) << "desvia el manifiesto";
        EXPECT_NE("", takeCutsAgree(base, v.cuts, base)) << "desvia el wmad";
        EXPECT_NE("", takeCutsAgree(base, base, v.cuts)) << "desvia el TSV";
    }
}

/// Un directorio temporal con UNA toma escrita adentro y lo que hace falta para pasarla por `checkTakeFile`.
struct TakeFixture {
    std::string dir = corpus_ns::makeTempDir();
    std::string name = "g42-criolla-20261028.wav";
    ~TakeFixture() { corpus_ns::removeTempDir(dir); }

    std::string path() const { return dir + "/" + name; }
    void write(const std::vector<uint8_t>& bytes) const { ASSERT_TRUE(writeSyntheticTake(path(), bytes)); }

    TakeEntry entryFor(const std::vector<TakeCutLine>& cuts, int rate = kTakeTestRate, int64_t frames = static_cast<int64_t>(kTakeTestFrames)) const {
        TakeEntry e;
        e.name = name;
        e.sha256 = corpus_ns::sha256Of(path());
        e.release = "corpus-g42-v1";
        e.rate = rate;
        e.frames = frames;
        e.family = "guitarra";
        e.cuts = cuts;
        return e;
    }

    std::string tsvFor(const std::string& sha, int rate, int64_t frames, const std::vector<TakeCutLine>& cuts,
                       const std::string& cutsOf = "") const {
        const std::string owner = cutsOf.empty() ? name : cutsOf;
        std::string s = "# archivo " + name + " sha256 " + sha + " rate " + std::to_string(rate) + " frames " + takeNum(frames) + "\n";
        for (const TakeCutLine& c : cuts)
            s += "# corte " + owner + " " + takeNum(c.ini) + " " + takeNum(c.fin) + " " +
                 (c.lost < 0 ? std::string("?") : takeNum(c.lost)) + "\n";
        return s;
    }

    /// La toma sana de siempre (los tres cortes base), escrita; devuelve su entrada y el TSV que la acompana.
    void writeBase(TakeEntry* e, std::string* tsv) const {
        write(takeBytesOf(takeBaseCuts()));
        *e = entryFor(takeBaseLines());
        *tsv = tsvFor(e->sha256, kTakeTestRate, static_cast<int64_t>(kTakeTestFrames), takeBaseLines());
    }
};

// D36. atrapa: un `checkTakeFile` que rechaza la toma buena (si no, todos los rechazos de abajo valen cero).
TEST(TakeManifest, ASoundTakeAgreesInTheThreeWays) {
    TakeFixture fx;
    ASSERT_FALSE(fx.dir.empty());
    TakeEntry e;
    std::string tsv;
    fx.writeBase(&e, &tsv);
    ASSERT_EQ(64u, e.sha256.size()) << "shasum no respondio";
    WmadV2 w;
    EXPECT_EQ("", checkTakeFile(fx.dir, e, tsv, &w));
    EXPECT_EQ(3u, w.cuts.size()) << "`wmadOut` recibe lo leido";
}

// D36. atrapa: un TSV que trae `# corte` de OTRA toma contado como propio (no filtrar por nombre): la toma
// buena tiene que seguir pasando.
TEST(TakeManifest, CutsOfAnotherTakeInTheTsvAreNotOurs) {
    TakeFixture fx;
    ASSERT_FALSE(fx.dir.empty());
    TakeEntry e;
    std::string tsv;
    fx.writeBase(&e, &tsv);
    tsv += "# corte otra-toma.wav 10 20 ?\n# corte otra-toma.wav 30 30 4\n";
    EXPECT_EQ("", checkTakeFile(fx.dir, e, tsv));
}

// D36. atrapa: el TSV atribuye los cortes a OTRA toma (la propia queda sin ninguno): manifiesto == wmad pero el
// TSV no los declara para esta toma; si el chequeo mira al TSV solo por la existencia de algun `# corte`, pasa.
TEST(TakeManifest, CutsAttributedToAnotherTakeInTheTsvFail) {
    TakeFixture fx;
    ASSERT_FALSE(fx.dir.empty());
    TakeEntry e;
    std::string tsv;
    fx.writeBase(&e, &tsv);
    const std::string theirs = fx.tsvFor(e.sha256, kTakeTestRate, static_cast<int64_t>(kTakeTestFrames), takeBaseLines(), "otra-toma.wav");
    ASSERT_EQ("", checkTakeFile(fx.dir, e, tsv));
    EXPECT_NE("", checkTakeFile(fx.dir, e, theirs));
}

// D36. atrapa: comparar el manifiesto con el wmad SIN el TSV (mutante de la lista), o el TSV con el wmad sin el
// manifiesto, o solo INI; cada desvio de ±1 en INI/FIN/PERDIDOS, `?` por numero, faltante y sobrante, en cada via
// por separado con `checkTakeFile` completo (archivo en disco, sha incluido).
TEST(TakeManifest, EachOfTheThreeWaysAloneBreaksTheCheck) {
    TakeFixture fx;
    ASSERT_FALSE(fx.dir.empty());
    TakeEntry e;
    std::string tsv;
    fx.writeBase(&e, &tsv);
    ASSERT_EQ("", checkTakeFile(fx.dir, e, tsv));
    const int64_t frames = static_cast<int64_t>(kTakeTestFrames);

    for (const TakeLineVariant& v : takeLineVariants(takeBaseLines())) {
        SCOPED_TRACE(v.what);
        // Solo el manifiesto se desvia: wmad == TSV.
        TakeEntry m = e;
        m.cuts = v.cuts;
        EXPECT_NE("", checkTakeFile(fx.dir, m, tsv)) << "manifiesto != wmad == TSV";
        // Solo el TSV se desvia: manifiesto == wmad. Mata comparar el manifiesto con el wmad sin el TSV.
        EXPECT_NE("", checkTakeFile(fx.dir, e, fx.tsvFor(e.sha256, kTakeTestRate, frames, v.cuts))) << "manifiesto == wmad != TSV";
    }

    // Solo el wmad se desvia: manifiesto == TSV. El archivo cambia (y su sha); manifiesto y TSV lo siguen en el sha.
    std::vector<std::pair<std::string, std::vector<SyntheticTakeCut>>> wmadVariants;
    for (size_t i = 0; i < 3; ++i) {
        for (int d : {-1, 1}) {
            const std::string at = "wmad corte " + std::to_string(i) + (d < 0 ? " -1" : " +1");
            auto f = takeBaseCuts(); f[i].fileFrame = static_cast<uint64_t>(static_cast<int64_t>(f[i].fileFrame) + d);
            wmadVariants.push_back({at + " fileFrame", f});
            if (!f[i].seam) {
                auto l = takeBaseCuts(); l[i].lost = static_cast<uint64_t>(static_cast<int64_t>(l[i].lost) + d);
                wmadVariants.push_back({at + " lostFrames", l});
            } else {
                auto s = takeBaseCuts(); s[i].span = static_cast<uint32_t>(static_cast<int64_t>(s[i].span) + d);
                wmadVariants.push_back({at + " fileSpan", s});
            }
        }
    }
    {
        auto drop = takeBaseCuts(); drop.erase(drop.begin() + 1); wmadVariants.push_back({"wmad sin la costura", drop});
        auto extra = takeBaseCuts();
        SyntheticTakeCut x; x.fileFrame = 950; x.lost = 1;
        extra.push_back(x);
        wmadVariants.push_back({"wmad con un corte de mas", extra});
    }
    for (const auto& wv : wmadVariants) {
        SCOPED_TRACE(wv.first);
        const std::vector<uint8_t> bytes = takeBytesOf(wv.second);
        ASSERT_EQ("", takeParse(bytes)) << "la variante del wmad tiene que ser una toma sana (si no el rechazo vendria de otro lado)";
        fx.write(bytes);
        const TakeEntry me = fx.entryFor(takeBaseLines());
        EXPECT_NE("", checkTakeFile(fx.dir, me, fx.tsvFor(me.sha256, kTakeTestRate, frames, takeBaseLines()))) << "manifiesto == TSV != wmad";
    }
}

// D36. atrapa: sha256 sin comparar contra una de las dos fuentes: manifiesto vs archivo, manifiesto vs `# archivo`
// del TSV, un TSV sin `# archivo`, y un archivo que no esta.
TEST(TakeManifest, TheShaMustMatchInTheThreeWays) {
    TakeFixture fx;
    ASSERT_FALSE(fx.dir.empty());
    TakeEntry e;
    std::string tsv;
    fx.writeBase(&e, &tsv);
    ASSERT_EQ("", checkTakeFile(fx.dir, e, tsv));
    const int64_t frames = static_cast<int64_t>(kTakeTestFrames);
    const std::string other(64, 'b');

    TakeEntry badManifest = e;
    badManifest.sha256 = other;
    EXPECT_NE("", checkTakeFile(fx.dir, badManifest, tsv)) << "archivo != manifiesto";
    // El TSV dice otro sha y manifiesto == archivo.
    EXPECT_NE("", checkTakeFile(fx.dir, e, fx.tsvFor(other, kTakeTestRate, frames, takeBaseLines()))) << "TSV != manifiesto";
    // Manifiesto == TSV pero ninguno es el del archivo.
    EXPECT_NE("", checkTakeFile(fx.dir, badManifest, fx.tsvFor(other, kTakeTestRate, frames, takeBaseLines()))) << "archivo != manifiesto == TSV";
    EXPECT_NE("", checkTakeFile(fx.dir, e, "# corte " + fx.name + " 300 300 7\n")) << "TSV sin su '# archivo'";

    // El archivo cambia de contenido (un bit de las muestras) y el manifiesto sigue declarando el sha de antes.
    std::vector<uint8_t> flipped = takeBytesOf(takeBaseCuts());
    flipped[flipped.size() - 1] ^= 0x01;
    fx.write(flipped);
    EXPECT_NE("", checkTakeFile(fx.dir, e, tsv)) << "el archivo ya no es el que declara el manifiesto";

    TakeEntry missing = e;
    missing.name = "no-existe.wav";
    EXPECT_NE("", checkTakeFile(fx.dir, missing, tsv)) << "archivo faltante";
}

// D36. atrapa: rate o frames sin cruzar entre manifiesto, WAV y TSV. Cada uno desviado de a uno.
TEST(TakeManifest, RateAndFramesMustMatchInTheThreeWays) {
    TakeFixture fx;
    ASSERT_FALSE(fx.dir.empty());
    TakeEntry e;
    std::string tsv;
    fx.writeBase(&e, &tsv);
    ASSERT_EQ("", checkTakeFile(fx.dir, e, tsv));
    const int64_t frames = static_cast<int64_t>(kTakeTestFrames);

    TakeEntry rateM = e;
    rateM.rate = 44100;
    EXPECT_NE("", checkTakeFile(fx.dir, rateM, tsv)) << "rate: manifiesto != WAV == TSV";
    EXPECT_NE("", checkTakeFile(fx.dir, e, fx.tsvFor(e.sha256, 44100, frames, takeBaseLines()))) << "rate: TSV != WAV == manifiesto";

    TakeEntry framesM = e;
    framesM.frames = frames + 1;
    EXPECT_NE("", checkTakeFile(fx.dir, framesM, tsv)) << "frames: manifiesto != WAV == TSV";
    EXPECT_NE("", checkTakeFile(fx.dir, e, fx.tsvFor(e.sha256, kTakeTestRate, frames + 1, takeBaseLines()))) << "frames: TSV != WAV == manifiesto";

    // El WAV a otro rate (44100) y otros frames (1100), con manifiesto y TSV que no se enteran. Los cortes
    // siguen siendo los mismos: lo unico que difiere es rate o frames.
    fx.write(takeBytesOf(takeBaseCuts(), kTakeTestFrames, 44100));
    TakeEntry wavRate = fx.entryFor(takeBaseLines());
    wavRate.rate = kTakeTestRate;
    EXPECT_NE("", checkTakeFile(fx.dir, wavRate, fx.tsvFor(wavRate.sha256, kTakeTestRate, frames, takeBaseLines()))) << "rate: WAV != manifiesto == TSV";
    fx.write(takeBytesOf(takeBaseCuts(), kTakeTestFrames + 100, kTakeTestRate));
    TakeEntry wavFrames = fx.entryFor(takeBaseLines());
    wavFrames.frames = frames;
    EXPECT_NE("", checkTakeFile(fx.dir, wavFrames, fx.tsvFor(wavFrames.sha256, kTakeTestRate, frames, takeBaseLines()))) << "frames: WAV != manifiesto == TSV";
}

// D36. atrapa: `checkTakeFile` que no pasa por `parseWmadV2Chunk` (un wmad con valid=0 o con una perdida NO
// ubicada, con manifiesto y TSV coherentes entre si y con el sha del archivo, tiene que fallar igual).
TEST(TakeManifest, AnUnsoundWmadFailsEvenWhenEverythingElseAgrees) {
    TakeFixture fx;
    ASSERT_FALSE(fx.dir.empty());
    std::vector<uint8_t> bytes = takeBytesOf(takeBaseCuts());
    takePut(&bytes, kTakeWmad + 36, 0, 4);   // valid = 0
    fx.write(bytes);
    const TakeEntry e = fx.entryFor(takeBaseLines());
    const std::string tsv = fx.tsvFor(e.sha256, kTakeTestRate, static_cast<int64_t>(kTakeTestFrames), takeBaseLines());
    EXPECT_NE("", checkTakeFile(fx.dir, e, tsv));
}

// ============================ parseTakeManifest: la gramatica ============================

const char* const kTakeGrammarName = "g42-criolla-20261028.wav";

std::string takeGrammarSha(char c = 'a') { return std::string(64, c); }

/// La linea de una toma: NOMBRE SHA256 RELEASE RATE FRAMES FAMILIA descripcion.
std::string takeGrammarHead(const std::string& name = kTakeGrammarName, const std::string& sha = takeGrammarSha(),
                            const std::string& rate = "48000", const std::string& frames = "480000",
                            const std::string& family = "guitarra") {
    return name + " " + sha + " corpus-g42-v1 " + rate + " " + frames + " " + family + " toma de prueba con espacios";
}

enum TakeGrammarLine : size_t { kTakeLineHead = 0, kTakeLineCutA, kTakeLineCutB, kTakeLineTargetA, kTakeLineTargetB, kTakeLineClap };

/// La toma sana: una perdida, una costura, dos objetivos (E2 y A2 de la guitarra) y un aplauso.
std::vector<std::string> takeGrammarLines() {
    const std::string n = kTakeGrammarName;
    return {takeGrammarHead(),
            "# corte " + n + " 100000 100000 12",
            "# corte " + n + " 200000 200500 ?",
            "# objetivo " + n + " 20000 82.407",
            "# objetivo " + n + " 150000 110.000",
            "# aplauso " + n + " 5000"};
}

std::string takeGrammarJoin(const std::vector<std::string>& lines) {
    std::string s;
    for (const std::string& l : lines) s += l + "\n";
    return s;
}

std::string takeGrammarParse(const std::vector<std::string>& lines, TakeManifest* out = nullptr) {
    TakeManifest scratch;
    return parseTakeManifest(takeGrammarJoin(lines), out != nullptr ? out : &scratch);
}

// D35. atrapa: un parser que no lee algo de lo que declara (descripcion, familia, un dato) o que lo atribuye mal.
TEST(TakeManifest, ASoundManifestParsesToEverythingItDeclares) {
    TakeManifest m;
    ASSERT_EQ("", takeGrammarParse(takeGrammarLines(), &m));
    ASSERT_EQ(1u, m.takes.size());
    const TakeEntry& e = m.takes[0];
    EXPECT_EQ(kTakeGrammarName, e.name);
    EXPECT_EQ(takeGrammarSha(), e.sha256);
    EXPECT_EQ("corpus-g42-v1", e.release);
    EXPECT_EQ(48000, e.rate);
    EXPECT_EQ(480000, e.frames);
    EXPECT_EQ("guitarra", e.family);
    EXPECT_EQ("toma de prueba con espacios", e.description);
    ASSERT_EQ(2u, e.cuts.size());
    EXPECT_TRUE((e.cuts[0] == TakeCutLine{100000, 100000, 12}));
    EXPECT_TRUE((e.cuts[1] == TakeCutLine{200000, 200500, -1}));
    ASSERT_EQ(2u, e.targets.size());
    EXPECT_EQ(20000, e.targets[0].frame);
    EXPECT_DOUBLE_EQ(82.407, e.targets[0].hz);
    EXPECT_EQ(150000, e.targets[1].frame);
    EXPECT_DOUBLE_EQ(110.0, e.targets[1].hz);
    ASSERT_EQ(1u, e.claps.size());
    EXPECT_EQ(5000, e.claps[0]);
}

// D35. atrapa: comentarios y lineas en blanco que rompen el parser o se leen como datos; CRLF (un manifiesto
// editado en otra maquina); `# nota` con la palabra corte adentro NO es un dato.
TEST(TakeManifest, CommentsBlanksAndCrlfAreHarmless) {
    std::vector<std::string> l = takeGrammarLines();
    std::string text = "# un comentario\n\n   \n";
    for (const std::string& s : l) text += s + "\r\n";
    // Un comentario puede NOMBRAR un corte; no puede EMPEZAR como un dato (eso es un dato mal escrito).
    text += "# nota: este corte no es un dato\n# el corte-no-es-un-dato\n";
    TakeManifest m;
    EXPECT_EQ("", parseTakeManifest(text, &m));
    ASSERT_EQ(1u, m.takes.size());
    EXPECT_EQ(2u, m.takes[0].cuts.size());
}

// D35. atrapa: un archivo vacio o de puros comentarios rechazado por la gramatica (la decision de «ausente» es
// de `takeStateOf`, no del parser).
TEST(TakeManifest, AnEmptyManifestParsesToNoTakes) {
    TakeManifest m;
    EXPECT_EQ("", parseTakeManifest("", &m));
    EXPECT_TRUE(m.takes.empty());
    EXPECT_EQ("", parseTakeManifest("# solo comentarios\n\n", &m));
    EXPECT_TRUE(m.takes.empty());
}

// D35. atrapa: los datos van a la toma que NOMBRAN, no a la ultima declarada (con dos tomas, un `# corte`
// de la primera escrito despues de la segunda).
TEST(TakeManifest, EachDataLineGoesToTheTakeItNames) {
    const std::string a = "a-toma.wav", b = "b-toma.wav";
    const std::vector<std::string> lines = {
        takeGrammarHead(a), "# objetivo " + a + " 100 82.407", "# aplauso " + a + " 50",
        takeGrammarHead(b, takeGrammarSha('b')), "# objetivo " + b + " 200 110.000", "# aplauso " + b + " 60",
        "# corte " + a + " 300 300 9",   // despues de la linea de B, pero es de A
    };
    TakeManifest m;
    ASSERT_EQ("", takeGrammarParse(lines, &m));
    ASSERT_EQ(2u, m.takes.size());
    ASSERT_EQ(1u, m.takes[0].cuts.size());
    EXPECT_EQ(300, m.takes[0].cuts[0].ini);
    EXPECT_TRUE(m.takes[1].cuts.empty());
    EXPECT_EQ(100, m.takes[0].targets[0].frame);
    EXPECT_EQ(200, m.takes[1].targets[0].frame);
}

/// Una modificacion de la toma sana que la grámatica tiene que rechazar, con el motivo en el nombre.
/// Renombra la toma en TODAS sus lineas (cabecera y datos): asi el unico defecto es el nombre.
void takeRenameAll(std::vector<std::string>* l, const std::string& from, const std::string& to) {
    for (std::string& s : *l) {
        size_t at = 0;
        while ((at = s.find(from, at)) != std::string::npos) {
            s.replace(at, from.size(), to);
            at += to.size();
        }
    }
}

struct TakeGrammarCase {
    std::string what;
    std::function<void(std::vector<std::string>*)> edit;
};

std::vector<TakeGrammarCase> takeGrammarCases() {
    const std::string n = kTakeGrammarName;
    std::vector<TakeGrammarCase> v;
    auto add = [&v](const std::string& what, std::function<void(std::vector<std::string>*)> edit) {
        v.push_back({what, std::move(edit)});
    };
    add("dato antes de la linea de su toma", [](std::vector<std::string>* l) { std::rotate(l->begin(), l->begin() + kTakeLineClap, l->begin() + kTakeLineClap + 1); });
    // Una COPIA de un objetivo antes de la toma: ignorarla en vez de rechazarla dejaria la toma sana (el
    // caso de arriba no lo distingue: sin su aplauso la toma ya falla por otra regla).
    add("copia de un dato antes de la linea de su toma", [](std::vector<std::string>* l) { l->insert(l->begin(), (*l)[kTakeLineTargetA]); });
    add("dato de una toma que no existe", [](std::vector<std::string>* l) { (*l)[kTakeLineClap] = "# aplauso otra.wav 5000"; });
    // Un dato mal escrito no es un comentario: tomarlo como tal pierde un cambio de cuerda sin que nada lo cruce.
    add("'#objetivo' pegado", [](std::vector<std::string>* l) { (*l)[kTakeLineTargetB].replace(0, 2, "#"); });
    add("'# Objetivo' con mayuscula", [](std::vector<std::string>* l) { (*l)[kTakeLineTargetB].replace(2, 1, "O"); });
    add("'# objetivos' en plural", [](std::vector<std::string>* l) { (*l)[kTakeLineTargetB].replace(2, 8, "objetivos"); });
    add("'# Aplauso' con mayuscula", [](std::vector<std::string>* l) { (*l)[kTakeLineClap].replace(2, 1, "A"); });
    // El NOMBRE termina en un path y en el popen de `sha256Of` entre comillas simples (security-auditor).
    add("NOMBRE con comilla simple", [n](std::vector<std::string>* l) { takeRenameAll(l, n, "x'y.wav"); });
    add("NOMBRE con barra", [n](std::vector<std::string>* l) { takeRenameAll(l, n, "a/b.wav"); });
    add("NOMBRE que sube de directorio", [n](std::vector<std::string>* l) { takeRenameAll(l, n, "../x.wav"); });
    add("NOMBRE con punto inicial", [n](std::vector<std::string>* l) { takeRenameAll(l, n, ".x.wav"); });
    add("NOMBRE con $", [n](std::vector<std::string>* l) { takeRenameAll(l, n, "x$y.wav"); });
    add("objetivos desordenados", [](std::vector<std::string>* l) { std::swap((*l)[kTakeLineTargetA], (*l)[kTakeLineTargetB]); });
    add("objetivo repetido (no estrictamente creciente)", [n](std::vector<std::string>* l) { (*l)[kTakeLineTargetB] = "# objetivo " + n + " 20000 110.000"; });
    add("objetivo fuera de la familia (123 Hz no es cuerda)", [n](std::vector<std::string>* l) { (*l)[kTakeLineTargetB] = "# objetivo " + n + " 150000 123.0"; });
    add("objetivo de otra familia (E1 del bajo en una guitarra)", [n](std::vector<std::string>* l) { (*l)[kTakeLineTargetB] = "# objetivo " + n + " 150000 41.203"; });
    add("objetivo con HZ ilegible", [n](std::vector<std::string>* l) { (*l)[kTakeLineTargetB] = "# objetivo " + n + " 150000 110.000x"; });
    add("objetivo con HZ en cero", [n](std::vector<std::string>* l) { (*l)[kTakeLineTargetB] = "# objetivo " + n + " 150000 0"; });
    add("objetivo en FRAMES (fuera de [0, FRAMES))", [n](std::vector<std::string>* l) { (*l)[kTakeLineTargetB] = "# objetivo " + n + " 480000 110.000"; });
    add("objetivo dentro de una costura (INI+1)", [n](std::vector<std::string>* l) { l->insert(l->begin() + kTakeLineClap, "# objetivo " + n + " 200001 82.407"); });
    add("objetivo en el FIN de una costura (FIN incluido)", [n](std::vector<std::string>* l) { l->insert(l->begin() + kTakeLineClap, "# objetivo " + n + " 200500 82.407"); });
    // Lo mismo con los objetivos ANTES de los cortes: la regla cruza lineas, no depende del orden del archivo.
    add("objetivo dentro de una costura, escrito antes del corte", [n](std::vector<std::string>* l) {
        const std::string clap = (*l)[kTakeLineClap];
        *l = {(*l)[kTakeLineHead], "# objetivo " + n + " 20000 82.407", "# objetivo " + n + " 200250 110.000", clap,
              "# corte " + n + " 100000 100000 12", "# corte " + n + " 200000 200500 ?"};
    });
    add("dos aplausos", [n](std::vector<std::string>* l) { l->push_back("# aplauso " + n + " 6000"); });
    add("cero aplausos", [](std::vector<std::string>* l) { l->erase(l->begin() + kTakeLineClap); });
    add("aplauso fuera de [0, FRAMES)", [n](std::vector<std::string>* l) { (*l)[kTakeLineClap] = "# aplauso " + n + " 480000"; });
    add("sin ningun objetivo", [](std::vector<std::string>* l) { l->erase(l->begin() + kTakeLineTargetA, l->begin() + kTakeLineTargetB + 1); });
    add("sha con un caracter no hex", [](std::vector<std::string>* l) { (*l)[kTakeLineHead] = takeGrammarHead(kTakeGrammarName, std::string(63, 'a') + "g"); });
    add("sha de 63 caracteres", [](std::vector<std::string>* l) { (*l)[kTakeLineHead] = takeGrammarHead(kTakeGrammarName, std::string(63, 'a')); });
    add("sha de 65 caracteres", [](std::vector<std::string>* l) { (*l)[kTakeLineHead] = takeGrammarHead(kTakeGrammarName, std::string(65, 'a')); });
    add("RATE en cero", [](std::vector<std::string>* l) { (*l)[kTakeLineHead] = takeGrammarHead(kTakeGrammarName, takeGrammarSha(), "0"); });
    add("FRAMES en cero", [](std::vector<std::string>* l) { (*l)[kTakeLineHead] = takeGrammarHead(kTakeGrammarName, takeGrammarSha(), "48000", "0"); });
    add("familia desconocida", [](std::vector<std::string>* l) { (*l)[kTakeLineHead] = takeGrammarHead(kTakeGrammarName, takeGrammarSha(), "48000", "480000", "arpa"); });
    add("toma con menos de seis campos", [](std::vector<std::string>* l) { (*l)[kTakeLineHead] = std::string(kTakeGrammarName) + " " + takeGrammarSha() + " corpus-g42-v1 48000 480000"; });
    add("toma repetida", [](std::vector<std::string>* l) { l->insert(l->begin() + 1, takeGrammarHead()); });
    add("corte con INI repetido (no estrictamente creciente)", [n](std::vector<std::string>* l) { (*l)[kTakeLineCutB] = "# corte " + n + " 100000 100500 ?"; });
    add("corte con INI menor que el anterior", [n](std::vector<std::string>* l) { (*l)[kTakeLineCutB] = "# corte " + n + " 50000 50000 3"; });
    add("corte con FIN < INI", [n](std::vector<std::string>* l) { (*l)[kTakeLineCutB] = "# corte " + n + " 200500 200000 ?"; });
    add("corte con FIN > FRAMES", [n](std::vector<std::string>* l) { (*l)[kTakeLineCutB] = "# corte " + n + " 200000 480001 ?"; });
    add("corte sin el campo PERDIDOS", [n](std::vector<std::string>* l) { (*l)[kTakeLineCutB] = "# corte " + n + " 200000 200500"; });
    add("corte con PERDIDOS ilegible", [n](std::vector<std::string>* l) { (*l)[kTakeLineCutB] = "# corte " + n + " 200000 200500 x"; });
    add("corte con INI negativo", [n](std::vector<std::string>* l) { (*l)[kTakeLineCutB] = "# corte " + n + " -5 200500 ?"; });
    return v;
}

// D35 / D36. atrapa: CADA regla de la gramatica quitada (dato antes de su toma, objetivo desordenado o fuera de la
// familia o en (INI, FIN] de una costura, uno y solo un aplauso, al menos un objetivo, sha hex de 64, rate y frames
// positivos, familia conocida, cortes estrictamente crecientes y dentro de [0, FRAMES]). Cada caso es UNA edicion
// de la toma sana, que antes se comprueba que pasa.
// security-auditor. atrapa: un filtro de NOMBRE de mas que rechace una toma legitima ('_', '-', '.' interior,
// mayusculas, digitos) o uno de menos (cada caso de rechazo de nombre esta en `takeGrammarCases`, con la toma
// renombrada en todas sus lineas para que el nombre sea el unico defecto).
TEST(TakeManifest, ALegitimateTakeNameIsAccepted) {
    for (const std::string to : {"G42_criolla-20261028.v1.wav", "a.wav", "_x", "-x.wav", "9"}) {
        SCOPED_TRACE(to);
        std::vector<std::string> l = takeGrammarLines();
        takeRenameAll(&l, kTakeGrammarName, to);
        EXPECT_EQ("", takeGrammarParse(l));
    }
}

TEST(TakeManifest, EachGrammarRuleRejectsItsOwnCase) {
    ASSERT_EQ("", takeGrammarParse(takeGrammarLines())) << "la toma sana de la que parten los casos";
    for (const TakeGrammarCase& c : takeGrammarCases()) {
        SCOPED_TRACE(c.what);
        std::vector<std::string> lines = takeGrammarLines();
        c.edit(&lines);
        EXPECT_NE("", takeGrammarParse(lines));
    }
}

// D35 / D36. atrapa: bordes de la regla «nunca dentro de (INI, FIN] de una costura» corridos un frame, el borde
// FRAMES−1 y FIN == FRAMES del corte: el objetivo EN el INI de la costura vale, en FIN+1 vale, y uno en el frame de
// una PERDIDA (INI == FIN: el intervalo (INI, FIN] esta vacio) tambien.
TEST(TakeManifest, TheBordersOfTheTargetAndCutRulesAreInclusiveWhereTheyShouldBe) {
    const std::string n = kTakeGrammarName;
    auto withTarget = [&n](const std::string& frame) {
        std::vector<std::string> l = takeGrammarLines();
        l.insert(l.begin() + kTakeLineClap, "# objetivo " + n + " " + frame + " 82.407");
        return l;
    };
    EXPECT_EQ("", takeGrammarParse(withTarget("200000"))) << "en el INI de la costura (la costura aun no empezo)";
    EXPECT_EQ("", takeGrammarParse(withTarget("200501"))) << "una despues del FIN";
    EXPECT_EQ("", takeGrammarParse(withTarget("479999"))) << "FRAMES - 1";

    // Perdida en 150000 y un objetivo en 150000: el hueco esta ANTES del frame, no adentro de un intervalo.
    std::vector<std::string> loss = takeGrammarLines();
    loss[kTakeLineCutA] = "# corte " + n + " 150000 150000 12";
    EXPECT_EQ("", takeGrammarParse(loss));

    // Costura que termina en FRAMES.
    std::vector<std::string> tail = takeGrammarLines();
    tail[kTakeLineCutB] = "# corte " + n + " 479000 480000 ?";
    EXPECT_EQ("", takeGrammarParse(tail));
}

// ============================ takeStateOf ============================

/// Escribe `text` en `path`.
bool takeWriteText(const std::string& path, const std::string& text) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

// D35 / D41. atrapa: «sin tomas = verificado» (no haber verificado nada no es haber verificado), el manifiesto
// que no existe contado como verificado, la toma declarada que falta contada como corrupta o verificada, un sha
// distinto contado como verificado, y un manifiesto con la gramatica rota que habilita medir.
TEST(TakeManifest, TheStateIsAbsentCorruptOrVerifiedAndNeverVerifiedByDefault) {
    TakeFixture fx;
    ASSERT_FALSE(fx.dir.empty());
    const std::string manifest = fx.dir + "/manifest.txt";

    EXPECT_EQ(corpus_ns::State::kAbsent, takeStateOf(fx.dir, manifest)) << "sin manifiesto";

    ASSERT_TRUE(takeWriteText(manifest, "# sin tomas\n\n"));
    EXPECT_EQ(corpus_ns::State::kAbsent, takeStateOf(fx.dir, manifest)) << "manifiesto sin tomas = ausente, no verificado";

    // Una toma declarada, el archivo todavia no esta.
    const std::string name = kTakeGrammarName;
    fx.name = name;
    fx.write(takeBytesOf(takeBaseCuts()));
    const std::string sha = corpus_ns::sha256Of(fx.path());
    ASSERT_EQ(64u, sha.size());
    auto manifestFor = [&name](const std::string& hash, const std::string& declaredName) {
        std::vector<std::string> l = takeGrammarLines();
        for (std::string& s : l) {
            const size_t at = s.find(name);
            if (at != std::string::npos) s.replace(at, name.size(), declaredName);
        }
        l[kTakeLineHead] = takeGrammarHead(declaredName, hash);
        return takeGrammarJoin(l);
    };
    ASSERT_TRUE(takeWriteText(manifest, manifestFor(sha, "falta.wav")));
    EXPECT_EQ(corpus_ns::State::kAbsent, takeStateOf(fx.dir, manifest)) << "la toma declarada no esta";

    ASSERT_TRUE(takeWriteText(manifest, manifestFor(sha, name)));
    EXPECT_EQ(corpus_ns::State::kVerified, takeStateOf(fx.dir, manifest)) << "esta y su sha coincide";

    ASSERT_TRUE(takeWriteText(manifest, manifestFor(std::string(64, 'c'), name)));
    EXPECT_EQ(corpus_ns::State::kCorrupt, takeStateOf(fx.dir, manifest)) << "esta y su sha NO coincide";

    // El mismo manifiesto verificado, con la gramatica rota (sin aplauso): ya no habilita medir.
    std::string broken = manifestFor(sha, name);
    broken = broken.substr(0, broken.find("# aplauso"));
    ASSERT_TRUE(takeWriteText(manifest, broken));
    EXPECT_EQ(corpus_ns::State::kCorrupt, takeStateOf(fx.dir, manifest)) << "gramatica rota";
    // Y dice POR QUE (quien saltee la medicion tiene que poder decirlo): atrapa el motivo perdido.
    std::string why;
    EXPECT_EQ(corpus_ns::State::kCorrupt, takeStateOf(fx.dir, manifest, &why));
    EXPECT_NE(std::string::npos, why.find("aplauso")) << why;
}

// D35. atrapa: con dos tomas, una buena y una con el sha distinto, el conjunto queda verificado (la corrupta
// se pierde); y una buena con otra que falta queda ausente, no verificada.
TEST(TakeManifest, OneBadTakeOfTwoSpoilsTheWholeState) {
    TakeFixture fx;
    ASSERT_FALSE(fx.dir.empty());
    const std::string a = "a-toma.wav", b = "b-toma.wav";
    fx.name = a;
    fx.write(takeBytesOf(takeBaseCuts()));
    const std::string shaA = corpus_ns::sha256Of(fx.path());
    fx.name = b;
    fx.write(takeBytesOf(takeBaseCuts(), kTakeTestFrames + 10));
    const std::string shaB = corpus_ns::sha256Of(fx.path());
    ASSERT_NE(shaA, shaB);

    auto manifestOf = [&](const std::string& hashB, const std::string& nameB) {
        return takeGrammarJoin({takeGrammarHead(a, shaA), "# objetivo " + a + " 100 82.407", "# aplauso " + a + " 50",
                                takeGrammarHead(nameB, hashB), "# objetivo " + nameB + " 100 82.407", "# aplauso " + nameB + " 50"});
    };
    const std::string manifest = fx.dir + "/manifest.txt";
    ASSERT_TRUE(takeWriteText(manifest, manifestOf(shaB, b)));
    EXPECT_EQ(corpus_ns::State::kVerified, takeStateOf(fx.dir, manifest));
    ASSERT_TRUE(takeWriteText(manifest, manifestOf(shaA, b)));
    EXPECT_EQ(corpus_ns::State::kCorrupt, takeStateOf(fx.dir, manifest)) << "B no es lo declarado";
    ASSERT_TRUE(takeWriteText(manifest, manifestOf(shaB, "c-toma.wav")));
    EXPECT_EQ(corpus_ns::State::kAbsent, takeStateOf(fx.dir, manifest)) << "C no esta";
}

}  // namespace
}  // namespace wma_test::take
