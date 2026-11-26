// MasterStage (REQ-055 S12, W1): ganancia + limitador brickwall true-peak.
// Cada test responde a un defecto plausible; la tabla guarda -> test vive en docs/dev/mezcla-y-master.md.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include "../MasterStage.h"
#include "../../core/tests/support/RtAllocCounter.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kSr = 48000;

using Signal = std::vector<float>;   // estereo intercalado

float dbToLin(double db) { return static_cast<float>(std::pow(10.0, db / 20.0)); }
double linToDb(double lin) { return 20.0 * std::log10(std::max(lin, 1e-30)); }

Signal fs4Sine(double amp, int frames) {   // fs/4 a 45 grados: las muestras son +-0,707*amp, el pico verdadero amp
    Signal s(static_cast<size_t>(frames) * 2);
    for (int n = 0; n < frames; ++n) {
        const float v = static_cast<float>(amp * std::sin(kPi / 2.0 * n + kPi / 4.0));
        s[static_cast<size_t>(n) * 2] = v;
        s[static_cast<size_t>(n) * 2 + 1] = v;
    }
    return s;
}

Signal noise(double amp, int frames, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);
    Signal s(static_cast<size_t>(frames) * 2);
    for (float& v : s) v = static_cast<float>(amp) * d(rng);
    return s;
}

// Ruido blanco pasado por un pasabajos FIR (Blackman, 129 taps) y normalizado a pico `amp`: material
// con la banda de un programa real. El FIR de 4x del BS.1770 subestima el pico de la reconstruccion
// ideal cuanto mas cerca de Nyquist hay energia (ruido blanco pleno: hasta ~9 dB de diferencia).
Signal bandNoise(double amp, int frames, double fc, uint32_t seed) {
    constexpr int kM = 64;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);
    std::vector<float> w(static_cast<size_t>(frames));
    for (float& v : w) v = d(rng);
    std::vector<double> h(2 * kM + 1);
    for (int k = -kM; k <= kM; ++k) {
        const double sinc = k == 0 ? 2 * fc : std::sin(2 * kPi * fc * k) / (kPi * k);
        h[static_cast<size_t>(k + kM)] = sinc * (0.42 + 0.5 * std::cos(kPi * k / kM) +
                                                 0.08 * std::cos(2 * kPi * k / kM));
    }
    Signal s(static_cast<size_t>(frames) * 2, 0.0f);
    double mx = 0.0;
    for (int n = kM; n < frames - kM; ++n) {
        double a = 0.0;
        for (int k = -kM; k <= kM; ++k) a += h[static_cast<size_t>(k + kM)] * w[static_cast<size_t>(n - k)];
        s[static_cast<size_t>(n) * 2] = s[static_cast<size_t>(n) * 2 + 1] = static_cast<float>(a);
        mx = std::max(mx, std::fabs(a));
    }
    for (float& v : s) v = static_cast<float>(v / mx * amp);
    return s;
}

Signal impulses(double amp, int frames) {   // aislados, en rafagas de signo alterno y pegados de a dos
    Signal s(static_cast<size_t>(frames) * 2, 0.0f);
    for (int n = 500; n < frames - 500; n += 997) {
        const float v = static_cast<float>(amp);
        s[static_cast<size_t>(n) * 2] = v;
        s[static_cast<size_t>(n) * 2 + 1] = -v;
        if ((n / 997) % 3 == 1) {   // dos pegadas del mismo signo: el pico verdadero cae entre ambas
            s[static_cast<size_t>(n + 1) * 2] = v;
            s[static_cast<size_t>(n + 1) * 2 + 1] = -v;
        }
    }
    return s;
}

Signal square(double amp, int frames) {   // 1 kHz, full scale * amp: rizado de Gibbs en el pico verdadero
    Signal s(static_cast<size_t>(frames) * 2);
    for (int n = 0; n < frames; ++n) {
        const float v = (n / 24) % 2 == 0 ? static_cast<float>(amp) : static_cast<float>(-amp);
        s[static_cast<size_t>(n) * 2] = v;
        s[static_cast<size_t>(n) * 2 + 1] = v;
    }
    return s;
}

// Referencia INDEPENDIENTE del FIR del limitador: reconstruccion sinc con ventana de Blackman-Harris
// (+-24 muestras) evaluada a 16x. Devuelve el pico verdadero en lineal, lejos de los bordes.
double truePeak16x(const Signal& s) {
    constexpr int kHalf = 24;
    const int frames = static_cast<int>(s.size() / 2);
    double table[16][2 * kHalf];   // sinc x ventana, por fase y tap
    for (int p = 0; p < 16; ++p) {
        for (int k = -kHalf + 1; k <= kHalf; ++k) {
            const double t = k - p / 16.0;
            const double sinc = std::fabs(t) < 1e-12 ? 1.0 : std::sin(kPi * t) / (kPi * t);
            const double u = (t + kHalf) / (2.0 * kHalf);
            const double w = 0.35875 - 0.48829 * std::cos(2 * kPi * u) +
                             0.14128 * std::cos(4 * kPi * u) - 0.01168 * std::cos(6 * kPi * u);
            table[p][k + kHalf - 1] = sinc * w;
        }
    }
    double best = 0.0;
    for (int ch = 0; ch < 2; ++ch) {
        for (int n = kHalf; n < frames - kHalf; ++n) {
            for (int p = 0; p < 16; ++p) {
                double acc = 0.0;
                for (int k = -kHalf + 1; k <= kHalf; ++k) {
                    acc += s[static_cast<size_t>(n + k) * 2 + static_cast<size_t>(ch)] *
                           table[p][k + kHalf - 1];
                }
                best = std::max(best, std::fabs(acc));
            }
        }
    }
    return best;
}

double samplePeak(const Signal& s) {
    double best = 0.0;
    for (float v : s) best = std::max(best, static_cast<double>(std::fabs(v)));
    return best;
}

wm::MasterParams params(float gainDb, float ceilDb = -1.0f, float releaseMs = 150.0f) {
    wm::MasterParams p;
    p.gainDb = gainDb;
    p.ceilingDbtp = ceilDb;
    p.releaseMs = releaseMs;
    return p;
}

// Procesa `in` en bloques de `block` frames con una instancia fresca.
Signal runStage(const Signal& in, const wm::MasterParams& p, int block, int sr = kSr,
                uint64_t* clampHits = nullptr) {
    wm::MasterStage st;
    st.prepareMaster(sr);
    st.configureMaster(p);
    st.resetMaster();
    Signal out = in;
    const int frames = static_cast<int>(in.size() / 2);
    for (int pos = 0; pos < frames; pos += block) {
        st.processMaster(out.data() + static_cast<size_t>(pos) * 2, std::min(block, frames - pos));
    }
    if (clampHits != nullptr) *clampHits = st.clampHits();
    return out;
}

Signal appendSilence(Signal s, int frames) {   // cola para vaciar la linea de retardo
    s.resize(s.size() + static_cast<size_t>(frames) * 2, 0.0f);
    return s;
}

}  // namespace

// La tabla del Anexo 2 transcripta a mano: un tap mal copiado cambia la ganancia DC de la fase o
// rompe la simetria que el anexo exige entre las fases 1 y 2, y 0 y 3.
TEST(MasterStage, FirTableIsTheAnnexTwoTable) {
    double sums[4] = {};
    for (int p = 0; p < 4; ++p) {
        for (int i = 0; i < 12; ++i) sums[p] += wm::MasterStage::firTap(p, i);
        EXPECT_NEAR(sums[p], 1.0, 0.03) << "fase " << p;
    }
    EXPECT_FLOAT_EQ(wm::MasterStage::firTap(0, 6), 0.97216797f);
    EXPECT_FLOAT_EQ(wm::MasterStage::firTap(1, 6), 0.77978516f);
    EXPECT_FLOAT_EQ(wm::MasterStage::firTap(2, 5), 0.77978516f);
    EXPECT_FLOAT_EQ(wm::MasterStage::firTap(3, 5), 0.97216797f);
    // Una senoidal fs/4 a 45 grados de amplitud 1: la fase 2 cae justo en la cresta.
    double crest = 0.0;
    for (int p = 0; p < 4; ++p) {
        double acc = 0.0;
        for (int i = 0; i < 12; ++i) {
            acc += wm::MasterStage::firTap(p, i) * std::sin(kPi / 2.0 * (100 - i) + kPi / 4.0);
        }
        crest = std::max(crest, std::fabs(acc));
    }
    EXPECT_NEAR(crest, 1.0, 0.03);
}

// AC: pico de muestra <= techo siempre; true-peak (referencia 16x) <= techo + 0,1 dB.
// Defecto que atrapa: un limitador que mide pico de muestra, un lookahead corto, o la guarda
// de clamp haciendo de limitador (clampHits > 0 la delata).
TEST(MasterStage, BatteryNeverExceedsCeilingSampleOrTruePeak) {
    struct Case { const char* name; Signal sig; float gainDb; };
    const std::vector<Case> cases = {
        {"fs4 45deg +6dB", fs4Sine(2.0, kSr / 4), 0.0f},
        {"fs4 45deg unidad con ganancia +6", fs4Sine(1.0, kSr / 4), 6.0f},
        {"ruido banda 0,25 fs +12dB", bandNoise(4.0, kSr / 4, 0.25, 21u), 0.0f},
        {"ruido banda 0,35 fs 0 dBFS con ganancia +12", bandNoise(1.0, kSr / 4, 0.35, 22u), 12.0f},
        {"impulsos +6dB", appendSilence(impulses(2.0, kSr / 4), 600), 0.0f},
        {"cuadrada 1 kHz +3dB", square(1.41, kSr / 4), 0.0f},
    };
    const float ceilDb = -1.0f;
    const double ceilLin = dbToLin(ceilDb);
    for (const Case& c : cases) {
        uint64_t hits = 0;
        const Signal out = runStage(c.sig, params(c.gainDb, ceilDb), 256, kSr, &hits);
        EXPECT_LE(samplePeak(out), ceilLin) << c.name;
        const double tpDb = linToDb(truePeak16x(out));
        EXPECT_LE(tpDb, ceilDb + 0.1) << c.name << " true-peak " << tpDb << " dBTP";
        EXPECT_EQ(hits, 0u) << c.name << ": la guarda de clamp actuo (el limitador no llego solo)";
    }
}

// Medidor BS.1770-4 (4x, el mismo FIR): lo que define dBTP. El limitador lo sostiene con CUALQUIER
// senal, ruido blanco pleno incluido, aunque ahi el medidor subestime la reconstruccion ideal.
double bs1770TruePeak(const Signal& s) {
    const int frames = static_cast<int>(s.size() / 2);
    double best = 0.0;
    for (int ch = 0; ch < 2; ++ch) {
        for (int j = 11; j < frames; ++j) {
            for (int p = 0; p < 4; ++p) {
                double acc = 0.0;
                for (int i = 0; i < 12; ++i) {
                    acc += wm::MasterStage::firTap(p, i) *
                           s[static_cast<size_t>(j - i) * 2 + static_cast<size_t>(ch)];
                }
                best = std::max(best, std::fabs(acc));
            }
        }
    }
    return best;
}

TEST(MasterStage, FullBandWhiteNoiseHoldsTheBs1770Meter) {
    const Signal in = appendSilence(noise(4.0, kSr / 2, 31u), 600);
    uint64_t hits = 0;
    const Signal out = runStage(in, params(0.0f, -1.0f), 128, kSr, &hits);
    EXPECT_LE(linToDb(bs1770TruePeak(out)), -1.0 + 0.01);
    EXPECT_LE(samplePeak(out), dbToLin(-1.0));
    EXPECT_EQ(hits, 0u);
}

// Que limite de verdad y no apague: una senoidal sostenida queda cerca del techo, no mil dB debajo.
TEST(MasterStage, SustainedSineSettlesNearTheCeiling) {
    const Signal out = runStage(fs4Sine(2.0, kSr / 2), params(0.0f, -1.0f), 480);
    Signal tail(out.end() - 4800 * 2, out.end());
    const double tpDb = linToDb(truePeak16x(tail));
    EXPECT_GE(tpDb, -1.4);
    EXPECT_LE(tpDb, -0.9);
}

// Una cuadrada tambien a otro techo y otro sample rate: el lookahead y la ventana escalan con el SR.
TEST(MasterStage, CeilingHoldsAtOtherRatesAndCeilings) {
    for (int sr : {44100, 96000}) {
        for (float ceilDb : {-1.0f, -6.0f}) {
            const Signal in = appendSilence(bandNoise(3.0, sr / 4, 0.25, 7u), sr / 100);
            const Signal out = runStage(in, params(0.0f, ceilDb), 512, sr);
            EXPECT_LE(samplePeak(out), dbToLin(ceilDb)) << sr << " " << ceilDb;
            EXPECT_LE(linToDb(truePeak16x(out)), ceilDb + 0.1) << sr << " " << ceilDb;
        }
    }
}

// AC: bajo el umbral = la entrada demorada, bit a bit. Defecto: ganancia que no es EXACTAMENTE 1.
TEST(MasterStage, BelowThresholdIsTheDelayedInputBitForBit) {
    const Signal in = noise(0.3, kSr, 11u);
    wm::MasterStage probe;
    probe.prepareMaster(kSr);
    const int d = probe.latencyFrames();
    const Signal out = runStage(in, params(0.0f), 311);
    for (int n = 0; n < d; ++n) {
        ASSERT_EQ(out[static_cast<size_t>(n) * 2], 0.0f) << n;
    }
    const int frames = static_cast<int>(in.size() / 2);
    for (int n = d; n < frames; ++n) {
        const size_t i = static_cast<size_t>(n) * 2;
        const size_t j = static_cast<size_t>(n - d) * 2;
        ASSERT_EQ(std::memcmp(&out[i], &in[j], 2 * sizeof(float)), 0) << "frame " << n;
    }
}

// Tras limitar un pico, la ganancia vuelve a EXACTAMENTE 1 (release + media entera, sin residuo).
TEST(MasterStage, GainReturnsToExactUnityAfterTheRelease) {
    Signal in = noise(0.2, kSr * 3, 5u);
    for (int n = 1000; n < 1100; ++n) {   // una rafaga fuerte al principio
        in[static_cast<size_t>(n) * 2] = 3.0f;
        in[static_cast<size_t>(n) * 2 + 1] = -3.0f;
    }
    const Signal out = runStage(in, params(0.0f, -1.0f, 50.0f), 256);
    const int d = 240;
    const int frames = static_cast<int>(in.size() / 2);
    for (int n = frames - kSr; n < frames; ++n) {   // el ultimo segundo
        const size_t i = static_cast<size_t>(n) * 2;
        const size_t j = static_cast<size_t>(n - d) * 2;
        ASSERT_EQ(std::memcmp(&out[i], &in[j], 2 * sizeof(float)), 0) << "frame " << n;
    }
}

// AC: latencia declarada = medida (impulso), a varios sample rates.
TEST(MasterStage, DeclaredLatencyEqualsMeasuredImpulseDelay) {
    for (int sr : {44100, 48000, 96000, 192000}) {
        wm::MasterStage st;
        st.prepareMaster(sr);
        st.configureMaster(params(0.0f));
        st.resetMaster();
        const int declared = st.latencyFrames();
        EXPECT_EQ(declared, static_cast<int>(std::lround(0.005 * sr))) << sr;
        Signal s(static_cast<size_t>(declared + 400) * 2, 0.0f);
        s[100 * 2] = 0.5f;
        s[100 * 2 + 1] = 0.25f;
        st.processMaster(s.data(), static_cast<int>(s.size() / 2));
        int first = -1;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] != 0.0f) { first = static_cast<int>(i / 2); break; }
        }
        EXPECT_EQ(first, 100 + declared) << sr;
        EXPECT_EQ(s[static_cast<size_t>(100 + declared) * 2], 0.5f);
        EXPECT_EQ(s[static_cast<size_t>(100 + declared) * 2 + 1], 0.25f);
    }
}

// AC: bit a bit con bloques de 1 / 64 / 4096 (y uno impar) aun limitando.
TEST(MasterStage, OutputIsBitExactAcrossBlockSizes) {
    const Signal in = appendSilence(noise(4.0, kSr / 2, 3u), 600);
    const wm::MasterParams p = params(3.0f, -1.0f, 120.0f);
    const Signal ref = runStage(in, p, 4096);
    for (int block : {1, 64, 333}) {
        const Signal out = runStage(in, p, block);
        ASSERT_EQ(std::memcmp(out.data(), ref.data(), ref.size() * sizeof(float)), 0) << block;
    }
    EXPECT_GT(samplePeak(in), 1.0);
    EXPECT_NE(std::memcmp(ref.data(), in.data(), ref.size() * sizeof(float)), 0);
}

// AC: dos corridas iguales; una instancia reusada tras reset == una fresca.
TEST(MasterStage, FreshAndResetInstancesAgree) {
    const Signal in = appendSilence(noise(4.0, kSr / 4, 9u), 300);
    const wm::MasterParams p = params(0.0f, -2.0f, 80.0f);
    const Signal a = runStage(in, p, 128);
    const Signal b = runStage(in, p, 128);
    ASSERT_EQ(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)), 0);

    wm::MasterStage st;
    st.prepareMaster(kSr);
    st.configureMaster(p);
    st.resetMaster();
    Signal warm = noise(5.0, kSr / 8, 1u);   // ensucia ring, deque y gain
    st.processMaster(warm.data(), static_cast<int>(warm.size() / 2));
    st.resetMaster();
    Signal c = in;
    st.processMaster(c.data(), static_cast<int>(c.size() / 2));
    ASSERT_EQ(std::memcmp(a.data(), c.data(), a.size() * sizeof(float)), 0);
}

// La ganancia rampea LINEAL dentro del bloque (sin zipper) y el reset la salta al objetivo.
TEST(MasterStage, GainRampsLinearlyPerBlockAndResetSnaps) {
    constexpr int kBlock = 480;
    wm::MasterStage st;
    st.prepareMaster(kSr);
    st.configureMaster(params(0.0f));
    st.resetMaster();
    const int d = st.latencyFrames();
    Signal first(static_cast<size_t>(kBlock) * 2, 0.1f);
    st.processMaster(first.data(), kBlock);
    st.configureMaster(params(6.0f));   // objetivo 1,995..., desde 1,0
    Signal blk(static_cast<size_t>(kBlock) * 2, 0.1f);
    st.processMaster(blk.data(), kBlock);
    Signal tail(static_cast<size_t>(2 * d + kBlock) * 2, 0.0f);
    st.processMaster(tail.data(), static_cast<int>(tail.size() / 2));
    // El bloque 2 se escucha D frames despues, ya multiplicado: 0,1 * (1 + (g-1)*(i+1)/N)
    const double g = std::pow(10.0, 6.0 / 20.0);
    Signal all;
    all.insert(all.end(), first.begin(), first.end());
    all.insert(all.end(), blk.begin(), blk.end());
    all.insert(all.end(), tail.begin(), tail.end());
    for (int i = 0; i < kBlock; ++i) {
        const double expect = 0.1 * (1.0 + (g - 1.0) * (i + 1) / kBlock);
        EXPECT_NEAR(all[static_cast<size_t>(kBlock + i + d) * 2], expect, 1e-6) << i;
    }
    // Reset: salta al objetivo sin rampa.
    st.resetMaster();
    Signal again(static_cast<size_t>(kBlock) * 2, 0.1f);
    st.processMaster(again.data(), kBlock);
    Signal rest(static_cast<size_t>(d) * 2, 0.1f);
    st.processMaster(rest.data(), d);
    EXPECT_NEAR(rest[0], 0.1 * g, 1e-6);
}

// AC: 0 alloc en process (el estado se dimensiona en prepare).
TEST(MasterStage, ProcessDoesNotAllocate) {
    wm::MasterStage st;
    st.prepareMaster(kSr);
    st.configureMaster(params(4.0f));
    st.resetMaster();
    Signal in = noise(3.0, 4096, 2u);
    wma_test::armAllocCounter();
    for (int i = 0; i < 20; ++i) st.processMaster(in.data(), 4096);
    st.resetMaster();
    const int64_t allocs = wma_test::disarmAllocCounter();
    EXPECT_EQ(allocs, 0);
}

// Entradas no finitas no llegan a la salida ni envenenan el limitador.
TEST(MasterStage, NonFiniteInputIsSilencedNotPropagated) {
    Signal in = noise(0.5, 4800, 4u);
    in[1000] = std::numeric_limits<float>::quiet_NaN();
    in[1001] = std::numeric_limits<float>::infinity();
    in[3000] = -std::numeric_limits<float>::infinity();
    const Signal out = runStage(in, params(0.0f), 480);
    for (float v : out) ASSERT_TRUE(std::isfinite(v));
}
