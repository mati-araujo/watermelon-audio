/**
 * test_soundfont_fine_tune_keytrack.cpp — MINI-025: la afinacion fina, el coarseTune y el
 * pitchCorrection del sample son OFFSETS ABSOLUTOS de pitch, fuera del keytrack.
 *
 * tsf (`tsf_voice_calcpitchratio`) metia los tres adentro del producto por `scaleTuning`
 * (gen 56): con scaleTuning 0 la afinacion fina desaparecia y con 50 entraba a la mitad. SF2
 * §8.1.2 los define como offsets del pitch de la nota, y FluidSynth 2.6.0 los aplica afuera:
 *
 *     pitch = scaletune * (key - root) + root + 100 * coarse + fine      (+ pitchadj en la raiz)
 *
 * Lo midio el spec-test (REQ-039 S3): el sample `Sine-375Hz` (raiz 66, correccion -23 c)
 * sonaba a 375,00 Hz donde va 370,05 en #3/#4/#8, las tres con scaleTuning 0.
 *
 * EL ORACULO ES LA FORMULA, no el motor: con una senoide de periodo P en un font generado, a
 * la tecla raiz y con el header a la tasa de render, la nota suena a `rate / P` Hz, y cada
 * offset la mueve `2^(cents/1200)`. Los diez estimulos de abajo se le preguntaron ANTES a
 * FluidSynth 2.6.0 sobre fonts minimos generados aparte (2026-09-14): dio los mismos numeros
 * con un sesgo propio de +0,5 c constante (441,135 Hz donde la formula da 441,000).
 *
 * El observable son cruces por cero interpolados (`wma_test::specmidi::pitchHz`), que sobre
 * una senoide limpia resuelven al centesimo de cent.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../../engines/SoundFontModulatorTable.h"
#include "../../engines/SoundFontNoteOn.h"
#include "support/MidiSpecHarness.h"
#include "support/MinimalSoundFont.h"
#include "tsf.h"
#include "tsf_ext.h"

using wma::sfmod::channelNoteOnWithModulators;
using wma::sfmod::ModulatorTable;
using wma_test::makeMinimalSoundFont;
using wma_test::sf2::PitchGenerators;

namespace {

constexpr int kRate = 44100;
constexpr int kPeriod = 100;   // 441 Hz en la raiz: un numero que se lee
constexpr int kRoot = 60;      // el originalPitch que escribe el fixture
constexpr int kFrames = kRate / 2;
constexpr double kRootHz = static_cast<double>(kRate) / kPeriod;

/// Lo que la formula del spec dice para una tecla y sus generadores, en Hz.
double expectedHz(int key, const PitchGenerators& g) {
    const double scale = g.scaleTuning >= 0 ? g.scaleTuning : 100.0;
    const double cents = (key - kRoot) * scale + 100.0 * g.coarseTune + g.fineTune + g.pitchCorrection;
    return kRootHz * std::pow(2.0, cents / 1200.0);
}

/// La nota renderizada por el camino de produccion (`channelNoteOnWithModulators`), medida.
double renderedHz(int key, const PitchGenerators& g) {
    PitchGenerators withSine = g;
    withSine.sinePeriod = kPeriod;
    const auto bytes = makeMinimalSoundFont(kRate, true, -1, -1, {}, 0, withSine);
    tsf* sf = tsf_load_memory(bytes.data(), static_cast<int>(bytes.size()));
    if (!sf) return 0.0;
    ModulatorTable table;
    table.buildFromFontBytes(bytes.data(), bytes.size());
    tsf_set_output(sf, TSF_STEREO_INTERLEAVED, kRate, 0.0f);
    tsf_set_max_voices(sf, 8);
    tsf_channel_set_presetindex(sf, 0, 0);
    channelNoteOnWithModulators(sf, &table, 0, key, 1.0f);
    std::vector<float> out(static_cast<size_t>(kFrames) * 2, 0.0f);
    tsf_render_float(sf, out.data(), kFrames, 0);
    tsf_close(sf);
    const wma_test::specmidi::Signal sig{out.data(), kFrames, kRate};
    // Desde 0,10 s: el ataque del default de tsf ya paso y quedan 0,30 s de senoide estable.
    return wma_test::specmidi::pitchHz(sig, 0.10, 0.30);
}

double centsOff(double hz, double refHz) { return wma_test::specmidi::centsBetween(hz, refHz); }

/// Un cent de tolerancia: 50 veces lo que el estimador resuelve y 23 veces menos que el defecto.
constexpr double kTolCents = 1.0;

}  // namespace

/**
 * El instrumento: sin generadores la raiz suena a rate/P y la tecla de arriba una octava mas
 * (keytrack default 100). Si esto no da, nada de lo de abajo mide.
 */
TEST(SoundFontFineTuneKeytrack, TheSineFixtureSoundsAtItsRootFrequency) {
    const double raiz = renderedHz(kRoot, {});
    ASSERT_GT(raiz, 0.0) << "el fixture con senoide no sono";
    EXPECT_NEAR(centsOff(raiz, kRootHz), 0.0, kTolCents) << raiz << " Hz donde va " << kRootHz;
    const double octava = renderedHz(kRoot + 12, {});
    EXPECT_NEAR(centsOff(octava, 2.0 * kRootHz), 0.0, kTolCents)
        << "una octava arriba con keytrack 100: " << octava << " Hz";
}

/**
 * AC-M025.2: fineTune -23 mide raiz * 2^(-23/1200) con scaleTuning 0, 50 y 100 — la afinacion
 * fina NO depende del keytrack. Antes del arreglo: 0,00 c con scale 0 y -11,5 con 50.
 */
TEST(SoundFontFineTuneKeytrack, FineTuneIsAnAbsoluteOffsetWhateverTheKeytrack) {
    for (int scale : {0, 50, 100}) {
        PitchGenerators g;
        g.scaleTuning = scale;
        g.fineTune = -23;
        const double hz = renderedHz(kRoot, g);
        EXPECT_NEAR(centsOff(hz, expectedHz(kRoot, g)), 0.0, kTolCents)
            << "scaleTuning " << scale << ": fineTune -23 dio " << centsOff(hz, kRootHz)
            << " c donde van -23,00 (tsf lo escalaba por scaleTuning/100)";
    }
}

/**
 * Lo que #3/#4/#8 del spec-test ejercitan de verdad: NO es el generador fineTune sino el
 * `pitchCorrection` del `shdr` (ninguna zona de ese font declara el gen 52). Mismo contrato.
 */
TEST(SoundFontFineTuneKeytrack, ThePitchCorrectionOfTheSampleIsAbsoluteToo) {
    for (int scale : {0, 100}) {
        PitchGenerators g;
        g.scaleTuning = scale;
        g.pitchCorrection = -23;
        const double hz = renderedHz(kRoot, g);
        EXPECT_NEAR(centsOff(hz, expectedHz(kRoot, g)), 0.0, kTolCents)
            << "scaleTuning " << scale << ": pitchCorrection -23 dio " << centsOff(hz, kRootHz) << " c";
    }
}

/**
 * coarseTune tambien: +2 semitonos son +200 c con scaleTuning 0 y con 50. El mutante b de
 * REQ-039 S3 sacaba SOLO `tune` del producto y dejaba `transpose` adentro; FluidSynth saca los
 * dos, y GeneralUser lo usa asi en 570 zonas de sus baterias y efectos (medido el 2026-09-14
 * con `read-sf2-modulators.py --pitch`: |delta| hasta 2450 c).
 */
TEST(SoundFontFineTuneKeytrack, CoarseTuneIsAnAbsoluteOffsetToo) {
    for (int scale : {0, 50}) {
        PitchGenerators g;
        g.scaleTuning = scale;
        g.coarseTune = 2;
        const double hz = renderedHz(kRoot, g);
        EXPECT_NEAR(centsOff(hz, expectedHz(kRoot, g)), 0.0, kTolCents)
            << "scaleTuning " << scale << ": coarseTune +2 dio " << centsOff(hz, kRootHz)
            << " c donde van +200,00";
    }
}

/**
 * Y el keytrack sigue haciendo lo suyo SOBRE LA DISTANCIA A LA RAIZ, no sobre los offsets:
 * scaleTuning 50 a una octava de la raiz son +600 c, y con coarse +2 y fine -23 encima, +777.
 * Un mutante que tirara el keytrack (o que se lo aplicara a los offsets) muere aca.
 */
TEST(SoundFontFineTuneKeytrack, TheKeytrackScalesOnlyTheDistanceToTheRoot) {
    PitchGenerators g;
    g.scaleTuning = 50;
    const double solo = renderedHz(kRoot + 12, g);
    EXPECT_NEAR(centsOff(solo, expectedHz(kRoot + 12, g)), 0.0, kTolCents)
        << "scaleTuning 50 a +12 teclas: " << centsOff(solo, kRootHz) << " c donde van +600";

    g.coarseTune = 2;
    g.fineTune = -23;
    const double conOffsets = renderedHz(kRoot + 12, g);
    EXPECT_NEAR(centsOff(conOffsets, expectedHz(kRoot + 12, g)), 0.0, kTolCents)
        << "scaleTuning 50 a +12 con coarse +2 y fine -23: " << centsOff(conOffsets, kRootHz)
        << " c donde van +777";

    // Y con scaleTuning 0 la tecla no importa: la octava suena igual que la raiz, con su offset.
    PitchGenerators plano;
    plano.scaleTuning = 0;
    plano.fineTune = -23;
    EXPECT_NEAR(centsOff(renderedHz(kRoot + 12, plano), renderedHz(kRoot, plano)), 0.0, kTolCents)
        << "con scaleTuning 0 la tecla movio el pitch";
}

/**
 * La zona exacta de `Concert Bass Drum 2` en GeneralUser (raiz 60, scaleTuning 40, coarse -13)
 * a la tecla 36, sobre el fixture: -2260 c. Es el estimulo que se le pregunto a FluidSynth
 * (119,57 Hz sobre 441) y donde tsf daba -1480 (188 Hz): 780 c de mas en un bombo.
 */
TEST(SoundFontFineTuneKeytrack, TheConcertBassDrumZoneOnTheFixtureLandsWhereFluidSynthDoes) {
    PitchGenerators g;
    g.scaleTuning = 40;
    g.coarseTune = -13;
    const double hz = renderedHz(36, g);
    EXPECT_NEAR(centsOff(hz, expectedHz(36, g)), 0.0, kTolCents)
        << centsOff(hz, kRootHz) << " c donde van -2260 (tsf viejo: -1480)";
}

// ---------------------------------------------------------------------------
// GeneralUser 1.471, el font que se shippea. SKIPPED sin el archivo, nunca passed.
// ---------------------------------------------------------------------------

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
 * `8:116 Concert Bass Drum` -> instrumento `Concert Bass Drum 2`: raiz 60, scaleTuning 40,
 * coarse -13 y fine 0 o -50 segun la capa (leido del archivo con
 * `read-sf2-modulators.py --pitch`). A la tecla 36 el pitch resuelto de cada voz tiene que
 * ser `60 + (36 - 60) * 0,4 - 13 [- 0,5]` semitonos = 3740 / 3690 cents. tsf daba 4520 / 4500:
 * el bombo sonaba 780 c (mas de una quinta) arriba de como lo afino su autor. Se lee el pitch
 * resuelto por `tsf_ext` porque un bombo no tiene pitch medible por cruces; el oraculo es la
 * formula sobre lo que el archivo declara, como en el resto de esta suite.
 */
TEST(SoundFontFineTuneKeytrack, GeneralUserConcertBassDrumResolvesItsCoarseTuneOutsideTheKeytrack) {
    const auto bytes = generalUserBytes();
    if (bytes.empty()) GTEST_SKIP() << "sin GeneralUser_GS.sf3 — WMA_GENERALUSER_SF3 o el checkout hermano de NoisyPad";

    tsf* f = tsf_load_memory(bytes.data(), static_cast<int>(bytes.size()));
    ASSERT_NE(f, nullptr);
    ModulatorTable table;
    table.buildFromFontBytes(bytes.data(), bytes.size());
    tsf_set_output(f, TSF_MONO, 48000, 0.0f);
    tsf_set_max_voices(f, 64);
    tsf_channel_set_bank_preset(f, 0, 8, 116);
    constexpr int kKey = 36;
    channelNoteOnWithModulators(f, &table, 0, kKey, 100 / 127.0f);
    tsf_ext_started_voice st[16];
    const int n = tsf_ext_voices_started_by_last_note_on(f, st, 16);
    ASSERT_GT(n, 0) << "8:116 a la tecla 36 no arranco ninguna voz";

    // Los dos valores que el archivo admite para esa tecla: coarse -13 con fine 0 o con -50.
    const double kRootCents = 60 * 100.0, kKeytrack = (kKey - 60) * 40.0, kCoarse = -1300.0;
    const double sinFine = kRootCents + kKeytrack + kCoarse;         // 3740
    const double conFine = sinFine - 50.0;                            // 3690
    const double viejoSinFine = kRootCents + (kKey - 13 - 60) * 40.0;  // 4520: coarse ADENTRO
    for (int i = 0; i < n && i < 16; ++i) {
        const double p = st[i].pitchTimecents;
        std::printf("  [MINI-025] Concert Bass Drum voz %d: pitch resuelto %.1f c (viejo: %.1f)\n",
                    i, p, viejoSinFine);
        const bool ok = std::fabs(p - sinFine) < 0.5 || std::fabs(p - conFine) < 0.5;
        EXPECT_TRUE(ok) << "voz " << i << ": " << p << " c; el archivo declara 3740 o 3690 y tsf daba "
                        << viejoSinFine << " (coarse adentro del keytrack)";
    }
    tsf_close(f);
}
