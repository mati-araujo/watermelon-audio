/**
 * @file test_initial_attenuation_generator.cpp
 * @brief MINI-024 — `initialAttenuation` (SF2 generador 48) entra al nivel a 0,4 dB por dB
 *        declarado, el spec-quirk que el SoundFont-Spec-Test pide emular y FluidSynth aplica.
 *
 * EL ORACULO ES LA FORMULA DEL SPEC-QUIRK, NO EL MOTOR: `dB = 0,4 · cB / 10`. Un font
 * generado con 100 cB en la zona del instrumento tiene que sonar exactamente 4,0 dB mas bajo
 * que el mismo font sin el generador. Hasta MINI-024 tsf aplicaba 0,01 dB/cB (1,0 dB): todo
 * preset con atenuacion declarada sonaba mas fuerte de lo programado, y las capas que su autor
 * puso 20 dB abajo quedaban 5 dB abajo. La prueba #11 del spec-test lo mide contra FluidSynth
 * (`SfSpecConformance.TheTwentyTwoAgainstTheirOracles`, fila S); este archivo lo afirma sobre
 * un font propio con un numero que no sale de ningun sintetizador.
 *
 * LO QUE ESTE ARCHIVO NO AFIRMA, a proposito: que los MODULADORES a `initialAttenuation`
 * (default #1 y los del archivo) entren SIN el factor. Eso lo afirman las escaleras de #13
 * (`TheVelocityLaddersFollowWhatTheFileDeclares`: 2,34 dB de 127 -> 111, el spec) y el gemelo a
 * v=64 de `TouchExpressionChannelLayer`; el mutante "0,4 tambien en el modulador" murio ahi,
 * con #11 intacta. Aca no hay moduladores: el fixture no declara ninguno y la tabla no se usa.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "support/MinimalSoundFont.h"
#include "tsf.h"

using wma_test::makeMinimalSoundFont;

namespace {

constexpr int kRate = 44100;     // NO 48000: es el default de medio mundo y no distingue
constexpr int kFrames = 8192;
constexpr int kKey = 60;
constexpr float kVelocity = 1.0f;  // velocity maxima: el termino de velocity de tsf vale 0 dB

/** Renderiza UNA nota de un font con `attenuationCentibels` declarados y devuelve el estereo. */
std::vector<float> renderWith(int attenuationCentibels) {
    // looping = true: el one-shot dura 4 ms y no da para medir nivel (ver el fixture).
    auto bytes = makeMinimalSoundFont(22050, true, -1, -1, {}, attenuationCentibels);
    tsf* sf = tsf_load_memory(bytes.data(), static_cast<int>(bytes.size()));
    EXPECT_NE(sf, nullptr) << "el fixture con " << attenuationCentibels << " cB no carga";
    std::vector<float> out(static_cast<size_t>(kFrames) * 2, 0.0f);
    if (!sf) return out;
    tsf_set_output(sf, TSF_STEREO_INTERLEAVED, kRate, 0.0f);
    tsf_channel_set_presetindex(sf, 0, 0);
    tsf_channel_note_on(sf, 0, kKey, kVelocity);
    tsf_render_float(sf, out.data(), kFrames, 0);
    tsf_close(sf);
    return out;
}

double rmsDb(const std::vector<float>& x) {
    double acc = 0.0;
    for (float v : x) acc += static_cast<double>(v) * v;
    return 10.0 * std::log10(acc / static_cast<double>(x.size()) + 1e-30);
}

/** El spec-quirk: 0,4 dB por dB declarado (10 cB = 1 dB). Calculado ACA, no leido del motor. */
double expectedDropDb(int centibels) { return 0.4 * centibels / 10.0; }

}  // namespace

// ---------------------------------------------------------------------------
// AC-M024.3 — 100 cB declarados bajan 4,0 dB, y son una GANANCIA: la misma onda escalada
// ---------------------------------------------------------------------------
TEST(InitialAttenuationGenerator, OneHundredCentibelsLowerTheLevelByFourDecibels) {
    const auto plain = renderWith(0);
    const auto attenuated = renderWith(100);
    ASSERT_GT(rmsDb(plain), -40.0) << "el render sin atenuacion salio en silencio: el fixture no suena";

    const double drop = rmsDb(plain) - rmsDb(attenuated);
    EXPECT_NEAR(drop, expectedDropDb(100), 0.1)
        << "100 cB declarados bajaron " << drop << " dB; el spec-quirk manda " << expectedDropDb(100)
        << " (0,4 por dB). Con el 0,1 de tsf daria 1,0";

    // Y es SOLO una ganancia: muestra a muestra, la atenuada es la plana escalada. Si el
    // generador tocara otra cosa (el filtro, la envolvente), esto lo veria y el RMS no.
    const double gain = std::pow(10.0, -expectedDropDb(100) / 20.0);
    double worst = 0.0;
    for (size_t i = 0; i < plain.size(); ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(attenuated[i]) - plain[i] * gain));
    EXPECT_LT(worst, 1e-4) << "la atenuada no es la plana escalada por " << gain
                           << ": el generador cambio algo mas que el nivel (peor muestra: " << worst << ")";
}

// ---------------------------------------------------------------------------
// El gemelo: sin el generador, dos renders son IDENTICOS. Sostiene la comparacion de arriba:
// si el instrumento no fuera determinista, "la atenuada es la plana escalada" no diria nada.
//
// 🔴 Lo que este gemelo NO puede ver, medido con un mutante: un fixture que escriba el
// generador con 0 cB. Es un mutante EQUIVALENTE — 0 cB declarados y "sin generador" son lo
// mismo por definicion del SF2 (default 0) — asi que sobrevive por construccion, no por
// debilidad del test. La primera version de este comentario prometia lo contrario.
// ---------------------------------------------------------------------------
TEST(InitialAttenuationGenerator, WithoutTheGeneratorNothingChanges) {
    const auto a = renderWith(0);
    const auto b = renderWith(0);
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size() * sizeof(float)))
        << "dos renders del mismo fixture sin atenuacion difieren: el instrumento no es determinista";
}

// ---------------------------------------------------------------------------
// La escalera de #11 del spec-test, sobre el font propio: 0/5/10/.../30 dB declarados dan
// pasos de EXACTAMENTE 2,0 dB. Es la misma afirmacion que la fila S de las 22, sin FluidSynth.
// ---------------------------------------------------------------------------
TEST(InitialAttenuationGenerator, EachFiveDeclaredDecibelsAreTwoRealOnes) {
    double previous = rmsDb(renderWith(0));
    for (int cb = 50; cb <= 300; cb += 50) {
        const double level = rmsDb(renderWith(cb));
        EXPECT_NEAR(previous - level, 2.0, 0.1)
            << "de " << (cb - 50) << " a " << cb << " cB el paso fue " << (previous - level)
            << " dB; el README del spec-test dice \"exactly 2 dB\"";
        previous = level;
    }
}

// ---------------------------------------------------------------------------
// El tope: 1440 cB (144 dB declarados) son 57,6 dB reales, y mas de 1440 se CLAMPEA ahi.
// Con el factor viejo el tope era 14,4; un factor nuevo con el tope viejo recortaria todo
// preset con mas de 36 dB declarados, y GeneralUser llega a 72 (720 cB).
// ---------------------------------------------------------------------------
TEST(InitialAttenuationGenerator, TheCeilingIsFiftySevenPointSixDecibelsAndClampsAbove) {
    const double plain = rmsDb(renderWith(0));
    const double atCeiling = rmsDb(renderWith(1440));
    EXPECT_NEAR(plain - atCeiling, expectedDropDb(1440), 0.2)
        << "1440 cB bajaron " << (plain - atCeiling) << " dB; el tope del spec-quirk es 57,6";

    const auto ceiling = renderWith(1440);
    const auto beyond = renderWith(2000);
    EXPECT_EQ(0, std::memcmp(ceiling.data(), beyond.data(), ceiling.size() * sizeof(float)))
        << "2000 cB no se clampeo a 1440: el tope no es 57,6 dB";
}
