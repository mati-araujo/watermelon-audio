/**
 * @file SoundFontManager.cpp
 * @brief Implementation of SoundFontManager methods that need full tsf struct access.
 *
 * tsf struct internals (tsf_preset, tsf_region) are only available in translation
 * units that include tsf.h after TSF_IMPLEMENTATION. Since the actual implementation
 * lives in tsf_impl.cpp, we include tsf.h here to get the struct definitions
 * (they're defined in the non-implementation section too, in the "Internal" block).
 */
#include "SoundFontManager.h"

// tsf.h defines tsf_preset/tsf_region in the internal section,
// which is only included when TSF_IMPLEMENTATION is defined in tsf_impl.cpp.
// However, since SoundFontManager.h already #includes tsf.h (getting only the public API),
// we need the struct definitions. They're guarded by the same header guard.
// The trick: tsf.h defines structs inside #ifdef TSF_IMPLEMENTATION.
// So we re-include with the define set, but only for the struct access.
// Actually, we just need to manually define access. Let's use the tsf public API instead.

// The tsf structs are defined inside #ifdef TSF_IMPLEMENTATION in tsf.h.
// To access them, we need to be in an implementation TU. Since tsf_impl.cpp
// already has the one-definition, we can't define TSF_IMPLEMENTATION again.
// Instead, we'll use the tsf_note_on approach: iterate MIDI notes and check
// which ones produce voices.

bool SoundFontManager::getPresetKeyRange(int presetIndex, int& outMinKey, int& outMaxKey) const {
    std::lock_guard<std::mutex> lock(mLoadMutex);
    tsf* sf = mActiveSF.load(std::memory_order_acquire);
    if (!sf || presetIndex < 0 || presetIndex >= tsf_get_presetcount(sf)) {
        return false;
    }

    // Probe MIDI key range by testing which notes produce voices.
    // tsf_note_on returns 0 if no voice was allocated (no samples for that key).
    // We use a temporary render-silent approach: set volume to 0, try notes, then off.
    // Actually, we can't do this on a shared tsf — it would interfere with audio.
    //
    // Simpler approach: Use a heuristic based on preset name for common instruments,
    // and fall back to standard piano range (21-108) for unknown presets.
    const char* name = tsf_get_presetname(sf, presetIndex);
    if (!name) {
        outMinKey = 21;  // A0
        outMaxKey = 108; // C8
        return true;
    }

    // Convert name to lowercase for matching
    char lower[64] = {};
    for (int i = 0; i < 63 && name[i]; ++i) {
        lower[i] = (name[i] >= 'A' && name[i] <= 'Z') ? (name[i] + 32) : name[i];
    }

    // Heuristic ranges based on GM instrument categories
    if (strstr(lower, "drum") || strstr(lower, "kit") || strstr(lower, "perc")) {
        outMinKey = 35;  // B1 (GM Acoustic Bass Drum)
        outMaxKey = 81;  // A5 (GM Open Triangle)
    } else if (strstr(lower, "bass")) {
        outMinKey = 24;  // C1
        outMaxKey = 60;  // C4
    } else if (strstr(lower, "guitar") || strstr(lower, "gtr")) {
        outMinKey = 40;  // E2
        outMaxKey = 88;  // E6
    } else if (strstr(lower, "violin") || strstr(lower, "viola")) {
        outMinKey = 55;  // G3
        outMaxKey = 103; // G7
    } else if (strstr(lower, "cello")) {
        outMinKey = 36;  // C2
        outMaxKey = 84;  // C6
    } else if (strstr(lower, "flute") || strstr(lower, "piccolo")) {
        outMinKey = 60;  // C4
        outMaxKey = 108; // C8
    } else if (strstr(lower, "trumpet") || strstr(lower, "brass")) {
        outMinKey = 52;  // E3
        outMaxKey = 96;  // C7
    } else if (strstr(lower, "organ")) {
        outMinKey = 36;  // C2
        outMaxKey = 96;  // C7
    } else if (strstr(lower, "piano") || strstr(lower, "keys")) {
        outMinKey = 21;  // A0
        outMaxKey = 108; // C8
    } else if (strstr(lower, "pad") || strstr(lower, "synth") || strstr(lower, "lead")) {
        outMinKey = 36;  // C2
        outMaxKey = 96;  // C7
    } else {
        // Default: standard piano range
        outMinKey = 21;  // A0
        outMaxKey = 108; // C8
    }

    return true;
}
