/**
 * test_c_api_mix.cpp — la frontera C de la mezcla y el master (REQ-055 S12, W4): cada argumento
 * invalido tiene su codigo, y un valor invalido NO se aplica (nunca se acota en silencio).
 */

#include "support/CApiFixture.h"

#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace wma_test {
namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

WmaMixSpace goodSpace() {
    WmaMixSpace s{};
    s.reverb_return = 0.5f;
    s.reverb_size = 0.8f;
    s.reverb_decay_s = 2.0f;
    s.reverb_damping = 0.5f;
    s.reverb_predelay_ms = 20.0f;
    s.reverb_low_cut_hz = 80.0f;
    s.reverb_high_cut_hz = 12000.0f;
    s.delay_return = 0.3f;
    s.delay_time_ms = 250.0f;
    s.delay_feedback = 0.4f;
    return s;
}

class MixApi : public ::testing::Test {
protected:
    void SetUp() override {
        wma::setLogCallback([](wma::LogLevel, const char*, const char*) {});
        e = wma_engine_create();
        ASSERT_NE(e, nullptr);
    }
    void TearDown() override {
        wma_engine_destroy(e);
        wma::setLogCallback(nullptr);
    }
    WmaEngine* e = nullptr;
};

}  // namespace

// Defecto: un valor fuera de rango o no finito se acota o se aplica en silencio. Cada borde
// invalido da PARAMETER_OUT_OF_RANGE y el valor anterior queda; los bordes validos pasan.
TEST_F(MixApi, SetTrackRejectsEveryOutOfRangeValueAndAppliesNothing) {
    ASSERT_EQ(wma_mix_set_track(e, 0, 0.5f, 0.25f, 0.75f), WMA_OK);
    auto& looper = e->engine->getAudioLooper();
    struct Bad { int32_t track; float gain, pan, send; };
    const int active = looper.getMaxActiveTracks();
    const Bad bad[] = {
        {-1, 1, 0, 0},       {active, 1, 0, 0},  {1000, 1, 0, 0},
        {0, -0.01f, 0, 0},   {0, 2.01f, 0, 0},   {0, kNaN, 0, 0},   {0, kInf, 0, 0},
        {0, 1, -1.01f, 0},   {0, 1, 1.01f, 0},   {0, 1, kNaN, 0},   {0, 1, -kInf, 0},
        {0, 1, 0, -0.01f},   {0, 1, 0, 1.01f},   {0, 1, 0, kNaN},   {0, 1, 0, kInf},
    };
    for (const Bad& b : bad) {
        EXPECT_EQ(wma_mix_set_track(e, b.track, b.gain, b.pan, b.send), WMA_ERROR_PARAMETER_OUT_OF_RANGE)
            << "track " << b.track << " gain " << b.gain << " pan " << b.pan << " send " << b.send;
    }
    EXPECT_EQ(wma_mix_set_track(nullptr, 0, 1, 0, 0), WMA_ERROR_NOT_INITIALIZED);
    // los bordes validos
    EXPECT_EQ(wma_mix_set_track(e, active - 1, 2.0f, -1.0f, 1.0f), WMA_OK);
    EXPECT_EQ(wma_mix_set_track(e, 0, 0.0f, 1.0f, 0.0f), WMA_OK);
    // el rechazo no toco el estado: el ultimo valor valido es el que quedo
    ASSERT_EQ(wma_mix_set_track(e, 0, 0.5f, 0.25f, 0.75f), WMA_OK);
    EXPECT_EQ(wma_mix_set_track(e, 0, 5.0f, 0.0f, 0.0f), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_FLOAT_EQ(looper.getSongMixTrackGain(0), 0.5f) << "un valor rechazado se aplico";
}

// Defecto: el espacio se aplica a medias (un campo invalido deja pasar los otros). Los diez campos
// se validan juntos y un NULL no revienta.
TEST_F(MixApi, SetSpaceValidatesAllTenFieldsAndRejectsNull) {
    WmaMixSpace s = goodSpace();
    EXPECT_EQ(wma_mix_set_space(e, &s), WMA_OK);
    EXPECT_EQ(wma_mix_set_space(e, nullptr), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_EQ(wma_mix_set_space(nullptr, &s), WMA_ERROR_NOT_INITIALIZED);
    struct Field { float WmaMixSpace::*member; float low, high; };
    const Field fields[] = {
        {&WmaMixSpace::reverb_return, 0.0f, 1.0f},
        {&WmaMixSpace::reverb_size, 0.1f, 2.0f},
        {&WmaMixSpace::reverb_decay_s, 0.1f, 10.0f},
        {&WmaMixSpace::reverb_damping, 0.0f, 0.95f},
        {&WmaMixSpace::reverb_predelay_ms, 0.0f, 100.0f},
        {&WmaMixSpace::reverb_low_cut_hz, 20.0f, 500.0f},
        {&WmaMixSpace::reverb_high_cut_hz, 1000.0f, 20000.0f},
        {&WmaMixSpace::delay_return, 0.0f, 1.0f},
        {&WmaMixSpace::delay_time_ms, 1.0f, 2000.0f},
        {&WmaMixSpace::delay_feedback, 0.0f, 0.9f},
    };
    for (const Field& f : fields) {
        for (float v : {f.low, f.high}) {
            WmaMixSpace ok = goodSpace();
            ok.*(f.member) = v;
            EXPECT_EQ(wma_mix_set_space(e, &ok), WMA_OK) << "borde valido " << v;
        }
        // un paso claramente afuera (un denormal pegado al borde se aplana a 0 con el flush de la FPU)
        const float step = 0.01f * std::max(std::abs(f.low), std::abs(f.high));
        for (float v : {f.low - step, f.high + step, kNaN, kInf, -kInf}) {
            WmaMixSpace bad = goodSpace();
            bad.*(f.member) = v;
            EXPECT_EQ(wma_mix_set_space(e, &bad), WMA_ERROR_PARAMETER_OUT_OF_RANGE) << "valor " << v;
        }
    }
}

// Defecto (hallazgo 5 del review): el delay acepta 1..2000 ms a cualquier sample rate pero su linea
// tiene un largo fijo en muestras (192000): a 192 kHz el maximo real es 1000 ms y lo demas se
// acotaba en silencio. Fuera de la linea al rate vigente => PARAMETER_OUT_OF_RANGE y no se aplica.
TEST_F(MixApi, TheDelayTimeIsRejectedWhenItDoesNotFitTheLineAtTheCurrentRate) {
    WmaMixSpace s = goodSpace();
    for (const auto& c : {std::pair<int, float>{48000, 2000.0f}, {96000, 2000.0f}, {192000, 1000.0f}}) {
        ASSERT_EQ(wma_offline_start(e, c.first, 256), WMA_OK);
        s.delay_time_ms = c.second;
        EXPECT_EQ(wma_mix_set_space(e, &s), WMA_OK) << c.first << " Hz, " << c.second << " ms";
    }
    // a 192 kHz (el rate vigente) 1000 ms cabe y 1100 no; a 96 kHz 2000 ms cabe aun
    s.delay_time_ms = 1100.0f;
    EXPECT_EQ(wma_mix_set_space(e, &s), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    s.delay_time_ms = 2000.0f;
    EXPECT_EQ(wma_mix_set_space(e, &s), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    ASSERT_EQ(wma_offline_start(e, 96000, 256), WMA_OK);
    EXPECT_EQ(wma_mix_set_space(e, &s), WMA_OK);
}

TEST_F(MixApi, SetMasterAndProfileAndLatencyErrors) {
    const WmaMixMaster good = {0.0f, -1.0f, 150.0f};
    EXPECT_EQ(wma_mix_set_master(e, &good), WMA_OK);
    EXPECT_EQ(wma_mix_set_master(e, nullptr), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_EQ(wma_mix_set_master(nullptr, &good), WMA_ERROR_NOT_INITIALIZED);
    const WmaMixMaster bad[] = {
        {-60.1f, -1, 150}, {40.1f, -1, 150}, {kNaN, -1, 150}, {0, -20.1f, 150}, {0, 0.1f, 150},
        {0, kNaN, 150},    {0, -1, 0.9f},    {0, -1, 2000.1f}, {0, -1, kNaN},    {0, -1, kInf},
    };
    for (const WmaMixMaster& m : bad) {
        EXPECT_EQ(wma_mix_set_master(e, &m), WMA_ERROR_PARAMETER_OUT_OF_RANGE)
            << m.gain_db << " " << m.ceiling_dbtp << " " << m.release_ms;
    }
    const WmaMixMaster edges[] = {{-60, -20, 1}, {40, 0, 2000}};
    for (const WmaMixMaster& m : edges) EXPECT_EQ(wma_mix_set_master(e, &m), WMA_OK);

    EXPECT_EQ(wma_mix_get_master_latency_frames(nullptr), 0);
    EXPECT_EQ(wma_mix_get_master_latency_frames(e), 240) << "5 ms a 48 kHz";
    EXPECT_EQ(wma_get_xrun_count(nullptr), 0);
    EXPECT_EQ(wma_get_xrun_count(e), 0);
}

// Defecto: el render con taps acepta lo que no debe (escribe en un buffer NULL, una pista que no
// existe, mas frames que el bloque declarado) o deja pasar un render sin start.
TEST_F(MixApi, RenderTapsValidatesItsArguments) {
    std::vector<float> mix(256 * 2), fx(256 * 2), tracks(256 * 2 * 8);
    EXPECT_EQ(wma_offline_render_taps(nullptr, mix.data(), fx.data(), tracks.data(), 1, 64),
              WMA_ERROR_NOT_INITIALIZED);
    EXPECT_EQ(wma_offline_render_taps(e, mix.data(), fx.data(), tracks.data(), 1, 64),
              WMA_ERROR_INVALID_OPERATION) << "sin wma_offline_start";
    ASSERT_EQ(wma_offline_start(e, 48000, 256), WMA_OK);
    const int active = e->engine->getAudioLooper().getMaxActiveTracks();
    EXPECT_EQ(wma_offline_render_taps(e, nullptr, fx.data(), tracks.data(), 1, 64), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_EQ(wma_offline_render_taps(e, mix.data(), nullptr, tracks.data(), 1, 64), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_EQ(wma_offline_render_taps(e, mix.data(), fx.data(), nullptr, 1, 64), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_EQ(wma_offline_render_taps(e, mix.data(), fx.data(), tracks.data(), -1, 64), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_EQ(wma_offline_render_taps(e, mix.data(), fx.data(), tracks.data(), active + 1, 64),
              WMA_ERROR_PARAMETER_OUT_OF_RANGE) << "track_count > pistas activas";
    EXPECT_EQ(wma_offline_render_taps(e, mix.data(), fx.data(), tracks.data(), 1, 0), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_EQ(wma_offline_render_taps(e, mix.data(), fx.data(), tracks.data(), 1, 257), WMA_ERROR_PARAMETER_OUT_OF_RANGE)
        << "frames > max_block_frames";
    // sin pistas, tracks puede ser NULL
    EXPECT_EQ(wma_offline_render_taps(e, mix.data(), fx.data(), nullptr, 0, 64), WMA_OK);
    EXPECT_EQ(wma_offline_render_taps(e, mix.data(), fx.data(), tracks.data(), active, 256), WMA_OK);
}

TEST(MasterApi, CreateConfigureProcessAndDestroyErrors) {
    EXPECT_EQ(wma_master_create(7999), nullptr);
    EXPECT_EQ(wma_master_create(192001), nullptr);
    EXPECT_EQ(wma_master_create(0), nullptr);
    EXPECT_EQ(wma_master_create(-48000), nullptr);
    WmaMaster* m = wma_master_create(48000);
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(wma_master_get_latency_frames(m), 240);
    EXPECT_EQ(wma_master_get_latency_frames(nullptr), 0);
    const WmaMixMaster good = {0.0f, -1.0f, 150.0f};
    EXPECT_EQ(wma_master_configure(m, &good), WMA_OK);
    EXPECT_EQ(wma_master_configure(nullptr, &good), WMA_ERROR_NOT_INITIALIZED);
    EXPECT_EQ(wma_master_configure(m, nullptr), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    const WmaMixMaster bad = {kNaN, -1.0f, 150.0f};
    EXPECT_EQ(wma_master_configure(m, &bad), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_EQ(wma_master_reset(m), WMA_OK);
    EXPECT_EQ(wma_master_reset(nullptr), WMA_ERROR_NOT_INITIALIZED);
    std::vector<float> buf(64 * 2, 0.1f);
    EXPECT_EQ(wma_master_process(m, buf.data(), 64), WMA_OK);
    EXPECT_EQ(wma_master_process(nullptr, buf.data(), 64), WMA_ERROR_NOT_INITIALIZED);
    EXPECT_EQ(wma_master_process(m, nullptr, 64), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_EQ(wma_master_process(m, buf.data(), 0), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_EQ(wma_master_process(m, buf.data(), -5), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_EQ(wma_master_process(m, buf.data(), WMA_MASTER_MAX_FRAMES + 1), WMA_ERROR_PARAMETER_OUT_OF_RANGE)
        << "frames sobre el maximo: no puede tocar memoria que el llamador no dimensiono";
    wma_master_destroy(m);
    wma_master_destroy(nullptr);   // no-op
}

}  // namespace wma_test
