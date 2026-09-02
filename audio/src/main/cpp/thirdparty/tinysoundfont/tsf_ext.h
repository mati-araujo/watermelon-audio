#pragma once

// Minimal extension accessors for TinySoundFont.
//
// The public tsf API only lets you map (bank, program) -> preset index
// (tsf_get_presetindex). We need the reverse — the SF2 bank and GM program of a
// given preset index — to classify instruments correctly (bank 128 = percussion
// kit, regardless of the preset's often-ambiguous name). These are implemented
// in tsf_impl.cpp, the single TU where the full `struct tsf` is visible under
// TSF_IMPLEMENTATION.

struct tsf;

#ifdef __cplusplus
extern "C" {
#endif

// SF2 bank of preset [preset_index] (0 = melodic banks, 128 = GM percussion),
// or -1 when the font/index is invalid.
int tsf_get_preset_bank(const tsf* f, int preset_index);

// GM program number (0-127) of preset [preset_index], or -1 when invalid.
int tsf_get_preset_number(const tsf* f, int preset_index);

// MIDI key range actually declared by preset [preset_index]: the min of `lokey`
// and the max of `hikey` across its regions. Writes *out_lo / *out_hi and returns
// 1 on success; returns 0 — touching neither out-param — when the font or the
// index is invalid, or when the preset declares no regions at all.
//
// This is the datum SoundFontManager used to GUESS from the preset name. Its old
// comment said probing regions "requires private struct access that isn't
// reachable from this translation unit" — true of that TU, and never a blocker:
// THIS is that translation unit, and it is where the bank/program accessors above
// already live for the very same reason.
int tsf_get_preset_key_range(const tsf* f, int preset_index, int* out_lo, int* out_hi);

#ifdef __cplusplus
}
#endif
