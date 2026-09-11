/**
 * test_soundfont_note_on.cpp — REQ-039 S2, tarea 2.5 (AC-039.7): el valor modulado
 * vive POR VOZ, no en `tsf_region`.
 *
 * `tsf_region` es compartida e inmutable entre voces. Si el corte de filtro modulado
 * se escribiera ahi, la segunda nota de la misma region PISARIA a la primera: dos
 * notas simultaneas con distinta velocity sonarian con el MISMO corte — el de la
 * ultima. Este test toca dos notas de la misma region, una a 127 y otra a 15, y
 * mide que la mezcla suena como la SUMA de las dos solas, no como dos de la misma.
 *
 * EL ORACULO ES LA LINEALIDAD, no un numero de brillo: `brillo(mezcla)` tiene que
 * caer estrictamente ENTRE `brillo(v127 sola)` y `brillo(v15 sola)`, lejos de los
 * dos extremos, y en los DOS ordenes de disparo. Un valor por region falla en un
 * orden o en el otro segun quien pise a quien.
 *
 * El fixture declara vel -> filterFc a -7200 cents (identidad 2.04: REEMPLAZA al
 * default #2) para que el efecto sea grande — a velocity 15 el corte cae a ~510 Hz
 * sobre una onda cuadrada de ~344 Hz, o sea queda solo la fundamental—, y ANULA el
 * default #1 (vel -> atenuacion) para que la voz de 15 no desaparezca del nivel y
 * el brillo mida solo el filtro. Es lo que hace el spec-test en sus pruebas #14.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "../../engines/SoundFontModulatorTable.h"
#include "../../engines/SoundFontNoteOn.h"
#include "support/MinimalSoundFont.h"
#include "tsf.h"

using wma::sfmod::channelNoteOnWithModulators;
using wma::sfmod::ModulatorTable;
using wma_test::makeMinimalSoundFont;
using wma_test::sf2::kCurveConcave;
using wma_test::sf2::kCurveLinear;
using wma_test::sf2::kGenInitialAttenuation;
using wma_test::sf2::kGenInitialFilterFc;
using wma_test::sf2::kSrcNone;
using wma_test::sf2::kSrcVelocity;
using wma_test::sf2::ModulatorPlacement;
using wma_test::sf2::srcOper;

namespace {

constexpr int kRate = 44100;
constexpr int kFrames = 8192;  // ~186 ms: de sobra para el espectro, sin la cola
constexpr int kKey = 60;

ModulatorPlacement filtroFuerteSinAtenuacion() {
    ModulatorPlacement m;
    // vel -> filterFc, lineal decreciente, -7200 cents, sin fuente secundaria: la
    // identidad del default #2 de 2.04, asi que lo REEMPLAZA.
    m.instrumentGlobal.push_back({srcOper(kSrcVelocity, false, true, false, kCurveLinear),
                                  kGenInitialFilterFc, -7200, kSrcNone, 0});
    // Y el default #1 anulado: misma identidad, amount 0.
    m.instrumentGlobal.push_back({srcOper(kSrcVelocity, false, true, false, kCurveConcave),
                                  kGenInitialAttenuation, 0, kSrcNone, 0});
    return m;
}

struct Font {
    std::vector<uint8_t> bytes;
    tsf* sf = nullptr;
    ModulatorTable table;
    Font() {
        bytes = makeMinimalSoundFont(22050, true, -1, -1, filtroFuerteSinAtenuacion());
        sf = tsf_load_memory(bytes.data(), static_cast<int>(bytes.size()));
        if (sf) {
            tsf_set_output(sf, TSF_STEREO_INTERLEAVED, kRate, 0.0f);
            tsf_set_max_voices(sf, 8);
            table.buildFromFontBytes(bytes.data(), bytes.size());
        }
    }
    ~Font() { if (sf) tsf_close(sf); }
};

/** Brillo: energia de la primera diferencia sobre la energia total. Sube con los agudos. */
double brillo(const std::vector<float>& stereo) {
    double num = 0.0, den = 0.0;
    float prev = 0.0f;
    for (size_t i = 0; i + 1 < stereo.size(); i += 2) {
        const float m = 0.5f * (stereo[i] + stereo[i + 1]);
        const float d = m - prev;
        num += static_cast<double>(d) * d;
        den += static_cast<double>(m) * m;
        prev = m;
    }
    return den > 1e-12 ? num / den : 0.0;
}

/**
 * Deja el font en silencio de verdad. `tsf_reset` termina las voces con un release
 * RAPIDO, no nulo: sin este descarte el render siguiente arranca con la cola del
 * anterior, y el gemelo de abajo lo delato — sin tabla, la nota de 15 salia un 14 %
 * mas brillante que la de 127 porque llevaba encima la cola de esta.
 */
void silenciar(Font& f) {
    tsf_reset(f.sf);
    std::vector<float> descarte(static_cast<size_t>(kFrames) * 2, 0.0f);
    tsf_render_float(f.sf, descarte.data(), kFrames, 0);
    // `tsf_reset` tambien LIBERA los canales, y sin canal con preset
    // `tsf_channel_note_on` devuelve sin tocar. Dos canales, uno por nota, para que
    // las dos voces sean de la MISMA region con distinta velocity.
    tsf_channel_set_presetindex(f.sf, 0, 0);
    tsf_channel_set_presetindex(f.sf, 1, 0);
}

/** Toca las notas pedidas (canal, velocity) en ese orden y renderiza. */
std::vector<float> render(Font& f, std::initializer_list<std::pair<int, float>> notas) {
    silenciar(f);
    for (const auto& n : notas) {
        channelNoteOnWithModulators(f.sf, &f.table, n.first, kKey, n.second);
    }
    std::vector<float> out(static_cast<size_t>(kFrames) * 2, 0.0f);
    tsf_render_float(f.sf, out.data(), kFrames, 0);
    return out;
}

}  // namespace

/** RMS de la diferencia entre la mezcla y la suma de las dos solas, relativo a la suma. */
double residuoRelativo(const std::vector<float>& mezcla, const std::vector<float>& a,
                       const std::vector<float>& b) {
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < mezcla.size(); ++i) {
        const double suma = static_cast<double>(a[i]) + b[i];
        const double d = mezcla[i] - suma;
        num += d * d;
        den += suma * suma;
    }
    return den > 1e-12 ? std::sqrt(num / den) : 1.0;
}

TEST(SoundFontNoteOn, DosNotasSimultaneasDeLaMismaRegionConDistintaVelocityMidenDistintoCorte) {
    Font f;
    ASSERT_NE(f.sf, nullptr);

    const auto sola127 = render(f, {{0, 127.0f / 127.0f}});
    const auto sola15 = render(f, {{1, 15.0f / 127.0f}});
    const double abierta = brillo(sola127), filtrada = brillo(sola15);
    // Precondicion: el filtro por velocity ESTA actuando. Con Q = 0 un 2-polo a ~510 Hz
    // sobre armonicos impares desde 172 Hz baja el brillo a un cuarto, no a cero.
    ASSERT_GT(abierta, filtrada * 1.5)
        << "la nota a 15 no suena mas oscura que la de 127: el modulador de filtro no actua";

    /**
     * EL ORACULO ES LA LINEALIDAD DE LAS SEÑALES, y esta es la segunda version. La
     * primera comparaba el BRILLO de la mezcla con el de las dos solas, esperando que
     * cayera en el medio — y salio MAS brillante que la abierta sola: dos voces de la
     * misma tecla arrancan en fase, son coherentes, y el ratio de una suma coherente no
     * es intermedio. La linealidad vale para las señales: tsf suma voces al buffer sin
     * saturar, asi que si el corte vive EN LA VOZ, `mezcla == sola127 + sola15` salvo
     * float. Si vive en la REGION, la segunda nota pisa el corte de la primera y la
     * mezcla es OTRA cosa — en un orden o en el otro.
     */
    const auto mezclaAB = render(f, {{0, 127.0f / 127.0f}, {1, 15.0f / 127.0f}});
    const auto mezclaBA = render(f, {{1, 15.0f / 127.0f}, {0, 127.0f / 127.0f}});
    const double rAB = residuoRelativo(mezclaAB, sola127, sola15);
    const double rBA = residuoRelativo(mezclaBA, sola127, sola15);
    std::printf("  [REQ-039] brillo: v127 sola %.4f · v15 sola %.4f · residuo mezcla-suma %.2e / %.2e\n",
                abierta, filtrada, rAB, rBA);
    EXPECT_LT(rAB, 1e-3) << "127 y despues 15: la mezcla no es la suma de las dos solas — "
                         << "la segunda nota le cambio el corte a la primera (valor en la REGION)";
    EXPECT_LT(rBA, 1e-3) << "15 y despues 127: la mezcla no es la suma de las dos solas — "
                         << "la segunda nota le cambio el corte a la primera (valor en la REGION)";
}

/** El gemelo: sin moduladores (tabla nula) las dos notas suenan IGUAL de brillantes. */
TEST(SoundFontNoteOn, SinTablaLasDosVelocitiesSuenanConElMismoCorte) {
    Font f;
    ASSERT_NE(f.sf, nullptr);
    auto sinTabla = [&](float vel) {
        silenciar(f);
        channelNoteOnWithModulators(f.sf, nullptr, 0, kKey, vel);
        std::vector<float> out(static_cast<size_t>(kFrames) * 2, 0.0f);
        tsf_render_float(f.sf, out.data(), kFrames, 0);
        return brillo(out);
    };
    const double a = sinTabla(1.0f), b = sinTabla(15.0f / 127.0f);
    EXPECT_NEAR(a, b, 0.05 * a) << "sin tabla el brillo depende de la velocity: tsf pelado no hace eso";
}
