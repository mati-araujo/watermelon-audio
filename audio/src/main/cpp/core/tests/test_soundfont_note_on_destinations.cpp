/**
 * test_soundfont_note_on_destinations.cpp — MINI-027: los ocho destinos de note-on que
 * REQ-039 S2 dejo fuera, y el hueco que S2 declaro.
 *
 * S2 evaluo los moduladores de fuente de nota (velocity, keynum) hacia DOS destinos:
 * `initialAttenuation` e `initialFilterFc`. S3 conto sobre GeneralUser 30 moduladores hacia
 * otros ocho —las tres duraciones de la envolvente de volumen, el ataque de la de modulacion,
 * cuanta envolvente entra al filtro, el Q, el offset de arranque del sample y el paneo— y los
 * dejo NOMBRADOS en un trinquete. Este archivo los evalua.
 *
 * EL ORACULO ES LA DEFINICION DEL SPEC, no el motor. SF2 §8.4: "the modulator output is ADDED
 * to the generator". O sea que una nota con el modulador a velocity v tiene que sonar
 * EXACTAMENTE como una nota SIN modulador cuyo generador ya lleva sumado `amount * f(v)`.
 * Los dos fonts se generan; el segundo ("horneado") no pasa por ninguna linea de MINI-027 —
 * es tsf leyendo un generador—, asi que la comparacion muestra a muestra separa "el modulador
 * se aplico" de "se aplico a otra escala, otra unidad o en otro orden" sin que el test tenga
 * que conocer la forma del filtro ni la curva del ataque. Cada test lleva ademas el CONTROL
 * POSITIVO (con y sin modulador, a la misma velocity, suenan DISTINTO): sin el, un modulador
 * que no hiciera nada pasaria la igualdad por vacio.
 *
 * Los amounts son multiplos de 127 y la velocity es entera: `amount * (127 - v) / 127` es un
 * ENTERO y el generador horneado lo escribe sin redondeo.
 *
 * Y el hueco de S2 (`tsf_ext.h`, "LIMITE DECLARADO"): en una region con envolvente o LFO al
 * filtro, `tsf_voice_render` recalculaba el corte cada bloque desde `region->initialFilterFc`
 * y pisaba en el primer bloque lo que S2 habia escrito. Medido sobre GeneralUser el 2026-09-15:
 * 166 zonas de instrumento en 24 instrumentos con el velocity -> filtro INERTE. El corte es
 * ahora un campo por voz, y el test de abajo lo afirma con el mismo oraculo.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "../../engines/SoundFontModulatorTable.h"
#include "../../engines/SoundFontNoteOn.h"
#include "support/MidiSpecHarness.h"
#include "support/MinimalSoundFont.h"
#include "tsf.h"

using wma::sfmod::channelNoteOnWithModulators;
using wma::sfmod::ModulatorTable;
using wma_test::makeMinimalSoundFont;
using wma_test::sf2::ExtraGenerator;
using wma_test::sf2::Modulator;
using wma_test::sf2::ModulatorPlacement;
using wma_test::sf2::PitchGenerators;

namespace {

constexpr int kRate = 44100;
constexpr int kPeriod = 100;   // 441 Hz en la raiz
constexpr int kRoot = 60;

// Generadores de §8.1.2 que este archivo usa, por numero.
constexpr uint16_t kGenInitialFilterFc = 8;
constexpr uint16_t kGenModEnvToFilterFc = 11;
constexpr uint16_t kGenAttackModEnv = 26;
constexpr uint16_t kGenSustainModEnv = 29;
constexpr uint16_t kGenReleaseModEnv = 30;
constexpr uint16_t kGenAttackVolEnv = 34;
constexpr uint16_t kGenHoldVolEnv = 35;
constexpr uint16_t kGenSustainVolEnv = 37;
constexpr uint16_t kGenReleaseVolEnv = 38;

constexpr int16_t kInstant = -12000;   // 1 ms
constexpr int16_t kOneSecond = 0;

// Fuentes §8.2: velocity lineal unipolar DECRECIENTE (vale 1 - v/127: "toque suave = mas")
// y keynum lineal unipolar creciente (vale key/127).
constexpr uint16_t kVelocityDecreasing = wma_test::sf2::srcOper(2, false, true, false, 0);

/// Lo que un modulador de velocity decreciente de `amount` aporta a MIDI `v`: entero si
/// `amount` es multiplo de 127.
constexpr int velocityContribution(int amount, int v) { return amount * (127 - v) / 127; }
static_assert(velocityContribution(2540, 32) == 1900, "el amount tiene que ser multiplo de 127");

/// La velocity 0..1 que `tsf_note_on` trunca a EXACTAMENTE `v` (`(short)(vel * 127)`).
float velFor(int v) { return (static_cast<float>(v) + 0.5f) / 127.0f; }

struct Font {
    std::vector<ExtraGenerator> gens;
    ModulatorPlacement mods;
    bool sine = true;   // senoide para nivel/pitch; la cuadrada (armonicos) para el filtro
};

/// Renderiza `seconds` de una nota en `key` con velocity MIDI `v`; note-off a `noteOffSec` (o
/// nunca si <= 0). Estereo entrelazado. Camino de produccion: `channelNoteOnWithModulators`.
std::vector<float> render(const Font& font, double seconds, double noteOffSec, int v,
                          int key = kRoot) {
    PitchGenerators pitch;
    if (font.sine) pitch.sinePeriod = kPeriod;
    const auto bytes = makeMinimalSoundFont(kRate, true, -1, -1, font.mods, 0, pitch, font.gens);
    tsf* sf = tsf_load_memory(bytes.data(), static_cast<int>(bytes.size()));
    if (!sf) return {};
    ModulatorTable table;
    table.buildFromFontBytes(bytes.data(), bytes.size());
    tsf_set_output(sf, TSF_STEREO_INTERLEAVED, kRate, 0.0f);
    tsf_set_max_voices(sf, 8);
    tsf_channel_set_presetindex(sf, 0, 0);
    channelNoteOnWithModulators(sf, &table, 0, key, velFor(v));

    const int total = static_cast<int>(seconds * kRate);
    const int offAt = noteOffSec > 0.0 ? static_cast<int>(noteOffSec * kRate) : total;
    std::vector<float> out(static_cast<size_t>(total) * 2, 0.0f);
    int done = 0;
    while (done < total) {
        const int chunk = std::min(total - done, done < offAt ? offAt - done : 256);
        tsf_render_float(sf, out.data() + static_cast<size_t>(done) * 2, chunk, 0);
        done += chunk;
        if (done == offAt) tsf_channel_note_off(sf, 0, key);
    }
    tsf_close(sf);
    return out;
}

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 1e9;
    double m = 0.0;
    for (size_t i = 0; i < a.size(); ++i) m = std::max(m, std::fabs(double(a[i]) - double(b[i])));
    return m;
}

double peakAbs(const std::vector<float>& a) {
    double m = 0.0;
    for (float x : a) m = std::max(m, std::fabs(double(x)));
    return m;
}

/// Con modulador `m` en la zona global del instrumento.
Font modulated(std::vector<ExtraGenerator> gens, const Modulator& m, bool sine = true) {
    Font f;
    f.gens = std::move(gens);
    f.mods.instrumentGlobal.push_back(m);
    f.sine = sine;
    return f;
}

/// Sin modulador, con `gen` ya sumado ("horneado"): lo que el spec dice que tiene que sonar.
Font baked(std::vector<ExtraGenerator> gens, uint16_t oper, int add, bool sine = true) {
    Font f;
    f.gens = std::move(gens);
    bool found = false;
    for (ExtraGenerator& g : f.gens) {
        if (g.oper == oper) { g.amount = static_cast<int16_t>(g.amount + add); found = true; }
    }
    if (!found) f.gens.push_back({oper, static_cast<int16_t>(add)});
    f.sine = sine;
    return f;
}

/// Sin modulador y sin sumar: el control positivo (tiene que sonar DISTINTO del modulado).
Font plain(std::vector<ExtraGenerator> gens, bool sine = true) {
    Font f;
    f.gens = std::move(gens);
    f.sine = sine;
    return f;
}

// Las tolerancias son RELATIVAS al pico del render: a velocity 32 el default #1 (velocity ->
// atenuacion, 960 cB concava) deja la nota 48 dB abajo, y una tolerancia absoluta que sirve a
// velocity 127 seria vacia ahi. Dos fonts con el mismo generador salen del mismo tsf, asi que lo
// unico que separa a "modulado" de "horneado" es el redondeo de un float (timecents -> segundos
// -> muestras): medido, < 1e-4 del pico. Un desajuste real es 10x el minimo del control o mas.
constexpr double kSameRel = 5e-3;
constexpr double kDifferentRel = 0.05;

void expectModulatorEqualsBakedGenerator(const char* what, const Font& withMod, const Font& bakedFont,
                                          const Font& control, double seconds, double noteOff,
                                          int v, int key = kRoot) {
    const auto a = render(withMod, seconds, noteOff, v, key);
    const auto b = render(bakedFont, seconds, noteOff, v, key);
    const auto c = render(control, seconds, noteOff, v, key);
    ASSERT_FALSE(a.empty() || b.empty() || c.empty()) << what << ": algun font no cargo";
    const double peak = peakAbs(a);
    ASSERT_GT(peak, 1e-4) << what << ": el render modulado es silencio";
    const double same = maxAbsDiff(a, b) / peak, different = maxAbsDiff(a, c) / peak;
    std::printf("  [MINI-027] %-22s v=%3d: pico %.4f; modulado vs horneado %.2e del pico, vs sin "
                "modulador %.3f\n", what, v, peak, same, different);
    EXPECT_LT(same, kSameRel) << what << ": a velocity " << v
                              << " el modulador no suena como el generador ya sumado (max |dif| "
                              << same << " del pico)";
    EXPECT_GT(different, kDifferentRel)
        << what << ": CONTROL — con y sin modulador suenan igual (max |dif| " << different
        << " del pico); la igualdad de arriba seria vacia";
}

// ---- Los fonts de cada destino ----------------------------------------------------------

// Filtro: corte a 6000 c (261 Hz) sobre la CUADRADA, mod env con ataque de 0,5 s y sustain al
// 100 %, envolvente de volumen plana. `modEnvToFilterFc` lo pone cada test.
std::vector<ExtraGenerator> filterFont(int16_t modEnvToFilterFc) {
    return {{kGenInitialFilterFc, 6000}, {kGenModEnvToFilterFc, modEnvToFilterFc},
            {kGenAttackModEnv, -1200}, {kGenSustainModEnv, 0}, {kGenReleaseModEnv, kOneSecond},
            {kGenAttackVolEnv, kInstant}, {kGenHoldVolEnv, kInstant}, {kGenSustainVolEnv, 0},
            {kGenReleaseVolEnv, kOneSecond}};
}

Modulator velocityTo(uint16_t dest, int16_t amount) {
    Modulator m;
    m.srcOper = kVelocityDecreasing;
    m.destOper = dest;
    m.amount = amount;
    return m;
}

/**
 * Sin el default #2 (velocity -> initialFilterFc, -2400 c): un modulador del archivo con la
 * misma identidad y amount 0 lo BORRA (§8.4, MINI-028). Los tests del filtro lo borran en los
 * TRES fonts por dos razones: a velocity 32 el default cierra el corte 1795 c y deja la cuadrada
 * a -67 dB; y el modulador de `initialFilterFc` que prueba el hueco de S2 tiene esa misma
 * identidad, o sea que REEMPLAZA al default en el font modulado — el horneado tiene que partir
 * del mismo conjunto de defaults o la igualdad compara dos cosas distintas.
 */
Font withoutDefaultTwo(Font f) {
    f.mods.instrumentGlobal.push_back(velocityTo(kGenInitialFilterFc, 0));
    return f;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// El hueco de S2: el corte modulado en una region con envolvente al filtro
// ---------------------------------------------------------------------------------------

/**
 * AC-M027.4: en una region CON `modEnvToFilterFc` (gen 11 = 2400: filtro dinamico), un modulador
 * velocity -> initialFilterFc de 2540 c a MIDI 32 abre el corte 1900 c — como un font cuyo
 * `initialFilterFc` ya dice 6000 + 1900. Hasta MINI-027 `tsf_voice_render` recalculaba el corte
 * cada bloque desde `region->initialFilterFc` y este modulador era INERTE: la igualdad daba
 * rojo (el modulado sonaba como el control). Mutante: volver a leer `region->` en el render.
 */
TEST(SoundFontNoteOnDestinations, VelocityToFilterCutoffSurvivesADynamicLowpassRegion) {
    const auto gens = filterFont(2400);
    // El modulador tiene la identidad del default #2 y lo reemplaza; el horneado y el control
    // lo borran para partir del mismo conjunto.
    expectModulatorEqualsBakedGenerator(
        "initialFilterFc+env", modulated(gens, velocityTo(kGenInitialFilterFc, 2540), false),
        withoutDefaultTwo(baked(gens, kGenInitialFilterFc, velocityContribution(2540, 32), false)),
        withoutDefaultTwo(plain(gens, false)), 1.0, -1.0, 32);
}

// ---------------------------------------------------------------------------------------
// GeneralUser 1.471, el font que se shippea: cuanto pesaba el hueco de S2. SKIPPED sin el
// archivo, nunca passed.
// ---------------------------------------------------------------------------------------

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

#include "tsf_ext.h"

namespace {

std::vector<unsigned char> generalUserBytes() {
    const char* env = std::getenv("WMA_GENERALUSER_SF3");
    const std::string path = env ? env : WMA_GENERALUSER_SF3_DEFAULT;
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
}

}  // namespace

/**
 * AC-M027.5: el hueco de S2, MEDIDO sobre el font que se shippea y en la unidad del motor —
 * regiones de tsf (preset x zona de instrumento)—. Una region estaba en el hueco si tiene filtro
 * dinamico (mod env o mod LFO al corte, `dynamicLowpass`) Y resuelve un modulador velocity ->
 * initialFilterFc con amount != 0 (propio, o el default #2 si la region no lo borra): ahi el
 * corte que S2 escribia se pisaba en el primer bloque de render.
 *
 * El numero es un TRINQUETE: se declara lo que se midio el 2026-09-15 y si cambia, cambio el
 * font o el clasificador, y el diff del PR lo tiene que decir. La medicion por ZONAS de
 * instrumento (el lector de Python, `s2hole.py` del journal) dio 166 zonas en 24 instrumentos;
 * aca son regiones, que multiplican cada zona por los presets que la usan.
 */
TEST(SoundFontNoteOnDestinations, GeneralUserRegionsWhereTheVelocityToFilterWasInertAreCounted) {
    const auto bytes = generalUserBytes();
    if (bytes.empty()) GTEST_SKIP() << "sin GeneralUser_GS.sf3 — WMA_GENERALUSER_SF3 o el checkout hermano de NoisyPad";

    tsf* f = tsf_load_memory(bytes.data(), static_cast<int>(bytes.size()));
    ASSERT_NE(f, nullptr);
    ModulatorTable table;
    ASSERT_TRUE(table.buildFromFontBytes(bytes.data(), bytes.size()));

    int regions = 0, dynamic = 0, inert = 0;
    std::set<int> presetsInert;
    const int presets = tsf_get_presetcount(f);
    for (int p = 0; p < presets; ++p) {
        const int n = tsf_ext_preset_region_count(f, p);
        for (int r = 0; r < n; ++r) {
            int fc = 0, env = 0, lfo = 0;
            ASSERT_EQ(tsf_ext_region_filter(f, p, r, &fc, &env, &lfo), 1);
            ++regions;
            if (env == 0 && lfo == 0) continue;
            ++dynamic;
            const wma::sfmod::RegionModulatorList* mods = table.regionModulators(p, r);
            if (!mods) continue;
            bool velocityToFc = false;
            for (const wma::sfmod::NoteOnModulator& m : mods->mods) {
                if (m.destOper == wma::sfmod::kDestInitialFilterFc &&
                    m.primarySource == wma::sfmod::NoteSource::Velocity && m.amount != 0.0f) {
                    velocityToFc = true;
                }
            }
            if (velocityToFc) { ++inert; presetsInert.insert(p); }
        }
    }
    std::printf("  [MINI-027] GeneralUser: %d regiones, %d con filtro dinamico, %d con velocity -> filtro "
                "que S2 dejaba INERTE, en %d presets\n", regions, dynamic, inert,
                static_cast<int>(presetsInert.size()));
    if (std::getenv("WMA_LIST_PRESETS")) {
        for (int p : presetsInert)
            std::printf("    %3d:%-3d %s\n", tsf_get_preset_bank(f, p), tsf_get_preset_number(f, p),
                        tsf_get_presetname(f, p));
    }
    tsf_close(f);
    EXPECT_EQ(regions, 12311) << "las regiones de tsf cambiaron: ¿cambio el font?";
    // Declarado el 2026-09-15 con lo que imprimio esta misma corrida: 7212 regiones con filtro
    // dinamico (58 %: las capas de strings y pads pesan) y 3380 en 111 presets con el velocity ->
    // filtro inerte. Por ZONAS de instrumento eran 166 en 24; por REGIONES, esto.
    EXPECT_EQ(dynamic, 7212) << "las regiones con filtro dinamico cambiaron: ¿cambio el font?";
    EXPECT_EQ(inert, 3380) << "el hueco de S2 medido en regiones cambio";
    EXPECT_EQ(static_cast<int>(presetsInert.size()), 111);
}
