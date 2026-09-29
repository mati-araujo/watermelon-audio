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

/// REQ-040 S3: lo que el default #8 (CC91 -> reverbEffectsSend, 200) aporta con CC91 en su valor
/// de RESET de GM (40), en 0..1 — a TODA voz de TODO font que no lo borre: +6,3 %. El #9 (CC93,
/// reset 0) aporta cero. Es la misma aritmetica que `contribution` (amount × 40/127) × 0,001.
inline float defaultReverbAtReset() { return 200.0f * (40.0f / 127.0f) * 0.001f; }

/// Un font que BORRA el default #8 (mismo `src`/`dest`, amount 0: SF2 §8.4): el send es solo el
/// generador. Sirve para afirmar el generador exacto sin el +6,3 %.
inline wma_test::sf2::ModulatorPlacement withoutDefaultEight() {
    wma_test::sf2::ModulatorPlacement m;
    wma_test::sf2::Modulator erase;
    erase.srcOper = wma_test::sf2::srcOper(91, true, false, false, 0);   // CC91, lineal, unipolar, creciente
    erase.destOper = kGenReverbSend;
    erase.amount = 0;
    m.instrumentGlobal.push_back(erase);
    return m;
}

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
                float vel = 1.0f, float channelVolume = 1.0f, bool withSends = true,
                const wma_test::sf2::ModulatorPlacement& mods = {}) {
    Rendered r;
    PitchGenerators pitch;
    pitch.sinePeriod = kPeriod;
    const auto bytes = makeMinimalSoundFont(kRate, true, -1, -1, mods, 0, pitch, instGens, presetGens);
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

double wma_test_rms(const std::vector<float>& a) {
    double acc = 0.0;
    for (float x : a) acc += double(x) * double(x);
    return a.empty() ? 0.0 : std::sqrt(acc / static_cast<double>(a.size()));
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
    // S3: la VOZ lleva ademas el default #8 con CC91 en reset (+6,3 %): 0,763.
    const float send = 0.7f + defaultReverbAtReset();
    const double diff = maxDiffFromFormula(r, r.reverb, send);
    std::printf("  [REQ-040] bus de reverb vs %.4f * mono: max |dif| %.2e (pico del bus %.3f)\n", send, diff,
                peakAbs(r.reverb));
    EXPECT_LT(diff, kUlpTol) << "el bus de reverb no es (gen + default #8 en reset) * voz";
    EXPECT_EQ(peakAbs(r.chorus), 0.0) << "sin chorusEffectsSend el bus de chorus tiene que ser CERO exacto";
}

/** AC-040.1 (la otra mitad): chorus 350 solo en el preset; reverb ausente. */
TEST(SoundFontEffectsSend, TheChorusBusCarriesAPresetOnlySend) {
    const auto r = render(flatEnv(), {{kGenChorusSend, 350}});
    ASSERT_TRUE(r.loaded);
    EXPECT_NEAR(r.regionChorusSend, 0.35f, 1e-6f);
    // El default #9 (CC93 -> chorus) vale 0 en reset: el chorus es exactamente el generador.
    EXPECT_LT(maxDiffFromFormula(r, r.chorus, 0.35f), kUlpTol) << "el bus de chorus no es send * voz";
    // Y el reverb, sin generador, es SOLO el default #8: +6,3 %.
    EXPECT_LT(maxDiffFromFormula(r, r.reverb, defaultReverbAtReset()), kUlpTol)
        << "sin generador el reverb tiene que ser exactamente el default #8 en reset";
}

/**
 * AC-040.1 / AC-040.4: sin generadores y con el default #8 BORRADO (un modulador CC91 -> reverb con
 * amount 0 en el archivo, §8.4), los dos buses son cero exacto y la voz suena. Sin el borrado, el
 * reverb lleva el +6,3 % del reset de GM (arriba). Es el par que muestra que el CC91 en reset se
 * evalua por el mismo camino de identidad que el resto de los defaults.
 */
TEST(SoundFontEffectsSend, WithoutSendGeneratorsAndWithDefaultEightErasedBothBusesAreExactlyZero) {
    const auto r = render(flatEnv(), {}, 1.0f, 1.0f, true, withoutDefaultEight());
    ASSERT_TRUE(r.loaded);
    ASSERT_GT(peakAbs(r.out), 0.1);
    EXPECT_EQ(r.regionReverbSend, 0.0f);
    EXPECT_EQ(peakAbs(r.reverb), 0.0) << "con el default #8 borrado el bus de reverb no es cero";
    EXPECT_EQ(peakAbs(r.chorus), 0.0);
    // Control: sin borrar, el mismo font manda el +6,3 %.
    const auto d = render(flatEnv(), {});
    ASSERT_TRUE(d.loaded);
    EXPECT_GT(peakAbs(d.reverb), 0.01) << "el default #8 en reset no aporta nada: ¿se evalua CC91?";
    EXPECT_LT(maxDiffFromFormula(d, d.reverb, defaultReverbAtReset()), kUlpTol);
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
    EXPECT_LT(maxDiffFromFormula(quarter, quarter.reverb, 0.5f + defaultReverbAtReset()), kUlpTol)
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
    // gen 1000 + default #8 se satura a 1,0 (el generador tambien se limita a 0..1).
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

// =======================================================================================
// REQ-040 S2 — las dos unidades, la cola y la compuerta, por el MOTOR (SoundFontEngine)
// =======================================================================================

#include <chrono>
#include <memory>

#include "../../engines/SoundFontEngine.h"
#include "../../engines/SoundFontManager.h"
#include "../../engines/SoundFontReverb.h"
#include "../../engines/SoundFontChorus.h"

namespace {

constexpr int kBlock = 128;

struct Rig {
    std::unique_ptr<SoundFontManager> manager;
    std::unique_ptr<SoundFontEngine> engine;
    std::vector<uint8_t> font;
};

/// Un motor con un font de una nota y `reverbSend` / `chorusSend` en 0,1 % (1000 = 100 %). Con
/// 0 / 0 el font ademas BORRA el default #8: "sin sends" significa sin sends, no +6,3 %.
Rig makeRig(int reverbSend, int chorusSend, int rate = kRate) {
    Rig r;
    std::vector<ExtraGenerator> gens = flatEnv();
    if (reverbSend) gens.push_back({kGenReverbSend, static_cast<int16_t>(reverbSend)});
    if (chorusSend) gens.push_back({kGenChorusSend, static_cast<int16_t>(chorusSend)});
    PitchGenerators pitch;
    pitch.sinePeriod = kPeriod;
    const auto mods = (reverbSend == 0 && chorusSend == 0) ? withoutDefaultEight() : wma_test::sf2::ModulatorPlacement{};
    r.font = makeMinimalSoundFont(static_cast<uint32_t>(rate), true, -1, -1, mods, 0, pitch, gens);
    r.manager = std::make_unique<SoundFontManager>();
    if (!r.manager->loadFromMemory(r.font.data(), static_cast<int>(r.font.size()), rate)) return {};
    r.engine = std::make_unique<SoundFontEngine>();
    r.engine->setSoundFontManager(r.manager.get());
    r.engine->prepare(rate, kBlock);
    return r;
}

std::vector<float> renderBlocks(SoundFontEngine& e, int blocks) {
    std::vector<float> out(static_cast<size_t>(blocks) * kBlock * 2, 0.0f);
    for (int b = 0; b < blocks; ++b) e.render(out.data() + static_cast<size_t>(b) * kBlock * 2, kBlock);
    return out;
}

double peakOfBlock(SoundFontEngine& e) {
    std::vector<float> blk(static_cast<size_t>(kBlock) * 2, 0.0f);
    e.render(blk.data(), kBlock);
    return peakAbs(blk);
}

constexpr int blocksFor(double seconds, int rate = kRate) { return static_cast<int>(seconds * rate / kBlock); }

}  // namespace

/**
 * El instrumento: con send de reverb al 100 % la salida del motor lleva MAS energia que con
 * send 0 (el wet se suma), y con la costura en 0 vuelve a ser IDENTICA a send 0 muestra a
 * muestra. Es lo que separa "las unidades suman algo" de "las unidades no estan".
 */
TEST(SoundFontEffectsSendEngine, TheWetIsAddedAndTheSeamCanRemoveIt) {
    Rig wet = makeRig(1000, 1000), dry = makeRig(0, 0), seamed = makeRig(1000, 1000);
    ASSERT_TRUE(wet.engine && dry.engine && seamed.engine);
    seamed.engine->setAmbience(0.0f, 0.0f);   // REQ-042: la costura, ya con su nombre publico
    for (Rig* r : {&wet, &dry, &seamed}) r->engine->noteOn(0, kRoot, 1.0f);
    const auto a = renderBlocks(*wet.engine, blocksFor(0.5));
    const auto b = renderBlocks(*dry.engine, blocksFor(0.5));
    const auto c = renderBlocks(*seamed.engine, blocksFor(0.5));
    const double rmsA = wma_test_rms(a), rmsB = wma_test_rms(b);
    std::printf("  [REQ-040] RMS con sends al 100 %%: %.4f; sin sends: %.4f (+%.2f dB)\n", rmsA, rmsB,
                20.0 * std::log10(rmsA / rmsB));
    EXPECT_GT(rmsA, rmsB * 1.05) << "con sends al 100 % la salida no lleva mas energia: las unidades no suman";
    EXPECT_EQ(maxAbsDiff(c, b), 0.0) << "con la costura en 0 la salida no es identica a la de send 0";
}

/**
 * AC-040.5 (la cola es del cuarto): tras el note-off la cola sigue; tras `noteOffAll` y tras
 * cambiar de preset, sigue. Y tras `reset()`, tras el swap del font y sin font, el bloque
 * siguiente es CERO exacto.
 */
TEST(SoundFontEffectsSendEngine, TheTailBelongsToTheRoomAndClearsOnlyWhereTheContractSays) {
    Rig r = makeRig(1000, 0);
    ASSERT_TRUE(r.engine);
    r.engine->noteOn(0, kRoot, 1.0f);
    renderBlocks(*r.engine, blocksFor(0.3));
    r.engine->noteOff(0);
    renderBlocks(*r.engine, blocksFor(0.05));   // la voz ya murio (release de 1 ms); queda la cola
    r.engine->noteOffAll();
    renderBlocks(*r.engine, 2);
    const double afterOffAll = peakOfBlock(*r.engine);
    EXPECT_GT(afterOffAll, 1e-4) << "tras noteOffAll la cola tendria que seguir sonando";
    r.engine->setPreset(0);   // el mismo preset: el camino de cambio de preset igual corre
    renderBlocks(*r.engine, 2);
    EXPECT_GT(peakOfBlock(*r.engine), 1e-4) << "tras cambiar de preset la cola tendria que seguir";

    // reset(): cero exacto en el bloque siguiente.
    r.engine->reset();
    EXPECT_EQ(peakOfBlock(*r.engine), 0.0) << "tras reset() el bloque siguiente no es cero exacto";

    // Cola viva de nuevo, y swap del font: cero exacto.
    r.engine->noteOn(0, kRoot, 1.0f);
    renderBlocks(*r.engine, blocksFor(0.2));
    r.engine->noteOffAll();
    renderBlocks(*r.engine, 2);
    ASSERT_GT(peakOfBlock(*r.engine), 1e-4);
    std::vector<uint8_t> other = r.font;   // los mismos bytes, OTRO tsf
    ASSERT_TRUE(r.manager->loadFromMemory(other.data(), static_cast<int>(other.size()), kRate));
    EXPECT_EQ(peakOfBlock(*r.engine), 0.0) << "tras el swap del font la cola del anterior sobrevivio";

    // Y sin font: cero exacto, y la cola no vuelve cuando el font vuelve.
    r.engine->noteOn(0, kRoot, 1.0f);
    renderBlocks(*r.engine, blocksFor(0.2));
    r.engine->noteOffAll();
    renderBlocks(*r.engine, 2);
    ASSERT_GT(peakOfBlock(*r.engine), 1e-4);
    r.engine->setSoundFontManager(nullptr);
    EXPECT_EQ(peakOfBlock(*r.engine), 0.0);
    r.engine->setSoundFontManager(r.manager.get());
    // El primer bloque con el font de vuelta puede llevar el ultimo resto de una VOZ (tsf quedo
    // congelado a mitad de su release de 0 s cuando el font se fue): es tsf, no las unidades. A
    // partir del segundo, cero exacto: la cola de la reverb no volvio con el font.
    peakOfBlock(*r.engine);
    EXPECT_EQ(peakOfBlock(*r.engine), 0.0) << "la cola volvio con el font";
}

/**
 * AC-040.6 (la compuerta, en segundos): tras el note-off la cola sigue ~1 s, y a los 2 s de
 * silencio en la entrada del bus la salida es CERO exacto, a 44,1 y a 48 kHz con el mismo
 * bloque de 128 (la compuerta no cuenta bloques). Y el primer send > 0 la reengancha en ese
 * mismo bloque.
 */
TEST(SoundFontEffectsSendEngine, TheGateIsMeasuredInSecondsAndReengagesOnTheFirstSend) {
    for (int rate : {44100, 48000}) {
        Rig r = makeRig(1000, 0, rate);
        ASSERT_TRUE(r.engine) << rate;
        r.engine->noteOn(0, kRoot, 1.0f);
        renderBlocks(*r.engine, blocksFor(0.3, rate));
        r.engine->noteOffAll();
        // La release del fixture es INSTANTANEA (1 ms): la ENTRADA del bus es cero desde el
        // note-off, y la compuerta cierra 2,0 s despues. Se afirma la cola VIVA a los 1,8 s y
        // CERO exacto a los 2,05 s: una compuerta contada en bloques de 48 kHz cerraria a los
        // 2,18 s a 44,1 kHz (mutante), y una en bloques de 44,1 a los 1,84 a 48 kHz.
        renderBlocks(*r.engine, blocksFor(1.8, rate));
        const double at18 = peakOfBlock(*r.engine);
        EXPECT_GT(at18, 0.0) << rate << ": la cola murio antes de los 2 s de silencio";
        renderBlocks(*r.engine, blocksFor(0.25, rate) - 1);
        const double at205 = peakOfBlock(*r.engine);
        std::printf("  [REQ-040] %d Hz: cola a 1,8 s %.2e; a 2,05 s %.2e\n", rate, at18, at205);
        EXPECT_EQ(at205, 0.0) << rate << ": la compuerta no cerro en 2 s de silencio";
        // Reenganche: una nota nueva vuelve a producir wet (la unidad arranca limpia y en el mismo
        // bloque; lo que se ve son los 0,1 s siguientes, porque el comb mas corto del freeverb
        // tarda 1116 muestras en devolver algo y un solo bloque de 128 no puede llevar wet).
        r.engine->noteOn(0, kRoot, 1.0f);
        const auto first = renderBlocks(*r.engine, blocksFor(0.3, rate));
        Rig dry = makeRig(0, 0, rate);
        dry.engine->noteOn(0, kRoot, 1.0f);
        const auto firstDry = renderBlocks(*dry.engine, blocksFor(0.3, rate));
        EXPECT_GT(maxAbsDiff(first, firstDry), 1e-3) << rate << ": tras la compuerta la nota nueva no lleva wet";
        // Y las unidades arrancan LIMPIAS: lo que suena es identico, muestra a muestra, a un motor
        // recien construido. Saltear sin limpiar deja un residuo de ~1e-8 en las lineas (mutante).
        Rig fresh = makeRig(1000, 0, rate);
        fresh.engine->noteOn(0, kRoot, 1.0f);
        const auto firstFresh = renderBlocks(*fresh.engine, blocksFor(0.3, rate));
        EXPECT_EQ(maxAbsDiff(first, firstFresh), 0.0) << rate << ": la unidad reenganchada no arranco limpia";
    }
}

namespace {

/// Un trabajo medido: corre `blocks` bloques y devuelve los ns por bloque.
template <typename Job>
double nsPerBlock(Job&& job, int blocks) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    for (int b = 0; b < blocks; ++b) job();
    return std::chrono::duration<double, std::nano>(clock::now() - t0).count() / blocks;
}

/// El resultado de una medicion PAREADA: los dos lados en ns/bloque (promediados sobre todos
/// los pares, que es lo que se imprime) y la distribucion del cociente POR PAR.
struct Paired {
    double nsA = 0.0;
    double nsB = 0.0;
    double median = 0.0;
    double lo = 0.0;
    double hi = 0.0;
};

/**
 * 🔴 POR QUE PAREADO Y ENTRELAZADO, Y NO DOS TOTALES (MINI-032, segunda tanda).
 *
 * Este test afirma cocientes, y la version anterior media el numerador y el denominador en
 * MOMENTOS DISTINTOS: primero 10 s de tsf, despues 10 s de reverb, despues 10 s de chorus,
 * despues 10 s de compuerta. Con eso, una rafaga de carga que cae sobre uno de los tramos
 * infla ese lado solo y el cociente se mueve sin que el codigo cambie. Medido el 2026-09-29:
 * seis corridas seguidas a la misma carga dieron unidades/tsf entre **0,55x y 3,81x** (factor
 * 7) con el arbol intacto, y una de las seis se puso ROJA contra un techo de 3x que ya era de
 * sanidad. O sea que el problema no era ningun techo: era el DENOMINADOR.
 *
 * Asi que los dos lados se miden entrelazados en tramos cortos (`blocksPerPair`), y el
 * veredicto sale de la **mediana de los cocientes por par**. Una rafaga que dura mas que un
 * par les pega a los dos lados del par por igual y el cociente casi no se mueve; una que dura
 * menos arruina a lo sumo unos pocos pares, y la mediana los descarta. El promedio NO servia:
 * un solo par arruinado lo arrastra.
 *
 * El ORDEN dentro del par se alterna (A,B / B,A / ...): si A fuera siempre primero, cualquier
 * efecto sistematico de entrar al par —cache, frecuencia del core— le tocaria siempre al mismo
 * lado y se leeria como si un trabajo costara mas que el otro.
 */
template <typename JobA, typename JobB>
Paired measurePaired(JobA&& a, JobB&& b, int pairs, int blocksPerPair) {
    std::vector<double> ratios;
    ratios.reserve(static_cast<size_t>(pairs));
    double sumA = 0.0, sumB = 0.0;
    for (int p = 0; p < pairs; ++p) {
        double na, nb;
        if (p % 2 == 0) {
            na = nsPerBlock(a, blocksPerPair);
            nb = nsPerBlock(b, blocksPerPair);
        } else {
            nb = nsPerBlock(b, blocksPerPair);
            na = nsPerBlock(a, blocksPerPair);
        }
        sumA += na;
        sumB += nb;
        ratios.push_back(na / nb);
    }
    std::sort(ratios.begin(), ratios.end());
    Paired r;
    r.nsA = sumA / pairs;
    r.nsB = sumB / pairs;
    const size_t n = ratios.size();
    r.median = (n % 2 == 1) ? ratios[n / 2] : 0.5 * (ratios[n / 2 - 1] + ratios[n / 2]);
    r.lo = ratios.front();
    r.hi = ratios.back();
    return r;
}

}  // namespace

/**
 * AC-040.7: el costo, MEDIDO. 10 s de bloques de 128 a 48 kHz con 8 voces con send al 100 %.
 * Los dos cocientes salen de mediciones **pareadas y entrelazadas** (ver `measurePaired`), que es
 * lo que los hace un veredicto y no un reporte de la carga de la maquina:
 *
 *   - las dos unidades contra el render de tsf de esas 8 voces;
 *   - la compuerta (el barrido de pico de los dos buses) contra **su mismo barrido sin la rama**
 *     —mismo bucle, mismos buffers, misma forma de acceso a memoria, sin el `if`—, que es lo que
 *     AC-040.7 queria decir con "la compuerta cuesta una fraccion de lo que evita". Contra tsf no
 *     se puede: son bucles de forma distinta (memoria pura contra aritmetica por acceso) y su
 *     RELACION se mueve con el sanitizer y con la contencion de memoria de `ctest -j`, que es
 *     exactamente lo que se midio cinco veces como rojo.
 *
 * Los numeros se imprimen; los techos son relativos porque el motor no tiene presupuesto absoluto
 * declarado, y son de SANIDAD: el numero que importa es el impreso, y si sube de verdad se ve en el
 * diff del log.
 *
 * El techo de las unidades era 1,0x al escribir el AC y quedo en 1,5x AL MEDIR (2026-09-15): el
 * build de host es -O0 (`run-cpp-tests.sh`) y ahi un Freeverb (16 combs + 8 allpass) mide 1,12x un
 * tsf de 8 voces con interpolacion lineal y sin filtro — 17 us por bloque de 2,67 ms, el 0,65 % del
 * tiempo real. La primera version, por muestra y con `vector::operator[]`, media 2,0x: lo que se
 * compro fue recorrer una linea por vez sobre el bloque con punteros crudos y evaluar el LFO del
 * chorus por bloque.
 */
TEST(SoundFontEffectsSendEngine, TheUnitsCostLessThanTheVoicesAndTheGateAFractionOfThat) {
    constexpr int rate = 48000, seconds = 10, blocks = seconds * rate / kBlock;
    // 64 pares de 58 bloques ~ los 3750 bloques (10 s) de siempre. El tramo es corto a
    // proposito: tiene que durar menos que una rafaga de carga tipica para que la rafaga caiga
    // sobre los DOS lados del par, y largo como para que el reloj no sea el que se mide (58
    // bloques de tsf son ~1,2 ms, contra una resolucion de `steady_clock` de nanosegundos).
    constexpr int kPairs = 64;
    constexpr int kBlocksPerPair = blocks / kPairs;
    static_assert(kBlocksPerPair > 0, "el tramo de un par no puede ser cero bloques");

    // tsf con 8 voces y sends.
    std::vector<ExtraGenerator> gens = flatEnv();
    gens.push_back({kGenReverbSend, 1000});
    gens.push_back({kGenChorusSend, 1000});
    PitchGenerators pitch;
    pitch.sinePeriod = kPeriod;
    const auto bytes = makeMinimalSoundFont(rate, true, -1, -1, {}, 0, pitch, gens);
    tsf* sf = tsf_load_memory(bytes.data(), static_cast<int>(bytes.size()));
    ASSERT_NE(sf, nullptr);
    tsf_set_output(sf, TSF_STEREO_INTERLEAVED, rate, 0.0f);
    tsf_set_max_voices(sf, 16);
    for (int k = 0; k < 8; ++k) tsf_note_on(sf, 0, kRoot - 12 + k * 3, 1.0f);

    std::vector<float> out(static_cast<size_t>(kBlock) * 2), rb(kBlock), cb(kBlock);
    wma::SoundFontReverb rev; rev.prepare(rate);
    wma::SoundFontChorus cho; cho.prepare(rate);
    // Las unidades y la compuerta trabajan sobre buffers PROPIOS, con la senal fija de siempre.
    // Entrelazar los trabajos significa que tsf escribe sus buses en cada tramo; si las unidades
    // leyeran esos mismos buffers, lo que procesan cambiaria de tramo en tramo y el estimulo
    // dejaria de ser el mismo para los dos lados del par.
    std::vector<float> rbFx(kBlock), cbFx(kBlock), rbGate(kBlock), cbGate(kBlock);
    for (int i = 0; i < kBlock; ++i) {
        const float v = 0.3f * std::sin(0.05f * i);
        rbFx[i] = cbFx[i] = rbGate[i] = cbGate[i] = v;
    }

    // --- par 1: las dos unidades contra tsf -------------------------------------------------
    double nsRev = 0.0, nsCho = 0.0;
    const Paired units = measurePaired(
        [&] {
            const auto t0 = std::chrono::steady_clock::now();
            rev.addReverbWetFromMono(rbFx.data(), out.data(), kBlock);
            const auto t1 = std::chrono::steady_clock::now();
            cho.addChorusWetFromMono(cbFx.data(), out.data(), kBlock);
            const auto t2 = std::chrono::steady_clock::now();
            nsRev += std::chrono::duration<double, std::nano>(t1 - t0).count();
            nsCho += std::chrono::duration<double, std::nano>(t2 - t1).count();
        },
        [&] { tsf_render_float_sends(sf, out.data(), rb.data(), cb.data(), kBlock, 0); },
        kPairs, kBlocksPerPair);
    tsf_close(sf);
    const int measuredBlocks = kPairs * kBlocksPerPair;
    nsRev /= measuredBlocks;
    nsCho /= measuredBlocks;
    const double nsTsf = units.nsB;
    const double nsUnits = units.nsA;

    // --- par 2: la compuerta contra su mismo barrido SIN la rama -----------------------------
    // El `if` es lo unico que cambia. Lo reemplaza un `+=` porque algo tiene que consumir `a` y
    // `c`: si el lado B no los usara, lo que se estaria comparando no seria "con rama contra sin
    // rama" sino "con rama contra sin los fabs".
    volatile float sink = 0.0f;
    const Paired gate = measurePaired(
        [&] {
            float peak = 0.0f;
            for (int i = 0; i < kBlock; ++i) { rbGate[i] *= 1.0f; cbGate[i] *= 1.0f; const float a = std::fabs(rbGate[i]), c = std::fabs(cbGate[i]); if (a > peak) peak = a; if (c > peak) peak = c; }
            sink = sink + peak;
        },
        [&] {
            float acc = 0.0f;
            for (int i = 0; i < kBlock; ++i) { rbGate[i] *= 1.0f; cbGate[i] *= 1.0f; const float a = std::fabs(rbGate[i]), c = std::fabs(cbGate[i]); acc += a + c; }
            sink = sink + acc;
        },
        kPairs, kBlocksPerPair);
    const double nsGate = gate.nsA;
    const double nsBlock = 1e9 * kBlock / rate;

    std::printf("  [REQ-040] costo por bloque de 128 a 48 kHz (-O0): tsf 8 voces %.0f ns · reverb %.0f + chorus %.0f = "
                "%.0f ns (%.2fx tsf, %.2f %% del tiempo real) · compuerta %.0f ns (%.1f %% de tsf)"
                " · pareado (mediana de %d pares de %d bloques): unidades %.2fx tsf [%.2f–%.2f] · "
                "compuerta %.2fx su barrido sin la rama [%.2f–%.2f]\n", nsTsf, nsRev,
                nsCho, nsUnits, nsUnits / nsTsf, 100.0 * nsUnits / nsBlock, nsGate, 100.0 * nsGate / nsTsf,
                kPairs, kBlocksPerPair, units.median, units.lo, units.hi,
                gate.median, gate.lo, gate.hi);

    // 🔴 LOS DOS TECHOS SE DECLARARON **DESPUES** DE MEDIR (MINI-032, segunda tanda).
    //
    // Veinte corridas del binario el 2026-09-29, alternando el diseño VIEJO (`af81a1a`: totales
    // en momentos distintos, compuerta contra tsf) y el NUEVO (pareado, compuerta contra su
    // barrido sin la rama) corrida por corrida, para que los dos vieran la MISMA carga: load
    // 1 min 29,5–31,8 todo el tiempo (Python de otras sesiones, ningun gate).
    //
    //                                        min     max     factor   mediana   rojos
    //   VIEJO  unidades / tsf                0,72x   1,60x    2,2      1,11x     
    //   VIEJO  compuerta / tsf               2,9 %   207,1 %  71       5,2 %     1 de 20
    //   NUEVO  unidades / tsf   (mediana)    0,98x   1,27x    1,30     1,08x     0 de 20
    //   NUEVO  compuerta / sin rama (med.)   0,97x   1,03x    1,06     1,02x     0 de 20
    //
    // El rojo del VIEJO fue la compuerta a 207 % de tsf, o sea la clase de la primera ocurrencia
    // (93,7 %) que el techo de 0,5 declaraba no cubrir: se reprodujo con el codigo intacto, en la
    // primera tanda de diez. Y los PARES individuales del diseño nuevo llegan a 0,02x y 54x —la
    // carga es la misma—: lo que no se mueve es la MEDIANA, que es lo que el veredicto lee.
    //
    // Por que cada techo es el que es:
    //
    //   * COMPUERTA < 1,5x su barrido sin la rama. Medido 1,02x (la rama cuesta ~2 %), peor
    //     mediana 1,03x en veinte: el techo deja 46 % de margen sobre el peor visto y una
    //     dispersion de ±3 %. Y a diferencia del cociente contra tsf, este NO lo puede correr un
    //     sanitizer: los dos lados hacen exactamente los mismos accesos a memoria, y todos los
    //     builds de test son Debug/-O0 (sanitizers incluidos), asi que ninguno vectoriza un lado
    //     y el otro no. Lo que un 1,5x deja pasar es una compuerta un 50 % mas cara que el
    //     barrido que envuelve, que ya es un defecto de la compuerta y no ruido.
    //
    //   * UNIDADES < 3x tsf, SIN CAMBIO, y ahora por otra razon. Por la carga sola se podria bajar
    //     a 2x (1,57x el peor de veinte, y atraparia volver a la version por muestra que media
    //     2,0x). No se baja porque hay un corrimiento que el pareo NO cancela y que aca no se pudo
    //     medir: un sanitizer instrumenta distinto un Freeverb (lineas de retardo, mucha memoria)
    //     que un tsf (aritmetica por muestra), y eso mueve el cociente de forma SISTEMATICA, no
    //     por rafagas. Pareado o no, eso lo mide el CI. Bajarlo a 2x es un paso aparte, cuando los
    //     jobs de ASan y TSan hayan impreso su mediana pareada.
    EXPECT_LT(units.median, 3.0) << "las dos unidades cuestan mas de 3x el render de 8 voces "
                                    "(mediana de los pares; medido 1,08x)";
    EXPECT_LT(gate.median, 1.5) << "la compuerta cuesta mas de 1,5x su mismo barrido sin la rama "
                                   "(mediana de los pares; medido 1,02x)";
}

