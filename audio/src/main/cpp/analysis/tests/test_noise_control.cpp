/**
 * test_noise_control.cpp — REQ-058 S1, tarea 1.8: EL CONTROL NEGATIVO DE MINI-046 (AC-058.8).
 *
 * Con objetivo cargado y ruido de sala, la lectura temprana no puede publicar `cents` en mas ventanas
 * que el motor de hoy. La ausencia (R-PITCH-34/35/54) no se toca aca: el arreglo es de L3 (D3 de
 * REQ-055). Dos fuentes:
 *   · en host, ruido SINTETIZADO: xorshift32 blanco a −48,5 dBFS (la sala del g42) y a −40 dBFS, solo y
 *     con zumbido de red de 50 Hz y armonicos, objetivo en las seis cuerdas de guitarra, 10 s cada una;
 *   · el fixture del aparato `g42-20261014-afinador-control` (corrida `smoke-20261014-132457`): un
 *     registro del harness, no audio. Da la LINEA DE BASE DEL APARATO —cuantos ticks con cents publico
 *     el motor de hoy en la ventana de silencio de la fase de juicio— para comparar en la proxima corrida
 *     del smoke en el g42 con el artefacto nuevo.
 */

#include "support/SyntheticSignal.h"

#include "AnalysisRing.h"
#include "AnalysisSnapshot.h"
#include "AnalysisThread.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace wma_test {
namespace {

/// Publicaciones con cents del MOTOR DE HOY (master 5ed111d0, sin camino temprano) sobre el mismo ruido
/// de este archivo: medido en REQ-058 S1 corriendo este test con el camino temprano apagado a mano. El
/// prototipo del architect dio lo mismo (2808 publicaciones, 0 con cents, ruido blanco).
constexpr int kTodayCentsWithRoomNoise = 0;

std::vector<float> roomNoise(int frames, double dbfs, bool hum, uint32_t seed) {
    std::vector<float> out(static_cast<size_t>(frames));
    uint32_t x = seed;
    const double amp = std::pow(10.0, dbfs / 20.0) * std::sqrt(3.0);   // rms de la uniforme = amp/√3
    for (int i = 0; i < frames; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        double v = amp * ((static_cast<double>(x) / 4294967296.0) * 2.0 - 1.0);
        if (hum) {
            // Zumbido de red: 50 Hz y armonicos 2..6 con caida 1/n, al nivel del ruido.
            const double t = static_cast<double>(i) / 48000.0;
            for (int n = 1; n <= 6; ++n) v += (amp / n) * std::sin(2.0 * M_PI * 50.0 * n * t);
        }
        out[static_cast<size_t>(i)] = static_cast<float>(v);
    }
    return out;
}

struct NoiseCount {
    int publications = 0;
    int withCents = 0;
};

NoiseCount countCents(const std::vector<float>& mono, double target) {
    constexpr int rate = 48000;
    wma::analysis::AnalysisRing ring;
    wma::analysis::AnalysisSnapshot snap;
    wma::analysis::AnalysisThread th(ring, snap);
    ring.setCaptureRate(rate);
    th.setTargetHz(target);
    (void)th.drainOnce();
    NoiseCount c;
    std::vector<float> st(512);
    for (size_t off = 0; off < mono.size(); off += 256) {
        const int n = static_cast<int>(std::min<size_t>(256, mono.size() - off));
        for (int i = 0; i < n; ++i) st[2 * i] = st[2 * i + 1] = mono[off + i];
        ring.writeStereo(st.data(), n);
        for (;;) {
            const auto o = th.drainOnce();
            if (o == wma::analysis::AnalysisThread::DrainOutcome::kRingEmpty) break;
            if (o != wma::analysis::AnalysisThread::DrainOutcome::kPublished) continue;
            float v[wma::analysis::kSnapshotValueCount];
            if (!snap.read(v)) continue;
            ++c.publications;
            if (!std::isnan(v[wma::analysis::kSnapCents])) ++c.withCents;
        }
    }
    return c;
}

TEST(NoiseControl, NoMoreCentsThanTodayWithRoomNoise) {
    // AC-058.8. Mutante: admitir en el camino temprano sin el piso de energia del bin (o sin la gruesa)
    // ⇒ cents sobre ruido. El conteo de hoy es la constante de arriba.
    constexpr double kGuitar[6] = {82.40689, 110.0, 146.83238, 195.99772, 246.94165, 329.62756};
    NoiseCount total;
    for (double dbfs : {-48.5, -40.0})
        for (bool hum : {false, true}) {
            NoiseCount cell;
            for (double f0 : kGuitar) {
                const NoiseCount c = countCents(roomNoise(10 * 48000, dbfs, hum, 88172645u), f0);
                cell.publications += c.publications;
                cell.withCents += c.withCents;
            }
            std::printf("  [REQ-058 AC-058.8] ruido %.1f dBFS%s: %d publicaciones, %d con cents\n", dbfs,
                        hum ? " + zumbido 50 Hz" : "", cell.publications, cell.withCents);
            total.publications += cell.publications;
            total.withCents += cell.withCents;
        }
    RecordProperty("publicaciones_con_cents_sobre_ruido", total.withCents);
    ASSERT_GT(total.publications, 0) << "el motor no publico nada: el control no mide";
    EXPECT_LE(total.withCents, kTodayCentsWithRoomNoise);
}

std::string fixturePath() {
    return std::string(WMA_ANALYSIS_GOLDEN_DIR) +
           "/../../../../../../../scripts/smoke-device-fixtures/g42-20261014-afinador-control/harness-smoke.txt";
}

/// El valor de `clave=` en una linea del harness, o "" si no esta.
std::string field(const std::string& line, const std::string& key) {
    const std::string k = " " + key + "=";
    const size_t at = line.find(k);
    if (at == std::string::npos) return "";
    const size_t from = at + k.size();
    const size_t to = line.find(' ', from);
    return line.substr(from, to == std::string::npos ? std::string::npos : to - from);
}

struct DeviceBaseline {
    int silentTicks = 0;      ///< ticks de la fase de juicio con el nivel de la sala (<= −45 dBFS)
    int silentWithCents = 0;  ///< de esos, los que trajeron cents
};

/// Parser del fixture: los ticks `step=afinador fase=juicio` cuyo `nivel-dbfs` es el de la sala (la
/// ventana de silencio de la fase de juicio) y cuantos traen `cents` (no `-`).
DeviceBaseline parseDeviceBaseline(const std::string& text) {
    DeviceBaseline b;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.rfind("HARNESS-SMOKE", 0) != 0 || field(line, "step") != "afinador" || field(line, "fase") != "juicio")
            continue;
        const std::string lvl = field(line, "nivel-dbfs");
        if (lvl.empty() || std::strtod(lvl.c_str(), nullptr) > -45.0) continue;
        ++b.silentTicks;
        const std::string cents = field(line, "cents");
        if (!cents.empty() && cents != "-") ++b.silentWithCents;
    }
    return b;
}

TEST(NoiseControl, TheDeviceFixtureGivesTodaysBaseline) {
    // AC-058.8 (D-4): la linea de base del aparato. MINI-046 la documento: en la ventana de silencio de la
    // fase de juicio (objetivo A4), UN tick MEASURING con cents a −48 dBFS. El parser se prueba primero
    // contra lineas a mano (control positivo y negativo) y despues lee el fixture commiteado.
    const std::string hand =
        "HARNESS-SMOKE v=1 run=x panel=afinador step=afinador ok=true fase=juicio n=1 cents=+0.5 nivel-dbfs=-48.0\n"
        "HARNESS-SMOKE v=1 run=x panel=afinador step=afinador ok=true fase=juicio n=2 cents=- nivel-dbfs=-48.2\n"
        "HARNESS-SMOKE v=1 run=x panel=afinador step=afinador ok=true fase=juicio n=3 cents=-0.1 nivel-dbfs=-14.6\n"
        "HARNESS-SMOKE v=1 run=x panel=afinador step=afinador ok=true fase=reproduccion n=4 cents=+1.0 nivel-dbfs=-50.0\n";
    const DeviceBaseline h = parseDeviceBaseline(hand);
    EXPECT_EQ(h.silentTicks, 2);
    EXPECT_EQ(h.silentWithCents, 1);

    std::FILE* f = std::fopen(fixturePath().c_str(), "rb");
    ASSERT_NE(f, nullptr) << "falta el fixture " << fixturePath();
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    const DeviceBaseline b = parseDeviceBaseline(text);
    std::printf("  [REQ-058 AC-058.8] linea de base del aparato (smoke-20261014-132457): %d ticks de sala en el juicio, "
                "%d con cents\n", b.silentTicks, b.silentWithCents);
    RecordProperty("aparato_ticks_sala", b.silentTicks);
    RecordProperty("aparato_ticks_sala_con_cents", b.silentWithCents);
    EXPECT_GT(b.silentTicks, 0) << "el parser no encontro la ventana de silencio del juicio";
    EXPECT_EQ(b.silentWithCents, 1) << "MINI-046 documento UN tick con cents en esa ventana";
}

}  // namespace
}  // namespace wma_test
