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

// ---- REQ-039 S2: las voces que un note-on acaba de arrancar --------------------
//
// `tsf_note_on` calcula por voz la ganancia de velocity (`tsf.h:1619`, cableada
// como 20·log10(vel)) y el corte del filtro (de `region->initialFilterFc`, fijo).
// SF2 §8.4 dice que las dos vienen de MODULADORES, y tsf los descarta al cargar.
// Esta extension NO parchea tsf.h: deja que note_on haga lo suyo y despues expone
// las voces recien creadas para que el motor REEMPLACE esos dos valores por los
// modulados. Todo corre en el thread de audio: sin alocar, sin lock, sin log.
//
// El region index es el que `tsf_load_presets` numera dentro del preset — el mismo
// que reconstruye SoundFontModulatorReader —, asi que la tabla de moduladores y
// la voz hablan del mismo `tsf_region`.

typedef struct tsf_ext_started_voice {
    int voiceIndex;   // indice en f->voices, para las dos escrituras de abajo
    int presetIndex;  // preset YA ordenado por (bank, program)
    int regionIndex;  // orden de la region dentro del preset
    int initialFilterFc;  // el corte de la region en cents absolutos (13500 = abierto):
                          // la base sobre la que un modulador de filtro SUMA
} tsf_ext_started_voice;

// Las voces que arranco el ULTIMO tsf_note_on / tsf_channel_note_on: comparten el
// `playIndex` mas reciente. Escribe hasta `max` entradas y devuelve cuantas hay
// (puede ser mas que `max`; el llamador decide si le alcanza). RT-safe.
int tsf_ext_voices_started_by_last_note_on(const tsf* f, tsf_ext_started_voice* out, int max);

// Reemplaza el termino de velocity que tsf.h:1619 resto (`gainToDecibels(1/vel)`)
// por `attenuationDB`, la atenuacion MODULADA (SF2 §8.4.1 y lo que el archivo
// declare). `vel` es el MISMO float 0..1 que se le paso a note_on: se deshace con
// la misma aritmetica con la que se hizo. Reemplaza, no suma: sumar daria la curva
// del archivo MAS la cableada. RT-safe.
void tsf_ext_voice_replace_velocity_gain(tsf* f, int voiceIndex, float vel, float attenuationDB);

// Re-setupea el low-pass de la voz con un corte en cents absolutos, reproduciendo
// el setup de note_on (13500 = abierto). RT-safe.
//
// LIMITE DECLARADO: si la region tiene `modLfoToFilterFc` o `modEnvToFilterFc`,
// `tsf_voice_render` recalcula el corte cada bloque desde `region->initialFilterFc`
// y pisa esto. Modular ESE caso por voz exige un campo que tsf_voice no tiene, o
// sea un parche a tsf.h — fuera de esta etapa.
void tsf_ext_voice_set_filter_cutoff(tsf* f, int voiceIndex, float cutoffCents);

#ifdef __cplusplus
}
#endif
