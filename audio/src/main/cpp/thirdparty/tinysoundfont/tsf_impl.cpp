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

// ---- MINI-027: los generadores de filtro de una region (medicion, thread de control) --
extern "C" int tsf_ext_preset_region_count(const tsf* f, int presetIndex) {
    if (!f || presetIndex < 0 || presetIndex >= f->presetNum) return 0;
    return f->presets[presetIndex].regionNum;
}

extern "C" int tsf_ext_region_filter(const tsf* f, int presetIndex, int regionIndex, int* initialFilterFc,
                                     int* modEnvToFilterFc, int* modLfoToFilterFc) {
    if (!f || presetIndex < 0 || presetIndex >= f->presetNum) return 0;
    const struct tsf_preset& p = f->presets[presetIndex];
    if (!p.regions || regionIndex < 0 || regionIndex >= p.regionNum) return 0;
    const struct tsf_region& r = p.regions[regionIndex];
    if (initialFilterFc) *initialFilterFc = r.initialFilterFc;
    if (modEnvToFilterFc) *modEnvToFilterFc = r.modEnvToFilterFc;
    if (modLfoToFilterFc) *modLfoToFilterFc = r.modLfoToFilterFc;
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
            out[n].initialFilterQ = v.region->initialFilterQ;
            out[n].modEnvToFilterFc = v.region->modEnvToFilterFc;
            out[n].reverbSend = v.region->reverbSend;
            out[n].chorusSend = v.region->chorusSend;
            out[n].pitchTimecents = v.pitchInputTimecents;
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

namespace {

// Misma formula que el "Setup lowpass filter" de tsf_note_on, sobre los valores que la
// VOZ tiene (MINI-027: el corte es por voz y el render lo lee de ahi).
void setupVoiceLowpass(tsf* f, struct tsf_voice& v) {
    const float lowpassFc =
        (v.initialFilterFc <= 13500.0f ? tsf_cents2Hertz(v.initialFilterFc) / f->outSampleRate : 1.0f);
    v.lowpass.z1 = v.lowpass.z2 = 0;
    v.lowpass.active = (lowpassFc < 0.499f);
    if (v.lowpass.active) tsf_voice_lowpass_setup(&v.lowpass, lowpassFc);
}

struct tsf_voice* voiceAt(tsf* f, int voiceIndex) {
    if (!f || !f->voices || voiceIndex < 0 || voiceIndex >= f->voiceNum) return nullptr;
    return &f->voices[voiceIndex];
}

}  // namespace

extern "C" void tsf_ext_voice_set_filter_cutoff(tsf* f, int voiceIndex, float cutoffCents) {
    struct tsf_voice* v = voiceAt(f, voiceIndex);
    if (!v) return;
    v->initialFilterFc = cutoffCents;
    setupVoiceLowpass(f, *v);
}

// ---- MINI-027: los ocho destinos de note-on que S2 dejo fuera ----------------------

extern "C" void tsf_ext_voice_set_mod_env_to_filter(tsf* f, int voiceIndex, float cents) {
    struct tsf_voice* v = voiceAt(f, voiceIndex);
    if (!v) return;
    v->modEnvToFilterFc = cents;
}

extern "C" void tsf_ext_voice_set_filter_q(tsf* f, int voiceIndex, float qCentibels) {
    struct tsf_voice* v = voiceAt(f, voiceIndex);
    if (!v) return;
    // La MISMA escritura que tsf_note_on (REQ-041 S1): clip a 0..96 dB y el -3,01 de
    // FluidSynth. El modulado puede salirse de 0..960 cB; el generador ya venia saturado.
    tsf_voice_lowpass_set_q(&v->lowpass, qCentibels);
    setupVoiceLowpass(f, *v);
}

namespace {

// Multiplica una duracion en segundos por 2^(tc/1200). Un segmento que tsf pinneo en 0 s
// ("instantaneo": generador < -11950 tc, tsf_region_envtosecs) parte de 2^(-12000/1200)
// cuando el desplazamiento es positivo, que es lo que el spec da para -12000 + tc; con
// desplazamiento negativo se queda en 0.
float offsetSeconds(float seconds, float tc) {
    if (tc == 0.0f) return seconds;
    if (seconds <= 0.0f) {
        if (tc <= 0.0f) return seconds;
        seconds = tsf_timecents2Secsf(-12000.0f);
    }
    return seconds * tsf_timecents2Secsf(tc);
}

}  // namespace

extern "C" void tsf_ext_voice_offset_envelope(tsf* f, int voiceIndex, int isAmpEnv, float attackTc,
                                              float decayTc, float releaseTc) {
    struct tsf_voice* v = voiceAt(f, voiceIndex);
    if (!v) return;
    struct tsf_voice_envelope& e = isAmpEnv ? v->ampenv : v->modenv;
    e.parameters.attack = offsetSeconds(e.parameters.attack, attackTc);
    e.parameters.decay = offsetSeconds(e.parameters.decay, decayTc);
    e.parameters.release = offsetSeconds(e.parameters.release, releaseTc);
    // Desde el principio, como note_on: la voz no rindio todavia. keynumToHold/Decay ya
    // entraron en setup (parameters.hold/decay estan en segundos); multiplicar es correcto.
    tsf_voice_envelope_nextsegment(&e, TSF_SEGMENT_NONE, f->outSampleRate);
}

extern "C" void tsf_ext_voice_add_start_offset(tsf* f, int voiceIndex, float samples) {
    struct tsf_voice* v = voiceAt(f, voiceIndex);
    if (!v || !v->region) return;
    double pos = (double)v->region->offset + (double)samples;
    const double lo = (double)v->region->offset;
    const double hi = (double)v->region->end - 1.0;
    if (pos < lo) pos = lo;
    if (pos > hi) pos = hi;
    v->sourceSamplePosition = pos;
}

extern "C" void tsf_ext_voice_set_sends(tsf* f, int voiceIndex, float reverbSend, float chorusSend) {
    struct tsf_voice* v = voiceAt(f, voiceIndex);
    if (!v) return;
    v->reverbSend = reverbSend < 0.0f ? 0.0f : (reverbSend > 1.0f ? 1.0f : reverbSend);
    v->chorusSend = chorusSend < 0.0f ? 0.0f : (chorusSend > 1.0f ? 1.0f : chorusSend);
}

extern "C" void tsf_ext_voice_add_pan(tsf* f, int voiceIndex, float pan) {
    struct tsf_voice* v = voiceAt(f, voiceIndex);
    if (!v || !v->region) return;
    // La misma ley que tsf_channel_setup_voice: region + canal (+ lo modulado), saturando.
    float newpan = v->region->pan + pan;
    if (f->channels && v->playingChannel >= 0 && v->playingChannel < f->channels->channelNum)
        newpan += f->channels->channels[v->playingChannel].panOffset;
    if      (newpan <= -0.5f) { v->panFactorLeft = 1.0f; v->panFactorRight = 0.0f; }
    else if (newpan >=  0.5f) { v->panFactorLeft = 0.0f; v->panFactorRight = 1.0f; }
    else { v->panFactorLeft = TSF_SQRTF(0.5f - newpan); v->panFactorRight = TSF_SQRTF(0.5f + newpan); }
}

// ---- REQ-041 S1: la sonda del render neutral (solo tests, ver tsf_ext.h) --------------
extern "C" void tsf_ext_voice_bypass_lowpass(tsf* f, int voiceIndex) {
    struct tsf_voice* v = voiceAt(f, voiceIndex);
    if (!v) return;
    v->lowpass.active = TSF_FALSE;
}
