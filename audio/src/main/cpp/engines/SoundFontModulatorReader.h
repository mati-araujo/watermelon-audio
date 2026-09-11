#pragma once

/**
 * SoundFontModulatorReader.h — de los bytes de un `.sf2` a los moduladores de cada
 * región (REQ-039 S2, tarea 2.4).
 *
 * ## Qué contesta, y por qué no es "modulador → región"
 *
 * Para cada región que `tsf` va a construir, **cuáles son sus cuatro ámbitos** de
 * SF2 §9.5. No una lista plana: un modulador de zona global no pertenece a ninguna
 * región —aplica a todas las de su instrumento o preset— y el reparto no es un
 * detalle. Medido en S1 (1.6): el mapeo por región alcanza 1692 de 2812 en
 * `GeneralUser_GS.sf3` y **4 de 25** en el `SoundFont-Spec-Test`, donde **18 de los
 * 19** moduladores de velocity son globales.
 *
 * ## 🔴 Por qué parsea el archivo en vez de pedirle los datos a `tsf`
 *
 * Dos razones, las dos medidas:
 *
 * 1. **`tsf` los tira.** La hydra se libera dentro de `tsf_load` (`tsf.h:1451`),
 *    antes de que `tsf_load_memory` retorne, y `tsf_region` no guarda ningún índice
 *    a sus moduladores (`tsf.h:426-441`). `tsf_load_presets` ni los mira: los dos
 *    TODO comentados en `tsf.h:848` y `:856` lo dicen.
 * 2. **Las funciones de lectura de `tsf` son `static`**, así que sólo se ven desde
 *    `tsf_impl.cpp` — que vive en `thirdparty/`, el árbol que
 *    `scripts/cpp_callgraph.py:33` excluye. Apoyarse en ellas metería la lógica
 *    donde `check-rt-safety` y `check-mechanism-callers` son **ciegos**, y quedarían
 *    en verde revisando menos.
 *
 * El costo de la segunda pasada está medido y es despreciable: **0,21 ms sobre
 * 282 ms** de carga en `GeneralUser_GS.sf3` (0,07 %).
 *
 * ## Lo que hay que reproducir, y por qué se puede
 *
 * El orden en que `tsf_load_presets` numera las regiones —el sorting por
 * (bank, program), el filtrado por rango de tecla y velocity, las zonas globales—.
 * El prototipo de S1 1.6 lo verificó: **0 desajustes** sobre 12 311 regiones de
 * `GeneralUser_GS.sf3` y 174 del spec-test, contra un parche a `tsf.h` usado como
 * oráculo, con dos mutantes que matan la comparación (12 119 y 12 311 desajustes)
 * para probar que no era ciega.
 *
 * 🔴 **Eso es una duplicación con dueño, y hay que decirlo**: si upstream cambiara
 * ese recorrido, este lector apuntaría a la región equivocada **en silencio**. El
 * seguro es `tsf.h` vendoreado sin parches y fijado por versión; el día que se
 * actualice, esta reconstrucción se vuelve a medir contra un parche temporal, que
 * es exactamente el procedimiento que 1.6 dejó escrito.
 */

#include <cstdint>
#include <cstring>
#include <vector>

#include "SoundFontModulators.h"

namespace wma {
namespace sfmod {

/** Los cuatro ámbitos que le tocan a una región concreta de un preset. */
struct RegionModulators {
    int presetIndex = 0;  ///< índice del preset YA ordenado por (bank, program)
    int regionIndex = 0;  ///< orden dentro del preset, como lo numera `tsf`
    Scopes scopes;
};

namespace riff {

inline std::uint16_t rd16(const std::uint8_t* d) {
    return static_cast<std::uint16_t>(d[0] | (d[1] << 8));
}
inline std::uint32_t rd32(const std::uint8_t* d) {
    return static_cast<std::uint32_t>(d[0]) | (static_cast<std::uint32_t>(d[1]) << 8) |
           (static_cast<std::uint32_t>(d[2]) << 16) | (static_cast<std::uint32_t>(d[3]) << 24);
}
inline bool isFourCC(const std::uint8_t* d, const char* cc) {
    return std::memcmp(d, cc, 4) == 0;
}

/** Un chunk localizado dentro del `LIST pdta`. */
struct Span {
    const std::uint8_t* begin = nullptr;
    std::size_t count = 0;  ///< cantidad de registros, no de bytes
};

}  // namespace riff

/**
 * Los nueve chunks de la hydra, apuntando al buffer original.
 *
 * No copia nada: son vistas. El buffer tiene que seguir vivo mientras se use, y por
 * eso la lectura corre **entre `tsf_load_memory` y el `munmap`** de
 * `SoundFontManager.h:136` — los bytes no sobreviven a la carga.
 */
struct Hydra {
    riff::Span phdr, pbag, pmod, pgen, inst, ibag, imod, igen;
    bool complete = false;
};

namespace detail {

constexpr std::size_t kPhdrSize = 38, kBagSize = 4, kModSize = 10, kGenSize = 4,
                      kInstSize = 22;
constexpr std::uint16_t kGenInstrument = 41, kGenKeyRange = 43, kGenVelRange = 44,
                        kGenSampleId = 53;

/** Lee un modulador del chunk, en el orden del spec §8.2. */
inline Modulator modulatorAt(const riff::Span& span, std::size_t index) {
    const std::uint8_t* p = span.begin + index * kModSize;
    Modulator m;
    m.srcOper = riff::rd16(p);
    m.destOper = riff::rd16(p + 2);
    m.amount = static_cast<std::int16_t>(riff::rd16(p + 4));
    m.amtSrcOper = riff::rd16(p + 6);
    m.transOper = riff::rd16(p + 8);
    return m;
}

/** Los moduladores del rango [lo, hi) de un chunk, saltando el terminal. */
inline std::vector<Modulator> modulatorRange(const riff::Span& span, std::uint16_t lo,
                                             std::uint16_t hi) {
    std::vector<Modulator> out;
    // El último registro de `pmod`/`imod` es el TERMINAL que exige el spec: nunca es
    // un modulador. Sin este tope, un font sin moduladores devolvería uno de ceros.
    const std::size_t last = span.count > 0 ? span.count - 1 : 0;
    for (std::size_t i = lo; i < hi && i < last; ++i) out.push_back(modulatorAt(span, i));
    return out;
}

/** Un generador de un chunk `pgen`/`igen`: sólo hace falta el oper y el amount. */
inline std::uint16_t genOperAt(const riff::Span& span, std::size_t index) {
    return riff::rd16(span.begin + index * kGenSize);
}
inline std::uint16_t genAmountAt(const riff::Span& span, std::size_t index) {
    return riff::rd16(span.begin + index * kGenSize + 2);
}

/** Los rangos de tecla y velocity que filtran una zona. */
struct Range {
    std::uint8_t lokey = 0, hikey = 127, lovel = 0, hivel = 127;
};

inline void applyRangeGen(Range& r, std::uint16_t oper, std::uint16_t amount) {
    if (oper == kGenKeyRange) {
        r.lokey = static_cast<std::uint8_t>(amount & 0xFF);
        r.hikey = static_cast<std::uint8_t>((amount >> 8) & 0xFF);
    } else if (oper == kGenVelRange) {
        r.lovel = static_cast<std::uint8_t>(amount & 0xFF);
        r.hivel = static_cast<std::uint8_t>((amount >> 8) & 0xFF);
    }
}

}  // namespace detail

/**
 * Localiza los nueve chunks de la hydra dentro de un `.sf2` en memoria.
 *
 * Devuelve `complete = false` si falta alguno o el archivo no es un `RIFF…sfbk`:
 * un font que `tsf` no va a poder cargar tampoco tiene moduladores que leer, y
 * decirlo es mejor que devolver una lista vacía que se lee como "no tiene".
 */
inline Hydra findHydra(const void* data, std::size_t size) {
    Hydra h;
    const auto* d = static_cast<const std::uint8_t*>(data);
    if (size < 12 || !riff::isFourCC(d, "RIFF") || !riff::isFourCC(d + 8, "sfbk")) return h;

    const std::size_t riffEnd = std::min<std::size_t>(size, 8 + riff::rd32(d + 4));
    std::size_t at = 12;
    while (at + 8 <= riffEnd) {
        const std::uint32_t chunkSize = riff::rd32(d + at + 4);
        const std::size_t body = at + 8;
        if (riff::isFourCC(d + at, "LIST") && body + 4 <= size && riff::isFourCC(d + body, "pdta")) {
            std::size_t inner = body + 4;
            const std::size_t innerEnd = std::min<std::size_t>(size, body + chunkSize);
            while (inner + 8 <= innerEnd) {
                const std::uint32_t isize = riff::rd32(d + inner + 4);
                const std::uint8_t* ibody = d + inner + 8;
                if (inner + 8 + isize > size) break;
                auto put = [&](riff::Span& span, std::size_t recordSize) {
                    if (isize % recordSize == 0) {
                        span.begin = ibody;
                        span.count = isize / recordSize;
                    }
                };
                if (riff::isFourCC(d + inner, "phdr")) put(h.phdr, detail::kPhdrSize);
                else if (riff::isFourCC(d + inner, "pbag")) put(h.pbag, detail::kBagSize);
                else if (riff::isFourCC(d + inner, "pmod")) put(h.pmod, detail::kModSize);
                else if (riff::isFourCC(d + inner, "pgen")) put(h.pgen, detail::kGenSize);
                else if (riff::isFourCC(d + inner, "inst")) put(h.inst, detail::kInstSize);
                else if (riff::isFourCC(d + inner, "ibag")) put(h.ibag, detail::kBagSize);
                else if (riff::isFourCC(d + inner, "imod")) put(h.imod, detail::kModSize);
                else if (riff::isFourCC(d + inner, "igen")) put(h.igen, detail::kGenSize);
                inner += 8 + isize + (isize & 1);
            }
        }
        at = body + chunkSize + (chunkSize & 1);
    }
    h.complete = h.phdr.begin && h.pbag.begin && h.pmod.begin && h.pgen.begin && h.inst.begin &&
                 h.ibag.begin && h.imod.begin && h.igen.begin;
    return h;
}

/**
 * Los moduladores de cada región, en el mismo orden en que `tsf` las construye.
 *
 * Reproduce el recorrido de `tsf_load_presets` (`tsf.h:708-865`). Verificado en S1
 * 1.6 contra un parche a `tsf.h`: **0 desajustes** sobre 12 485 regiones de dos
 * fonts, con mutantes que prueban que la comparación no era ciega.
 */
/**
 * Los moduladores tal como estan EN EL ARCHIVO: cada entrada de `pmod` e `imod` una
 * vez, sin los dos terminadores. Es la unidad con la que se cuenta "cuantos declara
 * el font" y "cuantos manda rechazar el spec" — propiedades del ARCHIVO. Contarlas
 * por region multiplica cada modulador global por las regiones de su instrumento:
 * GeneralUser tiene 2812 en el archivo y 63 822 region × modulador. Medido.
 */
inline std::vector<Modulator> readFileModulators(const void* data, std::size_t size) {
    using namespace detail;
    std::vector<Modulator> out;
    const Hydra h = findHydra(data, size);
    if (!h.complete) return out;
    for (const riff::Span* span : {&h.pmod, &h.imod}) {
        if (span->count < 1) continue;
        for (std::size_t i = 0; i + 1 < span->count; ++i) out.push_back(modulatorAt(*span, i));
    }
    return out;
}

inline std::vector<RegionModulators> readRegionModulators(const void* data, std::size_t size) {
    using namespace detail;
    std::vector<RegionModulators> out;
    const Hydra h = findHydra(data, size);
    if (!h.complete || h.phdr.count < 2) return out;

    const std::size_t presetCount = h.phdr.count - 1;  // el último phdr es el terminal
    for (std::size_t p = 0; p < presetCount; ++p) {
        const std::uint8_t* phdr = h.phdr.begin + p * kPhdrSize;
        const std::uint16_t preset = riff::rd16(phdr + 20);
        const std::uint16_t bank = riff::rd16(phdr + 22);
        const std::uint16_t bagLo = riff::rd16(phdr + 24);
        const std::uint16_t bagHi = riff::rd16(h.phdr.begin + (p + 1) * kPhdrSize + 24);

        // El mismo orden que `tsf`: por (bank, program), y a igualdad, por posición.
        int sortedIndex = 0;
        for (std::size_t q = 0; q < presetCount; ++q) {
            if (q == p) continue;
            const std::uint8_t* other = h.phdr.begin + q * kPhdrSize;
            const std::uint16_t oPreset = riff::rd16(other + 20);
            const std::uint16_t oBank = riff::rd16(other + 22);
            if (oBank > bank) continue;
            if (oBank < bank) { ++sortedIndex; continue; }
            if (oPreset > preset) continue;
            if (oPreset < preset) { ++sortedIndex; continue; }
            if (q < p) ++sortedIndex;
        }

        int regionIndex = 0;
        Range presetGlobalRange;
        std::vector<Modulator> presetGlobalMods;

        for (std::uint16_t b = bagLo; static_cast<std::size_t>(b) + 1 < h.pbag.count && b < bagHi; ++b) {
            const std::uint16_t genLo = riff::rd16(h.pbag.begin + b * kBagSize);
            const std::uint16_t genHi = riff::rd16(h.pbag.begin + (b + 1) * kBagSize);
            const std::uint16_t modLo = riff::rd16(h.pbag.begin + b * kBagSize + 2);
            const std::uint16_t modHi = riff::rd16(h.pbag.begin + (b + 1) * kBagSize + 2);

            Range presetRange = presetGlobalRange;
            bool hadInstrument = false;

            for (std::uint16_t g = genLo; g < genHi && g < h.pgen.count; ++g) {
                const std::uint16_t oper = genOperAt(h.pgen, g);
                const std::uint16_t amount = genAmountAt(h.pgen, g);
                if (oper != kGenInstrument) {
                    applyRangeGen(presetRange, oper, amount);
                    continue;
                }
                hadInstrument = true;
                if (static_cast<std::size_t>(amount) + 1 >= h.inst.count) continue;

                const std::uint16_t iBagLo = riff::rd16(h.inst.begin + amount * kInstSize + 20);
                const std::uint16_t iBagHi =
                    riff::rd16(h.inst.begin + (amount + 1) * kInstSize + 20);

                Range instGlobalRange;
                std::vector<Modulator> instGlobalMods;

                for (std::uint16_t ib = iBagLo; static_cast<std::size_t>(ib) + 1 < h.ibag.count && ib < iBagHi; ++ib) {
                    const std::uint16_t iGenLo = riff::rd16(h.ibag.begin + ib * kBagSize);
                    const std::uint16_t iGenHi = riff::rd16(h.ibag.begin + (ib + 1) * kBagSize);
                    const std::uint16_t iModLo = riff::rd16(h.ibag.begin + ib * kBagSize + 2);
                    const std::uint16_t iModHi =
                        riff::rd16(h.ibag.begin + (ib + 1) * kBagSize + 2);

                    Range zoneRange = instGlobalRange;
                    bool hadSample = false;

                    for (std::uint16_t ig = iGenLo; ig < iGenHi && ig < h.igen.count; ++ig) {
                        const std::uint16_t iOper = genOperAt(h.igen, ig);
                        if (iOper != kGenSampleId) {
                            applyRangeGen(zoneRange, iOper, genAmountAt(h.igen, ig));
                            continue;
                        }
                        // El rango del preset FILTRA al de la zona: si no se cruzan,
                        // `tsf` no crea región y el índice no avanza.
                        if (zoneRange.hikey < presetRange.lokey ||
                            zoneRange.lokey > presetRange.hikey) {
                            continue;
                        }
                        if (zoneRange.hivel < presetRange.lovel ||
                            zoneRange.lovel > presetRange.hivel) {
                            continue;
                        }
                        RegionModulators rm;
                        rm.presetIndex = sortedIndex;
                        rm.regionIndex = regionIndex++;
                        rm.scopes.instrumentGlobal = instGlobalMods;
                        rm.scopes.instrumentZone = modulatorRange(h.imod, iModLo, iModHi);
                        rm.scopes.presetGlobal = presetGlobalMods;
                        rm.scopes.presetZone = modulatorRange(h.pmod, modLo, modHi);
                        out.push_back(std::move(rm));
                        hadSample = true;
                    }

                    // La zona GLOBAL del instrumento es la primera bag sin sample: sus
                    // rangos y sus moduladores valen para todas las zonas siguientes.
                    if (ib == iBagLo && !hadSample) {
                        instGlobalRange = zoneRange;
                        instGlobalMods = modulatorRange(h.imod, iModLo, iModHi);
                    }
                }
            }

            // Y la zona global del preset, con la misma regla.
            if (b == bagLo && !hadInstrument) {
                presetGlobalRange = presetRange;
                presetGlobalMods = modulatorRange(h.pmod, modLo, modHi);
            }
        }
    }
    return out;
}

}  // namespace sfmod
}  // namespace wma
