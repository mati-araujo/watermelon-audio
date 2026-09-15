/**
 * test_c_api_soundfont_ambience.cpp — REQ-042 S1: la perilla de la ambiencia del font.
 *
 * `wma_sf_set_ambience(engine, reverb, chorus)` y sus dos getters, sobre la costura que REQ-040
 * dejo adentro (`SoundFontSendBus::setSendScale`). Lo que se afirma aca, y desde donde:
 *
 *  - POR LA FACHADA (`CApiFixture`: `wma_engine_create` + backend falso + `onAudioReady`), que es
 *    lo que iOS y el JNI corren de verdad: el set llega a los getters (AC-042.5), NaN y fuera de
 *    rango (AC-042.2), la conservacion tras descarga/cambio de font/`prepare()` a otro rate
 *    (AC-042.4), y el render: con 0/0 la salida es MUESTRA A MUESTRA la del font sin sends, y con
 *    1/1 es byte a byte la del motor que nadie toco (AC-042.1).
 *
 *  - POR EL MOTOR (`SoundFontEngine` directo, sin la etapa de salida), lo que la fachada NO deja
 *    medir: la salida de la fachada pasa por un soft-clipper `tanh` y por dither, asi que "el wet"
 *    no se puede restar limpio ahi. La unidad de la perilla (0,5 = −6 dB sobre el send, decision
 *    2), el escalon 0/0 → 1/1 con nota sostenida (AC-042.7) y `reset()` (AC-042.4) se miden sobre
 *    el motor, donde `out − dry` ES el wet.
 *
 * EL DRY ES OTRO FONT, no otra perilla: el font "seco" no declara sends y ademas BORRA el default
 * #8 (CC91 → reverb, +6,3 % en reset de GM) con un modulador de amount 0 (SF2 §8.4). Es el control
 * de REQ-040 S3: si la perilla escalara solo el generador y no el default #8, "0/0 ≡ seco" se cae
 * por ese 6,3 % — y el test con el font que SOLO tiene el default #8 lo dice con su nombre.
 *
 * Mutantes previstos por el plan (todos ejercidos en la etapa; el veredicto va en el reporte):
 * escalador ignorado; aplicado DESPUES de las unidades; solo el generador y no el default #8;
 * saturacion apagada; pisado en `prepare()`.
 */

#include "support/CApiFixture.h"
#include "support/MinimalSoundFont.h"

#include "engines/SoundFontEngine.h"
#include "engines/SoundFontManager.h"
#include "platform/LogCaptureBuffer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace wma_test {
namespace {

using wma_test::sf2::ExtraGenerator;
using wma_test::sf2::PitchGenerators;

constexpr int kRate = 48000;
constexpr int kOtherRate = 44100;
constexpr int kBlock = 256;
constexpr int kEngineTypeSoundFont = 6;
constexpr int kNote = 60;
constexpr int kPeriod = 100;   // 480 Hz a 48 kHz

constexpr uint16_t kGenChorusSend = 15;
constexpr uint16_t kGenReverbSend = 16;
constexpr uint16_t kGenAttackVolEnv = 34;
constexpr uint16_t kGenHoldVolEnv = 35;
constexpr uint16_t kGenSustainVolEnv = 37;
constexpr int16_t kInstant = -12000;

/** El piso de silencio del motor mide ~1e-5. Esto es "sono", con margen. */
constexpr double kAudible = 0.005;

/// Envolvente plana e instantanea: la voz esta en sustain pleno desde la primera muestra.
std::vector<ExtraGenerator> flatEnv() {
    return {{kGenAttackVolEnv, kInstant}, {kGenHoldVolEnv, kInstant}, {kGenSustainVolEnv, 0}};
}

/// Un font que BORRA el default #8 (mismo src/dest, amount 0: SF2 §8.4).
wma_test::sf2::ModulatorPlacement withoutDefaultEight() {
    wma_test::sf2::ModulatorPlacement m;
    wma_test::sf2::Modulator erase;
    erase.srcOper = wma_test::sf2::srcOper(91, true, false, false, 0);
    erase.destOper = kGenReverbSend;
    erase.amount = 0;
    m.instrumentGlobal.push_back(erase);
    return m;
}

enum class Font {
    Sends,             ///< reverb 500 + chorus 300 en el instrumento; el default #8 VIVO
    OnlyDefaultEight,  ///< sin generadores de send; el default #8 VIVO (+6,3 % de reverb)
    Dry,               ///< sin generadores y con el default #8 BORRADO: cero sends de verdad
};

std::vector<uint8_t> makeFont(Font which, int rate) {
    std::vector<ExtraGenerator> gens = flatEnv();
    if (which == Font::Sends) {
        gens.push_back({kGenReverbSend, 500});
        gens.push_back({kGenChorusSend, 300});
    }
    PitchGenerators pitch;
    pitch.sinePeriod = kPeriod;
    const auto mods = which == Font::Dry ? withoutDefaultEight() : wma_test::sf2::ModulatorPlacement{};
    return makeMinimalSoundFont(static_cast<uint32_t>(rate), true, -1, -1, mods, 0, pitch, gens);
}

double rms(const std::vector<float>& buf) {
    if (buf.empty()) return 0.0;
    double acc = 0.0;
    for (const float s : buf) acc += static_cast<double>(s) * s;
    return std::sqrt(acc / static_cast<double>(buf.size()));
}

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double m = 0.0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) m = std::max(m, std::fabs(double(a[i]) - double(b[i])));
    return m;
}

double peakAbs(const std::vector<float>& a) {
    double m = 0.0;
    for (float x : a) m = std::max(m, std::fabs(double(x)));
    return m;
}

// =====================================================================================
// La fachada
// =====================================================================================

class CApiSoundFontAmbienceTest : public CApiFixture {
protected:
    void TearDown() override {
        wma_log_capture_set_enabled(false);
        wma::LogCaptureBuffer::instance().clear();
        CApiFixture::TearDown();
    }

    /**
     * Un motor NUEVO por escenario. `wma_engine_create` registra el BackendManager como instancia
     * global, asi que no pueden convivir dos: se destruye el de la fixture y se crea otro por el
     * mismo camino, y TearDown destruye el ultimo.
     */
    void recreate() {
        wma_engine_destroy(mWma);
        resetLastCreatedSystemBackend();
        mWma = wma_engine_create();
        ASSERT_NE(mWma, nullptr);
        mBackend = lastCreatedSystemBackend();
        ASSERT_NE(mBackend, nullptr);
    }

    /**
     * Motor nuevo con `font` cargado, en modo SoundFont, arrancado a `rate` sobre el backend falso
     * y con una nota sostenida en el toque 0. `beforeStart` corre entre la carga y el arranque:
     * lo que se ponga ahi atraviesa `prepare()` — que es donde un mutante lo pisaria.
     */
    void bringUp(Font font, int rate, const std::function<void()>& beforeStart = {}) {
        recreate();
        const auto sf2 = makeFont(font, rate);
        wma_set_engine_type(mWma, kEngineTypeSoundFont);
        ASSERT_TRUE(wma_sf_load_data(mWma, sf2.data(), static_cast<int>(sf2.size())));
        wma_sf_set_preset(mWma, 0);
        if (beforeStart) beforeStart();
        startAt(rate, 0);
        wma_sf_note_on(mWma, 0, kNote, 1.0f);
    }

    /// `blocks` bloques de `kBlock` por `onAudioReady`, concatenados (estereo entrelazado).
    std::vector<float> renderOut(int blocks) {
        std::vector<float> out;
        out.reserve(static_cast<size_t>(blocks) * kBlock * 2);
        std::vector<float> block(static_cast<size_t>(kBlock) * 2, 0.0f);
        for (int b = 0; b < blocks; ++b) {
            std::fill(block.begin(), block.end(), 0.0f);
            mWma->engine->onAudioReady(block.data(), nullptr, kBlock);
            out.insert(out.end(), block.begin(), block.end());
        }
        return out;
    }

    /// Un escenario entero: `bringUp` + `blocks` bloques.
    std::vector<float> renderScenario(Font font, int blocks, const std::function<void()>& beforeStart = {}) {
        bringUp(font, kRate, beforeStart);
        if (::testing::Test::HasFatalFailure()) return {};
        return renderOut(blocks);
    }

    static std::vector<std::string> drainLog() {
        WmaLogBatch* batch = wma_log_capture_drain();
        std::vector<std::string> out;
        const int count = wma_log_batch_count(batch);
        for (int i = 0; i < count; ++i) {
            const char* line = wma_log_batch_line(batch, i);
            out.emplace_back(line ? line : "<null>");
        }
        wma_log_batch_free(batch);
        return out;
    }
};

constexpr int kBlocksHalfSecond = kRate / 2 / kBlock;   // 93 bloques ≈ 0,5 s

// -------------------------------------------------------------------------------------
// El fixture, antes que nada: una configuracion contra si misma
// -------------------------------------------------------------------------------------

TEST_F(CApiSoundFontAmbienceTest, TheHarnessIsDeterministicAndActuallyMakesSound) {
    // Si esto falla, ninguna igualdad de abajo significa nada: el dither de la etapa de salida
    // es un xorshift SEMBRADO y `prepare()` lo resiembra, asi que dos motores nuevos con el
    // mismo font tienen que dar el mismo byte. Se afirma antes de apoyarse en ello.
    const auto a = renderScenario(Font::Sends, kBlocksHalfSecond);
    const auto b = renderScenario(Font::Sends, kBlocksHalfSecond);
    ASSERT_GT(rms(a), kAudible) << "el motor no esta sonando: no hay nada que medir";
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(maxAbsDiff(a, b), 0.0) << "dos renders identicos divergen: el arnes no es determinista";
}

// -------------------------------------------------------------------------------------
// AC-042.5 — el set llega a los getters; el default es 1/1 (= FluidSynth)
// -------------------------------------------------------------------------------------

TEST_F(CApiSoundFontAmbienceTest, TheDefaultIsOneOneAndASetRoundTripsPerBus) {
    // Un motor recien creado, sin arrancar y sin font: la perilla ya existe y vale 1/1.
    EXPECT_EQ(wma_sf_get_ambience_reverb(mWma), 1.0f) << "el default de reverb no es 1 (= FluidSynth)";
    EXPECT_EQ(wma_sf_get_ambience_chorus(mWma), 1.0f) << "el default de chorus no es 1 (= FluidSynth)";

    // Asimetrico a proposito: un set que cruce los dos buses pasa 0,3/0,3 y muere aca.
    wma_sf_set_ambience(mWma, 0.3f, 0.6f);
    EXPECT_EQ(wma_sf_get_ambience_reverb(mWma), 0.3f);
    EXPECT_EQ(wma_sf_get_ambience_chorus(mWma), 0.6f);

    // Y los bordes exactos pasan sin tocarse.
    wma_sf_set_ambience(mWma, 0.0f, 1.0f);
    EXPECT_EQ(wma_sf_get_ambience_reverb(mWma), 0.0f);
    EXPECT_EQ(wma_sf_get_ambience_chorus(mWma), 1.0f);
}

TEST_F(CApiSoundFontAmbienceTest, ANullHandleIsANoOpAndTheGettersAnswerTheDefault) {
    // El contrato de toda la C API: nunca deref de un handle nulo. Los getters contestan el
    // neutro (1/1), no 0: "sin motor" no es "ambiencia apagada".
    wma_sf_set_ambience(nullptr, 0.2f, 0.2f);
    EXPECT_EQ(wma_sf_get_ambience_reverb(nullptr), 1.0f);
    EXPECT_EQ(wma_sf_get_ambience_chorus(nullptr), 1.0f);
}

// -------------------------------------------------------------------------------------
// AC-042.2 — NaN → sin cambio + rastro; fuera de rango satura; nada de eso llega al atomic
// -------------------------------------------------------------------------------------

TEST_F(CApiSoundFontAmbienceTest, NaNLeavesThatBusUntouchedAndLeavesATraceInTheLog) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    wma_sf_set_ambience(mWma, 0.3f, 0.3f);

    wma::LogCaptureBuffer::instance().clear();
    wma_log_capture_set_enabled(true);
    wma_sf_set_ambience(mWma, nan, 0.4f);   // reverb NaN, chorus valido
    const auto lines = drainLog();
    wma_log_capture_set_enabled(false);

    // Por bus: el NaN no toca el suyo y el valor valido del otro SI entra.
    EXPECT_EQ(wma_sf_get_ambience_reverb(mWma), 0.3f) << "un NaN cambio el reverb (std::clamp(NaN) devuelve NaN)";
    EXPECT_EQ(wma_sf_get_ambience_chorus(mWma), 0.4f) << "el chorus valido no entro porque el reverb era NaN";
    EXPECT_FALSE(std::isnan(wma_sf_get_ambience_reverb(mWma)));

    bool traced = false;
    for (const auto& l : lines) traced = traced || l.find("NaN") != std::string::npos;
    EXPECT_TRUE(traced) << "el NaN se ignoro en silencio: el registro no tiene rastro (" << lines.size() << " lineas)";

    // Y al reves, para que el rastro no sea del bus equivocado.
    wma_sf_set_ambience(mWma, 0.7f, nan);
    EXPECT_EQ(wma_sf_get_ambience_reverb(mWma), 0.7f);
    EXPECT_EQ(wma_sf_get_ambience_chorus(mWma), 0.4f) << "un NaN cambio el chorus";
}

TEST_F(CApiSoundFontAmbienceTest, OutOfRangeSaturatesToZeroOne) {
    wma_sf_set_ambience(mWma, 1.5f, -1.0f);
    EXPECT_EQ(wma_sf_get_ambience_reverb(mWma), 1.0f) << "1,5 no saturo a 1";
    EXPECT_EQ(wma_sf_get_ambience_chorus(mWma), 0.0f) << "-1 no saturo a 0";

    const float inf = std::numeric_limits<float>::infinity();
    wma_sf_set_ambience(mWma, -inf, inf);
    EXPECT_EQ(wma_sf_get_ambience_reverb(mWma), 0.0f);
    EXPECT_EQ(wma_sf_get_ambience_chorus(mWma), 1.0f);
}

// -------------------------------------------------------------------------------------
// AC-042.4 — del INSTRUMENTO: sobrevive a descarga, cambio de font y prepare() a otro rate
// -------------------------------------------------------------------------------------

TEST_F(CApiSoundFontAmbienceTest, TheKnobSurvivesUnloadSwapAndPrepareAtAnotherRate) {
    // Puesta ANTES de cargar y de arrancar: todo lo que sigue tendria que respetarla.
    wma_sf_set_ambience(mWma, 0.3f, 0.3f);

    const auto a = makeFont(Font::Sends, kRate);
    const auto b = makeFont(Font::Dry, kRate);
    wma_set_engine_type(mWma, kEngineTypeSoundFont);
    ASSERT_TRUE(wma_sf_load_data(mWma, a.data(), static_cast<int>(a.size())));
    EXPECT_EQ(wma_sf_get_ambience_reverb(mWma), 0.3f) << "cargar un font piso la perilla";

    startAt(kRate, 0);   // prepare() a 48 kHz
    EXPECT_EQ(wma_sf_get_ambience_reverb(mWma), 0.3f) << "prepare() piso la perilla";
    EXPECT_EQ(wma_sf_get_ambience_chorus(mWma), 0.3f);

    wma_sf_note_on(mWma, 0, kNote, 1.0f);
    renderOut(4);
    ASSERT_TRUE(wma_sf_load_data(mWma, b.data(), static_cast<int>(b.size())));   // swap en caliente
    renderOut(4);
    EXPECT_EQ(wma_sf_get_ambience_reverb(mWma), 0.3f) << "el cambio de font piso la perilla";

    wma_sf_unload(mWma);
    renderOut(2);   // sin font: el render limpia la cola (decision 4 de REQ-040), no la perilla
    EXPECT_EQ(wma_sf_get_ambience_reverb(mWma), 0.3f) << "descargar el font piso la perilla";
    EXPECT_EQ(wma_sf_get_ambience_chorus(mWma), 0.3f);

    ASSERT_EQ(wma_engine_stop(mWma, 0), WMA_OK);
    startAt(kOtherRate, 0);   // prepare() a 44,1 kHz: otro rate, mismas perillas
    EXPECT_EQ(wma_sf_get_ambience_reverb(mWma), 0.3f) << "prepare() con otro rate piso la perilla";
    EXPECT_EQ(wma_sf_get_ambience_chorus(mWma), 0.3f);
}

// -------------------------------------------------------------------------------------
// AC-042.1 — el render por la fachada
// -------------------------------------------------------------------------------------

/**
 * Con 0/0 la salida es MUESTRA A MUESTRA la del font seco (sin generadores y con el default #8
 * borrado). El control es la misma pareja con 1/1: si no difiere, la igualdad de arriba seria
 * vacia (un motor mudo tambien "iguala" al seco).
 */
TEST_F(CApiSoundFontAmbienceTest, WithZeroZeroTheFacadeRendersTheDrySampleForSample) {
    const auto dry = renderScenario(Font::Dry, kBlocksHalfSecond);
    const auto zeroed = renderScenario(Font::Sends, kBlocksHalfSecond, [this] { wma_sf_set_ambience(mWma, 0.0f, 0.0f); });
    const auto wet = renderScenario(Font::Sends, kBlocksHalfSecond);
    ASSERT_GT(rms(dry), kAudible);
    ASSERT_EQ(dry.size(), zeroed.size());

    const double control = maxAbsDiff(wet, dry);
    std::printf("  [REQ-042] fachada: |1/1 - seco| max %.3e; |0/0 - seco| max %.3e\n", control, maxAbsDiff(zeroed, dry));
    ASSERT_GT(control, 1e-3) << "con 1/1 no hay wet: el control es vacio y la igualdad de abajo no dice nada";
    EXPECT_EQ(maxAbsDiff(zeroed, dry), 0.0) << "con 0/0 la salida no es identica, muestra a muestra, al font seco";
}

/**
 * Con 1/1 el render es BYTE A BYTE el del motor que nadie toco (= v2.18.0): el escalador
 * multiplica y no desplaza. Mutante: un `1 → 0,99`, o una rampa que arranque de otro valor.
 */
TEST_F(CApiSoundFontAmbienceTest, WithOneOneTheRenderIsByteForByteTheUntouchedDefault) {
    const auto untouched = renderScenario(Font::Sends, kBlocksHalfSecond);
    const auto explicitOne = renderScenario(Font::Sends, kBlocksHalfSecond, [this] { wma_sf_set_ambience(mWma, 1.0f, 1.0f); });
    ASSERT_GT(rms(untouched), kAudible);
    EXPECT_EQ(maxAbsDiff(untouched, explicitOne), 0.0) << "set(1, 1) no es neutro";
}

/**
 * El control "default #8 vivo" de REQ-040 S3: un font SIN generadores de send lleva igual +6,3 %
 * de reverb por CC91 en reset. Con 0/0 tiene que quedar identico al seco (que lo borra): si la
 * perilla escalara solo el generador y no el modulador, este es el test que lo dice.
 */
TEST_F(CApiSoundFontAmbienceTest, ZeroZeroAlsoSilencesTheDefaultEightReverbSend) {
    const auto dry = renderScenario(Font::Dry, kBlocksHalfSecond);
    const auto onlyEight = renderScenario(Font::OnlyDefaultEight, kBlocksHalfSecond);
    const auto zeroed = renderScenario(Font::OnlyDefaultEight, kBlocksHalfSecond, [this] { wma_sf_set_ambience(mWma, 0.0f, 0.0f); });
    ASSERT_GT(rms(dry), kAudible);

    const double control = maxAbsDiff(onlyEight, dry);
    std::printf("  [REQ-042] solo default #8: |1/1 - seco| max %.3e; |0/0 - seco| max %.3e\n", control, maxAbsDiff(zeroed, dry));
    ASSERT_GT(control, 1e-4) << "el default #8 en reset no aporta wet: el control es vacio";
    EXPECT_EQ(maxAbsDiff(zeroed, dry), 0.0) << "con 0/0 sobrevivio el send del default #8";
}

// =====================================================================================
// El motor: donde `out − dry` ES el wet
// =====================================================================================

struct Rig {
    std::unique_ptr<SoundFontManager> manager;
    std::unique_ptr<SoundFontEngine> engine;
    std::vector<uint8_t> font;
};

Rig makeRig(Font which, int rate, int block) {
    Rig r;
    r.font = makeFont(which, rate);
    r.manager = std::make_unique<SoundFontManager>();
    if (!r.manager->loadFromMemory(r.font.data(), static_cast<int>(r.font.size()), rate)) return {};
    r.engine = std::make_unique<SoundFontEngine>();
    r.engine->setSoundFontManager(r.manager.get());
    r.engine->prepare(rate, block);
    return r;
}

std::vector<float> renderEngineBlocks(SoundFontEngine& e, int blocks, int block,
                                      const std::function<void(int)>& before = {}) {
    std::vector<float> out(static_cast<size_t>(blocks) * block * 2, 0.0f);
    for (int b = 0; b < blocks; ++b) {
        if (before) before(b);
        e.render(out.data() + static_cast<size_t>(b) * block * 2, block);
    }
    return out;
}

/// Pico de |x[n] − x[n−1]| por canal sobre las MUESTRAS [from, to) (indices de frame).
double peakDerivative(const std::vector<float>& stereo, size_t fromFrame, size_t toFrame) {
    double m = 0.0;
    for (size_t f = std::max<size_t>(fromFrame, 1); f < toFrame; ++f) {
        for (size_t ch = 0; ch < 2; ++ch) {
            const double d = double(stereo[f * 2 + ch]) - double(stereo[(f - 1) * 2 + ch]);
            m = std::max(m, std::fabs(d));
        }
    }
    return m;
}

std::vector<float> subtract(const std::vector<float>& a, const std::vector<float>& b) {
    std::vector<float> out(a.size());
    for (size_t i = 0; i < a.size(); ++i) out[i] = a[i] - b[i];
    return out;
}

/**
 * AC-042.4 (la parte que la fachada no alcanza): `reset()` del engine —que pide limpiar la cola
 * en el proximo render— y `prepare()` con otro rate conservan la perilla. Se renderiza un bloque
 * despues de cada uno para que el camino que limpia (`clearSendEffects`) corra de verdad.
 */
TEST(SoundFontAmbienceEngine, ResetAndPrepareKeepTheKnob) {
    Rig r = makeRig(Font::Sends, kRate, kBlock);
    ASSERT_TRUE(r.engine);
    r.engine->setAmbience(0.3f, 0.6f);
    r.engine->noteOn(0, kNote, 1.0f);
    renderEngineBlocks(*r.engine, 4, kBlock);

    r.engine->reset();
    renderEngineBlocks(*r.engine, 2, kBlock);   // consume el reset: la cola se va, la perilla no
    EXPECT_EQ(r.engine->ambienceReverb(), 0.3f) << "reset() piso la perilla";
    EXPECT_EQ(r.engine->ambienceChorus(), 0.6f);

    r.engine->prepare(kOtherRate, kBlock);
    renderEngineBlocks(*r.engine, 2, kBlock);
    EXPECT_EQ(r.engine->ambienceReverb(), 0.3f) << "prepare() con otro rate piso la perilla";
    EXPECT_EQ(r.engine->ambienceChorus(), 0.6f);
}

/**
 * Decision 2, la unidad: lineal sobre la AMPLITUD del send. Las dos unidades son lineales (combs,
 * allpass y lineas de retardo moduladas), asi que con 0,5/0,5 el wet es exactamente la mitad del
 * wet con 1/1, muestra a muestra (a un ulp). Mutante: una perilla en dB, o al cuadrado.
 */
TEST(SoundFontAmbienceEngine, HalfScaleHalvesTheWetSampleForSample) {
    constexpr int blocks = kRate / kBlock;   // 1 s
    Rig dry = makeRig(Font::Dry, kRate, kBlock), full = makeRig(Font::Sends, kRate, kBlock), half = makeRig(Font::Sends, kRate, kBlock);
    ASSERT_TRUE(dry.engine && full.engine && half.engine);
    half.engine->setAmbience(0.5f, 0.5f);
    for (Rig* r : {&dry, &full, &half}) r->engine->noteOn(0, kNote, 1.0f);
    const auto d = renderEngineBlocks(*dry.engine, blocks, kBlock);
    const auto wetFull = subtract(renderEngineBlocks(*full.engine, blocks, kBlock), d);
    const auto wetHalf = subtract(renderEngineBlocks(*half.engine, blocks, kBlock), d);
    ASSERT_GT(peakAbs(wetFull), 1e-2) << "no hay wet que medir";

    double worst = 0.0;
    for (size_t i = 0; i < wetFull.size(); ++i) worst = std::max(worst, std::fabs(double(wetHalf[i]) - 0.5 * double(wetFull[i])));
    std::printf("  [REQ-042] pico del wet con 1/1: %.4f; |wet(0,5) - 0,5 * wet(1)| max %.2e\n", peakAbs(wetFull), worst);
    EXPECT_LT(worst, 1e-6) << "con 0,5 el wet no es la mitad: la perilla no es lineal sobre el send";
}

/**
 * AC-042.7 — el escalon 0/0 → 1/1 con una nota sostenida, MEDIDO antes de decidir rampa
 * (decision 5). El umbral es nuestro: el pico de la derivada del wet en el bloque de conmutacion
 * no supera el pico de la derivada del wet en regimen (el ultimo segundo con 1/1).
 *
 * El bloque es de 2048 frames (43 ms a 48 kHz) a proposito: mas largo que el retardo del chorus
 * (16 ms) y que el comb mas corto del freeverb (~25 ms), para que el arranque del wet CAIGA
 * ADENTRO del bloque de conmutacion. Con 256 el bloque seria trivialmente cero —las unidades no
 * devuelven nada antes de su primer retardo— y la guarda no guardaria nada.
 *
 * Se imprime ademas el pico sobre los 100 ms que siguen (la subida entera), como dato.
 *
 * Mutante que este test mata: el escalador aplicado DESPUES de las unidades. Con 0/0 las unidades
 * seguirian alimentadas y en regimen, y al pasar a 1/1 el wet apareceria ENTERO en la primera
 * muestra: una derivada del tamano del wet, no de su pendiente.
 */
TEST(SoundFontAmbienceEngine, TheStepFromZeroToOneIsMeasuredAgainstSteadyStateSlope) {
    constexpr int block = 2048;
    constexpr int blocksBefore = kRate / block;          // ~1 s con 0/0
    constexpr int blocksAfter = 2 * kRate / block;       // ~2 s con 1/1
    constexpr int total = blocksBefore + blocksAfter;
    Rig dry = makeRig(Font::Dry, kRate, block), wet = makeRig(Font::Sends, kRate, block);
    ASSERT_TRUE(dry.engine && wet.engine);
    wet.engine->setAmbience(0.0f, 0.0f);
    dry.engine->noteOn(0, kNote, 1.0f);
    wet.engine->noteOn(0, kNote, 1.0f);
    const auto d = renderEngineBlocks(*dry.engine, total, block);
    const auto w = subtract(renderEngineBlocks(*wet.engine, total, block, [&](int b) {
        if (b == blocksBefore) wet.engine->setAmbience(1.0f, 1.0f);
    }), d);

    const size_t switchFrame = static_cast<size_t>(blocksBefore) * block;
    const size_t frames = static_cast<size_t>(total) * block;
    // Con 0/0 desde el arranque las unidades nunca se engancharon: cero EXACTO antes del escalon.
    double beforePeak = 0.0;
    for (size_t i = 0; i < switchFrame * 2; ++i) beforePeak = std::max(beforePeak, std::fabs(double(w[i])));
    ASSERT_EQ(beforePeak, 0.0) << "con 0/0 hay wet antes del escalon";

    const double atSwitch = peakDerivative(w, switchFrame, switchFrame + block);
    const double buildUp = peakDerivative(w, switchFrame, switchFrame + static_cast<size_t>(kRate) / 10);
    const double steady = peakDerivative(w, frames - static_cast<size_t>(kRate), frames);
    const double wetPeak = peakAbs(std::vector<float>(w.begin() + static_cast<long>((frames - kRate) * 2), w.end()));
    std::printf("  [REQ-042] escalon 0/0 -> 1/1 (bloque %d a %d Hz): derivada pico en el bloque de conmutacion %.3e; "
                "en los 100 ms siguientes %.3e; en regimen %.3e (pico del wet en regimen %.4f); razon bloque/regimen %.3f\n",
                block, kRate, atSwitch, buildUp, steady, wetPeak, atSwitch / steady);
    ASSERT_GT(steady, 0.0) << "sin wet en regimen no hay umbral";
    EXPECT_LE(atSwitch, steady) << "el escalon supera la pendiente de regimen: decision 5 pide rampa";
}

}  // namespace
}  // namespace wma_test
