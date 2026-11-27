/**
 * @file test_hostile_corpus.cpp
 * @brief REQ-048 S1 y S2 (AC-048.1, AC-048.3, AC-048.5): el corpus hostil se rechaza, sin reportes.
 *
 * Dos corpus, los dos en `testdata/hostile/` y descritos en su `MANIFEST.txt`: los `.ogg` de
 * stb_vorbis (S1: lo que sigue en este comentario y la primera mitad del archivo) y los
 * `.sf2`/`.sf3` de `tsf.h` (S2: la segunda mitad, con su propio comentario).
 *
 * Cada `.ogg` de `testdata/hostile/` es un artefacto que produjo el fuzzer de S0, copiado tal cual
 * (ver su `MANIFEST.txt`); no se escriben inputs hostiles a mano. Con stb_vorbis 1.22 cada uno
 * dispara ASan o UBSan; con el fork de SDL_mixer y el parche de `lookup1_values`, el decoder lo
 * rechaza como un error. El "rojo antes" se prueba revirtiendo el arreglo (opcion 2 del humano,
 * 29/09): la evidencia esta en las Notas de la etapa S1.
 *
 * Lo que afirma solo tiene sentido bajo ASan+UBSan con `halt_on_error` (la Verification de S1 y
 * el job `cpp-tests-asan`): ahi un over-read o un cast UB TUMBA el test. 🔴 F5 es un cast UB
 * (`float-cast-overflow`), que GCC NO incluye en `-fsanitize=undefined`: el job lo pide aparte.
 * Sin sanitizers, F1-F3 en 1.22 piden una asignacion gigante que falla y el decoder igual
 * devuelve NULL, y F5 pasa.
 *
 * Lo que estos tests solos NO cubren: un decoder que rechaza TODO Ogg los pasa. Por eso el archivo
 * cierra con el control positivo (S2, tarea 2.2d): un Vorbis legitimo que tiene que cargar y
 * decodificar a la senoide que se codifico, en el CI y sin el font de NoisyPad.
 *
 * Dos caminos por archivo:
 *  - el decoder solo (`stb_vorbis_open_memory`), que es donde viven los defectos;
 *  - el camino real del motor: el Ogg como muestra de un `.sf3` mínimo por `tsf_load_memory`.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "tsf.h"
#include "tsf_ext.h"

// Solo las declaraciones: la implementacion la compila `tsf_impl.cpp`, con el mismo
// STB_VORBIS_NO_STDIO y la misma linkage (C++), y es la que corre en el producto.
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

namespace {

std::vector<uint8_t> readFile(const std::string& name, const char* dir = WMA_HOSTILE_TESTDATA_DIR) {
    std::ifstream in(std::string(dir) + "/" + name, std::ios::binary);
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
/// tsf (`tsf_decode_sf3_samples`). Por defecto `smpl` se rellena a largo par: hasta REQ-048 S2
/// (2.2c) tsf no respetaba el byte de relleno RIFF de un chunk impar, y asi los tests del corpus no
/// dependen de ese arreglo. Con `oddSmpl`, un Ogg impar va tal cual, con su byte de relleno RIFF
/// (lo que escribe un `.sf3` legitimo, y lo que tsf rechazaba). `smplTail` agrega bytes de `smpl` que
/// ninguna muestra referencia, y `eosAtSmplEnd` pone el `start` del EOS en `smplLength / 2` (la
/// convencion de GeneralUser) en vez de 0.
std::vector<uint8_t> wrapAsSf3(std::vector<uint8_t> ogg, bool oddSmpl = false, size_t smplTail = 0,
                               bool eosAtSmplEnd = false) {
    const uint32_t oggLen = static_cast<uint32_t>(ogg.size());
    if (!oddSmpl && ogg.size() % 2) ogg.push_back(0);
    ogg.insert(ogg.end(), smplTail, 0);
    const uint32_t eosStart = eosAtSmplEnd ? static_cast<uint32_t>(ogg.size() / 2) : 0;

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
    put32(shdr, eosStart); put32(shdr, 0); put32(shdr, 0); put32(shdr, 0); put32(shdr, 0);
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

    // Control: con la marca OggS rota, tsf no la decodifica como Vorbis y el font CARGA (el
    // registro terminal EOS convierte el `smpl` entero a PCM de 16 bits, `tsf_decode_sf3_samples`).
    // Si no cargara, el envoltorio estaria mal y el rechazo de abajo no probaria nada.
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

// ===========================================================================
// REQ-048 S2 — el corpus hostil de tsf.h (F6-F10 de S0)
// ===========================================================================
//
// Los `.sf2`/`.sf3` de `tsf_*` son artefactos del fuzzer sobre `tsf_load_memory`, copiados tal cual
// (`MANIFEST.txt` dice cual es cual y que lo mataba). Los de S0 van uno por firma; el de #123 lo
// genero el `gen.cpp` de S0 sobre `MinimalSoundFont.h`, con SampleID = 0x7FFF. Ninguno se escribio a
// mano.
//
// Con el `tsf.h` de antes cada uno de S0 lee fuera del heap (HBO, BUS o UAF; ver el MANIFEST). Con
// S2 cada uno lo rechaza un chequeo de `tsf.h` y la carga devuelve NULL. Los de S2 salieron de
// fuzzear el `tsf.h` de S2 sin UN chequeo (un mutante) que ningun artefacto de S0 aislaba: con el
// `tsf.h` de antes no leen fuera, porque son mutaciones de un `.sf3` de Ogg impar que se cortaba en
// el relleno RIFF (2.2c) antes del hydra; con 2.2c llegan, y su chequeo es lo unico que los para.
// Que chequeo mata a cual, y que cada chequeo tiene al menos un archivo que lo vuelve rojo, esta
// medido por mutacion en las Notas de la etapa S2. Como en la mitad de stb, lo que afirma solo pesa
// bajo ASan+UBSan.

namespace {

struct TsfSignature {
    const char* file;
    const char* what;  // el defecto, como lo clasifico S0
};

const TsfSignature kTsfSignatures[] = {
    {"tsf_F6_ogg_end_past_smpl.sf3", "F6: el end de una muestra Ogg pasa el chunk smpl (stb leia fuera)"},
    {"tsf_F7_ogg_start_past_smpl.sf3", "F7: el start de una muestra Ogg pasa el chunk smpl"},
    {"tsf_F7_ogg_range_wild.sf3", "F7: start/end de una muestra Ogg a ~1-3 GB"},
    {"tsf_F8_pcm_start_wild.sf3", "F8: muestra PCM tras un Ogg con start a 3,7 GB"},
    {"tsf_F8_pcm_start_past_decoded.sf3", "F8: muestra PCM con start mayor que lo decodificado"},
    {"tsf_F8_ogg_end_oom.sf3", "F8: muestra Ogg con end a 251 M (OOM en libFuzzer)"},
    {"tsf_F9_preset_bags_backwards.sf3", "F9: presetBagNdx que retrocede (UAF)"},
    {"tsf_F9_preset_bag_wild.sf2", "F9: presetBagNdx 65535"},
    {"tsf_F9_preset_bag_past_pbag.sf3", "F9: el EOP cierra mas alla de pbag"},
    {"tsf_F9_eop_bag_past_pbag.sf3", "F9 (S2): el EOP cierra en pbagNum o mas alla, con los presets en orden"},
    {"tsf_F9_empty_phdr.sf2", "F9 (S2): phdr de largo 0, sin terminal EOP"},
    {"tsf_F9_pbag_gens_backwards.sf3", "F9 (S2): un pbag cuyo genNdx retrocede"},
    {"tsf_F9_pbag_gen_past_pgen.sf2", "F9: el ultimo pbag cierra mas alla de pgen"},
    {"tsf_F9_inst_bags_backwards.sf2", "F9: instBagNdx que retrocede"},
    {"tsf_F9_inst_bag_past_ibag.sf3", "F9 (S2): el EOI cierra mas alla de ibag"},
    {"tsf_F9_ibag_gens_backwards.sf3", "F9 (S2): un ibag cuyo genNdx retrocede"},
    {"tsf_F9_ibag_gen_past_igen.sf2", "F9: el ultimo ibag cierra mas alla de igen"},
    {"tsf_F10_sampleid_past_shdr.sf2", "F10: sampleID al EOS y mas alla de shdr"},
    {"tsf_F10_tsf123_sampleid.sf2", "F10 / TinySoundFont #123: sampleID 0x7FFF"},
};

class HostileTsfCorpus : public ::testing::TestWithParam<TsfSignature> {};

}  // namespace

// AC-048.1 / AC-048.3 / AC-048.5 — el camino real: los bytes del archivo por `tsf_load_memory`, que
// es lo que hace `SoundFontManager::parseFont` con lo que NoisyPad importa o descarga. Bug que
// atrapa: tsf que sigue un indice del hydra, o un rango de `shdr`, sin acotarlo a su tabla o al
// chunk `smpl`, y lee fuera del buffer (o, con el rango acotado a medias, carga un font a medias).
TEST_P(HostileTsfCorpus, LoadingItIsRejected) {
    const TsfSignature& s = GetParam();
    const std::vector<uint8_t> bytes = readFile(s.file);
    ASSERT_FALSE(bytes.empty()) << "falta " << s.file << " en " << WMA_HOSTILE_TESTDATA_DIR;
    ASSERT_EQ(0, std::memcmp(bytes.data(), "RIFF", 4)) << s.file << " no empieza con RIFF";

    tsf* f = tsf_load_memory(bytes.data(), static_cast<int>(bytes.size()));
    if (f) tsf_close(f);
    EXPECT_EQ(f, nullptr) << s.what;
}

INSTANTIATE_TEST_SUITE_P(Tsf, HostileTsfCorpus, ::testing::ValuesIn(kTsfSignatures),
                         [](const ::testing::TestParamInfo<TsfSignature>& i) {
                             std::string n(i.param.file);  // "tsf_F6_x.sf3" -> "F6_x"
                             return n.substr(4, n.rfind('.') - 4);
                         });

// AC-048.3 — el unico artefacto que CARGA: una zona de preset cuyo instrumento es el terminal EOI
// (`pgen` -> inst = instNum - 1). Un instrumento fuera de rango se saltea, no rechaza el font (lo
// hacia tsf con `>= instNum`, y lo hace el lector de moduladores); S2 solo corre el limite para que
// el EOI no cuente, porque el instrumento w se lee con el w+1 que cierra sus bags. Bug que atrapa:
// el limite viejo, que lee un registro de `inst` de mas (UAF bajo ASan); o un arreglo que rechaza el
// font en vez de saltear la zona. La semilla de la que salio tiene 1 region: sin la zona, 0.
TEST(HostileTsfInstrument, AZoneWhoseInstrumentIsTheEoiTerminalIsSkippedNotRead) {
    const std::vector<uint8_t> bytes = readFile("tsf_F9_zone_inst_is_eoi.sf3");
    ASSERT_FALSE(bytes.empty()) << "falta tsf_F9_zone_inst_is_eoi.sf3 en " << WMA_HOSTILE_TESTDATA_DIR;

    tsf* f = tsf_load_memory(bytes.data(), static_cast<int>(bytes.size()));
    ASSERT_NE(f, nullptr) << "un instrumento fuera de rango rechazo el font: se saltea";
    EXPECT_EQ(tsf_get_presetcount(f), 1);
    EXPECT_EQ(tsf_ext_preset_region_count(f, 0), 0) << "la zona que apunta al EOI no se salteo";
    tsf_close(f);
}

// ===========================================================================
// REQ-048 S2 (tarea 2.2d) — el control positivo del decoder, en el CI
// ===========================================================================
//
// Sin esto, todo lo de arriba pasa con un decoder que rechaza TODO Ogg, o con un
// `tsf_decode_sf3_samples` que saltea cada muestra Vorbis: la unica prueba que decodificaba Vorbis
// de verdad era la linea de base de GeneralUser (`test_sf3_decode_baseline`), que sin el font de
// NoisyPad sale SKIPPED, en el CI y en los worktrees. El control es un Ogg legitimo versionado con
// su receta (`testdata/sf3-decoder-control/MANIFEST.txt`, decision del humano del 27/11): 0,25 s de
// una senoide de 441 Hz a 0,5. Se compara contra la senoide regenerada aca, por SNR y por largo, no
// por hash: el PCM de stb_vorbis puede diferir en el ultimo bit entre la libm (y el FMA) de macOS y
// la de Linux.
//
// El Ogg mide 4969 bytes, IMPAR a proposito: va en el `.sf3` tal cual, con su byte de relleno RIFF,
// que es el caso real de 2.2c (el `.sf3` legitimo de MINI-038 que tsf rechazaba).

namespace {

constexpr double kControlRate = 44100.0;
constexpr double kControlHz = 441.0;
constexpr double kControlAmplitude = 0.5;
constexpr unsigned kControlFrames = 11025;     // 0,25 s: lo que se codifico
constexpr unsigned kControlOggFrames = 11072;  // el granule de la ultima pagina (el encoder redondea a 64)
constexpr double kControlMinSnrDb = 35.0;      // medido: 44,5 dB (MANIFEST)

/// SNR de los primeros `kControlFrames` de la muestra contra la senoide que se codifico.
double controlSnrDb(const float* samples, unsigned offset) {
    double signal = 0.0, error = 0.0;
    for (unsigned n = 0; n < kControlFrames; ++n) {
        const double want = kControlAmplitude * std::sin(2.0 * M_PI * kControlHz * n / kControlRate);
        const double diff = samples[offset + n] - want;
        signal += want * want;
        error += diff * diff;
    }
    return 10.0 * std::log10(signal / error);
}

}  // namespace

// AC-048.4, y el control de AC-048.1 / AC-048.5 — un `.sf3` legitimo carga y su muestra sale
// como se codifico. Bug que atrapa: un decoder que rechaza todo Ogg o devuelve silencio o basura;
// un `tsf_decode_sf3_samples` que saltea las muestras Vorbis (o un acotado de `shdr` que las da por
// fuera del chunk); el relleno RIFF del `smpl` impar sin respetar (NULL, como antes de 2.2c); y un
// corrimiento de una muestra (24 dB) o la mitad de la ganancia (6 dB), que la SNR no deja pasar.
TEST(Sf3DecoderControl, AnOddLengthOggSineLoadsAndDecodesToTheSineThatWasEncoded) {
    const std::vector<uint8_t> ogg = readFile("sine441.ogg", WMA_SF3_DECODER_CONTROL_DIR);
    ASSERT_EQ(ogg.size(), 4969u) << "no es el Ogg del MANIFEST de " << WMA_SF3_DECODER_CONTROL_DIR;

    const std::vector<uint8_t> sf3 = wrapAsSf3(ogg, /*oddSmpl=*/true);
    tsf* f = tsf_load_memory(sf3.data(), static_cast<int>(sf3.size()));
    ASSERT_NE(f, nullptr) << "tsf rechazo un .sf3 legitimo con un smpl impar";

    const float* samples = nullptr;
    unsigned offset = 0, end = 0, loopStart = 0, loopEnd = 0;
    ASSERT_EQ(tsf_ext_region_samples(f, 0, 0, &samples, &offset, &end, &loopStart, &loopEnd), 1);
    // tsf cierra toda region en `end + 1` (`tsf_load_presets`): los frames que declara el Ogg, mas uno.
    EXPECT_EQ(end - offset, kControlOggFrames + 1) << "el decoder no entrego lo que declara el Ogg";
    ASSERT_GE(end - offset, kControlFrames) << "falta senoide";
    EXPECT_GE(controlSnrDb(samples, offset), kControlMinSnrDb);
    tsf_close(f);
}

// AC-048.4 (review de S2) — el EOS de un `.sf3` con la convencion de GeneralUser (`start =
// smplLength / 2`) puede quedar mas alla de lo decodificado: pasa en un font de muestras cortas,
// donde los encabezados Ogg (3-4 KB por muestra) pesan mas que el PCM. Aca se arma con el Ogg del
// control y bytes de `smpl` que nada referencia. Bug que atrapa: el rechazo de F8 (`start` PCM
// mayor que lo decodificado) aplicado tambien a ese EOS, que no tiene nada que convertir: un falso
// rechazo de un font legitimo (que en los ABI de 32 bits, por aritmetica modular, cargaba).
TEST(Sf3DecoderControl, AnEosAtTheEndOfSmplPastWhatWasDecodedStillLoads) {
    const std::vector<uint8_t> ogg = readFile("sine441.ogg", WMA_SF3_DECODER_CONTROL_DIR);
    ASSERT_EQ(ogg.size(), 4969u) << "no es el Ogg del MANIFEST de " << WMA_SF3_DECODER_CONTROL_DIR;
    constexpr size_t kTail = 20000;  // smpl = 4970 + 20000: el EOS en 12485, lo decodificado 11072
    ASSERT_GT((ogg.size() + 1 + kTail) / 2, kControlOggFrames) << "el EOS no quedaria pasado lo decodificado";

    const std::vector<uint8_t> sf3 = wrapAsSf3(ogg, /*oddSmpl=*/false, kTail, /*eosAtSmplEnd=*/true);
    tsf* f = tsf_load_memory(sf3.data(), static_cast<int>(sf3.size()));
    ASSERT_NE(f, nullptr) << "un EOS al final de smpl, pasado lo decodificado, rechazo un font legitimo";

    const float* samples = nullptr;
    unsigned offset = 0, end = 0, loopStart = 0, loopEnd = 0;
    ASSERT_EQ(tsf_ext_region_samples(f, 0, 0, &samples, &offset, &end, &loopStart, &loopEnd), 1);
    // Sin el EOS convertido no queda nada despues de la muestra, y tsf cierra la region en
    // `fontSampleCount` en vez de `end + 1`: los frames que declara el Ogg, justos.
    EXPECT_EQ(end - offset, kControlOggFrames);
    ASSERT_GE(end - offset, kControlFrames);
    EXPECT_GE(controlSnrDb(samples, offset), kControlMinSnrDb);
    tsf_close(f);
}
