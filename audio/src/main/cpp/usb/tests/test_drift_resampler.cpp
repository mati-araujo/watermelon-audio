#include "../../backends/DriftResampler.h"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

using namespace watermelon_audio;

namespace {

TEST(DriftResamplerTest, UnityRatioPreservesFrameCountAndSamples) {
    DriftResampler resampler(48000.0f, 48000.0f);

    std::vector<float> input = {
        0.0f, 0.1f,
        1.0f, 1.1f,
        2.0f, 2.1f,
        3.0f, 3.1f
    };
    std::vector<float> output(8, -1.0f);

    const int frames = resampler.process(input.data(), 4, 2, output.data(), 4);

    ASSERT_EQ(frames, 4);
    for (size_t i = 0; i < input.size(); ++i) {
        EXPECT_FLOAT_EQ(output[i], input[i]);
    }
}

// 🔴 Los dos tests de la perilla de ppm se borraron con ella (MINI-011). NO fue
// "sacar cobertura": existian SOLO para ejercer `setDriftCorrection`, que ningun
// camino de produccion llamaba, y cuya correccion ya la hace `ClockController`
// por cadencia de paquetes. Ver el encabezado de DriftResampler.h.
//
// Lo que esta clase SI corrige —el desajuste nominal de rates— lo cubre el test
// de arriba (ratio unitario) y `configure()`.

} // namespace
