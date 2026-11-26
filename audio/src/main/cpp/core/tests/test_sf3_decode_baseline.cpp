/**
 * @file test_sf3_decode_baseline.cpp
 * @brief REQ-048 S1 (AC-048.4): el PCM que deja la carga de un SF3 legitimo, contra su linea de base.
 *
 * Cambiar el decoder de SF3 (stb_vorbis 1.22 -> el fork de SDL_mixer) o la lectura del hydra en
 * `tsf.h` (S2) NO puede cambiar las muestras de un font legitimo sin que se vea. Se afirma sobre lo
 * que tsf deja en `fontSamples` despues de `tsf_load_memory` —por preset: los indices de cada region
 * (offset, end, loop) y sus floats, bit a bit—, no sobre un render, que mezcla envolventes y filtro y
 * puede tapar un cambio de muestras.
 *
 * La linea de base (`testdata/sf3-pcm-baseline/GeneralUser_GS.tsv`) se tomo con stb_vorbis 1.22
 * ANTES del cambio. Su diff es la revision: si un cambio la mueve a proposito, se regenera con
 *
 *     WMA_WRITE_SF3_PCM_BASELINE=1 ./core_tests --gtest_filter='Sf3DecodeBaseline.*'
 *
 * y el PR explica la diferencia (AC-048.4: dB RMS y pico por preset).
 *
 * El font es `GeneralUser_GS.sf3`, del bundle de NoisyPad (checkout hermano). Sin el archivo el test
 * sale SKIPPED y NUNCA passed (regla de REQ-032), igual que los demas tests sobre el font real.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "tsf.h"
#include "tsf_ext.h"

namespace {

constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

uint64_t fnv(uint64_t h, const void* p, size_t n) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * kFnvPrime;
    return h;
}

std::string hex64(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::vector<unsigned char> generalUserBytes() {
    const char* env = std::getenv("WMA_GENERALUSER_SF3");
    const std::string path = env ? env : WMA_GENERALUSER_SF3_DEFAULT;
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Una linea por preset: `bank \t program \t nombre \t regiones \t muestras \t fnv`, mas una
/// primera linea con la identidad del archivo (bytes y fnv de los bytes crudos): si NoisyPad cambia
/// el font, la linea de base no aplica y el test lo dice en vez de reportar 269 diferencias.
std::vector<std::string> measure(const std::vector<unsigned char>& bytes) {
    std::vector<std::string> lines;
    lines.push_back("font\t" + std::to_string(bytes.size()) + "\t" + hex64(fnv(kFnvOffset, bytes.data(), bytes.size())));
    tsf* f = tsf_load_memory(bytes.data(), static_cast<int>(bytes.size()));
    if (!f) return lines;
    for (int p = 0; p < tsf_get_presetcount(f); ++p) {
        uint64_t h = kFnvOffset;
        unsigned long long samples = 0;
        const int regions = tsf_ext_preset_region_count(f, p);
        for (int r = 0; r < regions; ++r) {
            const float* base = nullptr;
            unsigned int idx[4] = {0, 0, 0, 0};  // offset, end, loop_start, loop_end
            if (!tsf_ext_region_samples(f, p, r, &base, &idx[0], &idx[1], &idx[2], &idx[3])) continue;
            h = fnv(h, idx, sizeof idx);
            if (idx[1] > idx[0]) {
                h = fnv(h, base + idx[0], static_cast<size_t>(idx[1] - idx[0]) * sizeof(float));
                samples += idx[1] - idx[0];
            }
        }
        std::ostringstream line;
        line << tsf_get_preset_bank(f, p) << '\t' << tsf_get_preset_number(f, p) << '\t'
             << tsf_get_presetname(f, p) << '\t' << regions << '\t' << samples << '\t' << hex64(h);
        lines.push_back(line.str());
    }
    tsf_close(f);
    return lines;
}

std::vector<std::string> readBaseline(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    std::string l;
    while (std::getline(in, l)) {
        if (l.empty() || l[0] == '#') continue;
        lines.push_back(l);
    }
    return lines;
}

}  // namespace

// AC-048.4 — el PCM de los 269 presets de GeneralUser, bit a bit igual al de stb_vorbis 1.22.
// Bug que atrapa: un decoder (o un acotado de `shdr` en S2) que cambia, corre o recorta muestras
// de un font legitimo —un float distinto, un loop movido, una region que queda vacia—.
TEST(Sf3DecodeBaseline, GeneralUserDecodesBitIdenticalToTheBaseline) {
    const std::vector<unsigned char> bytes = generalUserBytes();
    if (bytes.empty()) GTEST_SKIP() << "sin GeneralUser_GS.sf3 (WMA_GENERALUSER_SF3 o el checkout hermano de NoisyPad)";

    const std::vector<std::string> got = measure(bytes);
    ASSERT_GT(got.size(), 1u) << "tsf_load_memory rechazo GeneralUser_GS.sf3";

    if (std::getenv("WMA_WRITE_SF3_PCM_BASELINE")) {
        std::ofstream out(WMA_SF3_PCM_BASELINE);
        out << "# REQ-048 S1 (AC-048.4): PCM de GeneralUser_GS.sf3 tras tsf_load_memory, por preset.\n"
               "# Lo escribe test_sf3_decode_baseline.cpp (WMA_WRITE_SF3_PCM_BASELINE=1); su diff es la revision.\n"
               "# font <bytes> <fnv> | bank program nombre regiones muestras fnv(indices+floats de cada region)\n";
        for (const std::string& l : got) out << l << '\n';
        GTEST_SKIP() << "linea de base escrita en " << WMA_SF3_PCM_BASELINE;
    }

    const std::vector<std::string> want = readBaseline(WMA_SF3_PCM_BASELINE);
    ASSERT_FALSE(want.empty()) << "falta la linea de base " << WMA_SF3_PCM_BASELINE;
    ASSERT_EQ(got.front(), want.front())
        << "GeneralUser_GS.sf3 no es el font de la linea de base: re-medir a proposito (ver el encabezado)";
    ASSERT_EQ(got.size(), want.size()) << "cambio la cantidad de presets";
    int differing = 0;
    for (size_t i = 1; i < got.size(); ++i) {
        if (got[i] != want[i]) {
            ++differing;
            ADD_FAILURE() << "preset distinto:\n  base: " << want[i] << "\n  hoy:  " << got[i];
        }
    }
    EXPECT_EQ(differing, 0);
}
