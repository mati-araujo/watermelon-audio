/**
 * SoundFontNoteOn.h — REQ-039 S2 (2.8): EL note-on del renderizador.
 *
 * `tsf_channel_note_on` crea las voces con la ganancia de velocity CABLEADA
 * (`tsf.h:1619`, `20·log10(vel)`) y el corte de filtro FIJO de la región. SF2 §8.4
 * dice que las dos salen de MODULADORES, y `tsf` los descarta al cargar. Esta
 * función es `tsf_channel_note_on` MÁS lo que el archivo declaró: deja que tsf
 * arranque las voces y después REEMPLAZA, por voz, esos dos valores por los que la
 * tabla resolvió al cargar (defaults #1 y #2 salvo que el archivo los pise o anule).
 *
 * 🔴 POR QUÉ ES UNA FUNCIÓN LIBRE Y NO UN MÉTODO DE `SoundFontEngine`
 * -------------------------------------------------------------------
 * El arnés de conformidad (`core/tests/support/MidiSpecHarness.h`) maneja `tsf`
 * como un reproductor MIDI —siete canales, pitch bend, CC— y NO pasa por la
 * fachada táctil, a propósito: mide el renderizador. Si el cableado viviera en la
 * fachada, el arnés tendría que duplicarlo en el test para verlo, y entonces
 * mediría que el mecanismo funciona *si alguien lo llama* — la forma exacta de
 * REQ-012. Con la función acá, producción (`SoundFontEngine::drainEvents`) y el
 * arnés cruzan el MISMO note-on. El renderizador de este motor es tsf + esto.
 *
 * RT: la tabla es de sólo lectura, la consulta es un índice, la suma es un bucle
 * acotado y las dos escrituras son aritmética sobre la voz. Sin alocar, sin lock,
 * sin log. El thread de audio la llama desde `drainEvents`.
 */
#pragma once

#include "SoundFontModulatorTable.h"
#include "tsf.h"
#include "tsf_ext.h"

namespace wma {
namespace sfmod {

/**
 * Un note-on de tsf arranca a lo sumo tantas voces como regiones matcheen la tecla
 * y la velocity. GeneralUser llega a 4 por tecla; 16 cubre fonts reales con margen.
 * Si un font raro arranca más, las que no entran quedan como antes de REQ-039 —
 * como sonaban ayer, no peor.
 */
constexpr int kMaxVoicesPerNoteOn = 16;

/**
 * `tsf_channel_note_on` + los moduladores del archivo. Devuelve lo que devuelve tsf.
 *
 * @param table  la tabla del font cargado, o `nullptr` = sin moduladores = tsf pelado.
 * @param vel    0..1, el MISMO float que tsf recibe: la ext deshace `tsf.h:1619` con
 *               la misma aritmética con la que se hizo.
 */
inline int channelNoteOnWithModulators(tsf* sf, const ModulatorTable* table, int channel,
                                       int key, float vel) noexcept {
    const int rc = tsf_channel_note_on(sf, channel, key, vel);
    if (!table || !(vel > 0.0f)) return rc;

    tsf_ext_started_voice started[kMaxVoicesPerNoteOn];
    const int n = tsf_ext_voices_started_by_last_note_on(sf, started, kMaxVoicesPerNoteOn);
    const int limit = n < kMaxVoicesPerNoteOn ? n : kMaxVoicesPerNoteOn;
    // La misma conversión que tsf_note_on: `(short)(vel * 127)`, truncada.
    const int midiVelocity = static_cast<int>(vel * 127.0f);

    for (int i = 0; i < limit; ++i) {
        const RegionModulatorList* mods =
            table->regionModulators(started[i].presetIndex, started[i].regionIndex);
        if (!mods) continue;  // la tabla no conoce la región: como antes de REQ-039
        const NoteOnContribution c = noteOnContributionsOf(mods, key, midiVelocity);
        // cB -> dB. REEMPLAZA el término de velocity; no lo corrige (2.6).
        tsf_ext_voice_replace_velocity_gain(sf, started[i].voiceIndex, vel,
                                            c.attenuationCentibels * 0.1f);
        // El corte modulado es RELATIVO al de la región: cents sobre initialFilterFc.
        if (c.filterFcCents != 0.0f) {
            tsf_ext_voice_set_filter_cutoff(
                sf, started[i].voiceIndex,
                static_cast<float>(started[i].initialFilterFc) + c.filterFcCents);
        }
    }
    return rc;
}

}  // namespace sfmod
}  // namespace wma
