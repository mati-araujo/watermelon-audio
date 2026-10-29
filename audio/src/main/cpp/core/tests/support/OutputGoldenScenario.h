#pragma once

/**
 * OutputGoldenScenario.h — el escenario del golden de la cadena de salida PROTEGIDA (REQ-055 S12).
 *
 * Usa SOLO la API que ya tenia watermelon-audio 2.23.0 (el motor C++ y nada del bus ni de los
 * perfiles), porque el mismo archivo se compilo contra 96ca8d61 para capturar el hash de la
 * constante `kGolden223Hash` de test_output_profiles.cpp. Si hay que recapturar, se compila
 * contra el arbol de 2.23.0 (no contra este), y su diff es la revision.
 *
 * La senal esta pensada para ejercitar TODA la cadena: un seno fuerte que activa el limitador
 * con lookahead, el soft clip TANH y el dither, y un cambio del volumen maestro a mitad de camino
 * (de 1,0 a 0,35). El `setMasterVolume(1.4f)` inicial queda tal cual: el motor lo acota a 1,0 en
 * 2.23.0 y ahora, y cambiar la llamada cambiaria la captura.
 */

#include "../../AudioEngine.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace wma_test {

constexpr int kGoldenRate = 48000;
constexpr int kGoldenBlock = 256;
constexpr int kGoldenBlocks = 60;

/// Las muestras de salida (estereo intercalado) del escenario, en bloques de 256.
inline std::vector<float> renderOutputGoldenScenario(AudioEngine& engine) {
    std::vector<float> out;
    out.reserve(static_cast<size_t>(kGoldenBlock) * kGoldenBlocks * 2);
    engine.setOscillatorEnabled(true);
    engine.setMasterVolume(1.4f);
    engine.setFrequencyAndAmplitude(220.0f, 1.0f);
    std::vector<float> block(static_cast<size_t>(kGoldenBlock) * 2);
    for (int b = 0; b < kGoldenBlocks; ++b) {
        if (b == kGoldenBlocks / 2) engine.setMasterVolume(0.35f);
        engine.renderBlock(block.data(), nullptr, kGoldenBlock);
        out.insert(out.end(), block.begin(), block.end());
    }
    return out;
}

/// FNV-1a de 64 bits sobre los bits de las muestras: bit a bit, sin tolerancia.
inline uint64_t fnv1aBits(const std::vector<float>& samples) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (float s : samples) {
        uint32_t bits;
        std::memcpy(&bits, &s, sizeof(bits));
        for (int k = 0; k < 4; ++k) {
            h ^= (bits >> (8 * k)) & 0xffu;
            h *= 0x100000001b3ULL;
        }
    }
    return h;
}

}  // namespace wma_test
