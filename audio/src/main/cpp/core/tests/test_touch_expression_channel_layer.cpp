/**
 * test_touch_expression_channel_layer.cpp — REQ-039 S3, tarea 3.3: AC-039.6.
 *
 * LA DECISION QUE ESTO AFIRMA (D5 del stage doc de S3, y R-MOT-39)
 * -----------------------------------------------------------------
 * Los diez moduladores por defecto del spec estan en la tabla, pero los OCHO de fuente
 * de canal (#3 presion, #4 CC1, #5 CC7, #6 CC10, #7 CC11, #8 CC91, #9 CC93, #10 rueda)
 * NO se evaluan al disparar la nota: la capa de canales de tsf (`midiVolume`,
 * `midiExpression`, `panOffset`, `pitchWheel`) ES su implementacion en este REQ, con la
 * aproximacion declarada. Dos hechos medidos el 2026-09-11 lo deciden: produccion no
 * tiene camino de rueda ni de CC —el unico `tsf_channel_*` que cruza la fachada es
 * `tsf_channel_set_volume`—, y ninguna prueba del spec-test mide la curva de CC7/CC11.
 *
 * Lo que hay que garantizar, entonces, es que los defaults de canal **no dupliquen**
 * lo que tsf ya aplica, y que la expresion por toque —que entra por la GANANCIA del
 * canal y no por CC7/CC11— quede ortogonal a #5/#7. Tres afirmaciones:
 *
 *   (i)   con la tabla puesta, el audio es IDENTICO muestra a muestra al de tsf pelado,
 *         en reposo Y con el canal movido (CC7, CC10, CC11, rueda) — a velocity 127,
 *         donde los defaults de NOTA (#1, #2) valen cero y lo unico que podria
 *         distinguir los dos renders es un default de canal evaluado de mas.
 *         Y el gemelo: a velocity 64 los dos renders DIFIEREN, porque ahi el default
 *         #1 si actua — si no difirieran, la comparacion de arriba seria vacia.
 *   (ii)  la expresion por toque escala el nivel LINEALMENTE (0,75 / 0,5 / 0,25), con
 *         la tabla activa. El mutante que la haga entrar por CC11 (o CC7) pasa por
 *         `tsf_channel_midi_control`, que aplica `(vol · expr)^3`: 0,42 / 0,125 /
 *         0,016 — rojo en los tres puntos. Y como el note-on con moduladores no toca
 *         la ganancia del canal, tampoco puede haber una segunda atenuacion encima.
 *   (iii) los ocho defaults de canal estan CONTADOS CON NOMBRE como
 *         `sourceNotAtNoteOn` por el clasificador de la tabla, y #1/#2 no. Si alguien
 *         empieza a evaluar uno (o deja de evaluar #1), esto se pone rojo: la
 *         decision es "en voz alta", no una omision.
 *
 * 🔴 Lo que esto NO prueba, a proposito: que la curva `^3` de tsf para CC7/CC11 sea
 * la del spec (concava, 960 cB). No hay oraculo externo para eso (hecho 6 del stage
 * doc), y afirmarlo contra nuestra propia formula seria el criterio de muerte del REQ.
 */
#include "support/MinimalSoundFont.h"

#include "engines/SoundFontEngine.h"
#include "engines/SoundFontManager.h"
#include "engines/SoundFontModulatorTable.h"
#include "engines/SoundFontNoteOn.h"
#include "tsf.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using wma::sfmod::channelNoteOnWithModulators;
using wma::sfmod::ModulatorTable;
using wma_test::makeMinimalSoundFont;

namespace {

constexpr int kRate = 44100;
constexpr int kFrames = 8192;
constexpr int kKey = 60;

/** Un font generado SIN moduladores de archivo: solo los diez defaults del spec. */
struct Font {
    std::vector<uint8_t> bytes;
    tsf* sf = nullptr;
    ModulatorTable table;
    Font() {
        bytes = makeMinimalSoundFont(22050, /*looping=*/true);
        sf = tsf_load_memory(bytes.data(), static_cast<int>(bytes.size()));
        if (sf) {
            tsf_set_output(sf, TSF_STEREO_INTERLEAVED, kRate, 0.0f);
            tsf_set_max_voices(sf, 8);
            table.buildFromFontBytes(bytes.data(), bytes.size());
        }
    }
    ~Font() { if (sf) tsf_close(sf); }
};

/** Un estado de canal: lo que la capa de canales de tsf aplica por su cuenta. */
struct ChannelState {
    const char* nombre;
    int cc7 = -1, cc10 = -1, cc11 = -1, wheel = -1;   // -1 = no se toca (reposo)
};

const ChannelState kEstados[] = {
    {"reposo"},
    {"CC7 = 100 (#5)", 100, -1, -1, -1},
    {"CC10 = 30 (#6)", -1, 30, -1, -1},
    {"CC11 = 80 (#7)", -1, -1, 80, -1},
    {"rueda = 10000 (#10)", -1, -1, -1, 10000},
    {"los cuatro a la vez", 100, 30, 80, 10000},
};

/**
 * Renderiza UNA nota con el canal en ese estado, con o sin la tabla. `tsf_reset`
 * libera los canales, asi que el preset y el estado se ponen despues de silenciar.
 */
std::vector<float> render(Font& f, const ChannelState& st, const ModulatorTable* table, float vel) {
    tsf_reset(f.sf);
    std::vector<float> descarte(static_cast<size_t>(kFrames) * 2, 0.0f);
    tsf_render_float(f.sf, descarte.data(), kFrames, 0);
    tsf_channel_set_presetindex(f.sf, 0, 0);
    if (st.cc7 >= 0) tsf_channel_midi_control(f.sf, 0, 7, st.cc7);
    if (st.cc10 >= 0) tsf_channel_midi_control(f.sf, 0, 10, st.cc10);
    if (st.cc11 >= 0) tsf_channel_midi_control(f.sf, 0, 11, st.cc11);
    if (st.wheel >= 0) tsf_channel_set_pitchwheel(f.sf, 0, st.wheel);
    channelNoteOnWithModulators(f.sf, table, 0, kKey, vel);
    std::vector<float> out(static_cast<size_t>(kFrames) * 2, 0.0f);
    tsf_render_float(f.sf, out.data(), kFrames, 0);
    return out;
}

double rms(const std::vector<float>& s) {
    double acc = 0.0;
    for (float x : s) acc += static_cast<double>(x) * x;
    return std::sqrt(acc / static_cast<double>(s.size()));
}

}  // namespace

// ===========================================================================
// AC-039.6 (i) — con la tabla puesta, el audio es el de la capa de canales, y nada mas
// ===========================================================================

TEST(TouchExpressionChannelLayer, TheChannelDefaultsAddNothingOnTopOfTsfChannelLayer) {
    Font f;
    ASSERT_NE(f.sf, nullptr);
    ASSERT_EQ(f.table.rejections().specRejections(), 0u);

    for (const ChannelState& st : kEstados) {
        // A velocity 127 los defaults de NOTA valen cero (#1 concava decreciente y #2
        // lineal decreciente dan 0 en el maximo), asi que lo UNICO que podria separar
        // estos dos renders es un default de CANAL evaluado ademas de la capa de tsf.
        const auto conTabla = render(f, st, &f.table, 1.0f);
        const auto sinTabla = render(f, st, nullptr, 1.0f);
        ASSERT_GT(rms(conTabla), 0.005) << st.nombre << ": el render no suena; no mide nada";
        EXPECT_EQ(std::memcmp(conTabla.data(), sinTabla.data(), conTabla.size() * sizeof(float)), 0)
            << st.nombre << ": con los defaults en la tabla el audio NO es identico al de la capa de "
            << "canales sola — algun default de canal (#5/#6/#7/#10) se esta aplicando DOS veces";
    }

    // El gemelo: a velocity 64 el default #1 (velocity -> atenuacion, 960 cB concava)
    // REEMPLAZA la curva cableada de tsf, asi que los dos renders tienen que DIFERIR.
    // Sin esto, "identico" arriba podria ser "la tabla no hace nada".
    //
    // 🔴 Y el gemelo NO es decoracion — es lo que atrapa al mutante que la identidad de
    // arriba no ve. Medido (M2, 2026-09-11): con el clasificador dejando pasar los CC
    // como velocity, los seis estados de arriba siguieron IDENTICOS a v=127 — las curvas
    // decrecientes (#5, #7) valen 0 en el maximo, asi que una duplicacion es invisible
    // en reposo por construccion del spec. A v=64 el valor salio -29,65 dB donde van
    // -5,95: #5 y #7 evaluados de mas, 2 × 11,9 dB encima. Por eso abajo se afirma el
    // VALOR y no solo "difieren".
    const auto conTabla64 = render(f, kEstados[0], &f.table, 64.0f / 127.0f);
    const auto sinTabla64 = render(f, kEstados[0], nullptr, 64.0f / 127.0f);
    EXPECT_NE(std::memcmp(conTabla64.data(), sinTabla64.data(), conTabla64.size() * sizeof(float)), 0)
        << "a velocity 64 los dos renders son identicos: la tabla no esta actuando y la "
        << "identidad de arriba es vacia";
    // Y el VALOR, derivado del spec y no del motor (un trinquete sobre la mera
    // diferencia no ve un desplazamiento comun): el default #1 es 960 cB por la curva
    // concava unipolar decreciente, que en SF2 §8.2.1 es `-(20/96)·log10(x²)` con
    // x = v/127 — o sea 96 dB · eso = -40·log10(x) = 11,9 dB a v=64. tsf cableaba
    // `-20·log10(x)` = 5,95 dB. La tabla REEMPLAZA (no suma), asi que con tabla la nota
    // suena 11,9 - 5,95 = 5,95 dB mas baja: exactamente `20·log10(x)`.
    const double x = 64.0 / 127.0;
    const double esperadoDb = 20.0 * std::log10(x);
    const double dB = 20.0 * std::log10(rms(conTabla64) / rms(sinTabla64));
    EXPECT_NEAR(dB, esperadoDb, 0.3)
        << "a v=64 la tabla actua pero no como el default #1 del spec (medido " << dB
        << " dB, spec " << esperadoDb << "). Si es ~" << 2.0 * esperadoDb
        << ", la curva se esta SUMANDO a la cableada de tsf en vez de reemplazarla (2.6)";
}

// ===========================================================================
// AC-039.6 (ii) — la expresion por toque entra por la ganancia del canal, LINEAL
// ===========================================================================

namespace {

constexpr int kEngineRate = 48000;
constexpr int kEngineFrames = 24000;
constexpr int kGestureFrame = 4800;

/** Como el fixture de REQ-008, con la tabla ACTIVA (el manager la construye al cargar). */
std::vector<float> renderWithExpression(float expression) {
    auto sf2 = makeMinimalSoundFont(kEngineRate, /*looping=*/true);
    auto manager = std::make_unique<SoundFontManager>();
    if (!manager->loadFromMemory(sf2.data(), static_cast<int>(sf2.size()), kEngineRate)) return {};
    SoundFontEngine engine;
    engine.setSoundFontManager(manager.get());
    engine.prepare(kEngineRate, 256);
    engine.noteOn(0, kKey, 0.9f);
    std::vector<float> stereo(static_cast<size_t>(kEngineFrames) * 2, 0.0f);
    int done = 0;
    while (done < kEngineFrames) {
        const int n = std::min(256, kEngineFrames - done);
        if (kGestureFrame >= done && kGestureFrame < done + n) engine.setTouchExpression(0, expression);
        engine.render(stereo.data() + static_cast<size_t>(done) * 2, n);
        done += n;
    }
    std::vector<float> left(static_cast<size_t>(kEngineFrames));
    for (int i = 0; i < kEngineFrames; ++i) left[static_cast<size_t>(i)] = stereo[static_cast<size_t>(i) * 2];
    return left;
}

double rmsFrom(const std::vector<float>& s, int from) {
    double acc = 0.0;
    for (size_t i = static_cast<size_t>(from); i < s.size(); ++i) acc += static_cast<double>(s[i]) * s[i];
    return std::sqrt(acc / static_cast<double>(s.size() - static_cast<size_t>(from)));
}

}  // namespace

TEST(TouchExpressionChannelLayer, TouchExpressionEntersByChannelGainLinearlyNeverByCC7OrCC11) {
    const auto neutro = renderWithExpression(1.0f);
    ASSERT_FALSE(neutro.empty());
    const int desde = kGestureFrame + 2400;   // bien pasada la convergencia del smoother
    const double rmsNeutro = rmsFrom(neutro, desde);
    ASSERT_GT(rmsNeutro, 0.005) << "la referencia no suena; el test no mide nada";

    // Tres puntos, no uno: la curva `^3` de `tsf_channel_midi_control` (CC7/CC11) coincide
    // con la lineal en 1,0 y en 0,0, y solo se separa en el medio. En 0,5 da 0,125.
    for (float e : {0.75f, 0.5f, 0.25f}) {
        const auto bajada = renderWithExpression(e);
        const double ratio = rmsFrom(bajada, desde) / rmsNeutro;
        EXPECT_NEAR(ratio, e, 0.05)
            << "expresion " << e << ": el nivel no escala LINEALMENTE (ratio " << ratio << "). "
            << "Si el ratio es ~" << e * e * e << ", la expresion esta entrando por CC7/CC11 "
            << "(`tsf_channel_midi_control`, curva ^3) y no por la ganancia del canal — y ahi "
            << "SI sumaria con los defaults #5/#7";
    }
}

// ===========================================================================
// AC-039.6 (iii) — los ocho de canal estan contados con nombre, y #1/#2 no
// ===========================================================================

TEST(TouchExpressionChannelLayer, TheEightChannelDefaultsAreNamedAsNotEvaluatedAtNoteOn) {
    using wma::sfmod::defaultModulators;
    using wma::sfmod::Modulator;
    using wma::sfmod::RejectionCounters;
    const std::vector<Modulator>& defs = defaultModulators();
    ASSERT_EQ(defs.size(), 10u) << "el spec declara DIEZ moduladores por defecto";

    // Indice -> numero del spec, y que hace cada uno. El orden es el de `defaultModulators()`.
    struct Esperado { int spec; const char* que; bool seEvaluaAlDisparar; };
    const Esperado kEsperados[10] = {
        {1, "velocity -> initialAttenuation", true},
        {2, "velocity -> initialFilterFc", true},
        {3, "presion de canal -> vibrato", false},
        {4, "CC1 (rueda de modulacion) -> vibrato", false},
        {5, "CC7 (volumen) -> initialAttenuation — la capa de canales de tsf", false},
        {6, "CC10 (pan) -> pan — la capa de canales de tsf", false},
        {7, "CC11 (expresion) -> initialAttenuation — la capa de canales de tsf", false},
        {8, "CC91 -> reverb send — REQ-040", false},
        {9, "CC93 -> chorus send — REQ-040", false},
        {10, "rueda de pitch -> pitch — la capa de canales de tsf", false},
    };

    RejectionCounters c;
    int evaluados = 0, deCanal = 0;
    for (size_t i = 0; i < defs.size(); ++i) {
        const Esperado& e = kEsperados[i];
        std::uint32_t* slot = wma::sfmod::detail::rejectionSlotFor(defs[i], c);
        if (e.seEvaluaAlDisparar) {
            ++evaluados;
            EXPECT_EQ(slot, nullptr) << "default #" << e.spec << " (" << e.que
                                     << ") tiene que evaluarse al disparar la nota";
        } else {
            ++deCanal;
            EXPECT_EQ(slot, &c.sourceNotAtNoteOn)
                << "default #" << e.spec << " (" << e.que << ") tiene que quedar CONTADO como fuente de "
                << "canal — ni evaluado (duplicaria la capa de tsf) ni perdido en otro contador";
        }
    }
    EXPECT_EQ(evaluados, 2);
    EXPECT_EQ(deCanal, 8);
}
