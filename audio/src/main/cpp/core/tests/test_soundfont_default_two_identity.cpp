/**
 * test_soundfont_default_two_identity.cpp — MINI-028: el default #2 tiene DOS
 * identidades, y las dos lo reemplazan.
 *
 * SF 2.01 definia el default #2 (velocity -> initialFilterFc, -2400) con `amtSrc`
 * velocity/switch (0x0D02); SF 2.04, sin `amtSrc`. El motor declara el 2.04. GeneralUser
 * 1.471 —el font que se shippea— lo borra en 1422 zonas SOLO con la identidad 2.01, y
 * con identidad exacta ese borrado no anulaba nada: el -2400 seguia vivo en los 269
 * presets. Lo encontro NoisyPad por el centroide de `Saw Lead` (1123 -> 880 Hz con la
 * velocity), y se midio: tecla 72, 1640 -> 1195 Hz donde el archivo pide 1638 -> 1546.
 *
 * Estos tests son el AC-M028.2 en el RENDER, muestra a muestra, con fonts propios que
 * difieren en un solo modulador. El oraculo es la igualdad entre dos renders que el
 * spec obliga a ser iguales (borrar con 2.01 = borrar con 2.04), y la DESIGUALDAD con
 * el que no borra (el default sigue existiendo para quien no lo borra: #14 A/C del
 * spec-test). El gemelo A2 —un 2.01 con amount propio se evalua EN LUGAR del default,
 * nunca sumado— usa la curva switch como discriminador: a velocity 38 vale 0 (igual al
 * borrado), a velocity 100 vale 1 (igual al mismo amount con identidad 2.04).
 *
 * AC-M028.1, sobre GeneralUser 1.471, esta al final: SKIPPED sin el font, nunca passed.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../../engines/SoundFontModulatorTable.h"
#include "../../engines/SoundFontNoteOn.h"
#include "support/MinimalSoundFont.h"
#include "tsf.h"
#include "tsf_ext.h"

using wma::sfmod::channelNoteOnWithModulators;
using wma::sfmod::ModulatorTable;
using wma_test::makeMinimalSoundFont;
using wma_test::sf2::kCurveLinear;
using wma_test::sf2::kCurveSwitch;
using wma_test::sf2::kGenInitialFilterFc;
using wma_test::sf2::kSrcNone;
using wma_test::sf2::kSrcVelocity;
using wma_test::sf2::Modulator;
using wma_test::sf2::ModulatorPlacement;
using wma_test::sf2::srcOper;

namespace {

constexpr int kRate = 44100;
constexpr int kFrames = 8192;
constexpr int kKey = 60;

/// La identidad 2.04 del default #2: velocity lineal unipolar decreciente, sin amtSrc.
Modulator defaultDos204(int16_t amount) {
    return {srcOper(kSrcVelocity, false, true, false, kCurveLinear), kGenInitialFilterFc, amount,
            kSrcNone, 0};
}

/// La identidad 2.01: la misma con amtSrc velocity, switch, unipolar, DECRECIENTE — 0x0D02,
/// tal como lo escriben GeneralUser (1422 veces) y el spec-test (`veloToFC-deleted2.01`,
/// 7 veces). El bit D en 1 importa: 0x0C02 (creciente) no es esa identidad, y con el el
/// primer borrador de este test fallaba contra un motor correcto.
Modulator defaultDos201(int16_t amount) {
    Modulator m = defaultDos204(amount);
    m.amtSrcOper = srcOper(kSrcVelocity, false, /*decreasing=*/true, false, kCurveSwitch);
    static_assert(srcOper(kSrcVelocity, false, true, false, kCurveSwitch) == 0x0D02,
                  "la identidad 2.01 del default #2 es 0x0D02");
    return m;
}

ModulatorPlacement enElInstrumento(std::initializer_list<Modulator> mods) {
    ModulatorPlacement p;
    for (const Modulator& m : mods) p.instrumentGlobal.push_back(m);
    return p;
}

struct Font {
    std::vector<uint8_t> bytes;
    tsf* sf = nullptr;
    ModulatorTable table;
    explicit Font(const ModulatorPlacement& mods) {
        bytes = makeMinimalSoundFont(22050, true, -1, -1, mods);
        sf = tsf_load_memory(bytes.data(), static_cast<int>(bytes.size()));
        if (sf) {
            tsf_set_output(sf, TSF_STEREO_INTERLEAVED, kRate, 0.0f);
            tsf_set_max_voices(sf, 8);
            tsf_channel_set_presetindex(sf, 0, 0);
            table.buildFromFontBytes(bytes.data(), bytes.size());
        }
    }
    ~Font() { if (sf) tsf_close(sf); }
};

std::vector<float> render(const ModulatorPlacement& mods, int velocity) {
    Font f(mods);
    if (!f.sf) return {};
    channelNoteOnWithModulators(f.sf, &f.table, 0, kKey, velocity / 127.0f);
    std::vector<float> out(static_cast<size_t>(kFrames) * 2, 0.0f);
    tsf_render_float(f.sf, out.data(), kFrames, 0);
    return out;
}

double worstDiff(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 1.0;
    double w = 0.0;
    for (size_t i = 0; i < a.size(); ++i) w = std::max(w, std::fabs(static_cast<double>(a[i]) - b[i]));
    return w;
}

double rms(const std::vector<float>& x) {
    double acc = 0.0;
    for (float v : x) acc += static_cast<double>(v) * v;
    return x.empty() ? 0.0 : std::sqrt(acc / static_cast<double>(x.size()));
}

/** Brillo: energia de la primera diferencia sobre la energia total. Sube con los agudos. */
double brillo(const std::vector<float>& stereo) {
    double num = 0.0, den = 0.0;
    float prev = 0.0f;
    for (size_t i = 0; i + 1 < stereo.size(); i += 2) {
        const float m = 0.5f * (stereo[i] + stereo[i + 1]);
        const float d = m - prev;
        num += static_cast<double>(d) * d;
        den += static_cast<double>(m) * m;
        prev = m;
    }
    return den > 1e-12 ? num / den : 0.0;
}

constexpr double kIdentical = 1e-6;   // un ulp de float sobre señal de amplitud ~0,5
constexpr double kDistinct = 1e-3;    // un filtro que se movio se ve muy por encima de esto

}  // namespace

/** El fixture puede producir la diferencia que los tests afirman: sin esto, "iguales" es vacio. */
TEST(SoundFontDefaultTwoIdentity, TheFixtureCanTellADeletedDefaultFromALiveOne) {
    const auto borrado = render(enElInstrumento({defaultDos204(0)}), 38);
    const auto vivo = render(enElInstrumento({}), 38);
    ASSERT_GT(rms(borrado), 0.01) << "el fixture no suena";
    EXPECT_GT(worstDiff(borrado, vivo), kDistinct)
        << "el default #2 (-2400 a velocity 38) no cambio la onda: el discriminador no existe";
    EXPECT_GT(brillo(borrado), brillo(vivo)) << "con el default vivo la nota tiene que ser MAS oscura";
}

/** AC-M028.2: borrar con la identidad 2.01 es borrar. Muestra a muestra igual que con 2.04. */
TEST(SoundFontDefaultTwoIdentity, DeletingWithThe201IdentityIsTheSameAsDeletingWith204) {
    const auto con201 = render(enElInstrumento({defaultDos201(0)}), 38);
    const auto con204 = render(enElInstrumento({defaultDos204(0)}), 38);
    const auto vivo = render(enElInstrumento({}), 38);

    EXPECT_LT(worstDiff(con201, con204), kIdentical)
        << "GeneralUser 1.471 borra el default #2 asi en 1422 zonas, y con identidad exacta el "
           "-2400 sobrevivia en los 269 presets (peor diferencia=" << worstDiff(con201, con204) << ")";
    // Y el gemelo: el default sigue existiendo para quien NO lo borra (#14 A y #14 C).
    EXPECT_GT(worstDiff(con201, vivo), kDistinct) << "el borrado 2.01 dejo la onda igual que sin borrar";
}

/**
 * A2, no A1: un 2.01 con amount propio se evalua EN LUGAR del default. El switch
 * DECRECIENTE lo discrimina: a velocity 100 (>= 64) vale 0 —igual al borrado—; a velocity
 * 38 vale 1 —igual al mismo amount declarado con la identidad 2.04. El mutante A1 (solo
 * el borrado equivale) deja el default vivo debajo: -2400·(1-v/127) de mas en los dos.
 */
TEST(SoundFontDefaultTwoIdentity, A201ModulatorWithItsOwnAmountReplacesTheDefaultInsteadOfAddingToIt) {
    const auto menos1200switch100 = render(enElInstrumento({defaultDos201(-1200)}), 100);
    const auto borrado100 = render(enElInstrumento({defaultDos204(0)}), 100);
    EXPECT_LT(worstDiff(menos1200switch100, borrado100), kIdentical)
        << "a velocity 100 el switch decreciente vale 0: sin default debajo, la onda es la del "
           "borrado (peor diferencia=" << worstDiff(menos1200switch100, borrado100) << ")";

    const auto menos1200switch38 = render(enElInstrumento({defaultDos201(-1200)}), 38);
    const auto menos1200lineal38 = render(enElInstrumento({defaultDos204(-1200)}), 38);
    EXPECT_LT(worstDiff(menos1200switch38, menos1200lineal38), kIdentical)
        << "a velocity 38 el switch vale 1: -1200 con identidad 2.01 tiene que sonar como -1200 "
           "con identidad 2.04 (peor diferencia=" << worstDiff(menos1200switch38, menos1200lineal38) << ")";
    // Control de que a velocity 38 el -1200 hace algo (si no, la igualdad de arriba es vacia).
    const auto borrado38 = render(enElInstrumento({defaultDos204(0)}), 38);
    EXPECT_GT(worstDiff(menos1200lineal38, borrado38), kDistinct);
}

// ---------------------------------------------------------------------------
// AC-M028.1 — sobre el font real
// ---------------------------------------------------------------------------

namespace {

/// `GeneralUser_GS.sf3` vive en el bundle de NoisyPad (checkout hermano). Sin el archivo
/// el test sale SKIPPED y NUNCA passed (regla de REQ-032). Mismo camino que
/// `test_soundfont_modulator_table.cpp`.
std::vector<unsigned char> generalUserBytes() {
    const char* env = std::getenv("WMA_GENERALUSER_SF3");
    const std::string path = env ? env : WMA_GENERALUSER_SF3_DEFAULT;
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
}

constexpr int kSawLeadBank = 0, kSawLeadProgram = 81, kSawLeadKey = 72;
constexpr int kGuRate = 48000;

/// Un render de `Saw Lead`; si `fileCutoffAmount != 0`, el cutoff de cada voz se pone A
/// MANO en `initialFilterFc + amount·(1 − vel/127)` — lo que el archivo declara, sin
/// ningun default. Es el oraculo (a) de MINI-028: el archivo, no otro sintetizador.
std::vector<float> renderSawLead(const std::vector<unsigned char>& bytes, int velocity,
                                 int fileCutoffAmount) {
    tsf* f = tsf_load_memory(bytes.data(), static_cast<int>(bytes.size()));
    if (!f) return {};
    ModulatorTable table;
    table.buildFromFontBytes(bytes.data(), bytes.size());
    tsf_set_output(f, TSF_MONO, kGuRate, 0.0f);
    tsf_set_max_voices(f, 64);
    tsf_channel_set_bank_preset(f, 0, kSawLeadBank, kSawLeadProgram);
    channelNoteOnWithModulators(f, &table, 0, kSawLeadKey, velocity / 127.0f);
    if (fileCutoffAmount != 0) {
        tsf_ext_started_voice st[16];
        const int n = tsf_ext_voices_started_by_last_note_on(f, st, 16);
        for (int i = 0; i < n; ++i) {
            tsf_ext_voice_set_filter_cutoff(
                f, st[i].voiceIndex,
                static_cast<float>(st[i].initialFilterFc) +
                    static_cast<float>(fileCutoffAmount) * (1.0f - velocity / 127.0f));
        }
    }
    std::vector<float> out(static_cast<size_t>(kGuRate) * 2, 0.0f);
    for (int done = 0; done < static_cast<int>(out.size()); done += 512) {
        tsf_render_float(f, out.data() + done, 512, 0);
    }
    tsf_close(f);
    return out;
}

/// Centroide espectral (Hz) de 16384 muestras desde 0,5 s, con Hann. El control de sentido (b).
double centroidHz(const std::vector<float>& x) {
    const int N = 16384;
    const int a = kGuRate / 2;
    std::vector<double> w(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) {
        w[static_cast<size_t>(i)] = x[static_cast<size_t>(a + i)] *
                                    (0.5 - 0.5 * std::cos(2.0 * M_PI * i / (N - 1)));
    }
    double num = 0.0, den = 0.0;
    for (int k = 1; k < N / 2; ++k) {
        double re = 0.0, im = 0.0;
        for (int n = 0; n < N; ++n) {
            const double ph = 2.0 * M_PI * k * n / N;
            re += w[static_cast<size_t>(n)] * std::cos(ph);
            im -= w[static_cast<size_t>(n)] * std::sin(ph);
        }
        const double p = re * re + im * im;
        num += p * (static_cast<double>(k) * kGuRate / N);
        den += p;
    }
    return den > 0.0 ? num / den : 0.0;
}

}  // namespace

/**
 * `0:81 Saw Lead` declara en su zona global de preset velocity -> initialFilterFc a -2000
 * cents (pmod), y su instrumento borra el default #2 con la identidad 2.01. Con el borrado
 * honrado, el note-on de produccion tiene que dar MUESTRA A MUESTRA lo mismo que el
 * cutoff puesto a mano en lo que el archivo declara. Antes de MINI-028 sumaba -2400 de mas:
 * centroide 1640 -> 1195 Hz a velocity 124 -> 38 donde el archivo pide 1638 -> 1546.
 */
TEST(SoundFontDefaultTwoIdentity, GeneralUserSawLeadSoundsLikeWhatTheFileDeclares) {
    const auto bytes = generalUserBytes();
    if (bytes.empty()) GTEST_SKIP() << "sin GeneralUser_GS.sf3 — WMA_GENERALUSER_SF3 o el checkout hermano de NoisyPad";

    constexpr int kFileAmount = -2000;  // el pmod de la zona global del preset, leido del archivo
    const auto motor38 = renderSawLead(bytes, 38, 0);
    const auto archivo38 = renderSawLead(bytes, 38, kFileAmount);
    ASSERT_FALSE(motor38.empty());
    ASSERT_GT(rms(motor38), 0.001) << "Saw Lead no sono";

    const double diff = worstDiff(motor38, archivo38);
    EXPECT_LT(diff, kIdentical)
        << "el motor no suena como el archivo declara: el default #2 sigue vivo debajo del "
           "borrado 2.01 de GeneralUser (peor diferencia=" << diff << ")";

    // (b) el control de sentido, no el oraculo: el centroide baja menos de la mitad que
    // con el default vivo (27 %), y el nivel no es de este MINI.
    const auto motor124 = renderSawLead(bytes, 124, 0);
    const double c124 = centroidHz(motor124), c38 = centroidHz(motor38);
    std::printf("  [MINI-028] Saw Lead tecla %d: centroide %.0f Hz (v124) -> %.0f Hz (v38), %.1f %%\n",
                kSawLeadKey, c124, c38, 100.0 * (1.0 - c38 / c124));
    EXPECT_LT(1.0 - c38 / c124, 0.135) << "el centroide baja como con el default vivo (27 %)";
    const double nivelRel = 20.0 * std::log10(rms(motor38) / rms(motor124));
    EXPECT_NEAR(nivelRel, -17.05, 0.2) << "el nivel se movio, y el nivel no es de este MINI";
}
