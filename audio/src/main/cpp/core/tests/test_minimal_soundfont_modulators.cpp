/**
 * test_minimal_soundfont_modulators.cpp — REQ-039 S2, tarea 2.1.
 *
 * Verifica el MATERIAL, no el motor: que `makeMinimalSoundFont` sepa emitir
 * moduladores en **los cuatro ámbitos** de SF2 §9.5 y que el archivo resultante
 * siga siendo cargable.
 *
 * ## Por qué esto es un test y no un detalle de andamiaje
 *
 * 🔴 **Un fixture que no puede producir el caso que el test dice cubrir deja el
 * test verde por vacío.** Toda la etapa S2 se apoya en este generador: si emitiera
 * los moduladores en el ámbito equivocado —o no los emitiera— los tests de
 * AC-039.3/.4/.7 pasarían sin ejercitar nada, y el defecto que REQ-039 persigue
 * seguiría ahí con la suite en verde.
 *
 * Y el ámbito no es un tecnicismo: medido en S1 (tarea 1.6), sobre el
 * `SoundFont-Spec-Test` **21 de 25** moduladores viven en la zona global de su
 * instrumento, y **18 de los 19** de velocity. Un fixture que sólo supiera emitir
 * moduladores de zona probaría el caso raro.
 *
 * ## El oráculo es independiente del motor
 *
 * Los `expect` salen de **releer el .sf2 generado** con un parser propio de este
 * archivo, no de preguntarle a `tsf`. Un valor esperado que sale del sistema bajo
 * prueba no es un oráculo — y acá el sistema bajo prueba es justamente el
 * generador. El parser mira el `pdta` crudo, que es lo que el spec define.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "support/MinimalSoundFont.h"

namespace {

using wma_test::makeMinimalSoundFont;
using wma_test::sf2::kCurveConcave;
using wma_test::sf2::kCurveConvex;
using wma_test::sf2::kCurveLinear;
using wma_test::sf2::kGenInitialAttenuation;
using wma_test::sf2::kGenInitialFilterFc;
using wma_test::sf2::kGenPan;
using wma_test::sf2::kSrcKeyNumber;
using wma_test::sf2::kSrcNone;
using wma_test::sf2::kSrcVelocity;
using wma_test::sf2::Modulator;
using wma_test::sf2::ModulatorPlacement;
using wma_test::sf2::srcOper;

// ---------------------------------------------------------------------------
// Un lector de `pdta` mínimo, independiente del motor. Sólo lo que hace falta
// para contestar "¿dónde quedó cada modulador?".
// ---------------------------------------------------------------------------

struct Chunk {
    size_t begin = 0;
    size_t end = 0;
    bool found = false;
};

uint16_t rd16(const std::vector<uint8_t>& d, size_t at) {
    return static_cast<uint16_t>(d[at] | (d[at + 1] << 8));
}
uint32_t rd32(const std::vector<uint8_t>& d, size_t at) {
    return static_cast<uint32_t>(d[at]) | (static_cast<uint32_t>(d[at + 1]) << 8) |
           (static_cast<uint32_t>(d[at + 2]) << 16) | (static_cast<uint32_t>(d[at + 3]) << 24);
}
bool fourCC(const std::vector<uint8_t>& d, size_t at, const char* cc) {
    return std::memcmp(d.data() + at, cc, 4) == 0;
}

/** Devuelve el rango [begin, end) del cuerpo del chunk `id` dentro del `LIST pdta`. */
Chunk findInPdta(const std::vector<uint8_t>& d, const char* id) {
    Chunk none;
    if (d.size() < 12 || !fourCC(d, 0, "RIFF") || !fourCC(d, 8, "sfbk")) return none;
    const size_t riffEnd = 8 + rd32(d, 4);
    size_t at = 12;
    while (at + 8 <= riffEnd && at + 8 <= d.size()) {
        const uint32_t size = rd32(d, at + 4);
        const size_t body = at + 8;
        if (fourCC(d, at, "LIST") && body + 4 <= d.size() && fourCC(d, body, "pdta")) {
            size_t inner = body + 4;
            const size_t innerEnd = body + size;
            while (inner + 8 <= innerEnd && inner + 8 <= d.size()) {
                const uint32_t isize = rd32(d, inner + 4);
                if (fourCC(d, inner, id)) {
                    Chunk c;
                    c.begin = inner + 8;
                    c.end = inner + 8 + isize;
                    c.found = true;
                    return c;
                }
                inner += 8 + isize + (isize & 1);
            }
        }
        at = body + size + (size & 1);
    }
    return none;
}

/** Cuántos registros de `recordSize` bytes tiene ese chunk. */
size_t recordCount(const std::vector<uint8_t>& d, const char* id, size_t recordSize) {
    const Chunk c = findInPdta(d, id);
    if (!c.found) return 0;
    return (c.end - c.begin) / recordSize;
}

/** El reparto de moduladores por ámbito, leído del archivo. */
struct Reparto {
    int instrumentGlobal = 0;
    int instrumentZone = 0;
    int presetGlobal = 0;
    int presetZone = 0;
    int total() const { return instrumentGlobal + instrumentZone + presetGlobal + presetZone; }
};

Reparto leerReparto(const std::vector<uint8_t>& d) {
    Reparto r;
    constexpr uint16_t kGenInstrument = 41;
    constexpr uint16_t kGenSampleId = 53;

    const Chunk pbag = findInPdta(d, "pbag");
    const Chunk pgen = findInPdta(d, "pgen");
    const Chunk ibag = findInPdta(d, "ibag");
    const Chunk igen = findInPdta(d, "igen");
    if (!pbag.found || !pgen.found || !ibag.found || !igen.found) return r;

    const size_t nPbag = (pbag.end - pbag.begin) / 4;
    for (size_t b = 0; b + 1 < nPbag; ++b) {
        const uint16_t genLo = rd16(d, pbag.begin + b * 4);
        const uint16_t genHi = rd16(d, pbag.begin + (b + 1) * 4);
        const uint16_t modLo = rd16(d, pbag.begin + b * 4 + 2);
        const uint16_t modHi = rd16(d, pbag.begin + (b + 1) * 4 + 2);
        bool hasInstrument = false;
        for (uint16_t g = genLo; g < genHi; ++g) {
            if (rd16(d, pgen.begin + g * 4) == kGenInstrument) hasInstrument = true;
        }
        const int n = static_cast<int>(modHi - modLo);
        // La zona global es la PRIMERA bag sin GenInstrument — así la reconoce tsf.
        if (b == 0 && !hasInstrument) {
            r.presetGlobal += n;
        } else {
            r.presetZone += n;
        }
    }

    const size_t nIbag = (ibag.end - ibag.begin) / 4;
    for (size_t b = 0; b + 1 < nIbag; ++b) {
        const uint16_t genLo = rd16(d, ibag.begin + b * 4);
        const uint16_t genHi = rd16(d, ibag.begin + (b + 1) * 4);
        const uint16_t modLo = rd16(d, ibag.begin + b * 4 + 2);
        const uint16_t modHi = rd16(d, ibag.begin + (b + 1) * 4 + 2);
        bool hasSample = false;
        for (uint16_t g = genLo; g < genHi; ++g) {
            if (rd16(d, igen.begin + g * 4) == kGenSampleId) hasSample = true;
        }
        const int n = static_cast<int>(modHi - modLo);
        if (b == 0 && !hasSample) {
            r.instrumentGlobal += n;
        } else {
            r.instrumentZone += n;
        }
    }
    return r;
}

/** Los seis moduladores de referencia: 2 + 1 + 2 + 1, uno por ámbito y de sobra. */
ModulatorPlacement placementDeReferencia() {
    ModulatorPlacement mods;
    mods.instrumentGlobal.push_back(
        {srcOper(kSrcVelocity, false, true, false, kCurveConcave), kGenInitialAttenuation, 960,
         kSrcNone, 0});
    mods.instrumentGlobal.push_back(
        {srcOper(kSrcVelocity, false, true, false, kCurveConcave), kGenInitialFilterFc, -2400,
         kSrcNone, 0});
    mods.instrumentZone.push_back(
        {srcOper(kSrcKeyNumber, false, false, false, kCurveLinear), kGenInitialFilterFc, 100,
         kSrcNone, 0});
    mods.presetGlobal.push_back(
        {srcOper(kSrcVelocity, false, true, false, kCurveLinear), kGenInitialAttenuation, 480,
         kSrcNone, 0});
    mods.presetGlobal.push_back(
        {srcOper(kSrcNone, false, false, false, kCurveLinear), kGenPan, 250, kSrcNone, 0});
    mods.presetZone.push_back(
        {srcOper(kSrcVelocity, false, false, false, kCurveConvex), kGenInitialAttenuation, 120,
         kSrcNone, 0});
    return mods;
}

// ---------------------------------------------------------------------------

/**
 * Sin moduladores, el archivo tiene que salir **exactamente** como antes de
 * REQ-039: hay cinco sitios de llamada que dependen de esa forma, y ninguno
 * pidió moduladores.
 */
TEST(MinimalSoundFontModulators, SinModuladoresElArchivoEsElDeSiempre) {
    const auto sf = makeMinimalSoundFont(22050, /*looping=*/true);

    // `pmod` e `imod` con SÓLO su registro terminal: 10 bytes cada uno.
    EXPECT_EQ(recordCount(sf, "pmod", 10), 1u) << "pmod deberia tener solo el terminal";
    EXPECT_EQ(recordCount(sf, "imod", 10), 1u) << "imod deberia tener solo el terminal";

    // Una zona real + terminal en cada nivel: sin globales no se agregan bags.
    EXPECT_EQ(recordCount(sf, "pbag", 4), 2u) << "sin moduladores globales no hay bag de mas";
    EXPECT_EQ(recordCount(sf, "ibag", 4), 2u) << "sin moduladores globales no hay bag de mas";

    const Reparto r = leerReparto(sf);
    EXPECT_EQ(r.total(), 0) << "un font sin moduladores no puede declarar ninguno";
}

/**
 * 🔑 El test que sostiene toda la etapa: cada modulador queda en **el ámbito que
 * se le pidió**. Si esto falla, cualquier verde de S2 es por vacío.
 */
TEST(MinimalSoundFontModulators, LosCuatroAmbitosSalenDondeSeDeclararon) {
    const auto sf = makeMinimalSoundFont(22050, /*looping=*/true, -1, -1, placementDeReferencia());

    const Reparto r = leerReparto(sf);
    EXPECT_EQ(r.instrumentGlobal, 2) << "los dos de la zona GLOBAL del instrumento";
    EXPECT_EQ(r.instrumentZone, 1) << "el de la zona del instrumento (la que trae el sample)";
    EXPECT_EQ(r.presetGlobal, 2) << "los dos de la zona GLOBAL del preset";
    EXPECT_EQ(r.presetZone, 1) << "el de la zona del preset (la que trae el instrumento)";
    EXPECT_EQ(r.total(), 6);

    // Y los chunks los contienen de verdad: 2 globales + 1 de zona + el TERMINAL = 4.
    EXPECT_EQ(recordCount(sf, "pmod", 10), 4u) << "2 globales + 1 de zona + terminal";
    EXPECT_EQ(recordCount(sf, "imod", 10), 4u) << "2 globales + 1 de zona + terminal";
}

/**
 * 🔴 **Los globales NO son alcanzables por región, y por eso el fixture importa.**
 *
 * Este test fija en miniatura el hallazgo de S1 1.6: cuatro de los seis
 * moduladores viven en zonas globales, así que un modelo de "modulador → región"
 * los perdería. Es el mismo fenómeno que en los fonts reales —39,8 % en
 * `GeneralUser_GS.sf3`, 84,0 % en el `SoundFont-Spec-Test`— reproducido en un
 * archivo de 736 bytes que el test controla entero.
 */
TEST(MinimalSoundFontModulators, LaMayoriaDeLosModuladoresNoPerteneceANingunaRegion) {
    const auto sf = makeMinimalSoundFont(22050, /*looping=*/true, -1, -1, placementDeReferencia());

    const Reparto r = leerReparto(sf);
    const int globales = r.instrumentGlobal + r.presetGlobal;
    EXPECT_EQ(globales, 4);
    EXPECT_GT(globales, r.instrumentZone + r.presetZone)
        << "el fixture tiene que reproducir el reparto de un font real, donde MANDAN los globales";
}

/**
 * Agregar bags no puede romper el archivo: si `tsf` dejara de encontrar la región,
 * los tests de S2 medirían silencio y lo leerían como "el modulador no se aplicó".
 * Ese modo de falla ya se pagó en este fixture — ver el comentario del terminal de
 * `ibag`, donde un índice corrido dejaba al instrumento sin sample.
 */
TEST(MinimalSoundFontModulators, ConModuladoresElArchivoSigueSiendoCargable) {
    const auto sf = makeMinimalSoundFont(22050, /*looping=*/true, -1, -1, placementDeReferencia());

    // La estructura que `tsf_load_presets` recorre tiene que seguir siendo coherente:
    // una bag global + una zona + el terminal en cada nivel.
    EXPECT_EQ(recordCount(sf, "pbag", 4), 3u);
    EXPECT_EQ(recordCount(sf, "ibag", 4), 3u);

    // Y el `phdr`/`inst` terminal tiene que contar las DOS bags, o la zona real
    // queda escondida detrás del centinela.
    const Chunk phdr = findInPdta(sf, "phdr");
    ASSERT_TRUE(phdr.found);
    EXPECT_EQ(rd16(sf, phdr.begin + 38 + 24), 2u)
        << "el presetBagNdx del terminal tiene que contar la global y la zona";

    const Chunk inst = findInPdta(sf, "inst");
    ASSERT_TRUE(inst.found);
    EXPECT_EQ(rd16(sf, inst.begin + 22 + 20), 2u)
        << "el instBagNdx del terminal tiene que contar la global y la zona";
}

}  // namespace
