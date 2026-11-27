/**
 * @file test_hostile_corpus.cpp
 * @brief REQ-048 S1 (AC-048.1, AC-048.5): el corpus hostil de stb_vorbis se rechaza, sin reportes.
 *
 * Cada `.ogg` de `testdata/hostile/` es un artefacto que produjo el fuzzer de S0, copiado tal cual
 * (ver su `MANIFEST.txt`); no se escriben inputs hostiles a mano. Con stb_vorbis 1.22 cada uno
 * dispara ASan o UBSan; con el fork de SDL_mixer y el parche de `lookup1_values`, el decoder lo
 * rechaza como un error. El "rojo antes" se prueba revirtiendo el arreglo (opcion 2 del humano,
 * 29/09): la evidencia esta en las Notas de la etapa S1.
 *
 * Lo que afirma solo tiene sentido bajo ASan+UBSan con `halt_on_error` (la Verification de S1 y
 * el job `cpp-tests-asan`): ahi un over-read o un cast UB TUMBA el test. Sin sanitizers, F1-F3 en
 * 1.22 piden una asignacion gigante que falla y el decoder igual devuelve NULL.
 *
 * Dos caminos por archivo:
 *  - el decoder solo (`stb_vorbis_open_memory`), que es donde viven los defectos;
 *  - el camino real del motor: el Ogg como muestra de un `.sf3` mínimo por `tsf_load_memory`.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "tsf.h"

// Solo las declaraciones: la implementacion la compila `tsf_impl.cpp`, con el mismo
// STB_VORBIS_NO_STDIO y la misma linkage (C++), y es la que corre en el producto.
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

namespace {

std::vector<uint8_t> readFile(const std::string& name) {
    std::ifstream in(std::string(WMA_HOSTILE_TESTDATA_DIR) + "/" + name, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void put16(std::vector<uint8_t>& v, uint32_t x) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); }
void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, x & 0xFFFF); put16(v, x >> 16); }
void putFourCC(std::vector<uint8_t>& v, const char* c) { v.insert(v.end(), c, c + 4); }
void putName20(std::vector<uint8_t>& v, const char* s) {
    char buf[20] = {};
    std::strncpy(buf, s, sizeof buf - 1);
    v.insert(v.end(), buf, buf + sizeof buf);
}
void putChunk(std::vector<uint8_t>& v, const char* id, const std::vector<uint8_t>& data) {
    putFourCC(v, id);
    put32(v, static_cast<uint32_t>(data.size()));
    v.insert(v.end(), data.begin(), data.end());
    if (data.size() % 2) v.push_back(0);  // relleno RIFF
}

/// Un `.sf3` mínimo: un preset -> un instrumento -> una zona -> una muestra cuyo `smpl` es `ogg`,
/// con el bit de compresion (0x10) en `sampleType` y `start`/`end` en BYTES, la convencion que lee
/// tsf (`tsf_decode_sf3_samples`). `smpl` se rellena a largo par: tsf hoy no respeta el byte de
/// relleno RIFF de un chunk impar y rechazaria el font antes de llegar al decoder (REQ-048 S2, 2.2c).
std::vector<uint8_t> wrapAsSf3(std::vector<uint8_t> ogg) {
    const uint32_t oggLen = static_cast<uint32_t>(ogg.size());
    if (ogg.size() % 2) ogg.push_back(0);

    std::vector<uint8_t> phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr;
    putName20(phdr, "Hostile"); put16(phdr, 0); put16(phdr, 0); put16(phdr, 0); put32(phdr, 0); put32(phdr, 0); put32(phdr, 0);
    putName20(phdr, "EOP");     put16(phdr, 0); put16(phdr, 0); put16(phdr, 1); put32(phdr, 0); put32(phdr, 0); put32(phdr, 0);
    put16(pbag, 0); put16(pbag, 0);
    put16(pbag, 1); put16(pbag, 0);
    for (int i = 0; i < 5; ++i) put16(pmod, 0);
    put16(pgen, 41); put16(pgen, 0);  // instrument -> 0
    put16(pgen, 0); put16(pgen, 0);
    putName20(inst, "Hostile"); put16(inst, 0);
    putName20(inst, "EOI");     put16(inst, 1);
    put16(ibag, 0); put16(ibag, 0);
    put16(ibag, 1); put16(ibag, 0);
    for (int i = 0; i < 5; ++i) put16(imod, 0);
    put16(igen, 53); put16(igen, 0);  // sampleID -> 0
    put16(igen, 0); put16(igen, 0);
    putName20(shdr, "Hostile");
    put32(shdr, 0); put32(shdr, oggLen); put32(shdr, 0); put32(shdr, 0);  // start, end (bytes), loops
    put32(shdr, 44100); shdr.push_back(60); shdr.push_back(0); put16(shdr, 0);
    put16(shdr, 0x11);  // monoSample | Vorbis
    putName20(shdr, "EOS");
    put32(shdr, 0); put32(shdr, 0); put32(shdr, 0); put32(shdr, 0); put32(shdr, 0);
    shdr.push_back(0); shdr.push_back(0); put16(shdr, 0); put16(shdr, 0);

    std::vector<uint8_t> info, ifil, sdta, pdta, body, riff;
    put16(ifil, 3); put16(ifil, 1);
    putFourCC(info, "INFO"); putChunk(info, "ifil", ifil);
    putFourCC(sdta, "sdta"); putChunk(sdta, "smpl", ogg);
    putFourCC(pdta, "pdta");
    putChunk(pdta, "phdr", phdr); putChunk(pdta, "pbag", pbag); putChunk(pdta, "pmod", pmod);
    putChunk(pdta, "pgen", pgen); putChunk(pdta, "inst", inst); putChunk(pdta, "ibag", ibag);
    putChunk(pdta, "imod", imod); putChunk(pdta, "igen", igen); putChunk(pdta, "shdr", shdr);
    putFourCC(body, "sfbk");
    putChunk(body, "LIST", info); putChunk(body, "LIST", sdta); putChunk(body, "LIST", pdta);
    putChunk(riff, "RIFF", body);
    return riff;
}

struct Signature {
    const char* file;
    const char* what;  // lo que lo mata en 1.22 (MANIFEST.txt)
};

const Signature kSignatures[] = {
    {"stb_F1_vendor_len.ogg", "F1: ASan requested-allocation-size, start_decoder (vendor len)"},
    {"stb_F2_comment_list_len.ogg", "F2: ASan requested-allocation-size, start_decoder (comment_list)"},
    {"stb_F3_comment_len.ogg", "F3: ASan requested-allocation-size, start_decoder (comment len)"},
    {"stb_F4_deinit_null.ogg", "F4: UBSan null load, vorbis_deinit (CVE-2023-45680)"},
    {"stb_F5_lookup1_values.ogg", "F5: UBSan float-cast-overflow, lookup1_values"},
};

class HostileCorpus : public ::testing::TestWithParam<Signature> {};

}  // namespace

// AC-048.1 — el decoder solo. Bug que atrapa: un decoder que, ante un setup hostil, asigna con un
// tamaño truncado, lee un puntero nulo al cerrar o convierte inf a int, en vez de devolver error.
// El `error` distinto de "no es Ogg" dice que el archivo SI llego al parser del setup.
TEST_P(HostileCorpus, TheDecoderRejectsIt) {
    const Signature& s = GetParam();
    const std::vector<uint8_t> ogg = readFile(s.file);
    ASSERT_FALSE(ogg.empty()) << "falta " << s.file << " en " << WMA_HOSTILE_TESTDATA_DIR;
    ASSERT_EQ(0, std::memcmp(ogg.data(), "OggS", 4)) << s.file << " no empieza con OggS";

    int error = 0;
    stb_vorbis* v = stb_vorbis_open_memory(ogg.data(), static_cast<int>(ogg.size()), &error, nullptr);
    if (v) stb_vorbis_close(v);
    EXPECT_EQ(v, nullptr) << s.what;
    EXPECT_NE(error, VORBIS__no_error) << s.what;
    EXPECT_NE(error, VORBIS_missing_capture_pattern) << s.what << ": el rechazo fue antes del setup";
}

// AC-048.1 / AC-048.5 — el camino real del motor: el mismo Ogg como muestra de un `.sf3`. Bug que
// atrapa: tsf que, con el decoder rechazando, igual deja un font a medias o lee fuera del buffer.
TEST_P(HostileCorpus, LoadingItAsAnSf3FailsCleanly) {
    const Signature& s = GetParam();
    const std::vector<uint8_t> ogg = readFile(s.file);
    ASSERT_FALSE(ogg.empty()) << "falta " << s.file;

    // Control: con la marca OggS rota, tsf saltea la muestra (`tsf_decode_sf3_samples`) y el font
    // CARGA. Si no cargara, el envoltorio estaria mal y el rechazo de abajo no probaria nada.
    std::vector<uint8_t> notOgg = ogg;
    notOgg[0] = 'X';
    const std::vector<uint8_t> control = wrapAsSf3(notOgg);
    tsf* ok = tsf_load_memory(control.data(), static_cast<int>(control.size()));
    ASSERT_NE(ok, nullptr) << "el .sf3 de control no carga: el envoltorio esta mal";
    tsf_close(ok);

    const std::vector<uint8_t> sf3 = wrapAsSf3(ogg);
    tsf* f = tsf_load_memory(sf3.data(), static_cast<int>(sf3.size()));
    if (f) tsf_close(f);
    EXPECT_EQ(f, nullptr) << s.what;
}

INSTANTIATE_TEST_SUITE_P(StbVorbis, HostileCorpus, ::testing::ValuesIn(kSignatures),
                         [](const ::testing::TestParamInfo<Signature>& i) {
                             return std::string(i.param.file).substr(4, 2);  // "F1".."F5"
                         });
