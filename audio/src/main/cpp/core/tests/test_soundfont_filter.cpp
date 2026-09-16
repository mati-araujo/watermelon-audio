/**
 * test_soundfont_filter.cpp — REQ-041 S1: el low-pass de la voz contra FluidSynth 2.6.0.
 *
 * Tres convenciones del filtro de tsf diferian de la referencia versionada, y dos de ellas
 * eran los numeros que el trinquete de REQ-039 tenia anotados sin dueño (#10 a 46,22 dB en la
 * Q mas alta; el "-1,43 dB global" de #1/#11 desde MINI-026):
 *
 *   1. q = 10^((Q_dB - 3,01)/20), con Q_dB = clip(initialFilterQ/10, 0, 96)
 *      (`fluid_iir_filter_q_from_dB`): Q = 0 es Butterworth, -3,01 dB en fc y SIN joroba. tsf
 *      usaba 10^(Q/20): +3 dB de resonancia a cualquier Q.
 *   2. la voz lleva un termino de nivel 1/sqrt(q) (SF2 p. 59: "gain reduction equal to half
 *      the height of the resonance peak"; `fluid_iir_filter_impl.cpp:86-101` lo mete en los
 *      coeficientes b). A Q = 0 es +1,505 dB EN TODA VOZ. tsf no lo tenia.
 *   3. el corte se clampea a [5 Hz, 0,45·sr] y el filtro CORRE SIEMPRE (a rates bajos es el
 *      anti-alias, `fluid_iir_filter_calc`). tsf lo apagaba si fc >= 0,499·sr, y con q = 0,707
 *      el filtro en 0,45·sr no es un bypass: -3,01 dB en fc.
 *
 * EL ORACULO ES LA FORMULA de FluidSynth 2.6.0 tal cual (decision 3 del spec), sobre un font
 * minimo con una senoide de frecuencia CONOCIDA y renderizado por el camino de produccion
 * (`channelNoteOnWithModulators`). La frecuencia del tono se elige con la tasa del `shdr`
 * (MINI-025): a la tecla raiz el sample suena a `tasa_del_header / periodo`, asi que un tono
 * EXACTAMENTE en fc, en fc/100 o en 0,45·sr es un numero del test, no del motor. Un tono en
 * 0,45·sr sale con razon de re-muestreo 45 entera, o sea sin interpolar: exacto.
 *
 * "Relativo al render con el filtro neutralizado" (AC-041.1) es literal: el mismo font, la
 * misma nota, con `tsf_ext_voice_bypass_lowpass` sobre la voz. Es la unica forma de aislar el
 * termino de nivel de todo lo demas (velocity, paneo, atenuacion), y cualquiera de esos puede
 * cambiar sin que este archivo se ponga rojo por eso.
 *
 * Los mutantes, nombrados ANTES de implementar (tarea 1.1/1.2), y donde tiene que morir cada uno:
 *   M1 sin el -3,01 en q          -> rojo en la joroba (Q - 3,01 dB en fc), y en el +1,5
 *   M2 sin el 1/sqrt(q)           -> rojo en el nivel relativo al neutral
 *   M3 1/sqrt(q) sobre 10^(Q/20)  -> rojo en el +1,505 (daria 0 a Q = 0)
 *   M4 el `active = fc < 0,499` de hoy -> rojo a 22 050 (el +1,5 ausente) y en el clamp
 */
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "../../engines/SoundFontModulatorTable.h"
#include "../../engines/SoundFontNoteOn.h"
#include "support/MidiSpecHarness.h"
#include "support/MinimalSoundFont.h"
#include "tsf.h"
#include "tsf_ext.h"

using wma::sfmod::channelNoteOnWithModulators;
using wma::sfmod::ModulatorTable;
using wma_test::makeMinimalSoundFont;
using wma_test::sf2::ExtraGenerator;
using wma_test::sf2::kGenInitialFilterFc;
using wma_test::sf2::kGenInitialFilterQ;
using wma_test::sf2::kSrcNone;
using wma_test::sf2::Modulator;
using wma_test::sf2::ModulatorPlacement;
using wma_test::sf2::PitchGenerators;

namespace {

constexpr int kPeriod = 100;  // muestras por periodo de la senoide del sample
constexpr int kRoot = 60;

/// SF2 §8.1.3: cents absolutos -> Hz (8,176 Hz = tecla 0). Es la formula del spec, no del motor.
double centsToHz(double cents) { return 8.176 * std::pow(2.0, cents / 1200.0); }

/// Lo que FluidSynth 2.6.0 hace con `initialFilterQ` (cB): Q_dB = clip(cB/10, 0, 96) y
/// q = 10^((Q_dB - 3,01)/20). El termino de nivel de la voz es 1/sqrt(q) = -(Q_dB - 3,01)/2 dB.
double gainTermDb(double qCentibels) {
    double qDb = qCentibels / 10.0;
    if (qDb < 0.0) qDb = 0.0;
    if (qDb > 96.0) qDb = 96.0;
    return -(qDb - 3.01) / 2.0;
}

struct Tone {
    double hz = 0.0;                    ///< la frecuencia del tono que suena (tasa del header / periodo)
    int rate = 48000;                   ///< tasa de salida
    int filterFcCents = -1;             ///< generador 8; -1 = no se escribe (tsf: 13500, abierto)
    int filterQCentibels = -1;          ///< generador 9; -1 = no se escribe (0)
    std::vector<Modulator> mods;        ///< moduladores en la zona global del instrumento
    bool neutral = false;               ///< apagar el filtro de la voz: el render NEUTRAL
};

Tone tone(double hz, int rate = 48000, int filterFcCents = -1, int filterQCentibels = -1) {
    Tone t;
    t.hz = hz;
    t.rate = rate;
    t.filterFcCents = filterFcCents;
    t.filterQCentibels = filterQCentibels;
    return t;
}

/// Renderiza `seconds` s del tono (canal 0, tecla raiz, velocity 1,0 = MIDI 127, donde los
/// defaults #1/#2 aportan cero) y devuelve el estereo entrelazado.
std::vector<float> render(const Tone& t, double seconds) {
    PitchGenerators withSine;
    withSine.sinePeriod = kPeriod;
    std::vector<ExtraGenerator> gens;
    if (t.filterFcCents >= 0) gens.push_back({kGenInitialFilterFc, static_cast<int16_t>(t.filterFcCents)});
    if (t.filterQCentibels >= 0) gens.push_back({kGenInitialFilterQ, static_cast<int16_t>(t.filterQCentibels)});
    ModulatorPlacement mods;
    mods.instrumentGlobal = t.mods;
    const auto headerRate = static_cast<uint32_t>(std::llround(t.hz * kPeriod));
    const auto bytes = makeMinimalSoundFont(headerRate, true, -1, -1, mods, 0, withSine, gens);
    tsf* sf = tsf_load_memory(bytes.data(), static_cast<int>(bytes.size()));
    if (!sf) return {};
    ModulatorTable table;
    table.buildFromFontBytes(bytes.data(), bytes.size());
    tsf_set_output(sf, TSF_STEREO_INTERLEAVED, t.rate, 0.0f);
    tsf_set_max_voices(sf, 8);
    tsf_channel_set_presetindex(sf, 0, 0);
    channelNoteOnWithModulators(sf, &table, 0, kRoot, 1.0f);
    if (t.neutral) {
        tsf_ext_started_voice started[4];
        const int n = tsf_ext_voices_started_by_last_note_on(sf, started, 4);
        for (int i = 0; i < n && i < 4; ++i) tsf_ext_voice_bypass_lowpass(sf, started[i].voiceIndex);
    }
    const int total = static_cast<int>(seconds * t.rate);
    std::vector<float> out(static_cast<size_t>(total) * 2, 0.0f);
    for (int done = 0; done < total; done += 256) {
        const int chunk = std::min(256, total - done);
        tsf_render_float(sf, out.data() + static_cast<size_t>(done) * 2, chunk, 0);
    }
    tsf_close(sf);
    return out;
}

/// RMS mono en dB de [t0, t0 + dur).
double levelDb(const std::vector<float>& out, int rate, double t0, double dur) {
    const wma_test::specmidi::Signal s{out.data(), static_cast<int>(out.size() / 2), rate};
    return wma_test::specmidi::rmsDbOf(s, t0, dur);
}

/// El nivel de un tono estable: 0,40 s desde los 0,10 s (el transitorio del filtro ya paso).
double toneLevel(const Tone& t) {
    const auto out = render(t, 0.5);
    EXPECT_FALSE(out.empty()) << "el fixture no cargo";
    return out.empty() ? -200.0 : levelDb(out, t.rate, 0.10, 0.40);
}

/// El termino de nivel MEDIDO: el tono con filtro contra el mismo tono con el filtro apagado.
double measuredGainTermDb(Tone t) {
    const double with = toneLevel(t);
    t.neutral = true;
    const double neutral = toneLevel(t);
    EXPECT_GT(neutral, -40.0) << "el render neutral no sono";
    return with - neutral;
}

/// Un modulador "sin controlador" (amount tal cual) hacia un generador: la forma de declarar un
/// valor que el generador solo no alcanza (fc > 13500 c, fc < 1500 c, Q > 960 cB).
Modulator constant(uint16_t dest, int16_t amount) {
    Modulator m;
    m.srcOper = kSrcNone;
    m.destOper = dest;
    m.amount = amount;
    return m;
}

}  // namespace

/**
 * El instrumento: el tono suena donde el test dice (tasa del header / periodo), y la sonda
 * neutral cambia algo. Si esto no da, nada de lo de abajo mide.
 */
TEST(SoundFontFilter, TheToneSoundsWhereTheTestSaysAndTheNeutralProbeBites) {
    Tone t = tone(1000.0, 48000);
    const auto out = render(t, 0.5);
    ASSERT_FALSE(out.empty());
    const wma_test::specmidi::Signal s{out.data(), static_cast<int>(out.size() / 2), t.rate};
    const double hz = wma_test::specmidi::pitchHz(s, 0.10, 0.25);
    EXPECT_NEAR(hz, 1000.0, 2.0) << "la tasa del header no fija la frecuencia del tono";
    EXPECT_GT(levelDb(out, t.rate, 0.10, 0.40), -30.0) << "no sono";

    // Con el corte en 1 kHz el tono de 4 kHz esta 2 octavas arriba: el filtro lo hunde ~24 dB.
    // Si la sonda neutral no lo devuelve, el "relativo al neutral" de abajo mide contra nada.
    Tone hi = tone(4000.0, 48000, 8321);  // corte en 8321 c = 1000 Hz, el tono dos octavas arriba
    Tone hiNeutral = hi;
    hiNeutral.neutral = true;
    EXPECT_LT(toneLevel(hi) - toneLevel(hiNeutral), -18.0)
        << "la sonda neutral no apaga el filtro: el tono filtrado y el neutral suenan igual";
}

/**
 * AC-041.1 (convenciones 1 y 2) — el TERMINO DE NIVEL. Con fc = 4 kHz y un tono en fc/100
 * (donde el filtro es 0 dB a cualquier Q), el nivel relativo al render neutral es 1/sqrt(q):
 * +1,505 / -8,50 / -46,49 dB para Q = 0 / 200 / 960 cB.
 *
 *   M2 (sin 1/sqrt(q)): 0 dB en los tres -> rojo aca.
 *   M3 (1/sqrt(q) sobre 10^(Q/20)): 0 / -10 / -48 -> rojo aca, empezando por el +1,5.
 *   M1 (sin -3,01): q = 1 a Q = 0, o sea 0 dB -> rojo aca tambien (y en la joroba, abajo).
 */
TEST(SoundFontFilter, TheVoiceLevelIsOneOverSqrtQ) {
    constexpr int kFc = 10721;                  // 3999 Hz
    const double fcOver100 = centsToHz(kFc) / 100.0;
    const int kQ[] = {0, 200, 960};
    const double kExpected[] = {+1.505, -8.495, -46.495};
    for (int i = 0; i < 3; ++i) {
        Tone t = tone(fcOver100, 48000, kFc, kQ[i]);
        const double term = measuredGainTermDb(t);
        std::printf("  [REQ-041] Q = %3d cB: nivel relativo al neutral %+7.3f dB (FluidSynth: %+7.3f)\n",
                    kQ[i], term, gainTermDb(kQ[i]));
        EXPECT_NEAR(term, kExpected[i], 0.1)
            << "Q = " << kQ[i] << " cB: el termino 1/sqrt(q) de la voz no es el de FluidSynth 2.6.0";
    }
}

/**
 * AC-041.1 (convencion 1) — LA JOROBA. Un tono EN fc contra el mismo tono en fc/100 queda a
 * Q_dB - 3,01: a Q = 0 es Butterworth (-3,01 en fc, sin joroba); a Q = 200 cB, +16,99. El
 * termino de nivel esta en los dos tonos y se cancela: esto mide SOLO la forma del filtro.
 *
 *   M1 (sin -3,01): q = 10^(Q/20) -> 0,00 y +20,00 -> rojo aca.
 *
 * Q = 960 cB no se mide en fc a proposito: con q = 44 617 el pico tiene 0,09 Hz de ancho y 3,5 s
 * de constante de tiempo; su termino de nivel ya esta afirmado arriba.
 */
TEST(SoundFontFilter, TheToneAtFcSitsQMinusThreeDbAboveTheToneFarBelow) {
    constexpr int kFc = 10721;
    const double fcHz = centsToHz(kFc);
    const int kQ[] = {0, 200};
    for (int q : kQ) {
        const double atFc = toneLevel(tone(fcHz, 48000, kFc, q));
        const double farBelow = toneLevel(tone(fcHz / 100.0, 48000, kFc, q));
        const double hump = atFc - farBelow;
        std::printf("  [REQ-041] Q = %3d cB: tono en fc - tono en fc/100 = %+7.3f dB (spec: %+7.3f)\n",
                    q, hump, q / 10.0 - 3.01);
        EXPECT_NEAR(hump, q / 10.0 - 3.01, 0.2)
            << "Q = " << q << " cB: la respuesta en fc no es la de q = 10^((Q - 3,01)/20)";
    }
}

/**
 * Convencion 1, la coercion: Q se clipea a 0..96 dB (FluidSynth `fluid_clip(q_dB, 0, 96)`) ANTES
 * de la formula, tambien cuando llega por modulador (que es el unico camino por el que puede
 * salirse: el generador ya viene saturado a 0..960 por GEN_INT_LIMITQ). 960 + 500 cB suena como
 * 960; 0 - 500 suena como 0. Sin el clip, 1460 cB daria -71,5 dB y -500 daria +26,5.
 *
 * Y de paso: la Q por GENERADOR y la Q por MODULADOR son el mismo Q (tsf_note_on y
 * tsf_ext_voice_set_filter_q son dos escrituras del mismo campo): un arreglo en una sola
 * seria un mutante de "arreglo de dos mitades".
 */
TEST(SoundFontFilter, QIsClippedToZeroNinetySixDbOnEveryPath) {
    constexpr int kFc = 10721;
    const double fcOver100 = centsToHz(kFc) / 100.0;

    Tone byGenerator = tone(fcOver100, 48000, kFc, 200);
    Tone byModulator = tone(fcOver100, 48000, kFc, -1);
    byModulator.mods.push_back(constant(kGenInitialFilterQ, 200));
    const double gen = measuredGainTermDb(byGenerator), mod = measuredGainTermDb(byModulator);
    EXPECT_NEAR(gen, mod, 0.05) << "Q por generador (" << gen << ") y por modulador (" << mod
                                << ") no dan el mismo nivel: dos escrituras, dos formulas";

    Tone over = tone(fcOver100, 48000, kFc, 960);
    over.mods.push_back(constant(kGenInitialFilterQ, 500));   // 1460 cB -> 96 dB
    EXPECT_NEAR(measuredGainTermDb(over), gainTermDb(960), 0.1) << "Q > 960 cB no se clipea a 96 dB";

    Tone under = tone(fcOver100, 48000, kFc, 0);
    under.mods.push_back(constant(kGenInitialFilterQ, -500));  // -500 cB -> 0 dB
    EXPECT_NEAR(measuredGainTermDb(under), gainTermDb(0), 0.1) << "Q < 0 no se clipea a 0 dB";
}

/**
 * AC-041.2 (convencion 3) — EL FILTRO NUNCA SE APAGA. Con el fc default (13500 c = 19 912 Hz)
 * el +1,505 esta en las cuatro tasas. Hoy tsf lo apaga si fc >= 0,499·sr: a 22 050 Hz el default
 * es 0,90·sr y la voz se queda sin el termino.
 *
 *   M4 (`active = fc < 0,499`): 0 dB a 22 050 -> rojo aca.
 */
TEST(SoundFontFilter, ThePlusOneAndAHalfIsThereAtEveryRate) {
    const int kRates[] = {22050, 44100, 48000, 96000};
    for (int rate : kRates) {
        Tone t = tone(100.0, rate);  // fc default, Q default
        const double term = measuredGainTermDb(t);
        std::printf("  [REQ-041] %5d Hz, fc default: nivel relativo al neutral %+7.3f dB\n", rate, term);
        EXPECT_NEAR(term, +1.505, 0.1) << rate << " Hz: el filtro se apago (o perdio su termino de nivel)";
    }
}

/**
 * AC-041.2 (convencion 3) — EL CLAMP SUPERIOR. Con un corte declarado por ENCIMA de 0,45·sr en
 * las cuatro tasas (13500 c + 2400 c por modulador = 79,6 kHz), el corte efectivo es 0,45·sr y un
 * tono AHI sale a -3,01 dB del tono de 100 Hz: el filtro es el anti-alias de FluidSynth.
 *
 *   M4: con fc/sr > 0,499 el filtro se apaga -> 0,00 en las cuatro -> rojo aca.
 *   Sin el clamp (tan(pi·1,66) del corte en 79,6 kHz): un filtro sin sentido -> rojo aca.
 */
TEST(SoundFontFilter, TheCutoffIsClampedToPointFourFiveOfTheRate) {
    const int kRates[] = {22050, 44100, 48000, 96000};
    for (int rate : kRates) {
        Tone at45 = tone(0.45 * rate, rate, 13500);
        at45.mods.push_back(constant(kGenInitialFilterFc, 2400));
        Tone low = at45;
        low.hz = 100.0;
        const double d = toneLevel(at45) - toneLevel(low);
        std::printf("  [REQ-041] %5d Hz, fc declarado 15900 c: tono en 0,45·sr - tono en 100 Hz = %+7.3f dB\n",
                    rate, d);
        EXPECT_NEAR(d, -3.01, 0.3) << rate << " Hz: el corte no esta clampeado a 0,45·sr";
    }
}

/**
 * AC-041.2 (convencion 3) — EL CLAMP INFERIOR. Un corte declarado por DEBAJO de 5 Hz (1500 c del
 * generador, el minimo, menos 3600 c por modulador = 2,4 Hz; es lo que el default #2 hace a
 * velocity baja sobre un corte cerrado) se clampea a 5 Hz: un tono de 5 Hz sale a -3,01 + 1,505
 * = -1,505 dB del neutral. Con el corte en 2,4 Hz el tono estaria a 2,05·fc: -12,5 dB.
 */
TEST(SoundFontFilter, TheCutoffIsClampedToFiveHertz) {
    Tone t = tone(5.0, 48000, 1500);
    t.mods.push_back(constant(kGenInitialFilterFc, -3600));
    const auto with = render(t, 3.0);
    t.neutral = true;
    const auto neutral = render(t, 3.0);
    ASSERT_FALSE(with.empty());
    ASSERT_FALSE(neutral.empty());
    // 5 Hz: dos segundos de ventana son diez periodos, desde 1 s (la constante de tiempo del
    // filtro en 5 Hz es ~0,05 s).
    const double d = levelDb(with, t.rate, 1.0, 2.0) - levelDb(neutral, t.rate, 1.0, 2.0);
    std::printf("  [REQ-041] fc declarado 2,4 Hz: tono de 5 Hz relativo al neutral %+7.3f dB (esperado -1,505)\n", d);
    EXPECT_NEAR(d, -3.01 + 1.505, 0.3) << "el corte no esta clampeado a 5 Hz";
}
