/**
 * test_soundfont_effects_send.cpp — REQ-040 S1: el send por voz y los dos buses.
 *
 * `tsf_region` no tenia `reverbEffectsSend` (16) ni `chorusEffectsSend` (15) — tsf los declaraba
 * "NOT YET IMPLEMENTED"— y sobre GeneralUser 11 117 de 12 311 zonas declaran el primero y 4390 el
 * segundo: casi todo el font esta programado con cola y salia seco. S1 pone los dos generadores en
 * la region (sumando preset + instrumento, SF2 §8.5) y DOS BUSES MONO en el render: cada voz suma
 * `send * val * gainMono`, con la voz ya atenuada por velocity, initialAttenuation, envolvente y
 * la ganancia del canal (la expresion por toque). Sin efecto todavia: S2 pone las unidades.
 *
 * EL ORACULO ES LA FORMULA, no el motor: el bus tiene que ser, muestra a muestra, `send * mono`
 * donde `mono` es lo que la misma nota deja en la salida SIN paneo. Con pan 0 el render estereo
 * deja `val * gainMono * sqrt(0.5)` en cada canal, asi que `mono = L / sqrt(0.5)` y el bus tiene
 * que dar `send * L / sqrt(0.5)` exacto (misma aritmetica, mismo float). Un send tomado ANTES de
 * la ganancia del canal (AC-040.2) o un preset que no suma (AC-040.1) se separan de eso por
 * factores enteros, no por redondeo.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "../../engines/SoundFontModulatorTable.h"
#include "../../engines/SoundFontNoteOn.h"
#include "support/MinimalSoundFont.h"
#include "tsf.h"
#include "tsf_ext.h"

using wma::sfmod::channelNoteOnWithModulators;
using wma::sfmod::ModulatorTable;
using wma_test::makeMinimalSoundFont;
using wma_test::sf2::ExtraGenerator;
using wma_test::sf2::PitchGenerators;

namespace {

constexpr int kRate = 44100;
constexpr int kPeriod = 100;
constexpr int kRoot = 60;
constexpr int kFrames = 2048;

constexpr uint16_t kGenChorusSend = 15;
constexpr uint16_t kGenReverbSend = 16;
constexpr uint16_t kGenAttackVolEnv = 34;
constexpr uint16_t kGenHoldVolEnv = 35;
constexpr uint16_t kGenSustainVolEnv = 37;
constexpr int16_t kInstant = -12000;

// Envolvente plana e instantanea: la voz esta en sustain pleno desde la primera muestra.
std::vector<ExtraGenerator> flatEnv() {
    return {{kGenAttackVolEnv, kInstant}, {kGenHoldVolEnv, kInstant}, {kGenSustainVolEnv, 0}};
}

struct Rendered {
    std::vector<float> out;     // estereo entrelazado
    std::vector<float> reverb;  // mono
    std::vector<float> chorus;  // mono
    float regionReverbSend = -1.0f, regionChorusSend = -1.0f;
    bool loaded = false;
};

/// Una nota en la raiz a velocity `vel` (0..1), `channelVolume` en el canal (la expresion por
/// toque de produccion entra por ahi), con los generadores dados en la zona del instrumento y
/// del preset. `withSends`: por `tsf_render_float_sends`; si no, por `tsf_render_float` (control).
Rendered render(const std::vector<ExtraGenerator>& instGens, const std::vector<ExtraGenerator>& presetGens,
                float vel = 1.0f, float channelVolume = 1.0f, bool withSends = true) {
    Rendered r;
    PitchGenerators pitch;
    pitch.sinePeriod = kPeriod;
    const auto bytes = makeMinimalSoundFont(kRate, true, -1, -1, {}, 0, pitch, instGens, presetGens);
    tsf* sf = tsf_load_memory(bytes.data(), static_cast<int>(bytes.size()));
    if (!sf) return r;
    r.loaded = true;
    ModulatorTable table;
    table.buildFromFontBytes(bytes.data(), bytes.size());
    tsf_set_output(sf, TSF_STEREO_INTERLEAVED, kRate, 0.0f);
    tsf_set_max_voices(sf, 8);
    tsf_channel_set_presetindex(sf, 0, 0);
    tsf_channel_set_volume(sf, 0, channelVolume);
    channelNoteOnWithModulators(sf, &table, 0, kRoot, vel);
    tsf_ext_started_voice st[4];
    if (tsf_ext_voices_started_by_last_note_on(sf, st, 4) > 0) {
        r.regionReverbSend = st[0].reverbSend;
        r.regionChorusSend = st[0].chorusSend;
    }
    r.out.assign(static_cast<size_t>(kFrames) * 2, 0.0f);
    r.reverb.assign(kFrames, 0.0f);
    r.chorus.assign(kFrames, 0.0f);
    if (withSends) {
        tsf_render_float_sends(sf, r.out.data(), r.reverb.data(), r.chorus.data(), kFrames, 0);
    } else {
        tsf_render_float(sf, r.out.data(), kFrames, 0);
    }
    tsf_close(sf);
    return r;
}

/// El bus esperado por la formula: `send * L / sqrt(0.5)` (pan 0: L = val * gainMono * sqrt(0.5)).
double maxDiffFromFormula(const Rendered& r, const std::vector<float>& bus, float send) {
    const float invPan = 1.0f / std::sqrt(0.5f);
    double m = 0.0;
    for (int i = 0; i < kFrames; ++i) {
        const float mono = r.out[static_cast<size_t>(i) * 2] * invPan;
        m = std::max(m, std::fabs(double(bus[static_cast<size_t>(i)]) - double(send * mono)));
    }
    return m;
}

double peakAbs(const std::vector<float>& a) {
    double m = 0.0;
    for (float x : a) m = std::max(m, std::fabs(double(x)));
    return m;
}

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double m = 0.0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) m = std::max(m, std::fabs(double(a[i]) - double(b[i])));
    return m;
}

// La formula y el motor hacen la misma multiplicacion en otro orden (`gainMono * send` por
// bloque contra `L / sqrt(0.5) * send` aca): un ulp de float sobre amplitudes de 0,5.
constexpr double kUlpTol = 1e-6;

}  // namespace

/**
 * AC-040.1: reverb 500 en el instrumento + 200 en el preset = 0,7 (§8.5, el preset SUMA); chorus
 * ausente = 0. El bus de reverb es `0,7 * mono` muestra a muestra; el de chorus, cero exacto.
 */
TEST(SoundFontEffectsSend, TheReverbBusCarriesTheSummedSendTimesTheVoice) {
    auto inst = flatEnv();
    inst.push_back({kGenReverbSend, 500});
    const auto r = render(inst, {{kGenReverbSend, 200}});
    ASSERT_TRUE(r.loaded);
    ASSERT_GT(peakAbs(r.out), 0.1) << "la nota no sono";
    EXPECT_NEAR(r.regionReverbSend, 0.7f, 1e-6f) << "la region no sumo preset (200) + instrumento (500)";
    EXPECT_EQ(r.regionChorusSend, 0.0f);
    const double diff = maxDiffFromFormula(r, r.reverb, 0.7f);
    std::printf("  [REQ-040] bus de reverb vs 0,7 * mono: max |dif| %.2e (pico del bus %.3f)\n", diff,
                peakAbs(r.reverb));
    EXPECT_LT(diff, kUlpTol) << "el bus de reverb no es send * voz";
    EXPECT_EQ(peakAbs(r.chorus), 0.0) << "sin chorusEffectsSend el bus de chorus tiene que ser CERO exacto";
}

/** AC-040.1 (la otra mitad): chorus 350 solo en el preset; reverb ausente. */
TEST(SoundFontEffectsSend, TheChorusBusCarriesAPresetOnlySend) {
    const auto r = render(flatEnv(), {{kGenChorusSend, 350}});
    ASSERT_TRUE(r.loaded);
    EXPECT_NEAR(r.regionChorusSend, 0.35f, 1e-6f);
    EXPECT_LT(maxDiffFromFormula(r, r.chorus, 0.35f), kUlpTol) << "el bus de chorus no es send * voz";
    EXPECT_EQ(peakAbs(r.reverb), 0.0);
}

/** AC-040.1: sin ninguno de los dos generadores, los dos buses son cero exacto y la voz suena. */
TEST(SoundFontEffectsSend, WithoutSendGeneratorsBothBusesAreExactlyZero) {
    const auto r = render(flatEnv(), {});
    ASSERT_TRUE(r.loaded);
    ASSERT_GT(peakAbs(r.out), 0.1);
    EXPECT_EQ(r.regionReverbSend, 0.0f);
    EXPECT_EQ(peakAbs(r.reverb), 0.0);
    EXPECT_EQ(peakAbs(r.chorus), 0.0);
}

/**
 * AC-040.2: el send se toma DESPUES de la ganancia del canal, que es por donde entra la expresion
 * por toque (R-MOT-40). Con el canal a 0,25 el bus baja x0,25 respecto del canal a 1,0 — y sigue
 * siendo `send * mono` del render atenuado. Mutante: tomar `val * send` sin gainMono.
 */
TEST(SoundFontEffectsSend, TheSendIsTakenAfterTheChannelGain) {
    auto inst = flatEnv();
    inst.push_back({kGenReverbSend, 500});
    const auto full = render(inst, {}, 1.0f, 1.0f);
    const auto quarter = render(inst, {}, 1.0f, 0.25f);
    ASSERT_TRUE(full.loaded && quarter.loaded);
    EXPECT_LT(maxDiffFromFormula(quarter, quarter.reverb, 0.5f), kUlpTol)
        << "con el canal a 0,25 el bus dejo de ser send * voz atenuada";
    const double ratio = peakAbs(quarter.reverb) / peakAbs(full.reverb);
    std::printf("  [REQ-040] bus con el canal a 0,25 / a 1,0: %.4f\n", ratio);
    EXPECT_NEAR(ratio, 0.25, 0.01) << "el send no siguio a la ganancia del canal";
}

/** Y con la velocity: el default #1 atenua la voz y el bus la sigue (misma formula). */
TEST(SoundFontEffectsSend, TheSendFollowsTheVelocityAttenuation) {
    auto inst = flatEnv();
    inst.push_back({kGenReverbSend, 1000});
    const auto soft = render(inst, {}, 32.5f / 127.0f);
    ASSERT_TRUE(soft.loaded);
    ASSERT_GT(peakAbs(soft.out), 1e-3);
    EXPECT_LT(maxDiffFromFormula(soft, soft.reverb, 1.0f), kUlpTol);
}

/**
 * CONTROL: sin buses (`tsf_render_float`) la salida es BYTE A BYTE la de siempre, con los mismos
 * generadores de send en la region. Si esto falla, S1 cambio el sonido de un font sin efecto.
 */
TEST(SoundFontEffectsSend, WithoutBusesTheOutputIsByteForByteTheSame) {
    auto inst = flatEnv();
    inst.push_back({kGenReverbSend, 500});
    inst.push_back({kGenChorusSend, 300});
    const auto a = render(inst, {{kGenReverbSend, 200}}, 0.8f, 0.6f, true);
    const auto b = render(inst, {{kGenReverbSend, 200}}, 0.8f, 0.6f, false);
    ASSERT_TRUE(a.loaded && b.loaded);
    EXPECT_EQ(maxAbsDiff(a.out, b.out), 0.0) << "la salida con y sin buses difiere";
    EXPECT_GT(peakAbs(a.reverb), 0.1) << "el control seria vacio: el bus no llevo nada";
}
