/**
 * test_output_profiles.cpp — los perfiles de salida del motor (REQ-055 S12, W3): PROTECTED (la
 * cadena de 2.23.0), MASTER (MasterStage + volumen de monitoreo) y RAW (nada, solo offline).
 */

#include "support/CApiFixture.h"
#include "support/OutputGoldenScenario.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

namespace wma_test {
namespace {

constexpr int kRate = 48000;
constexpr int kBlock = 256;

/// Un motor offline con un instrumento que suena fuerte (hace trabajar al limitador).
WmaEngine* soundingEngine(float amplitude, float masterVolume) {
    WmaEngine* e = wma_engine_create();
    wma_set_audio_mode(e, 0);
    wma_set_master_volume(e, masterVolume);
    EXPECT_EQ(wma_offline_start(e, kRate, kBlock), WMA_OK);
    e->engine->setOscillatorEnabled(true);
    e->engine->setFrequencyAndAmplitude(220.0f, amplitude);
    return e;
}

std::vector<float> renderBlocks(WmaEngine* e, int blocks) {
    std::vector<float> out;
    std::vector<float> block(kBlock * 2);
    for (int b = 0; b < blocks; ++b) {
        EXPECT_EQ(wma_offline_render(e, block.data(), nullptr, kBlock), WMA_OK);
        out.insert(out.end(), block.begin(), block.end());
    }
    return out;
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

float peakOf(const std::vector<float>& v) {
    float p = 0.0f;
    for (float x : v) p = std::max(p, std::abs(x));
    return p;
}

// FNV-1a de la salida de 2.23.0, capturado compilando OutputGoldenScenario.h contra 96ca8d61
// (v2.23.0) ANTES de tocar applyMasterAndProtectOutput. Debug, macOS arm64: el seno y el tanh de
// otra libm (glibc) difieren en el ultimo bit, asi que en otras plataformas solo vale la prueba
// estructural de abajo.
constexpr uint64_t kGolden223Hash = 0x45a7f1332cc0808bULL;

}  // namespace

// Defecto: el perfil por defecto cambia lo que sale por el DAC de Android/iOS. Con PROTECTED la
// salida son los MISMOS bits que daba 2.23.0 (la cadena entera: volumen maestro con su rampa,
// limitador con lookahead, soft clip, dither y hard limit).
TEST(OutputProfiles, ProtectedIsTheTwoTwentyThreeOutputBitForBit) {
#if defined(__APPLE__) && defined(__aarch64__)
    AudioEngine engine;
    ASSERT_TRUE(engine.startOffline(kGoldenRate, kGoldenBlock));
    const std::vector<float> out = renderOutputGoldenScenario(engine);
    ASSERT_GT(peakOf(out), 0.5f) << "el escenario no suena";
    EXPECT_EQ(fnv1aBits(out), kGolden223Hash);
#else
    GTEST_SKIP() << "golden de 2.23.0 capturado en macOS arm64; la prueba estructural lo cubre";
#endif
}

// Defecto (portable): el perfil PROTECTED ya no es "el volumen maestro y luego OutputStage": se
// cuela una etapa antes o despues, o el volumen cambia de lugar. La salida PROTECTED tiene que ser
// la de un OutputStage de siempre aplicado al volumen maestro sobre la salida RAW del mismo bloque.
TEST(OutputProfiles, ProtectedEqualsTheOldChainAppliedToTheRawMix) {
    constexpr int kBlocks = 40;
    WmaEngine* raw = soundingEngine(1.0f, 1.0f);
    ASSERT_EQ(wma_mix_set_output_profile(raw, WMA_OUTPUT_RAW), WMA_OK);
    const std::vector<float> premix = renderBlocks(raw, kBlocks);
    WmaEngine* prot = soundingEngine(1.0f, 1.0f);
    const std::vector<float> protectedOut = renderBlocks(prot, kBlocks);
    ASSERT_GT(peakOf(premix), 0.5f);

    OutputStage reference;
    reference.prepare(kRate, 0);
    reference.reset();
    std::vector<float> expected = premix;
    for (int b = 0; b < kBlocks; ++b) {
        reference.processOutput(expected.data() + static_cast<size_t>(b) * kBlock * 2, kBlock);
    }
    EXPECT_TRUE(sameBits(protectedOut, expected));
    EXPECT_FALSE(sameBits(protectedOut, premix)) << "PROTECTED no hizo nada";
    wma_engine_destroy(raw);
    wma_engine_destroy(prot);
}

// Defecto: RAW sale float sin proteger a un DAC. En un motor con device (o que no esta en un render
// offline) el perfil RAW se rechaza y el motor sigue protegido.
TEST_F(CApiFixture, RawIsRefusedOnAnEngineThatIsNotOffline) {
    EXPECT_EQ(wma_mix_set_output_profile(mWma, WMA_OUTPUT_RAW), WMA_ERROR_INVALID_OPERATION);
    EXPECT_EQ(mWma->engine->getOutputProfile(), AudioEngine::OutputProfile::Protected);
    startAt(48000, 0);
    EXPECT_EQ(wma_mix_set_output_profile(mWma, WMA_OUTPUT_RAW), WMA_ERROR_INVALID_OPERATION)
        << "un motor con device acepto RAW";
    EXPECT_EQ(mWma->engine->getOutputProfile(), AudioEngine::OutputProfile::Protected);
    EXPECT_EQ(wma_mix_set_output_profile(mWma, WMA_OUTPUT_MASTER), WMA_OK);
    EXPECT_EQ(wma_mix_set_output_profile(mWma, static_cast<WmaOutputProfile>(3)),   // 3 cabe en el rango del enum
              WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_EQ(wma_mix_set_output_profile(nullptr, WMA_OUTPUT_MASTER), WMA_ERROR_NOT_INITIALIZED);
}

// Defecto: una instancia que dejo de ser offline conserva RAW y el siguiente arranque con device
// saca float crudo. Parar (o reiniciar) vuelve a PROTECTED.
TEST(OutputProfiles, AnInstanceThatStopsBeingOfflineGoesBackToProtected) {
    WmaEngine* e = soundingEngine(0.5f, 1.0f);
    ASSERT_EQ(wma_mix_set_output_profile(e, WMA_OUTPUT_RAW), WMA_OK);
    EXPECT_EQ(e->engine->getOutputProfile(), AudioEngine::OutputProfile::Raw);
    ASSERT_EQ(wma_offline_stop(e), WMA_OK);
    EXPECT_EQ(e->engine->getOutputProfile(), AudioEngine::OutputProfile::Protected);
    ASSERT_EQ(wma_mix_set_output_profile(e, WMA_OUTPUT_RAW), WMA_ERROR_INVALID_OPERATION);
    wma_engine_destroy(e);
}

// Defecto: el perfil MASTER del motor y el procesador independiente (wma_master_*) divergen: el
// export mediria otro limitador que el que se escucha. Con la misma entrada son IDENTICOS, y el
// volumen de monitoreo va DESPUES del master.
TEST(OutputProfiles, MasterProfileEqualsTheStandaloneMasterOnTheSameInput) {
    constexpr int kBlocks = 40;
    const WmaMixMaster params = {6.0f, -3.0f, 100.0f};
    constexpr float kMonitor = 0.5f;

    WmaEngine* raw = soundingEngine(0.9f, 1.0f);
    ASSERT_EQ(wma_mix_set_output_profile(raw, WMA_OUTPUT_RAW), WMA_OK);
    const std::vector<float> premix = renderBlocks(raw, kBlocks);
    ASSERT_GT(peakOf(premix), 0.5f);

    // el master configurado ANTES del start: el reset del start salta a esa ganancia (sin rampa)
    WmaEngine* live = wma_engine_create();
    wma_set_audio_mode(live, 0);
    wma_set_master_volume(live, kMonitor);
    ASSERT_EQ(wma_mix_set_master(live, &params), WMA_OK);
    ASSERT_EQ(wma_offline_start(live, kRate, kBlock), WMA_OK);
    live->engine->setOscillatorEnabled(true);
    live->engine->setFrequencyAndAmplitude(220.0f, 0.9f);
    ASSERT_EQ(wma_mix_set_output_profile(live, WMA_OUTPUT_MASTER), WMA_OK);
    const std::vector<float> masterOut = renderBlocks(live, kBlocks);

    WmaMaster* standalone = wma_master_create(kRate);
    ASSERT_NE(standalone, nullptr);
    ASSERT_EQ(wma_master_configure(standalone, &params), WMA_OK);
    ASSERT_EQ(wma_master_reset(standalone), WMA_OK);
    std::vector<float> expected = premix;
    for (int b = 0; b < kBlocks; ++b) {
        ASSERT_EQ(wma_master_process(standalone, expected.data() + static_cast<size_t>(b) * kBlock * 2,
                                     kBlock), WMA_OK);
    }
    for (float& v : expected) v *= kMonitor;
    EXPECT_TRUE(sameBits(masterOut, expected));
    // y el master hizo algo: sin el limitador la salida pasaria el techo de -3 dBTP
    EXPECT_LE(peakOf(masterOut), std::pow(10.0f, -3.0f / 20.0f) * kMonitor + 1e-6f);
    EXPECT_GT(peakOf(masterOut), 0.1f);

    wma_master_destroy(standalone);
    wma_engine_destroy(raw);
    wma_engine_destroy(live);
}

// Defecto: la latencia declarada no es la que el master agrega (el export alinearia el master
// con los stems corrido). Bajo el umbral el MASTER es la entrada RAW demorada la latencia
// declarada, bit a bit.
TEST(OutputProfiles, TheDeclaredMasterLatencyIsTheMeasuredDelayOfTheLiveMaster) {
    constexpr int kBlocks = 20;
    WmaEngine* raw = soundingEngine(0.3f, 1.0f);
    ASSERT_EQ(wma_mix_set_output_profile(raw, WMA_OUTPUT_RAW), WMA_OK);
    const std::vector<float> premix = renderBlocks(raw, kBlocks);
    WmaEngine* live = soundingEngine(0.3f, 1.0f);
    ASSERT_EQ(wma_mix_set_output_profile(live, WMA_OUTPUT_MASTER), WMA_OK);
    const std::vector<float> masterOut = renderBlocks(live, kBlocks);
    const int d = wma_mix_get_master_latency_frames(live);
    ASSERT_EQ(d, 240);
    ASSERT_GT(peakOf(premix), 0.1f);
    for (size_t n = static_cast<size_t>(d); n < premix.size() / 2; ++n) {
        const size_t j = n - static_cast<size_t>(d);
        ASSERT_EQ(std::memcmp(&masterOut[n * 2], &premix[j * 2], 2 * sizeof(float)), 0) << "frame " << n;
    }
    EXPECT_EQ(wma_mix_get_master_latency_frames(nullptr), 0);
    wma_engine_destroy(raw);
    wma_engine_destroy(live);
}

// Defecto: los medidores de salida quedan mudos con MASTER o RAW (el medidor de nivel de la UI se
// congela al cambiar de perfil). Estan en la cola de los tres.
TEST(OutputProfiles, TheOutputMetersAreAtTheTailOfAllThreeProfiles) {
    for (WmaOutputProfile profile : {WMA_OUTPUT_PROTECTED, WMA_OUTPUT_MASTER, WMA_OUTPUT_RAW}) {
        WmaEngine* e = soundingEngine(0.5f, 1.0f);
        ASSERT_EQ(wma_mix_set_output_profile(e, profile), WMA_OK);
        const std::vector<float> out = renderBlocks(e, 30);
        ASSERT_GT(peakOf(out), 0.05f) << "perfil " << profile;
        EXPECT_GT(e->engine->getOutputPeakLevel(0), 0.05f) << "perfil " << profile;
        EXPECT_NEAR(e->engine->getOutputPeakLevel(0), peakOf(std::vector<float>(out.end() - kBlock * 2, out.end())), 0.2f)
            << "perfil " << profile;
        wma_engine_destroy(e);
    }
}

}  // namespace wma_test
