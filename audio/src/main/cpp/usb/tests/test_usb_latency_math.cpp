// Fase 0.5 — Host-side latency math (hallazgo L7)

#include <gtest/gtest.h>

#include "../UsbLatencyMath.h"

using watermelon_audio::usb::computeOutputLatencyMs;
using watermelon_audio::usb::computeInputLatencyMs;

TEST(UsbLatencyMathTest, OutputFullSpeedTypical) {
    // 48 kHz stereo, ring holds ~32 ms (1536 frames * 2 ch = 3072 samples),
    // 3 transfers in flight of 8 packets * 48 frames = 384 frames each.
    // ring: 1536 frames = 32 ms; inflight: 3 * 384 * 0.5 = 576 frames = 12 ms.
    const float ms = computeOutputLatencyMs(
        /*ringSamples=*/3072, /*channels=*/2, /*pending=*/3,
        /*framesPerTransfer=*/384, /*sampleRate=*/48000);
    EXPECT_NEAR(ms, 32.0f + 12.0f, 0.01f);
}

TEST(UsbLatencyMathTest, OutputNoPendingIsJustRing) {
    const float ms = computeOutputLatencyMs(960, 2, 0, 384, 48000);
    EXPECT_NEAR(ms, 10.0f, 0.01f);  // 480 frames / 48000 * 1000
}

TEST(UsbLatencyMathTest, InputTypical) {
    // 480 frames ring (10 ms) + half a 384-frame transfer (4 ms).
    const float ms = computeInputLatencyMs(960, 2, 384, 48000);
    EXPECT_NEAR(ms, 10.0f + 4.0f, 0.01f);
}

TEST(UsbLatencyMathTest, HighSpeedScaling) {
    // HS bInterval=1: 8000 packets/s, 6 frames/packet, transfer of 64 packets
    // = 384 frames. Same numeric latency as FS for the same frame counts.
    const float ms = computeOutputLatencyMs(0, 2, 1, 384, 48000);
    EXPECT_NEAR(ms, 4.0f, 0.01f);  // only inflight: 192 frames / 48000
}

TEST(UsbLatencyMathTest, GuardsInvalidInputs) {
    EXPECT_EQ(computeOutputLatencyMs(3072, 2, 3, 384, 0), 0.0f);
    EXPECT_EQ(computeOutputLatencyMs(3072, 0, 3, 384, 48000), 0.0f);
    EXPECT_EQ(computeInputLatencyMs(960, 0, 384, 48000), 0.0f);
}

// ============================================================================
// REQ-050 S3 (AC-050.9, D16) — el techo de latencia que el backend DECLARA.
//
// El runner USB fallaba por construcción: pedía <= 20 ms a un backend que en SAFE
// sostiene ~37 ms con tráfico perfecto (MINI-039). El techo ahora lo declara el
// backend con la misma definición que su latencia medida (ring + mitad de lo en vuelo),
// en el peor caso que su propio pacer permite.
// ============================================================================

#include "../LatencyProfile.h"

using watermelon_audio::usb::declaredOutputLatencyCeilingMs;
using watermelon_audio::usb::outputPacketsInFlightDepth;
using watermelon_audio::usb::outputRingTargetSamples;

namespace {
// SAFE en high-speed a 48 kHz: 64 paquetes de 6 frames por transfer (8 ms), 3 en vuelo,
// jitter budget inicial 24 ms con techo 24 + 12, y un bloque DSP de 256 frames.
constexpr int kPacketsPerTransfer = 64;
constexpr int kFramesPerPacket = 6;
constexpr int kFramesPerTransfer = kPacketsPerTransfer * kFramesPerPacket;  // 384
constexpr int kNumTransfers = 3;
constexpr int kJitterMaxMs = 36;
constexpr int kDspBlock = 256;
constexpr int kRate = 48000;
constexpr int kChannels = 2;
}  // namespace

// Bug que atrapa: el número exacto de SAFE. (384 + 36*48 + 256 + 3*384/2) / 48 = 61,33 ms.
TEST(UsbLatencyMathTest, AC050_9_DeclaredCeilingSafeHighSpeed) {
    const float ms = declaredOutputLatencyCeilingMs(
        kFramesPerTransfer, kJitterMaxMs, kDspBlock, kNumTransfers, kRate);
    EXPECT_NEAR(ms, (384.0f + 1728.0f + 256.0f + 576.0f) / 48.0f, 0.01f);
}

// La propiedad que hace al techo "algo que el backend puede cumplir": el peor caso que el
// pacer permite (ring justo debajo del target con el jitter budget en su máximo, más un
// bloque DSP producido encima, y la cola entera en vuelo) medido con computeOutputLatencyMs
// no lo supera. Bug que atrapa: un techo que omite el bloque DSP o lo en vuelo — fallaría
// en un backend sano, que es exactamente MINI-039.
TEST(UsbLatencyMathTest, AC050_9_TheWorstCaseThePacerAllowsFitsUnderTheCeiling) {
    const size_t target = outputRingTargetSamples(
        kPacketsPerTransfer, kFramesPerPacket, kJitterMaxMs, kRate, kChannels);
    const double worstRing = double(target) - 1.0 + double(kDspBlock * kChannels);
    const float worst = computeOutputLatencyMs(
        worstRing, kChannels, kNumTransfers, kFramesPerTransfer, kRate);
    const float ceiling = declaredOutputLatencyCeilingMs(
        kFramesPerTransfer, kJitterMaxMs, kDspBlock, kNumTransfers, kRate);
    EXPECT_LE(worst, ceiling);
    // Y no es un techo infinito: queda a menos de un frame del peor caso.
    EXPECT_LT(ceiling - worst, 1000.0f / kRate + 0.001f);
}

// El gemelo: el g42 midió 37,5 ms con tráfico perfecto (smoke-20261001-154601-71029).
TEST(UsbLatencyMathTest, AC050_9_TheLatencyMeasuredOnTheG42FitsUnderTheSafeCeiling) {
    EXPECT_LT(37.5f, declaredOutputLatencyCeilingMs(
        kFramesPerTransfer, kJitterMaxMs, kDspBlock, kNumTransfers, kRate));
}

TEST(UsbLatencyMathTest, AC050_9_WithoutARateThereIsNoCeiling) {
    EXPECT_EQ(declaredOutputLatencyCeilingMs(kFramesPerTransfer, kJitterMaxMs, kDspBlock, kNumTransfers, 0), 0.0f);
}

// AC-050.9 (D16): lo que está en vuelo es la cola entera de salida. El g42 mostraba
// enviados − completados = 192 con cero errores: 3 transfers × 64 paquetes.
TEST(UsbLatencyMathTest, AC050_9_InFlightDepthIsTheWholeOutputQueue) {
    EXPECT_EQ(outputPacketsInFlightDepth(kNumTransfers, kPacketsPerTransfer), 192u);
    EXPECT_EQ(outputPacketsInFlightDepth(0, kPacketsPerTransfer), 0u);
    EXPECT_EQ(outputPacketsInFlightDepth(-1, kPacketsPerTransfer), 0u);
}
