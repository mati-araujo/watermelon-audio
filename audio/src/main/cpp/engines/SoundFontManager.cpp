/**
 * @file SoundFontManager.cpp
 * @brief Builds the immutable preset metadata cache (AUD-4).
 *
 * The cache is populated once per load and read by JNI/main-thread code via
 * SoundFontManager::getPresetName / getPresetKeyRange / getPresetCount. The
 * audio thread continues to use the lock-free tsf pointer (mActiveSF) for
 * synthesis and never touches this cache.
 *
 * EL RANGO DE TECLAS SALE DE LAS REGIONES DEL PRESET (MINI-017)
 * -------------------------------------------------------------
 * Hasta el 2026-09-02 se ADIVINABA del NOMBRE del preset, con una cadena de
 * `strstr` de diez ramas, y este comentario lo justificaba así: *"probing tsf
 * regions requires private struct access that isn't reachable from this
 * translation unit"*.
 *
 * 🔴 Era cierto sobre ESTA unidad de traducción, y nunca fue un bloqueo: el
 * proyecto ya había construido la salida —`tsf_ext.h` + `tsf_impl.cpp`, la única
 * TU donde `struct tsf` está completo— y la estaba usando para `bank`/`program`,
 * cuyo propio comentario dice que se hizo para clasificar *"regardless of the
 * preset's often-ambiguous name"*. O sea que alguien ya había desconfiado del
 * nombre, resolvió su caso por esa vía, y dejó la heurística en pie.
 *
 * La adivinanza además NO era distinguible de una ausencia: sus dos fallbacks
 * —nombre nulo y nombre sin coincidencia— devolvían 21..108, exactamente lo mismo
 * que su rama `piano`. Un consumidor no podía saber cuál de los tres casos tenía.
 */
#include "SoundFontManager.h"

#include <cstring>

// (Acá vivía `inferKeyRange`. La borró MINI-017: ver la nota de arriba.)

std::shared_ptr<const std::vector<SoundFontManager::PresetInfo>>
SoundFontManager::buildPresetCache(tsf* sf, int presetCount) {
    if (!sf || presetCount <= 0) {
        return std::make_shared<const std::vector<PresetInfo>>();
    }

    auto cache = std::make_shared<std::vector<PresetInfo>>();
    cache->reserve(static_cast<size_t>(presetCount));

    for (int i = 0; i < presetCount; ++i) {
        const char* name = tsf_get_presetname(sf, i);
        PresetInfo info;
        info.name = name ? name : "";
        // El rango REAL, de las regiones. Si el preset no declara ninguna no hay
        // rango que informar: se marca ausente y `getPresetKeyRange` lo rechaza,
        // en vez de inventar un 21..108 que el llamador no podría distinguir de
        // un dato.
        if (!tsf_get_preset_key_range(sf, i, &info.minKey, &info.maxKey)) {
            info.minKey = -1;
            info.maxKey = -1;
        }
        info.bank = tsf_get_preset_bank(sf, i);
        info.program = tsf_get_preset_number(sf, i);
        cache->push_back(std::move(info));
    }

    return cache;
}
