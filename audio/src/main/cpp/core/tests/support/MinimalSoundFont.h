#pragma once

/**
 * MinimalSoundFont.h — host test support.
 *
 * Genera en memoria el .sf2 **más chico que TinySoundFont acepta**, para cubrir
 * el camino de ÉXITO de los tres `loadSoundFont*`. Ese camino era la deuda
 * abierta del bug 3 de WA-2.0: los caminos negativos ya están cubiertos
 * (`test_c_api_synth.cpp`), pero un loader que falla no configura ningún rate,
 * así que la tasa negociada sólo se puede observar después de una carga que
 * funcione.
 *
 * ## Por qué se genera y no se commitea un .sf2
 *
 * Un binario en el repo no dice por qué tiene la forma que tiene. Acá cada
 * chunk está donde está por un motivo que se puede leer, y si TinySoundFont
 * cambia de requisitos el compilador y el test lo dicen en vez de dejar un
 * archivo opaco que nadie sabe regenerar.
 *
 * ## Lo que el loader realmente exige (leído de `tsf_load`, no del spec SF2)
 *
 * - `RIFF` … `sfbk`.
 * - Un `LIST pdta` con **los nueve** chunks de la hydra —`phdr pbag pmod pgen
 *   inst ibag imod igen shdr`—; si falta **uno solo**, `tsf_load` aborta. Cada
 *   uno además tiene que medir múltiplo exacto de su tamaño de registro
 *   (38/4/10/4/22/4/10/4/46), o el parser lo saltea como desconocido y termina
 *   igual de nulo.
 * - Un `LIST sdta` con un `smpl` de al menos un `short`.
 * - **`ifil` NO hace falta**: `tsf_load` lo saltea. Se incluye igual porque un
 *   .sf2 sin versión es inválido para cualquier otra herramienta, y este
 *   fixture no debería ser el único programa del mundo que lo acepta.
 *
 * Las listas terminan con un registro **terminal** (EOP/EOI/EOS): tsf recorre
 * `num - 1` entradas y usa la última como centinela de índices. Por eso hay dos
 * de casi todo y uno solo de `pmod`/`imod`.
 */

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace wma_test {

namespace sf2 {

/** Generadores SF2 que se usan acá (`tsf.h`, enum de genOper). */
enum : uint16_t {
    kGenInstrument = 41,  ///< zona de preset -> índice de instrumento
    kGenSampleId = 53,    ///< zona de instrumento -> índice de sample
};

inline void put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

inline void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

inline void putFourCC(std::vector<uint8_t>& out, const char* cc) {
    out.insert(out.end(), cc, cc + 4);
}

/** Nombre de campo fijo de 20 bytes, rellenado con ceros (`tsf_char20`). */
inline void putName20(std::vector<uint8_t>& out, const char* name) {
    char buf[20];
    std::memset(buf, 0, sizeof(buf));
    std::strncpy(buf, name, sizeof(buf) - 1);
    out.insert(out.end(), buf, buf + sizeof(buf));
}

/** `<id><u32 size><payload>`, con el byte de padding que exige RIFF si es impar. */
inline void putChunk(std::vector<uint8_t>& out, const char* id,
                     const std::vector<uint8_t>& payload) {
    putFourCC(out, id);
    put32(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    if (payload.size() % 2 != 0) out.push_back(0);
}

}  // namespace sf2

/**
 * @brief Un SoundFont válido con **un** preset, **un** instrumento y **un** sample.
 *
 * @param sampleRateInHeader la tasa que va en el `shdr`. Es la del SAMPLE, y es
 *   deliberadamente distinta de la tasa de salida que el motor le pasa a
 *   `tsf_set_output`: confundir las dos es justo el error que el bug 3 hacía
 *   fácil de cometer. Un test que quiera distinguirlas puede mover ésta sin
 *   tocar la otra.
 */
inline std::vector<uint8_t> makeMinimalSoundFont(uint32_t sampleRateInHeader = 22050) {
    using namespace sf2;

    // ---- sdta: 64 samples de 16 bits. tsf pide >= un short; 64 deja lugar a
    // que el shdr declare un loop interno sin salirse del buffer.
    constexpr uint32_t kSampleCount = 64;
    std::vector<uint8_t> smpl;
    for (uint32_t i = 0; i < kSampleCount; ++i) put16(smpl, 0);

    std::vector<uint8_t> sdta;
    putFourCC(sdta, "sdta");
    putChunk(sdta, "smpl", smpl);

    // ---- pdta: los nueve, cada uno con su terminal.
    std::vector<uint8_t> phdr;
    putName20(phdr, "Test Preset");
    put16(phdr, 0);   // preset
    put16(phdr, 0);   // bank
    put16(phdr, 0);   // presetBagNdx -> pbag[0]
    put32(phdr, 0); put32(phdr, 0); put32(phdr, 0);  // library / genre / morphology
    putName20(phdr, "EOP");                          // terminal
    put16(phdr, 0); put16(phdr, 0);
    put16(phdr, 1);   // presetBagNdx del terminal = fin de las zonas
    put32(phdr, 0); put32(phdr, 0); put32(phdr, 0);

    std::vector<uint8_t> pbag;
    put16(pbag, 0); put16(pbag, 0);  // zona 0: genNdx=0, modNdx=0
    put16(pbag, 1); put16(pbag, 0);  // terminal

    std::vector<uint8_t> pmod;
    for (int i = 0; i < 5; ++i) put16(pmod, 0);  // sólo el terminal (10 bytes)

    std::vector<uint8_t> pgen;
    put16(pgen, kGenInstrument); put16(pgen, 0);  // -> instrumento 0
    put16(pgen, 0); put16(pgen, 0);               // terminal

    std::vector<uint8_t> inst;
    putName20(inst, "Test Instrument");
    put16(inst, 0);   // instBagNdx -> ibag[0]
    putName20(inst, "EOI");
    put16(inst, 1);   // terminal

    std::vector<uint8_t> ibag;
    put16(ibag, 0); put16(ibag, 0);
    put16(ibag, 1); put16(ibag, 0);  // terminal

    std::vector<uint8_t> imod;
    for (int i = 0; i < 5; ++i) put16(imod, 0);  // terminal

    std::vector<uint8_t> igen;
    put16(igen, kGenSampleId); put16(igen, 0);  // -> sample 0
    put16(igen, 0); put16(igen, 0);             // terminal

    std::vector<uint8_t> shdr;
    putName20(shdr, "Test Sample");
    put32(shdr, 0);                  // start
    put32(shdr, kSampleCount - 1);   // end
    put32(shdr, 1);                  // startLoop
    put32(shdr, kSampleCount - 2);   // endLoop
    put32(shdr, sampleRateInHeader);
    shdr.push_back(60);              // originalPitch = C4
    shdr.push_back(0);               // pitchCorrection
    put16(shdr, 0);                  // sampleLink
    put16(shdr, 1);                  // sampleType = monoSample
    putName20(shdr, "EOS");          // terminal
    put32(shdr, 0); put32(shdr, 0); put32(shdr, 0); put32(shdr, 0); put32(shdr, 0);
    shdr.push_back(0); shdr.push_back(0);
    put16(shdr, 0); put16(shdr, 0);

    std::vector<uint8_t> pdta;
    putFourCC(pdta, "pdta");
    putChunk(pdta, "phdr", phdr);
    putChunk(pdta, "pbag", pbag);
    putChunk(pdta, "pmod", pmod);
    putChunk(pdta, "pgen", pgen);
    putChunk(pdta, "inst", inst);
    putChunk(pdta, "ibag", ibag);
    putChunk(pdta, "imod", imod);
    putChunk(pdta, "igen", igen);
    putChunk(pdta, "shdr", shdr);

    // ---- INFO. tsf lo saltea; va para que el archivo sea un .sf2 de verdad.
    std::vector<uint8_t> ifil;
    put16(ifil, 2); put16(ifil, 1);  // SoundFont 2.01
    std::vector<uint8_t> info;
    putFourCC(info, "INFO");
    putChunk(info, "ifil", ifil);
    {
        std::vector<uint8_t> isng;
        const char* engine = "EMU8000";
        isng.insert(isng.end(), engine, engine + std::strlen(engine) + 1);
        putChunk(info, "isng", isng);
        std::vector<uint8_t> inam;
        const char* bank = "watermelon-test";
        inam.insert(inam.end(), bank, bank + std::strlen(bank) + 1);
        putChunk(info, "INAM", inam);
    }

    std::vector<uint8_t> body;
    putFourCC(body, "sfbk");
    putChunk(body, "LIST", info);
    putChunk(body, "LIST", sdta);
    putChunk(body, "LIST", pdta);

    std::vector<uint8_t> out;
    putChunk(out, "RIFF", body);
    return out;
}

}  // namespace wma_test
