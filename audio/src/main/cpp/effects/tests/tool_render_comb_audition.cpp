/**
 * tool_render_comb_audition.cpp — HERRAMIENTA, no test. Renderiza a WAV el caso de escucha
 * del AC-4 del bump a 2.8.1: cadena PARALLEL con DECI_HPF (latencia por parametro) contra
 * un FILTER transparente (latencia 0).
 *
 * Corre con `AC4_OUT_DIR=<dir> ctest -R CombAudition`. Sin la variable no escribe nada,
 * asi que en la suite normal es inerte.
 *
 * POR QUE ESTE PUNTO DE OPERACION Y NO LA REDUCCION MAXIMA
 * -------------------------------------------------------
 * latencia = ceil(fs/targetSR) - 1, y la desalineacion del defecto es UNA latencia entera
 * (retrasaba 2*(max-lat) en vez de (max-lat)). El primer notch cae en fs/(2*lat), o sea
 * ~targetSR/2 para CUALQUIER ajuste: el notch aterriza siempre en el Nyquist de la tasa
 * objetivo, que es el borde de la banda donde las dos ramas todavia se parecen.
 *
 * El bit depth va en 24 y el mix en 1 A PROPOSITO: se aisla el sample-and-hold, que es lo
 * que genera la latencia. Con el crusher puesto, la distorsion tapa lo que se quiere oir.
 *
 * LO QUE SE MIDIO CON ESTO (2026-08-24)
 * -------------------------------------
 * Espectro del tramo de ruido, roto menos arreglado:
 *
 *   target 1000 Hz  ->  -6.1 dB en 510 Hz y -6.5 dB en 1530 Hz, con picos de +4 dB entre medio.
 *   target  100 Hz  ->  +0.0 dB en 50 Hz. NO HAY PEINE.
 *
 * O sea que la "reduccion maxima" que pedia la receta de escucha del consumidor —479 muestras,
 * notch prometido en 50 Hz— es el PEOR punto para escuchar, no el mejor: el hold de 480 muestras
 * filtra la componente correlacionada con un sinc cuyo primer cero cae en 100 Hz, asi que abajo
 * de 100 Hz casi no queda senal comun entre las ramas para peinar. Por eso se renderizan los dos:
 * el segundo esta para mostrar que ahi NO se escucha.
 *
 * Control que descarta la explicacion aburrida: los dos WAV de 100 Hz SI difieren, y mucho
 * —rms(dif) a -0.5 dB de la senal—, pero es un corrimiento de ruido decorrelacionado, no un peine.
 */

#include "effects/EffectChain.h"
#include "effects/EffectTypes.h"
#include "effects/DeciHpfEffect.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kBlock = 256;

using Buf = std::vector<float>;

/** Escribe PCM 16 bit estereo intercalado. */
void writeWav(const std::string& path, const Buf& interleaved, int sampleRate) {
    const uint32_t frames = static_cast<uint32_t>(interleaved.size() / 2);
    const uint32_t dataBytes = frames * 2u * 2u;
    FILE* f = std::fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr) << "no se pudo abrir " << path;

    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };

    std::fwrite("RIFF", 1, 4, f); u32(36u + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16u); u16(1); u16(2);
    u32(static_cast<uint32_t>(sampleRate));
    u32(static_cast<uint32_t>(sampleRate) * 4u); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(dataBytes);

    for (float s : interleaved) {
        const float c = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
        u16(static_cast<uint16_t>(static_cast<int16_t>(c * 32767.0f)));
    }
    std::fclose(f);
}

/** Acorde de sierras sostenido, y despues ruido blanco. El ruido delata el peine. */
Buf source(double seconds) {
    const int frames = static_cast<int>(seconds * kSampleRate);
    const int chordEnd = frames / 2;
    Buf out(static_cast<size_t>(frames) * 2, 0.0f);

    const double partials[] = {110.0, 164.81, 220.0, 277.18};
    uint32_t rng = 0x13579bdfu;  // LCG propio: determinista y sin depender de <random>

    for (int n = 0; n < frames; ++n) {
        float l = 0.0f, r = 0.0f;
        if (n < chordEnd) {
            const double t = static_cast<double>(n) / kSampleRate;
            for (double f0 : partials) {
                // Sierra por suma de armonicos hasta 6 kHz: banda acotada, sin aliasing propio.
                for (int h = 1; h * f0 < 6000.0; ++h) {
                    const double a = 0.12 / h;
                    l += static_cast<float>(a * std::sin(2.0 * M_PI * h * f0 * t));
                    r += static_cast<float>(a * std::sin(2.0 * M_PI * h * (f0 * 1.002) * t));
                }
            }
            l *= 0.30f; r *= 0.30f;
        } else {
            rng = rng * 1664525u + 1013904223u;
            const float w = (static_cast<float>(rng >> 8) / 8388608.0f - 1.0f) * 0.20f;
            l = w; r = w;
        }
        // Rampa de 10 ms en los bordes y en la juntura, para no meter clicks propios.
        const int d = std::min({n, frames - 1 - n, std::abs(n - chordEnd)});
        const float env = d < 480 ? static_cast<float>(d) / 480.0f : 1.0f;
        out[static_cast<size_t>(n) * 2] = l * env;
        out[static_cast<size_t>(n) * 2 + 1] = r * env;
    }
    return out;
}

/** Cadena PARALLEL {DECI_HPF, FILTER}, con el DECI aislado en su sample-and-hold. */
void build(EffectChain& chain, float targetHz) {
    chain.setSampleRate(kSampleRate);
    chain.setRoutingMode(RoutingMode::PARALLEL);
    ASSERT_TRUE(chain.addEffect(DECI_HPF));
    ASSERT_TRUE(chain.addEffect(FILTER));
    chain.setParameter(0, DeciHpfEffect::PARAM_BIT_DEPTH, 24.0f);
    chain.setParameter(0, DeciHpfEffect::PARAM_HPF_CUTOFF, 20.0f);
    chain.setParameter(0, DeciHpfEffect::PARAM_SAMPLE_RATE, targetHz);
    chain.setParameter(0, DeciHpfEffect::PARAM_MIX, 1.0f);
    chain.setParameter(1, 0, 20000.0f);  // cutoff arriba: la otra rama, lo mas transparente posible
    chain.setParameter(1, 1, 0.707f);
    chain.setParameter(1, 2, 0.0f);      // LPF
}

/**
 * 🔴 El crossfade de cambio de modo dura 30 ms y arranca en progress=0: sin calentar,
 * los primeros bloques salen por SERIAL y no por el modo pedido.
 */
Buf render(float targetHz, const Buf& in) {
    EffectChain chain;
    build(chain, targetHz);

    Buf silence(static_cast<size_t>(kBlock) * 2, 0.0f), sink(static_cast<size_t>(kBlock) * 2, 0.0f);
    for (int i = 0; i < 48000 / kBlock; ++i) chain.process(silence.data(), sink.data(), kBlock);

    Buf out(in.size(), 0.0f);
    const int frames = static_cast<int>(in.size() / 2);
    for (int done = 0; done < frames; done += kBlock) {
        const int n = std::min(kBlock, frames - done);
        chain.process(const_cast<float*>(in.data()) + static_cast<size_t>(done) * 2,
                      out.data() + static_cast<size_t>(done) * 2, n);
    }
    return out;
}

}  // namespace

TEST(CombAudition, RenderWavs) {
    const char* dir = std::getenv("AC4_OUT_DIR");
    if (dir == nullptr) { GTEST_SKIP() << "sin AC4_OUT_DIR: la herramienta no escribe"; }

    const char* tagEnv = std::getenv("AC4_TAG");
    const std::string tag = tagEnv ? tagEnv : "sintag";

    const Buf in = source(6.0);
    writeWav(std::string(dir) + "/fuente_seca.wav", in, kSampleRate);

    for (float targetHz : {1000.0f, 100.0f}) {
        EffectChain probe;
        build(probe, targetHz);
        const int lat = probe.getEffectLatencySamples(0);
        const double notch = kSampleRate / (2.0 * std::max(lat, 1));
        std::printf("  target=%6.0f Hz | latencia=%3d muestras | notch previsto=%7.1f Hz | %s\n",
                    static_cast<double>(targetHz), lat, notch, tag.c_str());

        char name[256];
        std::snprintf(name, sizeof(name), "%s/comb_target%04.0f_%s.wav",
                      dir, static_cast<double>(targetHz), tag.c_str());
        writeWav(name, render(targetHz, in), kSampleRate);
    }
}
