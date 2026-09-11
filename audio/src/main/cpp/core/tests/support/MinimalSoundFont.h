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
 * de casi todo.
 *
 * ## Moduladores, y por qué son CUATRO ámbitos (REQ-039 S2)
 *
 * Sin `ModulatorPlacement` el archivo sale **byte a byte como antes**: `pmod` e
 * `imod` llevan sólo su terminal, que es lo que este fixture emitía desde que
 * existe. Al pedir moduladores aparecen las bags que hagan falta, y ahí la forma
 * importa: la zona **global** es la primera bag que NO declara `GenInstrument`
 * (preset) o `GenSampleID` (instrumento), y sus moduladores aplican a **todas**
 * las zonas de su padre — no a una región.
 *
 * 🔴 Emitir sólo moduladores de zona dejaría esta etapa probando el caso raro:
 * medido sobre el `SoundFont-Spec-Test`, **21 de 25** moduladores son globales, y
 * **18 de los 19** de velocity. Un fixture que no puede producir el caso que el
 * test dice cubrir deja el test verde **por vacío**.
 */

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace wma_test {

namespace sf2 {

/** Generadores SF2 que se usan acá (`tsf.h`, enum de genOper). */
enum : uint16_t {
    kGenKeyRange = 43,    ///< zona -> rango de teclas MIDI [lo, hi] (byte bajo = lo)
    kGenInstrument = 41,  ///< zona de preset -> índice de instrumento
    kGenSampleModes = 54, ///< zona de instrumento -> modo de loop (0 = one-shot, 1 = loop)
    kGenSampleId = 53,    ///< zona de instrumento -> índice de sample
};

/** Destinos de modulador que usan los tests (`genOper` como destino). */
enum : uint16_t {
    kGenInitialFilterFc = 8,       ///< corte del low-pass, en cents absolutos
    kGenInitialAttenuation = 48,   ///< atenuación, en centibeles
    kGenPan = 17,                  ///< paneo, en 0,1 %
};

/**
 * `modSrcOper` / `modAmtSrcOper` — el campo de 16 bits del spec SF2 §8.2.
 *
 * Se arma con [[srcOper]] en vez de a mano porque el orden de los bits es la
 * clase de detalle que se escribe mal una vez y después nadie relee:
 * `bit 0-6` índice, `bit 7` CC, `bit 8` dirección, `bit 9` polaridad,
 * `bit 10-15` curva.
 */
enum : uint16_t {
    kSrcNone = 0,        ///< "sin controlador": el modulador aporta su amount tal cual
    kSrcVelocity = 2,    ///< NoteOnVelocity
    kSrcKeyNumber = 3,   ///< NoteOnKeyNumber
};
enum : uint16_t { kCurveLinear = 0, kCurveConcave = 1, kCurveConvex = 2, kCurveSwitch = 3 };

/** Arma un `modSrcOper` del spec §8.2 a partir de sus cinco campos. */
inline constexpr uint16_t srcOper(uint16_t index, bool isCC, bool decreasing,
                                  bool bipolar, uint16_t curve) {
    return static_cast<uint16_t>((index & 0x7F) | (isCC ? 0x80 : 0) |
                                 (decreasing ? 0x100 : 0) | (bipolar ? 0x200 : 0) |
                                 ((curve & 0x3F) << 10));
}

/** Un modulador tal como vive en `pmod`/`imod`: cinco campos de 16 bits. */
struct Modulator {
    uint16_t srcOper = kSrcNone;
    uint16_t destOper = 0;
    int16_t amount = 0;
    uint16_t amtSrcOper = kSrcNone;
    uint16_t transOper = 0;  ///< 0 = lineal. Distinto de 0 es lo que AC-039.8 manda rechazar.
};

/**
 * Dónde va cada modulador — **los cuatro ámbitos de SF2 §9.5**, no dos.
 *
 * 🔴 Que sean cuatro y no dos es el hallazgo de REQ-039 S1 (tarea 1.6), y no es
 * teórico: sobre el `SoundFont-Spec-Test` **21 de 25** moduladores viven en la
 * zona global de su instrumento, y **18 de los 19** de velocity. Un fixture que
 * sólo supiera emitir moduladores de zona dejaría los tests de esta etapa verdes
 * sin poder producir el caso que dicen cubrir.
 *
 * La zona global es la **primera** bag que no declara `GenInstrument` (preset) o
 * `GenSampleID` (instrumento) — así la reconoce `tsf_load_presets`, y así se
 * emite acá. Las bags globales sólo se agregan **si se piden moduladores para
 * ellas**: sin eso el archivo sale byte a byte igual que antes de REQ-039.
 */
struct ModulatorPlacement {
    std::vector<Modulator> instrumentGlobal;  ///< aplica a todas las zonas del instrumento
    std::vector<Modulator> instrumentZone;    ///< sólo a la zona que trae el sample
    std::vector<Modulator> presetGlobal;      ///< aplica a todas las zonas del preset
    std::vector<Modulator> presetZone;        ///< sólo a la zona que trae el instrumento

    bool empty() const {
        return instrumentGlobal.empty() && instrumentZone.empty() &&
               presetGlobal.empty() && presetZone.empty();
    }
};

inline void put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

/** Escribe un modulador en su chunk, en el orden del spec. */
inline void putModulator(std::vector<uint8_t>& out, const Modulator& m) {
    put16(out, m.srcOper);
    put16(out, m.destOper);
    put16(out, static_cast<uint16_t>(m.amount));
    put16(out, m.amtSrcOper);
    put16(out, m.transOper);
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
 * @param looping si el sample se reproduce EN LOOP (generador `sampleModes` = 1) en vez de
 *   una sola vez. Default `false`, que es como estaba antes de REQ-008: los tests que sólo
 *   cargan y leen metadata no cambian en nada. Lo necesita cualquier test que mida NIVEL,
 *   porque el one-shot dura 4 ms y no le da tiempo a converger a ningún suavizado.
 * @param sampleRateInHeader la tasa que va en el `shdr`. Es la del SAMPLE, y es
 *   deliberadamente distinta de la tasa de salida que el motor le pasa a
 *   `tsf_set_output`: confundir las dos es justo el error que el bug 3 hacía
 *   fácil de cometer. Un test que quiera distinguirlas puede mover ésta sin
 *   tocar la otra.
 */
/**
 * @param keyRangeLo / @param keyRangeHi rango de teclas que el ARCHIVO declara, como
 *        generador `keyRange` (genOper 43) en la zona del instrumento. Con -1 no se
 *        escribe el generador y tsf aplica su default de region (0..127).
 *
 *        🔴 Existe para poder DISTINGUIR de dónde sale el rango publicado. Antes de
 *        MINI-017 el motor lo adivinaba del NOMBRE del preset, así que un fixture sin
 *        este generador no puede juzgar nada: mediría la heurística contra sí misma.
 *        Los tests que importan escriben un rango que CONTRADICE lo que la heurística
 *        habría dado para "Test Preset".
 */
inline std::vector<uint8_t> makeMinimalSoundFont(uint32_t sampleRateInHeader = 22050,
                                                bool looping = false,
                                                int keyRangeLo = -1,
                                                int keyRangeHi = -1,
                                                const sf2::ModulatorPlacement& mods = {}) {
    using namespace sf2;

    // ---- sdta: 64 samples de 16 bits. tsf pide >= un short; 64 deja lugar a
    // que el shdr declare un loop interno sin salirse del buffer.
    // El contenido NO es silencio, y eso importa: mientras las 64 muestras
    // fueron ceros, cualquier test que renderizara media "salio silencio" tanto
    // si el motor andaba como si no. Una onda cuadrada de amplitud media hace
    // que "sono algo" sea una afirmacion con contenido.
    constexpr uint32_t kSampleCount = 64;
    constexpr int16_t kAmplitude = 16384;
    std::vector<uint8_t> smpl;
    for (uint32_t i = 0; i < kSampleCount; ++i) {
        put16(smpl, static_cast<uint16_t>((i % 16 < 8) ? kAmplitude : -kAmplitude));
    }

    // ---- Los 46 sample points en cero que el spec de SF2 exige DESPUES de cada
    // sample (§6.1: "followed by a minimum of forty-six zero valued sample data
    // points"). No es ceremonia: el render de tsf interpola leyendo `pos + 1`,
    // asi que con `end` en el ultimo sample real la ultima interpolacion cae
    // justo afuera del buffer.
    //
    // Faltaban desde que se creo el fixture y NADIE lo noto, porque hasta
    // 2026-07-28 ningun test RENDERIZABA desde el: todos cargaban y leian
    // metadata. El primero que toco una nota lo destapo, y lo destapo ASan —
    // `heap-buffer-overflow ... READ of size 4` en `tsf_voice_render`, leyendo
    // 0 bytes despues de la region de 256 bytes de `fontSamples`.
    //
    // Se comprobo que NO dependia del re-rate: con la misma tasa, sin copia ni
    // swap de por medio, desbordaba igual. O sea que el defecto era del fixture,
    // no del codigo que se estaba probando.
    constexpr uint32_t kTrailingZeroPoints = 46;
    for (uint32_t i = 0; i < kTrailingZeroPoints; ++i) put16(smpl, 0);

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
    // El `presetBagNdx` del terminal marca DÓNDE TERMINAN las zonas de este preset,
    // así que tiene que contar las que de verdad se escribieron: con zona global
    // son dos, y dejarlo en 1 escondería la zona real detrás del centinela.
    put16(phdr, mods.presetGlobal.empty() ? 1 : 2);
    put32(phdr, 0); put32(phdr, 0); put32(phdr, 0);

    // ---- pbag / pmod.
    //
    // La zona GLOBAL de preset es la primera bag SIN `GenInstrument`, y sólo se
    // emite si se pidieron moduladores para ella: sin eso el archivo sale igual
    // que antes de REQ-039, y los tests que ya existían no ven ninguna
    // diferencia. Los `genNdx` de la global y de la zona coinciden, o sea que la
    // global no declara generadores — su rango es vacío, que es lo que se quiere.
    const bool tieneGlobalDePreset = !mods.presetGlobal.empty();
    const uint16_t nPresetGlobal = static_cast<uint16_t>(mods.presetGlobal.size());
    const uint16_t nPresetZone = static_cast<uint16_t>(mods.presetZone.size());

    std::vector<uint8_t> pbag;
    if (tieneGlobalDePreset) {
        put16(pbag, 0); put16(pbag, 0);              // zona GLOBAL: sin generadores, mods desde 0
    }
    put16(pbag, 0); put16(pbag, nPresetGlobal);      // zona: genNdx=0, sus mods empiezan tras los globales
    put16(pbag, 1); put16(pbag, static_cast<uint16_t>(nPresetGlobal + nPresetZone));  // terminal

    std::vector<uint8_t> pmod;
    for (const auto& m : mods.presetGlobal) putModulator(pmod, m);
    for (const auto& m : mods.presetZone) putModulator(pmod, m);
    for (int i = 0; i < 5; ++i) put16(pmod, 0);  // terminal (10 bytes)

    std::vector<uint8_t> pgen;
    put16(pgen, kGenInstrument); put16(pgen, 0);  // -> instrumento 0
    put16(pgen, 0); put16(pgen, 0);               // terminal

    std::vector<uint8_t> inst;
    putName20(inst, "Test Instrument");
    put16(inst, 0);   // instBagNdx -> ibag[0]
    putName20(inst, "EOI");
    // Igual que en `phdr`: el terminal cuenta las bags que de verdad se escriben.
    put16(inst, mods.instrumentGlobal.empty() ? 1 : 2);   // terminal

    const bool declaraRango = (keyRangeLo >= 0 && keyRangeHi >= 0);

    // ---- ibag / imod.
    //
    // La zona GLOBAL de instrumento es la primera bag SIN `GenSampleID`. Ahí es
    // donde viven casi todos los moduladores de un font real: medido sobre el
    // `SoundFont-Spec-Test`, 21 de 25 — y 18 de los 19 de velocity.
    const bool tieneGlobalDeInstrumento = !mods.instrumentGlobal.empty();
    const uint16_t nInstGlobal = static_cast<uint16_t>(mods.instrumentGlobal.size());
    const uint16_t nInstZone = static_cast<uint16_t>(mods.instrumentZone.size());

    std::vector<uint8_t> ibag;
    if (tieneGlobalDeInstrumento) {
        put16(ibag, 0); put16(ibag, 0);           // zona GLOBAL: sin generadores, mods desde 0
    }
    put16(ibag, 0); put16(ibag, nInstGlobal);     // zona: genNdx=0, sus mods tras los globales
    // El terminal dice DONDE TERMINAN los generadores de la zona, asi que tiene que contar
    // los que realmente se escribieron. Con `looping` son dos (`sampleModes` + `sampleID`) y
    // con el terminal en 1 el `sampleID` quedaba FUERA de la zona: el instrumento se quedaba
    // sin sample y el render daba silencio absoluto — no un sonido distinto, silencio.
    put16(ibag, (looping ? 2 : 1) + (declaraRango ? 1 : 0));
    put16(ibag, static_cast<uint16_t>(nInstGlobal + nInstZone));  // terminal

    std::vector<uint8_t> imod;
    for (const auto& m : mods.instrumentGlobal) putModulator(imod, m);
    for (const auto& m : mods.instrumentZone) putModulator(imod, m);
    for (int i = 0; i < 5; ++i) put16(imod, 0);  // terminal

    std::vector<uint8_t> igen;
    // `keyRange` va PRIMERO: el spec de SF2 (§7.5) exige que, si está, sea el generador
    // inicial de la zona. Su amount son dos bytes — lo en el bajo, hi en el alto.
    if (declaraRango) {
        put16(igen, kGenKeyRange);
        put16(igen, static_cast<uint16_t>((keyRangeHi << 8) | keyRangeLo));
    }
    // `sampleModes` va ANTES que `sampleID`: el spec de SF2 (§7.5) exige que sampleID sea el
    // ULTIMO generador de una zona de instrumento, y tsf recorre la lista en orden.
    //
    // Sin este generador el default es 0 = one-shot, y el `shdr` de abajo declara sus puntos
    // de loop para nadie: la nota dura las 64 muestras del sample y se apaga. Medido: ~200
    // frames a 48 kHz, o sea 4 ms — MENOS que los 240 frames que tarda en converger el
    // suavizador de parametros del engine, asi que ningun test de NIVEL es posible sobre el
    // one-shot. De ahi que exista esta opcion.
    if (looping) {
        put16(igen, kGenSampleModes); put16(igen, 1);
    }
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
