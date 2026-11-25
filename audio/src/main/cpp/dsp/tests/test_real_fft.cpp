/**
 * test_real_fft.cpp — contrato de dsp/RealFFT.h (MINI-058).
 *
 * Las tolerancias son las del spec; los sinteticos son deterministas (semilla fija).
 */
#include "RealFFT.h"

#include "../../core/tests/support/RtAllocCounter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <random>
#include <vector>

#if defined(WMA_WRAP_MALLOC)
// RtAllocCounter no ve `malloc` y pffft es C: `--wrap` cuenta las llamadas de todo lo que se
// linkea en este binario. Sin `--wrap` (Apple ld) queda solo el contador de `operator new`.
namespace {
thread_local bool tMallocArmed = false;
thread_local int64_t tMallocCount = 0;
}  // namespace
extern "C" {
void* __real_malloc(std::size_t);
void* __real_calloc(std::size_t, std::size_t);
void* __real_realloc(void*, std::size_t);
void* __wrap_malloc(std::size_t n) {
    if (tMallocArmed) ++tMallocCount;
    return __real_malloc(n);
}
void* __wrap_calloc(std::size_t n, std::size_t m) {
    if (tMallocArmed) ++tMallocCount;
    return __real_calloc(n, m);
}
void* __wrap_realloc(void* p, std::size_t n) {
    if (tMallocArmed) ++tMallocCount;
    return __real_realloc(p, n);
}
}
#endif

namespace {

constexpr int kSizes[] = {256, 512, 1024, 2048, 4096, 8192, 16384};
constexpr double kTwoPi = 6.283185307179586476925286766559;

std::vector<float> fftNoise(int n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> x(static_cast<size_t>(n));
    for (float& v : x) v = dist(rng);
    return x;
}

std::vector<float> fftSine(int n, int k) {
    std::vector<float> x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        // k*i mod n: el argumento queda en un ciclo y el seno no pierde precision.
        const int64_t phase = (static_cast<int64_t>(k) * i) % n;
        x[static_cast<size_t>(i)] = static_cast<float>(std::sin(kTwoPi * static_cast<double>(phase) / n));
    }
    return x;
}

// |X|² de un bin interior, contado dos veces (simetria hermitica).
double interiorEnergy(const std::vector<float>& s, int bin) {
    const double re = s[static_cast<size_t>(2 * bin)];
    const double im = s[static_cast<size_t>(2 * bin + 1)];
    return 2.0 * (re * re + im * im);
}

// Σ|X|² sobre los n bins.
double spectrumEnergy(const std::vector<float>& s, int n) {
    double e = static_cast<double>(s[0]) * s[0] + static_cast<double>(s[1]) * s[1];
    for (int b = 1; b < n / 2; ++b) e += interiorEnergy(s, b);
    return e;
}

}  // namespace

// AC: ida y vuelta, error maximo <= 1e-5.
TEST(RealFFT, RoundTripReturnsTheSignal) {
    for (int n : kSizes) {
        RealFFT fft(n);
        ASSERT_TRUE(fft.valid()) << n;
        const std::vector<float> x = fftNoise(n, 1u + static_cast<uint32_t>(n));
        std::vector<float> s(x.size()), y(x.size());
        ASSERT_TRUE(fft.transformForward(x.data(), s.data()));
        ASSERT_TRUE(fft.transformInverse(s.data(), y.data()));
        double worst = 0.0;
        for (size_t i = 0; i < x.size(); ++i) worst = std::max(worst, std::abs(static_cast<double>(y[i]) - x[i]));
        EXPECT_LE(worst, 1e-5) << n;
    }
}

// AC: un seno de k ciclos enteros concentra >= 1 - 1e-6 en el bin k.
TEST(RealFFT, SineOfWholeCyclesLandsInItsBin) {
    for (int n : kSizes) {
        RealFFT fft(n);
        ASSERT_TRUE(fft.valid()) << n;
        for (int k : {1, 2, 3, 37, n / 8, n / 4, n / 4 + 1, n / 2 - 1}) {
            const std::vector<float> x = fftSine(n, k);
            std::vector<float> s(x.size());
            ASSERT_TRUE(fft.transformForward(x.data(), s.data()));
            EXPECT_GE(interiorEnergy(s, k) / spectrumEnergy(s, n), 1.0 - 1e-6) << "n=" << n << " k=" << k;
        }
    }
}

// AC: Parseval.
TEST(RealFFT, ParsevalHoldsOnNoise) {
    for (int n : kSizes) {
        RealFFT fft(n);
        ASSERT_TRUE(fft.valid()) << n;
        const std::vector<float> x = fftNoise(n, 77u + static_cast<uint32_t>(n));
        std::vector<float> s(x.size());
        ASSERT_TRUE(fft.transformForward(x.data(), s.data()));
        double ex = 0.0;
        for (float v : x) ex += static_cast<double>(v) * v;
        EXPECT_LE(std::abs(ex - spectrumEnergy(s, n) / n), 1e-4 * ex) << n;
    }
}

// Layout, signo y escala contra la DFT de la definicion: {DC, Nyquist, re1, im1, ...}.
TEST(RealFFT, SpectrumLayoutMatchesTheDefinition) {
    constexpr int n = 256;
    RealFFT fft(n);
    ASSERT_TRUE(fft.valid());
    const std::vector<float> x = fftNoise(n, 5u);
    std::vector<float> s(x.size());
    ASSERT_TRUE(fft.transformForward(x.data(), s.data()));
    for (int b = 0; b <= n / 2; ++b) {
        std::complex<double> acc;
        for (int i = 0; i < n; ++i) {
            acc += static_cast<double>(x[static_cast<size_t>(i)]) * std::polar(1.0, -kTwoPi * ((b * i) % n) / n);
        }
        if (b == 0) {
            EXPECT_NEAR(s[0], acc.real(), 1e-3);
        } else if (b == n / 2) {
            EXPECT_NEAR(s[1], acc.real(), 1e-3);
        } else {
            EXPECT_NEAR(s[static_cast<size_t>(2 * b)], acc.real(), 1e-3) << b;
            EXPECT_NEAR(s[static_cast<size_t>(2 * b + 1)], acc.imag(), 1e-3) << b;
        }
    }
}

// El llamador no tiene que alinear, y puede transformar sobre el mismo buffer.
TEST(RealFFT, AcceptsUnalignedAndInPlaceBuffers) {
    constexpr int n = 1024;
    RealFFT fft(n);
    ASSERT_TRUE(fft.valid());
    const std::vector<float> x = fftNoise(n, 9u);
    std::vector<float> expected(x.size());
    ASSERT_TRUE(fft.transformForward(x.data(), expected.data()));
    for (size_t off = 0; off < 4; ++off) {
        std::vector<float> buf(static_cast<size_t>(n) + 4);
        std::copy(x.begin(), x.end(), buf.begin() + static_cast<std::ptrdiff_t>(off));
        float* p = buf.data() + off;
        ASSERT_TRUE(fft.transformForward(p, p));
        for (size_t i = 0; i < x.size(); ++i) ASSERT_EQ(p[i], expected[i]) << off << " " << i;
        ASSERT_TRUE(fft.transformInverse(p, p));
        for (size_t i = 0; i < x.size(); ++i) ASSERT_NEAR(p[i], x[i], 1e-5) << off << " " << i;
    }
}

// AC: transformar no aloca; el plan y el scratch se reservan en el constructor.
TEST(RealFFT, TransformsDoNotAllocate) {
    // Control positivo: los instrumentos cuentan.
    wma_test::armAllocCounter();
    { std::vector<float> probe(16); ASSERT_NE(probe.data(), nullptr); }
    ASSERT_GE(wma_test::disarmAllocCounter(), 1);
#if defined(WMA_WRAP_MALLOC)
    tMallocCount = 0;
    tMallocArmed = true;
    const bool probeValid = RealFFT(256).valid();
    tMallocArmed = false;
    ASSERT_TRUE(probeValid);
    // pffft_new_setup (2) y los buffers alineados (3).
    ASSERT_GE(tMallocCount, 5) << "el contador de malloc no ve a pffft";
#endif

    for (int n : kSizes) {
        RealFFT fft(n);
        ASSERT_TRUE(fft.valid()) << n;
        const std::vector<float> x = fftNoise(n, 3u);
        std::vector<float> s(x.size()), y(x.size());
        bool ok = true;
        wma_test::armAllocCounter();
#if defined(WMA_WRAP_MALLOC)
        tMallocCount = 0;
        tMallocArmed = true;
#endif
        for (int i = 0; i < 4; ++i) {
            ok = fft.transformForward(x.data(), s.data()) && ok;
            ok = fft.transformInverse(s.data(), y.data()) && ok;
        }
#if defined(WMA_WRAP_MALLOC)
        tMallocArmed = false;
        EXPECT_EQ(tMallocCount, 0) << n;
#endif
        EXPECT_EQ(wma_test::disarmAllocCounter(), 0) << n;
        EXPECT_TRUE(ok) << n;
    }
}

// AC: un n que no es potencia de 2 o esta fuera de rango falla explicito, y no escribe.
TEST(RealFFT, InvalidSizesAreRejectedExplicitly) {
    for (int n : {INT_MIN, -256, -1, 0, 1, 32, 128, 255, 257, 288, 384, 480, 1000, 16383, 16385, 32768, 1 << 27, INT_MAX}) {
        RealFFT fft(n);
        EXPECT_FALSE(fft.valid()) << n;
        const float in[4] = {1.0f, 2.0f, 3.0f, 4.0f};
        float out[4] = {9.0f, 9.0f, 9.0f, 9.0f};
        EXPECT_FALSE(fft.transformForward(in, out)) << n;
        EXPECT_FALSE(fft.transformInverse(in, out)) << n;
        for (float v : out) EXPECT_EQ(v, 9.0f) << n;
    }
}
