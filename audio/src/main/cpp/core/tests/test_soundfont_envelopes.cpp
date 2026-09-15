/**
 * test_soundfont_envelopes.cpp — MINI-026: las envolventes de tsf contra SF2 §8.1.2.
 *
 * Cuatro diferencias, medidas por el spec-test (REQ-039 S3) y explicadas en absoluto:
 *
 *   1. decay y release del VOLUMEN recorrian 80,13 dB en el tiempo declarado —el `-9.226` de
 *      LinuxSampler que tsf heredo—; el spec (#36/#38) dice **100 dB** y FluidSynth usa 96.
 *      80/96 = 0,833 explicaba al decimo el "2,46 contra 2,96 dB/0,25 s" de la prueba #3.
 *   2. el ataque del MOD ENV escalaba por velocity (`attack * (145 - vel) / 144`): un SFZ-ismo.
 *      A v=127 duraba 1/8 del generador.
 *   3. el ataque del mod env era lineal; el spec (#26, §9.1.7) lo pide **convexo**. No da la
 *      formula: la referencia (FluidSynth 2.6.0, `fluid_convex`) es 1 + (40/96) log10(t/T), y el
 *      spec-test #2 la mide (48 % a 50 ms, 60 % a 100 ms, 88 % a 500 ms de un ataque de 1 s).
 *   4. la release del mod env iba a `-level / T`: desde el sustain al 50 % tardaba T entero, y el
 *      spec (#30) dice "100 % de cambio en el tiempo declarado" — la mitad.
 *
 * EL ORACULO ES LA FORMULA, no el motor: un font generado con los generadores exactos y una
 * senoide de periodo conocido, renderizado por el camino de produccion
 * (`channelNoteOnWithModulators`). Nivel = RMS de 20 ms en dB; pitch = cruces por cero. Las
 * tolerancias son las del observable (una ventana de 20 ms sobre una pendiente de 100 dB/s
 * promedia 2 dB; un hop de pitch sobre una rampa de 1200 c/s, ~25 c), no las del defecto: cada
 * defecto esta a 5x o mas de su tolerancia, y el mutante que lo restaura lo demuestra.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "../../engines/SoundFontModulatorTable.h"
#include "../../engines/SoundFontNoteOn.h"
#include "support/MidiSpecHarness.h"
#include "support/MinimalSoundFont.h"
#include "tsf.h"

using wma::sfmod::channelNoteOnWithModulators;
using wma::sfmod::ModulatorTable;
using wma_test::makeMinimalSoundFont;
using wma_test::sf2::ExtraGenerator;
using wma_test::sf2::PitchGenerators;

namespace {

constexpr int kRate = 44100;
constexpr int kPeriod = 100;   // 441 Hz en la raiz
constexpr int kRoot = 60;
constexpr double kRootHz = static_cast<double>(kRate) / kPeriod;

// Generadores de §8.1.2 que este archivo usa, por numero.
constexpr uint16_t kGenModEnvToPitch = 7;
constexpr uint16_t kGenAttackModEnv = 26;
constexpr uint16_t kGenSustainModEnv = 29;
constexpr uint16_t kGenReleaseModEnv = 30;
constexpr uint16_t kGenAttackVolEnv = 34;
constexpr uint16_t kGenHoldVolEnv = 35;
constexpr uint16_t kGenDecayVolEnv = 36;
constexpr uint16_t kGenSustainVolEnv = 37;
constexpr uint16_t kGenReleaseVolEnv = 38;

constexpr int16_t kInstant = -12000;   // 1 ms: el "instantaneo" convencional del spec
constexpr int16_t kOneSecond = 0;      // timecents 0 = 1 s

/// Renderiza `seconds` de una nota en la raiz con los generadores dados; el note-off cae a
/// `noteOffSec` (o nunca, si es <= 0). Velocity en 0..1 como en el camino de produccion.
std::vector<float> render(const std::vector<ExtraGenerator>& gens, double seconds, double noteOffSec,
                          float velocity = 1.0f) {
    PitchGenerators withSine;
    withSine.sinePeriod = kPeriod;
    const auto bytes = makeMinimalSoundFont(kRate, true, -1, -1, {}, 0, withSine, gens);
    tsf* sf = tsf_load_memory(bytes.data(), static_cast<int>(bytes.size()));
    if (!sf) return {};
    ModulatorTable table;
    table.buildFromFontBytes(bytes.data(), bytes.size());
    tsf_set_output(sf, TSF_STEREO_INTERLEAVED, kRate, 0.0f);
    tsf_set_max_voices(sf, 8);
    tsf_channel_set_presetindex(sf, 0, 0);
    channelNoteOnWithModulators(sf, &table, 0, kRoot, velocity);

    const int total = static_cast<int>(seconds * kRate);
    const int offAt = noteOffSec > 0.0 ? static_cast<int>(noteOffSec * kRate) : total;
    std::vector<float> out(static_cast<size_t>(total) * 2, 0.0f);
    int done = 0;
    while (done < total) {
        const int chunk = std::min(total - done, done < offAt ? offAt - done : 256);
        tsf_render_float(sf, out.data() + static_cast<size_t>(done) * 2, chunk, 0);
        done += chunk;
        if (done == offAt) tsf_channel_note_off(sf, 0, kRoot);
    }
    tsf_close(sf);
    return out;
}

wma_test::specmidi::Signal view(const std::vector<float>& out) {
    return {out.data(), static_cast<int>(out.size() / 2), kRate};
}

double levelAt(const std::vector<float>& out, double t) {
    return wma_test::specmidi::levelDb(view(out), t, 0.020);
}

double centsAt(const std::vector<float>& out, double t) {
    const double hz = wma_test::specmidi::pitchHz(view(out), t, 0.020);
    return hz > 0.0 ? wma_test::specmidi::centsBetween(hz, kRootHz) : -1e9;
}

/// La curva convexa del ataque del mod env (fluid_convex), en fraccion del pico.
double convex(double x) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    return 1.0 + (40.0 / 96.0) * std::log10(x);
}

// Una envolvente de volumen "cuadrada": ataque y hold instantaneos, sin decay hasta que se
// pida. Con `sustain` en cB y `decay`/`release` en timecents.
std::vector<ExtraGenerator> volEnv(int16_t decayTc, int16_t sustainCb, int16_t releaseTc,
                                   int16_t holdTc = kInstant) {
    return {{kGenAttackVolEnv, kInstant}, {kGenHoldVolEnv, holdTc}, {kGenDecayVolEnv, decayTc},
            {kGenSustainVolEnv, sustainCb}, {kGenReleaseVolEnv, releaseTc}};
}

// Un mod env a pitch de +1200 c con ataque/sustain/release dados y la envolvente de volumen
// plana (sin decay: sustain 0 cB), para que el pitch se lea sobre una senoide estable.
// La release de VOLUMEN es de 4 s (2400 tc) a proposito: a los 0,5 s del note-off la senoide
// sigue a -12,5 dB y el estimador de cruces la lee; con 1 s estaria a -50 y no.
std::vector<ExtraGenerator> modEnv(int16_t attackTc, int16_t sustainPermille, int16_t releaseTc) {
    return {{kGenModEnvToPitch, 1200}, {kGenAttackModEnv, attackTc}, {kGenSustainModEnv, sustainPermille},
            {kGenReleaseModEnv, releaseTc}, {kGenAttackVolEnv, kInstant}, {kGenHoldVolEnv, kInstant},
            {kGenSustainVolEnv, 0}, {kGenReleaseVolEnv, 2400}};
}

}  // namespace

/**
 * El instrumento: con la envolvente plana la nota suena a nivel constante y en la raiz. Si esto
 * no da, nada de lo de abajo mide.
 */
TEST(SoundFontEnvelopes, TheFixtureSoundsFlatAndAtItsRoot) {
    const auto out = render(volEnv(kOneSecond, 0, kOneSecond), 1.0, -1.0);
    ASSERT_FALSE(out.empty()) << "el fixture no cargo";
    const double a = levelAt(out, 0.20), b = levelAt(out, 0.80);
    EXPECT_GT(a, -20.0) << "no sono";
    EXPECT_NEAR(a, b, 0.2) << "sin decay el nivel no puede moverse: " << a << " -> " << b;
    EXPECT_NEAR(centsAt(out, 0.5), 0.0, 1.0) << "la raiz no suena en la raiz";
}

/**
 * Spec #36: "the time for a 100% change in the Volume Envelope value during decay phase… If the
 * sustain level were -100dB, the Volume Envelope Decay Time would be the time spent in decay
 * phase". Con decay de 1 s y sustain 1000 cB (100 dB), el nivel baja 100 dB/s: -25 dB a los
 * 0,25 s, -50 a los 0,5. tsf bajaba 80,13 dB/s (-20 / -40): 5 y 10 dB fuera, contra 2 de
 * tolerancia (la ventana de 20 ms promedia 2 dB de pendiente).
 */
TEST(SoundFontEnvelopes, TheVolumeDecayCoversOneHundredDecibelsInTheDeclaredTime) {
    // Hold de 0,25 s (-2400 tc): el pico se lee a los 0,10 s y el decay arranca a los 0,25.
    const auto out = render(volEnv(kOneSecond, 1000, kOneSecond, -2400), 1.0, -1.0);
    ASSERT_FALSE(out.empty());
    const double peak = levelAt(out, 0.10);
    for (double dt : {0.25, 0.50}) {
        const double expected = -100.0 * dt;
        const double got = levelAt(out, 0.25 + dt) - peak;
        EXPECT_NEAR(got, expected, 2.0)
            << "a los " << dt << " s de decay va " << got << " dB bajo el pico; el spec dice "
            << expected << " (tsf con 80,13 dB/s daria " << -80.13 * dt << ")";
    }
}

/**
 * Spec #38: la release "ramps linearly toward zero from the current level… If the current level
 * were full scale, the Volume Envelope Release Time would be the time spent in release phase
 * until 100dB attenuation were reached". Desde el sustain (aca 0 cB: full scale), release de
 * 1 s: -25 dB a los 0,25 s del note-off, -50 a los 0,5.
 */
TEST(SoundFontEnvelopes, TheVolumeReleaseCoversOneHundredDecibelsInTheDeclaredTime) {
    const auto out = render(volEnv(kOneSecond, 0, kOneSecond), 1.5, 0.5);
    ASSERT_FALSE(out.empty());
    const double before = levelAt(out, 0.45);
    for (double dt : {0.25, 0.50}) {
        const double expected = -100.0 * dt;
        EXPECT_NEAR(levelAt(out, 0.5 + dt) - before, expected, 2.0)
            << dt << " s despues del note-off la release va " << (levelAt(out, 0.5 + dt) - before)
            << " dB bajo el sustain; el spec dice " << expected;
    }
}

/**
 * Spec #37: el sustain es una atenuacion EN CENTIBELES desde el pico: 120 cB = 12,0 dB. Es lo
 * que tsf ya hacia y FluidSynth no (lineariza 120/1000 sobre sus 960 cB: 11,5). Se afirma para
 * que el rango de 100 dB de arriba no arrastre el sustain a la misma escala.
 */
TEST(SoundFontEnvelopes, TheSustainIsAnAttenuationInCentibels) {
    // Hold de 0,25 s (-2400 tc) para poder leer el pico ANTES del decay de 50 ms.
    const auto out = render(volEnv(-5186 /* 50 ms */, 120, kOneSecond, -2400), 1.0, -1.0);
    ASSERT_FALSE(out.empty());
    const double peak = levelAt(out, 0.10);
    EXPECT_NEAR(levelAt(out, 0.50) - peak, -12.0, 0.3)
        << "sustain 120 cB: " << (levelAt(out, 0.50) - peak) << " dB bajo el pico";
}

/**
 * Spec #26: attackModEnv es "the time… until the point at which the Modulation Envelope value
 * reaches its peak" — sin ninguna dependencia de la velocity. tsf lo escalaba por
 * (145 - vel) / 144: a v=127, un ataque de 1 s duraba 0,125 s y a los 0,25 s ya estaba en
 * +1200 c. Con el ataque convexo del spec, a los 0,25 s va +899 c (0,749 del pico) y a los 0,5
 * s +1050 (0,875): las dos velocities tienen que dar lo mismo, y ninguna el pico.
 */
TEST(SoundFontEnvelopes, TheModEnvelopeAttackDoesNotScaleWithVelocity) {
    const auto loud = render(modEnv(kOneSecond, 0, kOneSecond), 1.2, -1.0, 1.0f);
    const auto soft = render(modEnv(kOneSecond, 0, kOneSecond), 1.2, -1.0, 0.5f);
    ASSERT_FALSE(loud.empty());
    ASSERT_FALSE(soft.empty());
    for (double t : {0.25, 0.50}) {
        const double expected = 1200.0 * convex(t);
        EXPECT_NEAR(centsAt(loud, t), expected, 30.0)
            << "v=127 a los " << t << " s: " << centsAt(loud, t) << " c donde la curva da " << expected;
        EXPECT_NEAR(centsAt(soft, t), expected, 30.0)
            << "v=64 a los " << t << " s: " << centsAt(soft, t) << " c donde la curva da " << expected;
        EXPECT_NEAR(centsAt(loud, t), centsAt(soft, t), 30.0)
            << "el ataque del mod env depende de la velocity";
    }
    EXPECT_NEAR(centsAt(loud, 1.10), 1200.0, 10.0) << "al segundo tiene que estar en el pico";
}

/**
 * Spec #26 / §9.1.7: el ataque es CONVEXO. La curva es la de la referencia
 * (1 + (40/96) log10(t/T)): a 50 ms el 46 %, a 100 ms el 58 %, a 500 ms el 88 %. Un ataque
 * lineal daria 5 %, 10 % y 50 %: el primer punto separa las dos curvas por 490 c.
 */
TEST(SoundFontEnvelopes, TheModEnvelopeAttackIsConvex) {
    const auto out = render(modEnv(kOneSecond, 0, kOneSecond), 1.2, -1.0);
    ASSERT_FALSE(out.empty());
    for (double t : {0.05, 0.10, 0.50}) {
        const double expected = 1200.0 * convex(t);
        EXPECT_NEAR(centsAt(out, t), expected, 40.0)
            << "a los " << t << " s: " << centsAt(out, t) << " c donde la curva convexa da " << expected
            << " (lineal daria " << 1200.0 * t << ")";
    }
}

/**
 * Spec #30: la release "linearly ramps toward zero from the current level… the time for a 100%
 * change… during release phase". Con sustain 500 (50 %) y release de 1 s, el pitch baja de
 * +600 c a 0 en 0,5 s: +300 a los 0,25 s del note-off, 0 a los 0,5. tsf iba a -level/T: +450
 * y +300.
 */
TEST(SoundFontEnvelopes, TheModEnvelopeReleaseCoversTheFullRangeInTheDeclaredTime) {
    // Ataque instantaneo y decay default (instantaneo): a los 1,0 s ya esta en el sustain.
    const auto out = render(modEnv(kInstant, 500, kOneSecond), 2.0, 1.0);
    ASSERT_FALSE(out.empty());
    ASSERT_NEAR(centsAt(out, 0.90), 600.0, 10.0) << "premisa: en el sustain del 50 % (+600 c)";
    EXPECT_NEAR(centsAt(out, 1.25), 300.0, 30.0)
        << "0,25 s tras el note-off: " << centsAt(out, 1.25) << " c donde van +300 (tsf daba +450)";
    EXPECT_NEAR(centsAt(out, 1.50), 0.0, 30.0)
        << "0,5 s tras el note-off: " << centsAt(out, 1.50) << " c donde va 0 (tsf daba +300)";
}
