/**
 * REQ-055 S15 — la configuracion del RE-RENDER de una toma, por la API C que expone el JNI
 * `nativeOffline*`: oscilador, modulador, rango de frecuencia, BPM, bypass por efecto, ruteo,
 * mezcla paralela y feedback sobre una instancia offline.
 *
 * Dos contratos:
 *  1. DETERMINISMO. Dos instancias frescas con la misma secuencia (engine, oscilador, rango, bpm,
 *     cadena con un efecto en bypass, ruteo, `wma_set_frequency_amplitude` en bloques de 128, el
 *     bloque fijo del re-render) dan el MISMO buffer bit a bit, y cada setter CAMBIA el audio (un
 *     setter que no llega al motor seria un re-render que suena igual con cualquier toma).
 *  2. AISLAMIENTO. Nada de eso toca al motor vivo: ni su estado observable ni su salida.
 */

#include "support/FakeAudioBackend.h"

#include "api/watermelon_audio.h"
#include "api/watermelon_audio_internal.h"
#include "backends/BackendManager.h"
#include "platform/Logger.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace {

constexpr int kRate = 48000;
constexpr int kBlock = 128;          // el bloque fijo del re-render
constexpr int kBlocks = 160;         // ~0,43 s
constexpr float kAudible = 0.005f;

constexpr int kDelay = 2;
constexpr int kChorus = 6;
constexpr int kHallReverb = 17;
constexpr int kFm = 2;
constexpr int kRoutingParallel = 1;

/// La secuencia del re-render, en el orden documentado en `IOfflineRenderer`. Cada campo es una
/// variante para el control negativo: el valor por defecto de todos es el "baseline".
struct Rerender {
    int engine = kFm;
    int oscillator = 2;            // sierra
    float minHz = 110.0f;
    float maxHz = 1760.0f;
    float bpm = 90.0f;
    bool bypassDelay = true;
    int routing = kRoutingParallel;
    float parallelMix = 0.35f;
    float feedback = 0.4f;
    int modulator = 0;
};


void configure(WmaEngine* e, const Rerender& r) {
    ASSERT_EQ(wma_offline_start(e, kRate, kBlock), WMA_OK);
    wma_set_audio_mode(e, 0);
    wma_set_engine_type(e, r.engine);
    wma_set_oscillator_type(e, r.oscillator);
    ASSERT_EQ(wma_set_modulator_type(e, r.modulator), WMA_OK);
    wma_set_frequency_range(e, r.minHz, r.maxHz);
    wma_set_bpm(e, r.bpm);
    ASSERT_EQ(wma_effect_clear_all(e), WMA_OK);
    for (int fx : {kChorus, kDelay, kHallReverb}) ASSERT_GE(wma_effect_add(e, fx), 0);
    ASSERT_EQ(wma_effect_set_bypass(e, 1, r.bypassDelay), WMA_OK);
    wma_set_routing_mode(e, r.routing);
    wma_set_parallel_mix(e, r.parallelMix);
    wma_set_feedback_amount(e, r.feedback);
}

std::vector<float> render(const Rerender& r, bool useXY = false) {
    WmaEngine* e = wma_engine_create();
    EXPECT_NE(e, nullptr);
    if (!e) return {};
    configure(e, r);
    std::vector<float> out(static_cast<size_t>(kBlocks) * kBlock * 2, 0.0f);
    for (int b = 0; b < kBlocks; ++b) {
        if (b % 8 == 0) {
            const float amp = b < kBlocks * 3 / 4 ? 0.6f : 0.0f;
            if (useXY) {
                wma_set_xy(e, 0.1f * static_cast<float>(1 + (b / 8) % 8), amp);
            } else {
                wma_set_frequency_amplitude(e, 196.0f + 22.0f * static_cast<float>((b / 8) % 5), amp);
            }
        }
        EXPECT_EQ(wma_offline_render(e, out.data() + static_cast<size_t>(b) * kBlock * 2, nullptr, kBlock), WMA_OK);
    }
    EXPECT_EQ(wma_offline_stop(e), WMA_OK);
    wma_engine_destroy(e);
    return out;
}

float peakOf(const std::vector<float>& x) {
    float p = 0.0f;
    for (float v : x) p = std::max(p, std::abs(v));
    return p;
}

bool bitEqual(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

class OfflineRerender : public ::testing::Test {
protected:
    void SetUp() override { wma::setLogCallback([](wma::LogLevel, const char*, const char*) {}); }
    void TearDown() override { wma::setLogCallback(nullptr); }
};

}  // namespace

// Bug plausible: un setter nuevo deja estado sin sembrar (un reloj, un global) o el reseteo del
// start no lo cubre: dos re-renders de la misma toma darian audio distinto y el A/B del usuario
// oiria una diferencia que no es de la toma.
TEST_F(OfflineRerender, AC_055_15_TwoFreshInstancesWithTheSameSequenceRenderTheSameBufferBitForBit) {
    const auto a = render(Rerender{});
    const auto b = render(Rerender{});
    ASSERT_GT(peakOf(a), kAudible) << "el render salio en silencio; no compararia nada";
    EXPECT_TRUE(bitEqual(a, b));
    const auto viaXy1 = render(Rerender{}, true);
    const auto viaXy2 = render(Rerender{}, true);
    ASSERT_GT(peakOf(viaXy1), kAudible);
    EXPECT_TRUE(bitEqual(viaXy1, viaXy2)) << "por wma_set_xy";
}

// Bug plausible: la instancia reiniciada hereda algo del render anterior que el start no resetea
// (el bypass, el ruteo o la mezcla paralela): re-renderizar la misma toma dos veces en el mismo
// handle (lo que hace Studio al cambiar un parametro) daria otro audio que en uno fresco.
TEST_F(OfflineRerender, AC_055_15_ARestartedInstanceRendersLikeAFreshOne) {
    const auto fresh = render(Rerender{});
    WmaEngine* e = wma_engine_create();
    ASSERT_NE(e, nullptr);
    Rerender other;
    other.routing = 0;
    other.bypassDelay = false;
    other.engine = 0;
    configure(e, other);
    std::vector<float> scratch(static_cast<size_t>(kBlock) * 2);
    for (int i = 0; i < 40; ++i) ASSERT_EQ(wma_offline_render(e, scratch.data(), nullptr, kBlock), WMA_OK);

    configure(e, Rerender{});
    std::vector<float> out(static_cast<size_t>(kBlocks) * kBlock * 2, 0.0f);
    for (int b = 0; b < kBlocks; ++b) {
        if (b % 8 == 0) {
            const float amp = b < kBlocks * 3 / 4 ? 0.6f : 0.0f;
            wma_set_frequency_amplitude(e, 196.0f + 22.0f * static_cast<float>((b / 8) % 5), amp);
        }
        ASSERT_EQ(wma_offline_render(e, out.data() + static_cast<size_t>(b) * kBlock * 2, nullptr, kBlock), WMA_OK);
    }
    wma_engine_destroy(e);
    EXPECT_TRUE(bitEqual(fresh, out));
}

// Bug plausible: un setter pasamanos apunta a otro campo o no llega al motor: el re-render suena
// igual con cualquier configuracion. Cada variante tiene que cambiar el buffer respecto del baseline.
TEST_F(OfflineRerender, AC_055_15_EachSetterChangesTheRenderedAudio) {
    const auto base = render(Rerender{});
    ASSERT_GT(peakOf(base), kAudible);
    {
        // FM no usa el oscilador clasico: el oscilador se mide con el engine CLASSIC
        Rerender classicA; classicA.engine = 0;
        Rerender classicB = classicA; classicB.oscillator = 1;
        EXPECT_FALSE(bitEqual(render(classicA), render(classicB))) << "oscillator type";
    }
    {
        Rerender r; r.bypassDelay = false;
        EXPECT_FALSE(bitEqual(base, render(r))) << "bypass de un efecto";
    }
    {
        Rerender r; r.routing = 0;
        EXPECT_FALSE(bitEqual(base, render(r))) << "routing mode";
    }
    {
        // la mezcla A/B la usa el ruteo SPLIT_2X2 (con 3 efectos: [0] contra [1,2])
        Rerender r; r.routing = 2; r.parallelMix = 0.2f;
        Rerender s = r; s.parallelMix = 0.9f;
        EXPECT_FALSE(bitEqual(render(r), render(s))) << "parallel mix (ruteo split 2x2)";
    }
    {
        Rerender r; r.routing = 5; r.feedback = 0.1f;
        Rerender s = r; s.feedback = 0.8f;
        EXPECT_FALSE(bitEqual(render(r), render(s))) << "feedback amount (ruteo feedback)";
    }
    {
        Rerender r; r.modulator = 2;   // AM
        EXPECT_FALSE(bitEqual(base, render(r))) << "modulator type";
    }
    // `setFrequencyRange` NO tiene un control negativo audible: el rango solo lo lee el mapeo XY del
    // OscillatorNode (`updateXY`/dual touch), que el render de la instancia no usa (el motor mapea
    // `wma_set_xy` con 50 + x * 1950 y los gestos del re-render entran por frecuencia directa). Se
    // fija igual por orden de uso y el determinismo de arriba lo cubre.
}

// Bug plausible: el BPM no llega a los efectos sincronizados (solo al Transport): el eco de la toma
// re-renderizada caeria en otro tiempo. Delay sincronizado: dos BPM (dentro del 60..200 del delay), dos colas.
TEST_F(OfflineRerender, AC_055_15_TheBpmReachesTheTempoSyncedEffects) {
    auto withBpm = [](float bpm) {
        WmaEngine* e = wma_engine_create();
        EXPECT_NE(e, nullptr);
        EXPECT_EQ(wma_offline_start(e, kRate, kBlock), WMA_OK);
        wma_set_audio_mode(e, 0);
        const int idx = wma_effect_add(e, kDelay);
        EXPECT_GE(idx, 0);
        EXPECT_EQ(wma_effect_set_param(e, idx, 2, 0.6f), WMA_OK);   // wet
        EXPECT_EQ(wma_effect_set_param(e, idx, 5, 1.0f), WMA_OK);   // sync al BPM global
        wma_set_bpm(e, bpm);
        std::vector<float> out(static_cast<size_t>(kBlocks) * kBlock * 2);
        for (int b = 0; b < kBlocks; ++b) {
            wma_set_frequency_amplitude(e, 220.0f, b < 20 ? 0.6f : 0.0f);
            EXPECT_EQ(wma_offline_render(e, out.data() + static_cast<size_t>(b) * kBlock * 2, nullptr, kBlock), WMA_OK);
        }
        EXPECT_EQ(wma_get_bpm(e), bpm);
        wma_engine_destroy(e);
        return out;
    };
    EXPECT_FALSE(bitEqual(withBpm(70.0f), withBpm(180.0f)));
}

// Bug plausible: un indice inexistente de bypass se descarta en silencio o rompe la cadena: el
// re-render creeria haber bypasseado un efecto que no esta.
TEST_F(OfflineRerender, AC_055_15_AnInvalidBypassIndexIsATypedErrorAndChangesNothing) {
    WmaEngine* e = wma_engine_create();
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(wma_effect_set_bypass(e, 0, true), WMA_ERROR_INVALID_EFFECT_INDEX) << "cadena vacia";
    ASSERT_GE(wma_effect_add(e, kDelay), 0);
    EXPECT_EQ(wma_effect_set_bypass(e, 1, true), WMA_ERROR_INVALID_EFFECT_INDEX);
    EXPECT_EQ(wma_effect_set_bypass(e, -1, true), WMA_ERROR_INVALID_EFFECT_INDEX);
    EXPECT_FALSE(wma_effect_is_bypassed(e, 0));
    EXPECT_EQ(wma_effect_set_bypass(e, 0, true), WMA_OK);
    EXPECT_TRUE(wma_effect_is_bypassed(e, 0));
    EXPECT_EQ(wma_set_modulator_type(e, 8), WMA_ERROR_INVALID_PARAMETER_ID);
    wma_engine_destroy(e);
}

namespace {

/// Un motor VIVO sobre el FakeAudioBackend, con callbacks bombeados desde el hilo del test.
struct LiveRig {
    WmaEngine* live = nullptr;
    wma_test::FakeAudioBackend* fake = nullptr;

    void bringUp() {
        wma_test::resetLastCreatedSystemBackend();
        live = wma_engine_create();
        ASSERT_NE(live, nullptr);
        fake = wma_test::lastCreatedSystemBackend();
        ASSERT_NE(fake, nullptr);
        fake->setNegotiatedSampleRate(kRate);
        wma_set_use_backend_manager(live, true);
        ASSERT_TRUE(wma_select_backend(1));
        wma_set_audio_mode(live, 0);
        wma_set_engine_type(live, 2);
        ASSERT_GE(wma_effect_add(live, kHallReverb), 0);
        wma_set_frequency_amplitude(live, 330.0f, 0.6f);
        ASSERT_EQ(wma_engine_start(live, 0), WMA_OK);
    }

    void tearDown() {
        if (!live) return;
        wma_engine_stop(live, 0);
        wma_engine_destroy(live);
        live = nullptr;
    }
};

constexpr int kLiveBlocks = 300;
constexpr int kLiveFrames = 256;

std::vector<float> liveOutput(bool withRerender, float* bpmSeen, int* routingSeen) {
    LiveRig rig;
    rig.bringUp();
    std::vector<float> out(static_cast<size_t>(kLiveBlocks) * kLiveFrames * 2, 0.0f);
    std::thread side;
    if (withRerender) {
        side = std::thread([] {
            // el re-render entero, con TODOS los setters nuevos, mientras el vivo suena
            for (int i = 0; i < 3; ++i) render(Rerender{});
        });
    }
    for (int b = 0; b < kLiveBlocks; ++b) {
        rig.fake->callback()->onAudioReady(out.data() + static_cast<size_t>(b) * kLiveFrames * 2, nullptr, kLiveFrames);
    }
    if (side.joinable()) side.join();
    if (bpmSeen) *bpmSeen = wma_get_bpm(rig.live);
    if (routingSeen) *routingSeen = wma_get_routing_mode(rig.live);
    rig.tearDown();
    return out;
}

}  // namespace

// Bug plausible: un setter offline cae sobre el motor vivo (por el BackendManager global o un
// estado compartido): el re-render en segundo plano cambiaria el BPM, el ruteo o el audio de lo
// que el usuario esta tocando.
TEST_F(OfflineRerender, AC_055_15_ARerenderDoesNotChangeTheLiveEngineNorItsOutput) {
    float bpmControl = 0.0f, bpmWith = 0.0f;
    int routingControl = -1, routingWith = -1;
    const auto control = liveOutput(false, &bpmControl, &routingControl);
    const auto with = liveOutput(true, &bpmWith, &routingWith);
    ASSERT_GT(peakOf(control), kAudible) << "el vivo de control no sono";
    EXPECT_EQ(bpmWith, bpmControl) << "el re-render movio el BPM del vivo";
    EXPECT_EQ(routingWith, routingControl) << "el re-render movio el ruteo del vivo";
    EXPECT_EQ(routingWith, 0);
    EXPECT_TRUE(bitEqual(control, with)) << "la salida del vivo cambio por un re-render a la par";
}
