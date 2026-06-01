/**
 * @file SoundFontManager.cpp
 * @brief Builds the immutable preset metadata cache (AUD-4).
 *
 * The cache is populated once per load and read by JNI/main-thread code via
 * SoundFontManager::getPresetName / getPresetKeyRange / getPresetCount. The
 * audio thread continues to use the lock-free tsf pointer (mActiveSF) for
 * synthesis and never touches this cache.
 *
 * The key-range heuristic mirrors the historical behavior (probing tsf
 * regions requires private struct access that isn't reachable from this
 * translation unit, so we infer ranges from GM preset names).
 */
#include "SoundFontManager.h"

#include <cstring>

namespace {

void inferKeyRange(const char* name, int& outMinKey, int& outMaxKey) {
    if (!name) {
        outMinKey = 21;
        outMaxKey = 108;
        return;
    }

    char lower[64] = {};
    for (int i = 0; i < 63 && name[i]; ++i) {
        lower[i] = (name[i] >= 'A' && name[i] <= 'Z')
                       ? static_cast<char>(name[i] + 32)
                       : name[i];
    }

    if (strstr(lower, "drum") || strstr(lower, "kit") || strstr(lower, "perc")) {
        outMinKey = 35; outMaxKey = 81;
    } else if (strstr(lower, "bass")) {
        outMinKey = 24; outMaxKey = 60;
    } else if (strstr(lower, "guitar") || strstr(lower, "gtr")) {
        outMinKey = 40; outMaxKey = 88;
    } else if (strstr(lower, "violin") || strstr(lower, "viola")) {
        outMinKey = 55; outMaxKey = 103;
    } else if (strstr(lower, "cello")) {
        outMinKey = 36; outMaxKey = 84;
    } else if (strstr(lower, "flute") || strstr(lower, "piccolo")) {
        outMinKey = 60; outMaxKey = 108;
    } else if (strstr(lower, "trumpet") || strstr(lower, "brass")) {
        outMinKey = 52; outMaxKey = 96;
    } else if (strstr(lower, "organ")) {
        outMinKey = 36; outMaxKey = 96;
    } else if (strstr(lower, "piano") || strstr(lower, "keys")) {
        outMinKey = 21; outMaxKey = 108;
    } else if (strstr(lower, "pad") || strstr(lower, "synth") || strstr(lower, "lead")) {
        outMinKey = 36; outMaxKey = 96;
    } else {
        outMinKey = 21; outMaxKey = 108;
    }
}

} // namespace

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
        inferKeyRange(name, info.minKey, info.maxKey);
        cache->push_back(std::move(info));
    }

    return cache;
}
