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

// ---- MINI-027: los generadores de filtro de una region, para MEDIR --------------
//
// Cuantas regiones del font que se shippea tienen filtro dinamico (mod env o mod LFO
// al corte) es el numero que dice cuanto pesaba el limite de S2, y un numero que se
// afirma se mide del arbol, no se escribe (REQ-021). Thread de control, solo lectura.
// Devuelve 1 y escribe los tres, o 0 sin tocar nada si el preset o la region no existen.
int tsf_ext_preset_region_count(const tsf* f, int presetIndex);
int tsf_ext_region_filter(const tsf* f, int presetIndex, int regionIndex, int* initialFilterFc,
                          int* modEnvToFilterFc, int* modLfoToFilterFc);

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
    int initialFilterQ;   // MINI-027: el Q de la region en centibeles (0..960), misma base
    int modEnvToFilterFc; // MINI-027: cuanto mod env entra al filtro en la region, en cents
    float reverbSend;     // REQ-040: los sends de la region (gens 16/15 ya sumados preset +
    float chorusSend;     //          instrumento), en 0..1
    double pitchTimecents;  // el pitch RESUELTO de la voz al arrancar, en cents absolutos
                            // (tecla * 100 con keytrack 100 y sin offsets): raiz + keytrack +
                            // coarse + fine + pitchCorrection (MINI-025), mas el tuning del
                            // canal. Solo lectura: es lo que `tsf_voice_calcpitchratio` dejo
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
// Hasta MINI-027 esto tenia un LIMITE DECLARADO: si la region tenia `modLfoToFilterFc`
// o `modEnvToFilterFc`, `tsf_voice_render` recalculaba el corte cada bloque desde
// `region->initialFilterFc` y pisaba esto en el primer bloque. Medido sobre GeneralUser
// (2026-09-15): 3380 de 12311 regiones, en 111 presets, con el velocity -> filtro
// INERTE (166 zonas de instrumento en 24 instrumentos). Desde MINI-027 el corte es un
// campo POR VOZ (`tsf_voice::initialFilterFc`), esto lo escribe y el render lo lee:
// sobrevive al bloque dinamico.
void tsf_ext_voice_set_filter_cutoff(tsf* f, int voiceIndex, float cutoffCents);

// ---- MINI-027: los ocho destinos de note-on que REQ-039 S2 dejo fuera ------------
//
// Cada uno REEMPLAZA por voz lo que tsf_note_on copio de la region, con el valor ya
// modulado (generador + lo que la tabla resolvio para esta tecla y velocity). Todo RT:
// aritmetica sobre la voz, sin alocar, sin lock, sin log. Se llaman DESPUES de
// tsf_channel_note_on y ANTES del primer render de la voz.

// Cuanto mod env entra al filtro, en cents (SF2 §8.1.2 #11), por voz. Con != 0 el
// render trata a la voz como de filtro dinamico aunque la region no lo declare.
void tsf_ext_voice_set_mod_env_to_filter(tsf* f, int voiceIndex, float cents);

// El Q del low-pass en centibeles (#9), por voz. Re-setupea el filtro con el corte que
// la voz tiene (el de la region o el que set_filter_cutoff dejo).
void tsf_ext_voice_set_filter_q(tsf* f, int voiceIndex, float qCentibels);

// Desplaza las tres duraciones de una envolvente por voz en TIMECENTS (#26/#34/#36/#38:
// el modulador SUMA timecents al generador, o sea MULTIPLICA la duracion por
// 2^(tc/1200)). `isAmpEnv` elige volumen (1) o modulacion (0). Un segmento que tsf
// habia fijado en 0 s (generador < -11950 tc, "instantaneo") parte de 2^(-12000/1200) =
// 0,98 ms si el desplazamiento es positivo, que es lo que el spec da para
// -12000 + tc. Reinicia la envolvente desde el principio: la voz todavia no rindio
// ninguna muestra, asi que es exactamente lo que note_on habria hecho con el
// generador ya sumado.
void tsf_ext_voice_offset_envelope(tsf* f, int voiceIndex, int isAmpEnv, float attackTc,
                                   float decayTc, float releaseTc);

// Corre el punto de arranque del sample `samples` muestras (#0, startAddrsOffset) desde
// donde note_on lo dejo (el offset de la region), sin salirse del sample.
void tsf_ext_voice_add_start_offset(tsf* f, int voiceIndex, float samples);

// Suma `pan` (-0.5..0.5, la misma escala que `region->pan`: 0,1 % = 0.001) al paneo de la
// voz y recalcula los dos factores con la ley de note_on / setup_voice (raiz cuadrada,
// saturando en los extremos). Tiene en cuenta el panOffset del canal si hay canales.
void tsf_ext_voice_add_pan(tsf* f, int voiceIndex, float pan);

#ifdef __cplusplus
}
#endif
