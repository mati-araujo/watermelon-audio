/**
 * @file tsf_impl.cpp
 * @brief TinySoundFont implementation compilation unit
 *
 * This file includes the tsf.h header with TSF_IMPLEMENTATION defined,
 * creating the actual function implementations. Only include this in
 * ONE compilation unit (handled by CMakeLists.txt).
 *
 * stb_vorbis is included BEFORE tsf.h so TinySoundFont enables its SF3
 * code path: SoundFont3 (.sf3) stores samples as Ogg/Vorbis, and tsf gates
 * all Ogg decoding behind `#ifdef STB_VORBIS_INCLUDE_STB_VORBIS_H`. Without
 * this include, .sf3 fonts still load but their compressed samples are read
 * as raw PCM — producing loud garbage (as if the sample rate were wrong).
 * Decoding happens at load time (off the audio thread), so it is RT-safe.
 *
 * STB_VORBIS_NO_STDIO: tsf only uses the in-memory decoder
 * (stb_vorbis_open_memory), so the file-based API and its stdio dependency
 * are dropped. Third-party warnings for this whole TU are already silenced
 * via `-w` in tinysoundfont.cmake.
 */
#define STB_VORBIS_NO_STDIO
#include "stb_vorbis.c"

#define TSF_IMPLEMENTATION
#include "tsf.h"

#include "tsf_ext.h"

// Preset bank/program accessors (see tsf_ext.h). `struct tsf` / `struct
// tsf_preset` are fully defined in this TU (TSF_IMPLEMENTATION), so we can read
// the sorted preset table directly. tsf sorts presets by (bank, program), so a
// bank of 128 marks a GM percussion kit even when its name lacks "kit"/"drums".
extern "C" int tsf_get_preset_bank(const tsf* f, int i) {
    return (f && i >= 0 && i < f->presetNum) ? static_cast<int>(f->presets[i].bank) : -1;
}
extern "C" int tsf_get_preset_number(const tsf* f, int i) {
    return (f && i >= 0 && i < f->presetNum) ? static_cast<int>(f->presets[i].preset) : -1;
}

// El rango de teclas REAL del preset: el mínimo de `lokey` y el máximo de `hikey`
// sobre sus regiones. Un preset se toca por regiones, así que la unión es lo que
// de verdad responde a una tecla.
//
// Devuelve 0 y NO toca los out-params cuando no hay nada que informar —fuente o
// índice inválidos, o cero regiones—. Un preset sin regiones no suena en ninguna
// tecla; darle un rango plausible sería la misma clase de mentira que la
// heurística por nombre que esto reemplaza (MINI-017).
extern "C" int tsf_get_preset_key_range(const tsf* f, int i, int* out_lo, int* out_hi) {
    if (!f || i < 0 || i >= f->presetNum) return 0;
    const struct tsf_preset& p = f->presets[i];
    if (!p.regions || p.regionNum <= 0) return 0;

    int lo = 127, hi = 0;
    for (int r = 0; r < p.regionNum; ++r) {
        const int rlo = static_cast<int>(p.regions[r].lokey);
        const int rhi = static_cast<int>(p.regions[r].hikey);
        if (rlo < lo) lo = rlo;
        if (rhi > hi) hi = rhi;
    }
    if (lo > hi) return 0;  // regiones presentes pero todas degeneradas

    if (out_lo) *out_lo = lo;
    if (out_hi) *out_hi = hi;
    return 1;
}

// ---- REQ-039 S2: las voces que un note-on acaba de arrancar --------------------
//
// tsf no guarda "las voces de la ultima llamada": las identifica el `playIndex`,
// que note_on toma de `f->voicePlayIndex++`, asi que las voces mas recientes
// llevan `f->voicePlayIndex - 1`. Si el note-on no creo ninguna (tecla fuera de
// rango), el contador igual avanzo y no matchea nada: se devuelve 0.
extern "C" int tsf_ext_voices_started_by_last_note_on(const tsf* f, tsf_ext_started_voice* out,
                                                        int max) {
    if (!f || !f->voices || f->voicePlayIndex == 0) return 0;
    const unsigned int last = f->voicePlayIndex - 1;
    int n = 0;
    for (int i = 0; i < f->voiceNum; ++i) {
        const struct tsf_voice& v = f->voices[i];
        if (v.playingPreset < 0 || v.playIndex != last || !v.region) continue;
        if (n < max && out) {
            const struct tsf_preset& p = f->presets[v.playingPreset];
            out[n].voiceIndex = i;
            out[n].presetIndex = v.playingPreset;
            out[n].regionIndex = static_cast<int>(v.region - p.regions);
            out[n].initialFilterFc = v.region->initialFilterFc;
        }
        ++n;
    }
    return n;
}

extern "C" void tsf_ext_voice_replace_velocity_gain(tsf* f, int voiceIndex, float vel,
                                                     float attenuationDB) {
    if (!f || !f->voices || voiceIndex < 0 || voiceIndex >= f->voiceNum) return;
    if (!(vel > 0.0f)) return;
    struct tsf_voice& v = f->voices[voiceIndex];
    // tsf.h:1619 hizo `noteGainDB = global - atten - gainToDecibels(1/vel)`.
    // Se suma de vuelta EXACTAMENTE ese termino y se resta el modulado.
    v.noteGainDB += tsf_gainToDecibels(1.0f / vel) - attenuationDB;
}

extern "C" void tsf_ext_voice_set_filter_cutoff(tsf* f, int voiceIndex, float cutoffCents) {
    if (!f || !f->voices || voiceIndex < 0 || voiceIndex >= f->voiceNum) return;
    struct tsf_voice& v = f->voices[voiceIndex];
    // Misma formula que el "Setup lowpass filter" de tsf_note_on, con el corte
    // modulado en lugar de region->initialFilterFc. QInv no cambia: el Q no se modula.
    const float lowpassFc =
        (cutoffCents <= 13500.0f ? tsf_cents2Hertz(cutoffCents) / f->outSampleRate : 1.0f);
    v.lowpass.z1 = v.lowpass.z2 = 0;
    v.lowpass.active = (lowpassFc < 0.499f);
    if (v.lowpass.active) tsf_voice_lowpass_setup(&v.lowpass, lowpassFc);
}
