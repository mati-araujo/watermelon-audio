/**
 * WD-3.3 — `OutputStage` es AHORA la unica proteccion de nivel del motor.
 *
 * POR QUE ESTE ARCHIVO EXISTE
 * ---------------------------
 * WD-3.3 saca el auto-gain por efecto de `EffectChain::processOneEffect`, y su
 * criterio 1 dice: *"La proteccion de salida queda solo en OutputStage"*. Al
 * escribir eso hay que preguntarse quien vigila lo que queda sosteniendo la
 * garantia — y la respuesta, medida, era **nadie**.
 *
 * El mutante que lo destapo: dejar `processOutput()` con el limiter, el soft
 * clip, el dither y el hard limit **borrados**, o sea sin ninguna proteccion.
 * **Sobrevivio los 891 tests de la suite.** Vaciar la capa de arriba apoyandose
 * en una capa que nadie mide es cambiar una proteccion mala por ninguna.
 *
 * QUE SE AFIRMA
 * -------------
 * 1. Acota: entra 3,7 de pico, sale ≤ 1,0.
 * 2. No es un mute: la senal sigue estando, no se resuelve acotando a cero.
 * 3. **Y no reintroduce el artefacto que WD-3.3 saca**: el limiter tiene
 *    lookahead, ataque y release, asi que su ganancia varia DENTRO del bloque —
 *    no en escalones en el borde. Se mide con el mismo instrumento de vecindad
 *    que `effects/tests/test_chain_gain.cpp`. Esta es la parte que hace que el
 *    cambio de lugar de la proteccion sea una MEJORA y no una mudanza.
 */

#include "../OutputStage.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kBlock = 256;
constexpr int kBlocks = 40;
constexpr int kNeighbourhood = 8;

/// Igual que en el test de la cadena: el corte entre las dos poblaciones
/// medidas alla (1,011 sin escalon contra 2,60-16,1 con el).
constexpr double kBoundaryRatioLimit = 1.7;

/// Amplitudes NO representables en binario a proposito. Con 2,0 o 4,0 un test
/// de nivel puede pasar por redondeo y no por comportamiento.
constexpr float kLoudAmp = 3.7f;
constexpr float kQuietAmp = 0.23f;

std::vector<float> note(float amp, int frames) {
    std::vector<float> b(static_cast<size_t>(frames) * 2);
    for (int n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / kSampleRate;
        const double env = amp * std::exp(-t / 0.25) * (1.0 - std::exp(-t / 0.002));
        const float s = static_cast<float>(env * std::sin(2.0 * M_PI * 220.0 * t));
        b[2 * n] = s;
        b[2 * n + 1] = s;
    }
    return b;
}

float peakOf(const std::vector<float>& b) {
    float p = 0.0f;
    for (float v : b) p = std::fmax(p, std::fabs(v));
    return p;
}

std::vector<float> runStage(OutputStage& stage, std::vector<float> buf, int frames) {
    for (int b = 0; b * kBlock < frames; ++b) {
        const int n = std::min(kBlock, frames - b * kBlock);
        stage.processOutput(buf.data() + static_cast<size_t>(b) * kBlock * 2, n);
    }
    return buf;
}

}  // namespace

TEST(OutputStageProtection, ALoudSignalComesOutBounded) {
    OutputStage stage;
    stage.prepare(kSampleRate, kBlock);

    const int frames = kBlock * kBlocks;
    const std::vector<float> in = note(kLoudAmp, frames);
    ASSERT_GT(peakOf(in), 1.0f) << "el montaje no exige nada al limitador";

    const std::vector<float> out = runStage(stage, in, frames);

    for (size_t i = 0; i < out.size(); ++i) {
        ASSERT_TRUE(std::isfinite(out[i])) << "no finito en " << i;
        ASSERT_LE(std::fabs(out[i]), 1.0f)
            << "la muestra " << i << " salio en " << out[i];
    }
    // Y no acotando a cero: eso tambien cumpliria lo de arriba.
    EXPECT_GT(peakOf(out), 0.5f) << "la proteccion resolvio el problema mudeando";
}

TEST(OutputStageProtection, AQuietSignalIsNotDestroyed) {
    OutputStage stage;
    stage.prepare(kSampleRate, kBlock);

    const int frames = kBlock * kBlocks;
    const std::vector<float> in = note(kQuietAmp, frames);
    ASSERT_LT(peakOf(in), 1.0f);

    const std::vector<float> out = runStage(stage, in, frames);

    // El dither suma ruido de muy bajo nivel a proposito, asi que no se pide
    // igualdad: se pide que el NIVEL sobreviva.
    EXPECT_GT(peakOf(out), peakOf(in) * 0.9f)
        << "una senal que no necesitaba proteccion salio atenuada";
}

/**
 * La razon de ser de la mudanza: `OutputStage` acota SIN dejar un escalon en el
 * borde del bloque. Mismo instrumento y mismo corte que el test de la cadena.
 */
TEST(OutputStageProtection, BoundingTheSignalLeavesNoBlockBoundaryStep) {
    OutputStage stage;
    stage.prepare(kSampleRate, kBlock);

    const int frames = kBlock * kBlocks;
    const std::vector<float> out = runStage(stage, note(kLoudAmp, frames), frames);

    auto neighbourRatio = [&out](size_t k) -> double {
        const double d = std::fabs(out[k] - out[k - 2]);
        double m = 0.0;
        for (int j = -kNeighbourhood; j <= kNeighbourhood; ++j) {
            if (j == 0) continue;
            const size_t kk = k + static_cast<size_t>(j) * 2;
            if (kk < 2 || kk >= out.size()) continue;
            m = std::fmax(m, std::fabs(out[kk] - out[kk - 2]));
        }
        return (m > 1e-9) ? d / m : -1.0;
    };

    double worstEdge = 0.0, worstInterior = 0.0;
    int measured = 0;
    for (int b = 1; b < kBlocks; ++b) {
        const size_t k = static_cast<size_t>(b) * kBlock * 2;
        for (int ch = 0; ch < 2; ++ch) {
            const double e = neighbourRatio(k + static_cast<size_t>(ch));
            const double c = neighbourRatio(k + static_cast<size_t>(kBlock / 3) * 2
                                              + static_cast<size_t>(ch));
            if (e >= 0.0) { worstEdge = std::fmax(worstEdge, e); ++measured; }
            if (c >= 0.0) worstInterior = std::fmax(worstInterior, c);
        }
    }

    ASSERT_GE(measured, 2 * (kBlocks - 1)) << "el barrido no midio todos los bordes";
    EXPECT_LE(worstEdge, kBoundaryRatioLimit)
        << "el limitador de salida dejo un escalon en un borde de bloque ("
        << worstEdge << "x sus vecinas); el control interior dio " << worstInterior;
    EXPECT_LE(worstInterior, kBoundaryRatioLimit)
        << "el instrumento acusa a una muestra interior: la medida esta sesgada";
    RecordProperty("worst_boundary_ratio", std::to_string(worstEdge));
}
