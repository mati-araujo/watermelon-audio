/**
 * test_c_api_song.cpp — la cancion programada por la C API (REQ-055 S11), con el motor entero.
 *
 * Aca se oye lo que oiria Studio: el motor offline (`wma_offline_*`) o el vivo sobre el
 * FakeAudioBackend, con la cadena de salida real. A nivel looper (envolvente analitica, SongVoice
 * contra mixInto, validaciones) mira `looper/tests/test_song_player.cpp`.
 *
 * AC-055.12: 5 secciones renderizadas offline, sin discontinuidades en las uniones.
 * AC-055.13: la cancion sigue sonando mientras se reprograma (0 alloc en RT, sin desgarro).
 *
 * El motor de la salida retrasa la senal 240 frames (limitador con lookahead de 5 ms), asi que las
 * uniones se buscan en `borde - P0 + kOutputDelay`.
 */

#include "support/CApiFixture.h"
#include "support/RenderChecks.h"
#include "support/RtAllocCounter.h"
#include "tests/support/TestSanitizer.h"
#include "tests/support/TestWait.h"

#include "looper/WavFile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

namespace wma_test {
namespace {

constexpr int kRate = 48000;
constexpr int kLoop = 4800;           // 100 ms
constexpr int kOutputDelay = 240;     // lookahead del limitador de salida
constexpr int kBlock = 512;

std::filesystem::path songFixture(const std::string& name) {
    return std::filesystem::temp_directory_path() /
           ("wm" + std::to_string(static_cast<long long>(getpid())) + "_csong_" + name + ".wav");
}

/// Un clip de `kLoop` frames: un seno de `period` frames (entero de periodos en el loop, o sea un
/// loop sin costura, como una toma) con fase inicial `phase`.
std::string writeSine(const std::string& name, float period, float phase, float amp = 0.8f,
                      int frames = kLoop) {
    std::vector<float> buf(static_cast<size_t>(frames) * 2);
    for (int n = 0; n < frames; ++n) {
        const float v = amp * std::sin(6.2831853f * static_cast<float>(n) / period + phase);
        buf[static_cast<size_t>(n) * 2] = v;
        buf[static_cast<size_t>(n) * 2 + 1] = v;
    }
    const auto path = songFixture(name);
    EXPECT_TRUE(wav::writeWav(path.c_str(), buf.data(), frames, kRate, wav::BitDepth::FLOAT_32));
    return path.string();
}

int32_t loadClip(WmaEngine* e, const std::string& path, int64_t loop = kLoop) {
    int32_t id = -1;
    EXPECT_EQ(wma_song_load_clip(e, path.c_str(), kRate, 0, 0, loop, &id), WMA_OK);
    return id;
}

WmaSongSection section(int64_t start, int32_t clip0, int32_t clip1 = WMA_SONG_SILENCE) {
    WmaSongSection s{};
    s.start_frame = start;
    for (int t = 0; t < WMA_SONG_MAX_TRACKS; ++t) s.clip[t] = WMA_SONG_SILENCE;
    s.clip[0] = clip0;
    s.clip[1] = clip1;
    return s;
}

/// Un motor sin instrumento: lo unico que se oye es lo que ponga la cancion.
WmaEngine* quietEngine() {
    WmaEngine* e = wma_engine_create();
    if (e == nullptr) return nullptr;
    wma_set_audio_mode(e, 0);
    wma_set_master_volume(e, 1.0f);
    wma_set_frequency_amplitude(e, 440.0f, 0.0f);
    return e;
}

std::vector<float> renderBlocks(WmaEngine* e, int64_t frames) {
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
    for (int64_t done = 0; done < frames; done += kBlock) {
        const int n = static_cast<int>(std::min<int64_t>(kBlock, frames - done));
        EXPECT_EQ(wma_offline_render(e, out.data() + static_cast<size_t>(done) * 2, nullptr, n),
                  WMA_OK);
    }
    return out;
}

float peakOf(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
    float p = 0.0f;
    for (size_t i = from; i < std::min(to, v.size()); ++i) p = std::max(p, std::abs(v[i]));
    return p;
}

int64_t threadCpuNs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

class SongApi : public ::testing::Test {
protected:
    void SetUp() override { wma::setLogCallback([](wma::LogLevel, const char*, const char*) {}); }
    void TearDown() override {
        for (const auto& p : mFiles) {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
        wma::setLogCallback(nullptr);
    }
    std::string sine(const std::string& name, float period, float phase, float amp = 0.8f) {
        mFiles.push_back(songFixture(name));
        return writeSine(name, period, phase, amp);
    }
    std::vector<std::filesystem::path> mFiles;
};

/// Las 5 secciones de la medicion: A, B, silencio, B, A. Cada una dura un numero entero de loops
/// (el borde sale y entra en la fase de un loop completo). Devuelve los starts relativos a P0.
struct Arrangement {
    std::vector<int64_t> startOffsets;   // 5
    int64_t endOffset;
    int64_t lead;
};

Arrangement fiveSections() {
    const int loops[5] = {3, 2, 1, 2, 2};
    Arrangement a;
    a.lead = 2 * kLoop;
    int64_t t = a.lead;
    for (int k = 0; k < 5; ++k) {
        a.startOffsets.push_back(t);
        t += static_cast<int64_t>(loops[k]) * kLoop;
    }
    a.endOffset = t;
    return a;
}

/// Renderiza las 5 secciones offline con F = `fadeOverride` (< 0: el real) y devuelve el render y
/// las uniones (indices del buffer de salida, ya con el retraso del limitador).
struct FiveRender {
    std::vector<float> out;
    std::vector<size_t> junctions;
};

FiveRender renderFiveSections(SongApi& test, int fadeOverride) {
    (void)test;
    FiveRender r;
    WmaEngine* e = quietEngine();
    EXPECT_NE(e, nullptr);
    EXPECT_EQ(wma_offline_start(e, kRate, kBlock), WMA_OK);
    if (fadeOverride >= 0) e->engine->getAudioLooper().song().setFadeOverride(fadeOverride);

    // Dos clips cuyo corte en seco suena: seno de 400 frames (12 periodos en el loop) con fase
    // inicial 1,0 (valor 0,67 en el frame 0) y seno de 300 (16 periodos) con fase 4,0 (-0,6).
    const std::string pa = songFixture("five_a").string();
    const std::string pb = songFixture("five_b").string();
    writeSine("five_a", 400.0f, 1.0f);
    writeSine("five_b", 300.0f, 4.0f);
    const int32_t a = loadClip(e, pa);
    const int32_t b = loadClip(e, pb);

    const int64_t p0 = wma_transport_get_play_frame(e);
    const Arrangement arr = fiveSections();
    const int32_t clips[5] = {a, b, WMA_SONG_SILENCE, b, a};
    std::vector<WmaSongSection> secs;
    for (int k = 0; k < 5; ++k) secs.push_back(section(p0 + arr.startOffsets[k], clips[k]));
    EXPECT_EQ(wma_song_set_schedule(e, kLoop, secs.data(), 5, p0 + arr.endOffset), WMA_OK);

    r.out = renderBlocks(e, arr.endOffset + 2 * kLoop);
    for (int64_t off : arr.startOffsets) r.junctions.push_back(static_cast<size_t>(off + kOutputDelay));
    r.junctions.push_back(static_cast<size_t>(arr.endOffset + kOutputDelay));
    EXPECT_EQ(wma_song_get_section(e), -1) << "despues del fin";
    wma_offline_stop(e);
    wma_engine_destroy(e);
    std::error_code ec;
    std::filesystem::remove(pa, ec);
    std::filesystem::remove(pb, ec);
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Offline: 5 secciones sin discontinuidades en las uniones (AC-055.12)
// ---------------------------------------------------------------------------------------

// Bug plausible: una union (cambio de clip, entrada/salida de silencio, inicio o fin) deja un
// escalon audible. El detector de S2 ve solo ~54 % de los cortes inyectados, asi que la prueba
// fuerte es la envolvente del test de looper; este mide el resultado de punta a punta por la C
// API. El MUTANTE (fundido de 0 frames) tiene que disparar el detector en CADA union: si no, el
// instrumento no distingue este render de uno con cortes y el "vacio" no prueba nada.
TEST_F(SongApi, AC_055_12_FiveSectionsRenderedOfflineHaveNoDiscontinuitiesAtTheJunctions) {
    const FiveRender clean = renderFiveSections(*this, -1);
    ASSERT_GT(peakOf(clean.out), 0.4f) << "el render no sono";
    ASSERT_EQ(clean.junctions.size(), 6u);
    const auto cleanBad = findJunctionDiscontinuities(clean.out, clean.junctions);
    EXPECT_TRUE(cleanBad.empty()) << "uniones con discontinuidad: " << cleanBad.size();

    const FiveRender cut = renderFiveSections(*this, 0);
    const auto cutBad = findJunctionDiscontinuities(cut.out, cut.junctions);
    EXPECT_EQ(cutBad.size(), cut.junctions.size())
        << "el mutante sin fundido no dispara el detector en todas las uniones";

    // Los numeros que se reportan: razon salto/entorno por union (umbral del detector: 2,0).
    std::printf("[ song ] umbral kJumpRatio=%.1f kJumpFloor=%.3f; razon por union (limpio / sin fundido):\n",
                kJumpRatio, kJumpFloor);
    for (size_t k = 0; k < clean.junctions.size(); ++k) {
        std::printf("[ song ]   union %zu: %.3f / %.1f\n", k,
                    junctionRatio(clean.out, clean.junctions[k]),
                    junctionRatio(cut.out, cut.junctions[k]));
    }
}

// Bug plausible: el segundo render de una misma instancia no reproduce el primero (las voces
// conservan los smoothers de volumen/paneo del render anterior). El volumen != 1 hace que el
// primer bloque rampee, asi que un smoother sin reiniciar cambia la salida.
TEST_F(SongApi, AC_055_12_ASecondOfflineRenderOfTheSameInstanceReproducesTheFirst) {
    WmaEngine* e = quietEngine();
    ASSERT_NE(e, nullptr);
    const std::string pa = sine("repro_a", 400.0f, 1.0f);
    ASSERT_EQ(wma_offline_start(e, kRate, kBlock), WMA_OK);
    const int32_t a = loadClip(e, pa);
    wma_looper_set_track_volume(e, 0, 0.5f);
    wma_looper_set_track_pan(e, 0, 0.6f);
    std::vector<float> renders[2];
    for (int pass = 0; pass < 2; ++pass) {
        ASSERT_EQ(wma_offline_start(e, kRate, kBlock), WMA_OK);   // un reinicio, no un render nuevo
        const int64_t p0 = wma_transport_get_play_frame(e);
        const WmaSongSection s = section(p0 + 2 * kLoop, a);
        ASSERT_EQ(wma_song_set_schedule(e, kLoop, &s, 1, p0 + 5 * kLoop), WMA_OK);
        renders[pass] = renderBlocks(e, 6 * kLoop);
    }
    ASSERT_GT(peakOf(renders[0]), 0.2f);
    ASSERT_EQ(renders[0].size(), renders[1].size());
    EXPECT_EQ(std::memcmp(renders[0].data(), renders[1].data(),
                          renders[0].size() * sizeof(float)),
              0)
        << "el segundo render no reproduce el primero";
    wma_offline_stop(e);
    wma_engine_destroy(e);
}

// Bug plausible: dos pistas se pisan o una se pierde (la agenda lee la fila equivocada, el
// mezclador cuenta una pista dos veces) o el volumen de la pista no llega a la voz de la
// cancion. El mezclado es lineal: pista 0 sola + pista 1 sola == las dos juntas.
TEST_F(SongApi, AC_055_12_TwoTracksMixLinearlyAndTheTrackVolumeReachesTheSong) {
    // Amplitud baja: el recorte suave de la etapa de salida no es lineal y a 0,4 ya se lleva 3 %.
    const std::string pa = sine("mix_a", 400.0f, 1.0f, 0.2f);
    const std::string pb = sine("mix_b", 300.0f, 2.0f, 0.2f);
    const auto render = [&](bool t0, bool t1, float vol0) {
        WmaEngine* e = quietEngine();
        EXPECT_EQ(wma_offline_start(e, kRate, kBlock), WMA_OK);
        const int32_t a = loadClip(e, pa);
        const int32_t b = loadClip(e, pb);
        wma_looper_set_track_volume(e, 0, vol0);
        const int64_t p0 = wma_transport_get_play_frame(e);
        const WmaSongSection s = section(p0 + 2 * kLoop, t0 ? a : WMA_SONG_SILENCE,
                                         t1 ? b : WMA_SONG_SILENCE);
        EXPECT_EQ(wma_song_set_schedule(e, kLoop, &s, 1, p0 + 6 * kLoop), WMA_OK);
        auto out = renderBlocks(e, 7 * kLoop);
        wma_offline_stop(e);
        wma_engine_destroy(e);
        return out;
    };
    const auto only0 = render(true, false, 1.0f);
    const auto only1 = render(false, true, 1.0f);
    const auto both = render(true, true, 1.0f);
    ASSERT_GT(peakOf(only0), 0.05f);
    ASSERT_GT(peakOf(only1), 0.05f);
    double worst = 0.0;
    for (size_t i = 0; i < both.size(); ++i) {
        worst = std::max(worst, double(std::abs(both[i] - (only0[i] + only1[i]))));
    }
    // Una pista perdida o contada dos veces se desvia ~100 % del pico; la no linealidad de la
    // salida, menos de 1 %.
    EXPECT_LT(worst, 0.02 * peakOf(both)) << "las pistas no se suman linealmente";
    // el volumen de la pista: a la mitad, la mitad del nivel (una vez asentado el smoother)
    const auto half = render(true, false, 0.5f);
    const size_t at = static_cast<size_t>(5 * kLoop) * 2;
    const size_t to = at + static_cast<size_t>(kLoop) * 2;
    EXPECT_NEAR(peakOf(half, at, to) / peakOf(only0, at, to), 0.5f, 0.01f);
}

// ---------------------------------------------------------------------------------------
// La C API: argumentos, errores, cuenta de memoria y el reloj que no se toca
// ---------------------------------------------------------------------------------------

// Bug plausible: la C API deja pasar un puntero nulo / un id basura al motor, o devuelve un
// codigo equivocado (PARAMETER_OUT_OF_RANGE por INVALID_OPERATION), y el JNI de la tarea
// siguiente hereda un contrato que no es el documentado.
TEST_F(SongApi, ArgumentsAndErrorCodesAreTheDocumentedOnes) {
    EXPECT_EQ(wma_song_set_schedule(nullptr, kLoop, nullptr, 0, 0), WMA_ERROR_NOT_INITIALIZED);
    EXPECT_EQ(wma_song_unload_clip(nullptr, 0), WMA_ERROR_NOT_INITIALIZED);
    int32_t id = -1;
    EXPECT_EQ(wma_song_load_clip(nullptr, "x", kRate, 0, 0, kLoop, &id), WMA_ERROR_NOT_INITIALIZED);
    EXPECT_EQ(wma_song_get_section(nullptr), -1);
    EXPECT_EQ(wma_song_get_late_adoptions(nullptr), 0);

    WmaEngine* e = quietEngine();
    ASSERT_NE(e, nullptr);
    const std::string pa = sine("api_a", 400.0f, 1.0f);
    EXPECT_EQ(wma_song_load_clip(e, nullptr, kRate, 0, 0, kLoop, &id), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_EQ(wma_song_load_clip(e, pa.c_str(), kRate, 0, 0, kLoop, nullptr),
              WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_EQ(wma_song_load_clip(e, pa.c_str(), 0, 0, 0, kLoop, &id), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_EQ(wma_song_load_clip(e, pa.c_str(), kRate, 0, 0, 0, &id), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_EQ(wma_song_load_clip(e, "/no/existe.wav", kRate, 0, 0, kLoop, &id), WMA_ERROR_IO);
    const std::string junk = songFixture("junk").string();
    mFiles.push_back(junk);
    {
        FILE* f = std::fopen(junk.c_str(), "wb");
        ASSERT_NE(f, nullptr);
        std::fputs("esto no es un RIFF", f);
        std::fclose(f);
    }
    EXPECT_EQ(wma_song_load_clip(e, junk.c_str(), kRate, 0, 0, kLoop, &id),
              WMA_ERROR_UNSUPPORTED_FORMAT);

    id = loadClip(e, pa);
    const WmaSongSection ok = section(100000, id);
    EXPECT_EQ(wma_song_set_schedule(e, kLoop, nullptr, 1, 200000), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_EQ(wma_song_set_schedule(e, kLoop, &ok, WMA_SONG_MAX_SECTIONS + 1, 200000),
              WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    EXPECT_EQ(wma_song_set_schedule(e, kLoop, &ok, 1, 100000), WMA_ERROR_PARAMETER_OUT_OF_RANGE)
        << "end_frame < ultimo start + 2F";
    EXPECT_EQ(wma_song_set_schedule(e, kLoop * 2, &ok, 1, 400000), WMA_ERROR_INVALID_OPERATION)
        << "clip de otro loop_frames";
    const WmaSongSection ghost = section(100000, 33);
    EXPECT_EQ(wma_song_set_schedule(e, kLoop, &ghost, 1, 200000), WMA_ERROR_INVALID_OPERATION);
    EXPECT_EQ(wma_song_set_schedule(e, kLoop, &ok, 1, 200000), WMA_OK);
    EXPECT_EQ(wma_song_unload_clip(e, id), WMA_ERROR_INVALID_OPERATION) << "la agenda lo usa";
    EXPECT_EQ(wma_song_unload_clip(e, 63), WMA_ERROR_INVALID_OPERATION) << "no existe";
    EXPECT_EQ(wma_song_unload_clip(e, -1), WMA_ERROR_INVALID_OPERATION);

    // grabar / armar con la cancion activa: la API de siempre, sin efecto
    wma_looper_start_recording(e, 1);
    EXPECT_EQ(wma_looper_arm_in_frames(e, 1, 100), -1);
    EXPECT_EQ(wma_looper_get_armed_track(e), -1);
    wma_engine_destroy(e);
}

// Bug plausible: el pool deja de contar los retirados (se llena en silencio), o el presupuesto de
// memoria no ve los clips: 64 clips de 16 s por pista agotarian la RAM sin un MEMORY_BUDGET.
TEST_F(SongApi, ThePoolHasAFixedSizeAndTheClipsCountAgainstTheMemoryBudget) {
    WmaEngine* e = quietEngine();
    ASSERT_NE(e, nullptr);
    const std::string pa = sine("pool_a", 400.0f, 1.0f);
    int32_t ids[WMA_SONG_MAX_CLIPS];
    for (int k = 0; k < WMA_SONG_MAX_CLIPS; ++k) {
        ASSERT_EQ(wma_song_load_clip(e, pa.c_str(), kRate, 0, 0, kLoop, &ids[k]), WMA_OK) << k;
        EXPECT_EQ(ids[k], k);
    }
    int32_t extra = -1;
    EXPECT_EQ(wma_song_load_clip(e, pa.c_str(), kRate, 0, 0, kLoop, &extra),
              WMA_ERROR_PARAMETER_OUT_OF_RANGE) << "pool lleno";
    ASSERT_EQ(wma_song_unload_clip(e, 7), WMA_OK);
    EXPECT_EQ(wma_song_load_clip(e, pa.c_str(), kRate, 0, 0, kLoop, &extra), WMA_OK);
    EXPECT_EQ(extra, 7) << "el id retirado se reusa";

    AudioLooper::LooperCapabilities caps;
    caps.memoryBudgetBytes = static_cast<size_t>(WMA_SONG_MAX_CLIPS) * kLoop * 2 * sizeof(float);
    e->engine->getAudioLooper().setCapabilities(caps);
    ASSERT_EQ(wma_song_unload_clip(e, 8), WMA_OK);
    ASSERT_EQ(e->engine->getAudioLooper().song().reclaim(true),
              static_cast<size_t>(WMA_SONG_MAX_CLIPS - 1) * kLoop * 2 * sizeof(float));
    caps.memoryBudgetBytes -= kLoop * 2 * sizeof(float);   // un clip menos de lo que ya hay + 1
    e->engine->getAudioLooper().setCapabilities(caps);
    EXPECT_EQ(wma_song_load_clip(e, pa.c_str(), kRate, 0, 0, kLoop, &extra), WMA_ERROR_MEMORY_BUDGET);
    wma_engine_destroy(e);
}

// Bug plausible: un camino de la cancion toca el play frame (reset al armar, al cargar, al
// reprogramar o al apagar): el ancla P0 de NoisyPad deja de valer y la cancion suena corrida.
// El play frame avanza exactamente lo renderizado, sin saltos, con la cancion adentro.
TEST_F(SongApi, AC_055_12_NoSongPathEverResetsThePlayFrame) {
    WmaEngine* e = quietEngine();
    ASSERT_NE(e, nullptr);
    const std::string pa = sine("pf_a", 400.0f, 1.0f);
    ASSERT_EQ(wma_offline_start(e, kRate, kBlock), WMA_OK);
    std::vector<float> scratch(static_cast<size_t>(kBlock) * 2);
    const auto advance = [&](int blocks) {
        for (int i = 0; i < blocks; ++i) wma_offline_render(e, scratch.data(), nullptr, kBlock);
    };
    advance(10);
    int64_t expected = wma_transport_get_play_frame(e);
    ASSERT_EQ(expected, 10 * kBlock);
    const auto step = [&](const char* what, int blocks) {
        advance(blocks);
        expected += static_cast<int64_t>(blocks) * kBlock;
        EXPECT_EQ(wma_transport_get_play_frame(e), expected) << what;
    };
    const int32_t a = loadClip(e, pa);
    step("tras cargar", 2);
    const WmaSongSection s = section(expected + 4 * kBlock, a);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop, &s, 1, expected + 40 * kBlock), WMA_OK);
    step("tras programar", 8);
    const WmaSongSection s2 = section(expected + 4 * kBlock, a);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop, &s2, 1, expected + 60 * kBlock), WMA_OK);
    step("tras reprogramar sonando", 8);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop, nullptr, 0, 0), WMA_OK);
    step("tras apagar", 4);
    ASSERT_EQ(wma_song_unload_clip(e, a), WMA_OK);
    step("tras descargar", 2);
    // y un reinicio del render offline no lo toca (S2): el ancla se relee, no se asume 0
    ASSERT_EQ(wma_offline_start(e, kRate, kBlock), WMA_OK);
    EXPECT_EQ(wma_transport_get_play_frame(e), expected);
    wma_engine_destroy(e);
}

// Bug plausible: offline, un clip retirado sigue ocupando memoria hasta que "el RT avance" y no
// hay RT (se acumulan 147 MB de clips muertos en un export), o se libera estando todavia en la
// agenda del render.
TEST_F(SongApi, AnOfflineEngineReleasesARetiredClipAtOnce) {
    WmaEngine* e = quietEngine();
    ASSERT_NE(e, nullptr);
    const std::string pa = sine("off_a", 400.0f, 1.0f);
    ASSERT_EQ(wma_offline_start(e, kRate, kBlock), WMA_OK);
    const int32_t a = loadClip(e, pa);
    auto& song = e->engine->getAudioLooper().song();
    EXPECT_EQ(song.reservedBytes(), static_cast<size_t>(kLoop) * 2 * sizeof(float));
    ASSERT_EQ(wma_song_unload_clip(e, a), WMA_OK);
    EXPECT_EQ(song.reservedBytes(), 0u) << "offline libera de inmediato";
    EXPECT_EQ(song.retiredClips(), 0);
    wma_engine_destroy(e);
}

// ---------------------------------------------------------------------------------------
// Vivo: una rafaga de publicaciones contra el callback (AC-055.13)
// ---------------------------------------------------------------------------------------

namespace {

/// Un motor vivo sobre el FakeAudioBackend: el callback lo bombea el hilo del test.
struct LiveSong {
    WmaEngine* e = nullptr;
    FakeAudioBackend* fake = nullptr;

    void bringUp() {
        resetLastCreatedSystemBackend();
        e = wma_engine_create();
        ASSERT_NE(e, nullptr);
        fake = lastCreatedSystemBackend();
        ASSERT_NE(fake, nullptr);
        fake->setNegotiatedSampleRate(kRate);
        wma_set_use_backend_manager(e, true);
        ASSERT_TRUE(wma_select_backend(1));
        wma_set_audio_mode(e, 0);
        wma_set_master_volume(e, 1.0f);
        wma_set_frequency_amplitude(e, 440.0f, 0.0f);
        ASSERT_EQ(wma_engine_start(e, 0), WMA_OK);
    }
    void tearDown() {
        if (e == nullptr) return;
        wma_engine_stop(e, 0);
        wma_engine_destroy(e);
        e = nullptr;
    }
};

}  // namespace

// Bug plausible (todo lo que dice AC-055.13 de "sin xruns"): el callback ALOCA al adoptar una
// agenda (un vector, un new), se bloquea con el control (un lock en el camino de adopcion), lee
// una agenda a medio escribir (desgarro del triple buffer) o tarda mas que su periodo cuando
// llega una publicacion. 1000 publicaciones de 1024 secciones contra el callback en marcha.
TEST_F(SongApi, AC_055_13_ABurstOfPublicationsAgainstTheCallbackAllocatesNothingAndNeverTears) {
    LiveSong live;
    live.bringUp();
    ASSERT_NE(live.e, nullptr);
    const std::string pa = sine("burst_a", 400.0f, 1.0f);
    const std::string pb = sine("burst_b", 300.0f, 4.0f);
    const int32_t clips[2] = {loadClip(live.e, pa), loadClip(live.e, pb)};
    auto& song = live.e->engine->getAudioLooper().song();
    song.setVerifyAgendas(true);

    // El instrumento del conteo tiene que poder fallar: un `new` en el hilo armado se ve.
    {
        static int* volatile sink;   // volatile: el compilador no puede elidir el par new/delete
        armAllocCounter();
        sink = new int(0);
        delete sink;
        ASSERT_GE(disarmAllocCounter(), 1) << "el contador de allocations no cuenta";
    }

    constexpr int kCallbackFrames = 256;
    constexpr double kPeriodNs = 1e9 * kCallbackFrames / kRate;
    std::atomic<bool> stop{false};
    std::atomic<int64_t> allocs{-1};
    std::atomic<int64_t> maxNs{0};
    std::atomic<int64_t> blocks{0};
    std::thread rt([&] {
        std::vector<float> buf(static_cast<size_t>(kCallbackFrames) * 2);
        for (int i = 0; i < 20; ++i) live.fake->callback()->onAudioReady(buf.data(), nullptr, kCallbackFrames);
        armAllocCounter();
        int64_t worst = 0;
        int64_t n = 0;
        while (!stop.load(std::memory_order_acquire)) {
            const int64_t t0 = threadCpuNs();
            live.fake->callback()->onAudioReady(buf.data(), nullptr, kCallbackFrames);
            worst = std::max(worst, threadCpuNs() - t0);
            ++n;
        }
        allocs.store(disarmAllocCounter());
        maxNs.store(worst);
        blocks.store(n);
    });

    constexpr int kPublications = 1000;
    int64_t seenAdopted = song.adoptedAgendas();
    int adoptedWaits = 0;
    for (int i = 0; i < kPublications; ++i) {
        const int64_t base = wma_transport_get_play_frame(live.e) + 4 * kLoop;
        std::vector<WmaSongSection> secs(WMA_SONG_MAX_SECTIONS);
        for (int k = 0; k < WMA_SONG_MAX_SECTIONS; ++k) {
            secs[k] = section(base + static_cast<int64_t>(k) * 2 * kLoop,
                              clips[(i + k) % 2], clips[(i + k + 1) % 2]);
        }
        const WmaResult published = wma_song_set_schedule(
            live.e, kLoop, secs.data(), WMA_SONG_MAX_SECTIONS,
            base + static_cast<int64_t>(WMA_SONG_MAX_SECTIONS) * 2 * kLoop);
        EXPECT_EQ(published, WMA_OK) << "publicacion " << i;
        if (published != WMA_OK) break;   // no ASSERT: el hilo del callback sigue corriendo
        if (i % 25 == 24) {   // cada tanto, asegurar que el callback adopto: el solapamiento es real
            const int64_t before = seenAdopted;
            if (waitUntil([&] { return song.adoptedAgendas() > before; },
                          std::chrono::milliseconds(20000))) {
                ++adoptedWaits;
            }
            seenAdopted = song.adoptedAgendas();
        }
    }
    stop.store(true, std::memory_order_release);
    rt.join();

    EXPECT_EQ(adoptedWaits, kPublications / 25) << "el callback dejo de adoptar agendas";
    EXPECT_GE(song.adoptedAgendas(), kPublications / 25);
    EXPECT_EQ(song.tornAgendas(), 0) << "una agenda adoptada no coincide con su checksum";
    EXPECT_EQ(allocs.load(), 0) << "el callback alocó mientras se publicaban agendas";
    const double maxUs = static_cast<double>(maxNs.load()) / 1000.0;
    std::printf("[ song ] rafaga: %lld callbacks de %d frames, %lld adopciones, %lld tardias, "
                "max CPU por callback %.1f us (periodo %.1f us)\n",
                static_cast<long long>(blocks.load()), kCallbackFrames,
                static_cast<long long>(song.adoptedAgendas()),
                static_cast<long long>(wma_song_get_late_adoptions(live.e)), maxUs,
                kPeriodNs / 1000.0);
#ifndef WMA_TEST_UNDER_SANITIZER
    EXPECT_LT(static_cast<double>(maxNs.load()), kPeriodNs)
        << "un callback tardo mas que su periodo";
#endif
    live.tearDown();
}

// Bug plausible: la seccion publicada no llega al hilo de control en vivo, o el callback no
// avanza la cancion con el play frame del motor (la fase se desfasa del reloj del Transport).
TEST_F(SongApi, AC_055_13_LiveSongFollowsThePlayFrameAndPublishesItsSection) {
    LiveSong live;
    live.bringUp();
    ASSERT_NE(live.e, nullptr);
    const std::string pa = sine("live_a", 400.0f, 1.0f);
    const int32_t a = loadClip(live.e, pa);
    std::vector<float> buf(static_cast<size_t>(kBlock) * 2);
    const auto pump = [&](int blocks) {
        for (int i = 0; i < blocks; ++i) {
            std::fill(buf.begin(), buf.end(), 0.0f);
            live.fake->callback()->onAudioReady(buf.data(), nullptr, kBlock);
        }
    };
    pump(4);
    const int64_t p0 = wma_transport_get_play_frame(live.e);
    const WmaSongSection secs[2] = {section(p0 + 8 * kBlock, a), section(p0 + 30 * kBlock, a)};
    ASSERT_EQ(wma_song_set_schedule(live.e, kLoop, secs, 2, p0 + 50 * kBlock), WMA_OK);
    EXPECT_EQ(wma_song_get_section(live.e), -1);
    pump(4);
    EXPECT_EQ(wma_song_get_section(live.e), -1) << "antes del primer start";
    pump(6);
    EXPECT_EQ(wma_song_get_section(live.e), 0);
    EXPECT_GT(peakOf(buf), 0.3f) << "la cancion no suena en vivo";
    pump(30);
    EXPECT_EQ(wma_song_get_section(live.e), 1);
    pump(40);
    EXPECT_EQ(wma_song_get_section(live.e), -1) << "despues del fin";
    EXPECT_EQ(wma_song_get_late_adoptions(live.e), 0);
    live.tearDown();
}

// Bug plausible (AC-055.13, IMPORTANTE 2): un motor SIN stream (parado, no offline) retiene cada
// clip descargado porque ningun callback avanza la foto del RT: tras 64 ciclos load+unload el
// pool da PARAMETER_OUT_OF_RANGE y los bytes de los clips muertos cuentan contra el presupuesto.
TEST_F(SongApi, AC_055_13_AStoppedEngineReleasesARetiredClipAtOnce) {
    WmaEngine* e = quietEngine();
    ASSERT_NE(e, nullptr);
    ASSERT_FALSE(e->engine->isOffline());
    const std::string pa = sine("stopped_a", 400.0f, 1.0f);
    const int32_t first = loadClip(e, pa);
    const WmaSongSection s = section(100000, first);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop, &s, 1, 200000), WMA_OK);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop, nullptr, 0, 0), WMA_OK);
    ASSERT_EQ(wma_song_unload_clip(e, first), WMA_OK);
    auto& song = e->engine->getAudioLooper().song();
    EXPECT_EQ(song.retiredClips(), 0) << "con el motor parado el clip no espera a un callback";
    for (int k = 0; k < 3 * WMA_SONG_MAX_CLIPS; ++k) {
        int32_t id = -1;
        ASSERT_EQ(wma_song_load_clip(e, pa.c_str(), kRate, 0, 0, kLoop, &id), WMA_OK) << k;
        ASSERT_EQ(wma_song_unload_clip(e, id), WMA_OK) << k;
    }
    EXPECT_EQ(song.reservedBytes(), 0u);
    wma_engine_destroy(e);
}

}  // namespace wma_test
