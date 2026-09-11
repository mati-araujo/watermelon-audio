/**
 * SoundFontModulatorTable.h — REQ-039 S2 (2.7 y 2.8): de los cuatro ámbitos de cada
 * región a lo que el thread de audio evalúa al disparar la nota.
 *
 * DOS MITADES, DOS THREADS
 * ------------------------
 *   - [ModulatorTable::buildFromFontBytes] corre en el thread de CONTROL, entre `tsf_load_memory` y
 *     el `munmap`: lee los moduladores con `SoundFontModulatorReader`, los resuelve con
 *     `resolve()` (defaults + cuatro ámbitos, precedencia MEDIDA en 2.3) y deja por
 *     región una lista ya clasificada. Acá se aloca, y es legal.
 *   - [noteOnContributionsOf] corre en el thread de AUDIO (`drainEvents` → note-on):
 *     recorre la lista de la región y suma. Sin alocar, sin lock, sin log.
 *
 * EL RECHAZO ES CONTABLE (AC-039.8)
 * ----------------------------------
 * Un modulador que esta capa no evalúa NO desaparece en silencio: cae en un contador
 * con su razón. Un rechazo mudo es exactamente el defecto que REQ-039 existe para
 * arreglar —el renderizador descartaba los 2812 moduladores de GeneralUser sin dejar
 * rastro—, y este repo ya lo pagó una vez en la C API (MINI-016).
 *
 * Los contadores separan lo que el SPEC manda rechazar (encadenados, `transOper` no
 * lineal) de lo que ES ALCANCE de otra etapa (fuentes de canal → S3; destinos que S2
 * no aplica → S3 / REQ-040). Mezclarlos haría que AC-039.10 ("cero descartes sobre
 * GeneralUser") no se pueda afirmar sin mentir: GeneralUser tiene moduladores de CC.
 *
 * LO QUE S2 EVALÚA EN NOTE-ON, Y LO QUE NO
 * -----------------------------------------
 * Fuentes: las de NOTA — velocity, número de tecla, y "sin controlador" (constante 1).
 * Los CC, la presión y la rueda viven en el canal y los aplica `tsf` por su lado; S3
 * decide cómo no duplicar esa capa (AC-039.6). Destinos: `initialAttenuation` (48) e
 * `initialFilterFc` (8). Son los dos que las diez sub-pruebas de velocity del
 * spec-test necesitan, y los dos que la extensión de `tsf_impl.cpp` puede escribir
 * por voz sin parchear `tsf.h`.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "SoundFontModulatorReader.h"
#include "SoundFontModulators.h"

namespace wma {
namespace sfmod {

// ---- Los diez moduladores por defecto de SF2 2.04 §8.4 --------------------------

/** Destinos (`genOper`) que esta capa nombra. Los números son los del spec §8.1.2. */
enum : std::uint16_t {
    kDestInitialFilterFc = 8,
    kDestChorusSend = 15,
    kDestReverbSend = 16,
    kDestPan = 17,
    kDestInitialAttenuation = 48,
    kDestVibLfoToPitch = 6,
    /// "Initial Pitch" del §8.4.10 no es un generador del §8.1.2. FluidSynth lo mapea a
    /// su GEN_PITCH interno (59); se usa el mismo número para que un modulador de
    /// archivo con esa identidad REEMPLACE al default como el spec manda.
    kDestInitialPitch = 59,
};

/**
 * Arma un `modSrcOper` §8.2 desde sus cinco campos. Misma forma que el fixture de
 * tests, y por la misma razón: el orden de los bits se escribe mal una vez y nadie
 * relee.
 */
inline constexpr std::uint16_t makeSrcOper(std::uint16_t index, bool isCC, bool decreasing,
                                           bool bipolar, Curve curve) {
    return static_cast<std::uint16_t>((index & 0x7F) | (isCC ? 0x80 : 0) |
                                      (decreasing ? 0x100 : 0) | (bipolar ? 0x200 : 0) |
                                      ((static_cast<std::uint16_t>(curve) & 0x3F) << 10));
}

/**
 * Los diez por defecto, en el orden del spec. `resolve()` los toma como base, así que
 * un modulador de archivo con la misma identidad los REEMPLAZA (y con `amount` 0 los
 * anula) — que es lo que #13 E del spec-test mide.
 *
 * 🔴 #1 es `0x0502`: velocity, cóncava, unipolar, DECRECIENTE, 960 cB. Es el que
 * `tsf.h:1619` tenía cableado como `20·log10(vel)` y el que 2.6 reemplaza, no suma.
 */
inline const std::vector<Modulator>& defaultModulators() {
    static const std::vector<Modulator> kDefaults = {
        // #1  velocity → initialAttenuation
        {makeSrcOper(2, false, true, false, Curve::Concave), kDestInitialAttenuation, 960, 0, 0},
        // #2  velocity → initialFilterFc
        {makeSrcOper(2, false, true, false, Curve::Linear), kDestInitialFilterFc, -2400, 0, 0},
        // #3  channel pressure → vibrato LFO pitch depth
        {makeSrcOper(13, false, false, false, Curve::Linear), kDestVibLfoToPitch, 50, 0, 0},
        // #4  CC1 (mod wheel) → vibrato LFO pitch depth
        {makeSrcOper(1, true, false, false, Curve::Linear), kDestVibLfoToPitch, 50, 0, 0},
        // #5  CC7 (volume) → initialAttenuation
        {makeSrcOper(7, true, true, false, Curve::Concave), kDestInitialAttenuation, 960, 0, 0},
        // #6  CC10 (pan) → pan
        {makeSrcOper(10, true, false, true, Curve::Linear), kDestPan, 1000, 0, 0},
        // #7  CC11 (expression) → initialAttenuation
        {makeSrcOper(11, true, true, false, Curve::Concave), kDestInitialAttenuation, 960, 0, 0},
        // #8  CC91 → reverb send
        {makeSrcOper(91, true, false, false, Curve::Linear), kDestReverbSend, 200, 0, 0},
        // #9  CC93 → chorus send
        {makeSrcOper(93, true, false, false, Curve::Linear), kDestChorusSend, 200, 0, 0},
        // #10 pitch wheel → initial pitch, escalado por la sensibilidad (índice 16)
        {makeSrcOper(14, false, false, true, Curve::Linear), kDestInitialPitch, 12700,
         makeSrcOper(16, false, false, false, Curve::Linear), 0},
    };
    return kDefaults;
}

// ---- Clasificación al cargar -----------------------------------------------------

/** Qué fuente de NOTA alimenta un modulador que se evalúa al disparar. */
enum class NoteSource : std::uint8_t {
    Constant,  ///< "sin controlador": vale 1 (§8.2.1, índice 0 sin CC)
    Velocity,  ///< índice 2
    Key,       ///< índice 3
};

/** Un modulador ya resuelto, clasificado y listo para el thread de audio. */
struct NoteOnModulator {
    std::uint16_t destOper = 0;
    float amount = 0.0f;
    NoteSource primarySource = NoteSource::Constant;
    Transform primary;
    NoteSource amountSource = NoteSource::Constant;
    Transform secondary;
};

/**
 * Los contadores del rechazo. Dos familias, y la separación es el punto:
 *
 *  - `linked` y `nonLinearTransform` son lo que el SPEC manda descartar (§8.2.4 y
 *    §8.3). AC-039.10 exige que sobre GeneralUser sumen CERO.
 *  - `sourceNotAtNoteOn` y `destinationUnsupported` son ALCANCE de este REQ: S3 y
 *    REQ-040 los toman. No son cero sobre GeneralUser y no tienen por qué serlo.
 *
 * 🔴 Se cuentan SOBRE EL ARCHIVO —cada entrada de `pmod`/`imod` una vez—, no sobre la
 * resolución por región. La primera versión contaba por región y sobre GeneralUser
 * daba 63 822 "declarados" donde el archivo tiene 2812, y 98 883 "fuera de alcance"
 * —más que los declarados— porque cada global se repetía en las 12 311 regiones y los
 * ocho defaults de canal entraban una vez por región. Un contador que infla no miente
 * en cero (los rechazos del spec daban 0 igual), miente en todo lo demás. Los defaults
 * NO son del archivo y no se cuentan acá: son los diez del spec, siempre.
 */
struct RejectionCounters {
    std::uint32_t linked = 0;
    std::uint32_t nonLinearTransform = 0;
    std::uint32_t sourceNotAtNoteOn = 0;
    std::uint32_t destinationUnsupported = 0;

    std::uint32_t specRejections() const { return linked + nonLinearTransform; }
    std::uint32_t outOfScope() const { return sourceNotAtNoteOn + destinationUnsupported; }
};

/** Los moduladores de UNA región. Lo que el thread de audio lee. */
struct RegionModulatorList {
    std::vector<NoteOnModulator> mods;
};

namespace detail {

/// Bit 15 del `destOper`: el destino es OTRO modulador (§8.2.4). No se soporta.
inline bool isLinked(const Modulator& m) { return (m.destOper & 0x8000) != 0; }

/// §8.3: 0 = lineal, 2 = valor absoluto. Sólo lineal se evalúa.
inline bool hasNonLinearTransform(const Modulator& m) { return m.transOper != 0; }

/// `true` si la fuente es de nota, y cuál. CC, presión, rueda y "link" (127) no lo son.
inline bool classifyNoteSource(std::uint16_t oper, NoteSource& out) {
    if (sourceIsMidiCC(oper)) return false;
    switch (sourceIndexOf(oper)) {
        case 0: out = NoteSource::Constant; return true;
        case 2: out = NoteSource::Velocity; return true;
        case 3: out = NoteSource::Key; return true;
        default: return false;
    }
}

inline bool isSupportedDestination(std::uint16_t destOper) {
    return destOper == kDestInitialAttenuation || destOper == kDestInitialFilterFc;
}

/**
 * De un modulador resuelto a su forma de note-on, o a un contador. Thread de control.
 *
 * El ORDEN de los chequeos importa para lo que cuenta cada contador: un encadenado con
 * fuente de CC se cuenta como encadenado (lo manda el spec), no como fuera de alcance.
 */
/** En qué contador cae un modulador que esta capa no evalúa, o `nullptr` si lo evalúa. */
inline std::uint32_t* rejectionSlotFor(const Modulator& m, RejectionCounters& c) {
    if (isLinked(m)) return &c.linked;
    if (hasNonLinearTransform(m)) return &c.nonLinearTransform;
    NoteSource primary, secondary;
    if (!classifyNoteSource(m.srcOper, primary) || !classifyNoteSource(m.amtSrcOper, secondary)) {
        return &c.sourceNotAtNoteOn;
    }
    if (!isSupportedDestination(m.destOper)) return &c.destinationUnsupported;
    return nullptr;
}

/**
 * De un modulador resuelto a su forma de note-on, o descartado. Thread de control.
 * NO cuenta: contar es por archivo, y esto corre por región (ver `RejectionCounters`).
 */
inline bool classify(const Modulator& m, NoteOnModulator& out) {
    RejectionCounters scratch;
    if (rejectionSlotFor(m, scratch) != nullptr) return false;
    NoteSource primary, secondary;
    classifyNoteSource(m.srcOper, primary);
    classifyNoteSource(m.amtSrcOper, secondary);
    out.destOper = m.destOper;
    out.amount = static_cast<float>(m.amount);
    out.primarySource = primary;
    out.primary = transformOf(m.srcOper);
    out.amountSource = secondary;
    out.secondary = transformOf(m.amtSrcOper);
    return true;
}

}  // namespace detail

/**
 * La tabla: por (preset, región) la lista ya clasificada. Se construye UNA vez al cargar
 * y después sólo se lee. Viaja junto al `tsf*` bajo el mismo hazard pointer
 * (`SoundFontManager::ActiveFont`): dos atómicos separados dejarían una ventana en la que
 * el thread de audio evalúa los moduladores de OTRO font.
 */
class ModulatorTable {
public:
    /**
     * Thread de CONTROL. Aloca. Los bytes son los del archivo, ANTES del `munmap`.
     * @return `false` si los bytes no describen un SoundFont; la tabla queda vacía y
     *         cada consulta devuelve "sin moduladores".
     */
    bool buildFromFontBytes(const void* data, std::size_t size) {
        mByPreset.clear();
        mCounters = RejectionCounters{};
        mTotal = 0;
        const std::vector<RegionModulators> regions = readRegionModulators(data, size);
        if (regions.empty()) return false;
        // Los contadores, sobre el ARCHIVO: cada pmod/imod una vez.
        for (const Modulator& m : readFileModulators(data, size)) {
            ++mTotal;
            if (std::uint32_t* slot = detail::rejectionSlotFor(m, mCounters)) ++*slot;
        }
        for (const RegionModulators& r : regions) {
            if (r.presetIndex < 0 || r.regionIndex < 0) continue;
            const auto p = static_cast<std::size_t>(r.presetIndex);
            const auto g = static_cast<std::size_t>(r.regionIndex);
            if (mByPreset.size() <= p) mByPreset.resize(p + 1);
            if (mByPreset[p].size() <= g) mByPreset[p].resize(g + 1);
            const std::vector<Modulator> resolved = resolve(defaultModulators(), r.scopes);
            for (const Modulator& m : resolved) {
                NoteOnModulator n;
                if (detail::classify(m, n)) mByPreset[p][g].mods.push_back(n);
            }
        }
        return true;
    }

    /**
     * Thread de AUDIO. Sin alocar. Índice fuera de rango ⇒ `nullptr`, que el llamador
     * trata como "sin moduladores": un preset que la tabla no conoce no puede sonar
     * distinto de como sonaba antes de REQ-039.
     */
    const RegionModulatorList* regionModulators(int presetIndex, int regionIndex) const noexcept {
        if (presetIndex < 0 || regionIndex < 0) return nullptr;
        const auto p = static_cast<std::size_t>(presetIndex);
        if (p >= mByPreset.size()) return nullptr;
        const auto g = static_cast<std::size_t>(regionIndex);
        if (g >= mByPreset[p].size()) return nullptr;
        return &mByPreset[p][g];
    }

    const RejectionCounters& rejections() const noexcept { return mCounters; }
    /** Moduladores declarados en el archivo (los cuatro ámbitos), antes de resolver. */
    std::uint32_t declaredInFile() const noexcept { return mTotal; }

private:
    std::vector<std::vector<RegionModulatorList>> mByPreset;
    RejectionCounters mCounters;
    std::uint32_t mTotal = 0;
};

// ---- La evaluación al disparar (thread de audio) ---------------------------------

/** Lo que una nota aporta a sus dos destinos. Unidades del spec: cB y cents. */
struct NoteOnContribution {
    float attenuationCentibels = 0.0f;
    float filterFcCents = 0.0f;
};

namespace detail {
inline float noteSourceValue(NoteSource s, int key, int velocity) noexcept {
    switch (s) {
        case NoteSource::Velocity: return normalize7bit(velocity);
        case NoteSource::Key: return normalize7bit(key);
        case NoteSource::Constant: default: return 1.0f;
    }
}
}  // namespace detail

/**
 * Suma las contribuciones de la región para esta nota. RT: un bucle acotado y
 * aritmética. `nullptr` ⇒ cero, que es "como antes de REQ-039".
 *
 * 🔴 Devuelve la ATENUACIÓN modulada ENTERA, no un delta sobre `tsf`: el llamador
 * reemplaza el término de velocity que `tsf.h:1619` calculó, no lo corrige. Sumarle
 * encima daría la curva del archivo MÁS la cableada — el defecto de 2.6.
 */
inline NoteOnContribution noteOnContributionsOf(const RegionModulatorList* list, int key,
                                                int velocity) noexcept {
    NoteOnContribution out;
    if (list == nullptr) return out;
    const NoteOnModulator* mods = list->mods.data();
    const std::size_t n = list->mods.size();
    for (std::size_t i = 0; i < n; ++i) {
        const NoteOnModulator& m = mods[i];
        const float c = contribution(m.amount, detail::noteSourceValue(m.primarySource, key, velocity),
                                     m.primary, detail::noteSourceValue(m.amountSource, key, velocity),
                                     m.secondary);
        if (m.destOper == kDestInitialAttenuation) out.attenuationCentibels += c;
        else if (m.destOper == kDestInitialFilterFc) out.filterFcCents += c;
    }
    return out;
}

}  // namespace sfmod
}  // namespace wma
