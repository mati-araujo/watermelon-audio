/**
 * MidiSpecHarness.h — REQ-039 S1, tarea 1.3. El arnes de conformidad.
 *
 * QUE HACE
 * --------
 * Toca `sf_spec_test.mid` contra `sf_spec_test.sf2` a traves del renderizador y
 * devuelve el audio, mas los limites de cada nota DERIVADOS del propio MIDI. Con
 * eso un test puede reportar, prueba por prueba, cuanto se aparta el motor de una
 * referencia — que es lo unico que contesta "¿reproduce el font como fue
 * programado?" sin depender del oido.
 *
 * 🔴 POR QUE ESTO NO PASA POR `SoundFontEngine`, Y NO ES UN ATAJO
 * ---------------------------------------------------------------
 * El plan de S1 decia "entra por la superficie que ya existe
 * (`wma_sf_note_on`/`wma_sf_note_off`)". **Medido el 2026-09-10, eso es imposible
 * para este MIDI**, y no por poco:
 *
 *   | lo que el .mid necesita        | lo que `SoundFontEngine` expone |
 *   |--------------------------------|---------------------------------|
 *   | 7 canales con 7 presets A LA VEZ | UN preset para los 16 toques   |
 *   | 1440 pitch bends               | nada                            |
 *   | CC1 (265), CC7, CC10, CC91, CC93 | nada                          |
 *   | note-on con velocity           | si                              |
 *
 * `SoundFontEngine` es una fachada de INSTRUMENTO TACTIL sobre el renderizador:
 * un preset, 16 toques, expresion por toque. No es un sintetizador MIDI, y no
 * tiene por que serlo.
 *
 * Y lo que este REQ mide es el **RENDERIZADOR**, no la fachada. Los moduladores
 * viven en `tsf`; la pregunta es si `tsf` reproduce el font como fue programado.
 * Meter la fachada en el medio mediria **sus** limitaciones —un preset, sin CC— y
 * no la conformidad del renderizador, que es justo lo que hay que medir. Por eso
 * el arnes maneja `tsf` como lo maneja un reproductor de MIDI, que es como el
 * spec-test espera ser tocado.
 *
 * LO QUE ESTE ARNES NO CUBRE, Y ES DELIBERADO
 * -------------------------------------------
 * El camino de `SoundFontEngine` —el mapeo toque->canal, la expresion por toque,
 * la cola de eventos lock-free— **no lo toca nadie aca**. Ya tiene sus propios
 * tests (`test_touch_expression.cpp`, `test_touch_expression_surface.cpp`), y
 * duplicarlos no agregaria cobertura. Si un dia el arreglo de los moduladores
 * cambia algo de ese camino, lo dicen esos.
 */
#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "tml.h"
#include "tsf.h"

namespace wma_test::specmidi {

/// Un note-on del MIDI, con el instante en que cae. Los limites de las pruebas se
/// DERIVAN de esto (ver `Rendered::noteOnSec`), nunca de tiempos escritos a mano.
struct NoteOn {
    double sec = 0.0;
    int channel = 0;
    int key = 0;
    int velocity = 0;
};

struct Rendered {
    bool valid = false;
    int sampleRate = 0;
    std::vector<float> stereo;   ///< intercalado L/R, `frames * 2`
    int frames = 0;
    std::vector<NoteOn> noteOns; ///< en orden temporal
    /// Lo que `tml_get_info` reporta del archivo: sirve de control de que se cargo
    /// lo que se creia.
    int usedChannels = 0, usedPrograms = 0, totalNotes = 0;
};

/**
 * Renderiza el MIDI entero contra el font, en bloques.
 *
 * @param sf2Path   el banco del spec-test
 * @param midPath   el MIDI del spec-test
 * @param rate      tasa de salida
 * @param gain      ganancia global en dB. **0 por default, a proposito**: el
 *                  spec-test tiene pruebas de nivel (#11, #12) y una ganancia
 *                  distinta de 0 las movería. `tsf_set_volume` queda en 1.
 * @param blockSize frames por bloque. Multiplo de `TSF_RENDER_EFFECTSAMPLEBLOCK`
 *                  (64), que es la granularidad con la que tsf actualiza sus
 *                  envolventes y LFO.
 */
inline Rendered render(const std::string& sf2Path, const std::string& midPath, int rate = 44100,
                       float gain = 0.0f, int blockSize = 512) {
    Rendered out;

    tsf* f = tsf_load_filename(sf2Path.c_str());
    if (!f) return out;
    tml_message* midi = tml_load_filename(midPath.c_str());
    if (!midi) {
        tsf_close(f);
        return out;
    }

    tml_get_info(midi, &out.usedChannels, &out.usedPrograms, &out.totalNotes, nullptr, nullptr);

    // El banco entra como bank 0, que es lo que el README del spec-test pide.
    tsf_channel_set_bank_preset(f, 9, 0, 0);   // el canal 9 NO es percusion en este test
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, rate, gain);
    // Voces acotadas: sin esto `tsf_note_on` REALLOCA cuando se le acaban, y aunque
    // aca no corre en el thread de audio, un limite fijo hace el render reproducible.
    tsf_set_max_voices(f, 256);

    // La duracion sale del ULTIMO mensaje mas una cola para que las liberaciones
    // terminen. La cola es generosa: la prueba #1 tiene una release de 1 s entera.
    double lastSec = 0.0;
    for (tml_message* m = midi; m; m = m->next) lastSec = m->time / 1000.0;
    const double tailSec = 3.0;
    out.frames = static_cast<int>((lastSec + tailSec) * rate);
    out.stereo.assign(static_cast<size_t>(out.frames) * 2, 0.0f);
    out.sampleRate = rate;

    tml_message* msg = midi;
    int done = 0;
    while (done < out.frames) {
        const int n = (out.frames - done) < blockSize ? (out.frames - done) : blockSize;
        const double blockEndSec = static_cast<double>(done + n) / rate;

        // Todos los mensajes que caen dentro de este bloque se despachan ANTES de
        // renderizarlo. Eso cuantiza los eventos al bloque —igual que un
        // reproductor real— y es la misma resolucion para el motor y para la
        // referencia, asi que no sesga la comparacion.
        for (; msg && (msg->time / 1000.0) < blockEndSec; msg = msg->next) {
            switch (msg->type) {
                case TML_PROGRAM_CHANGE:
                    tsf_channel_set_presetnumber(f, msg->channel, msg->program, /*drums=*/0);
                    break;
                case TML_NOTE_ON:
                    if (msg->velocity > 0) {
                        out.noteOns.push_back(NoteOn{msg->time / 1000.0, msg->channel, msg->key,
                                                     msg->velocity});
                        tsf_channel_note_on(f, msg->channel, msg->key, msg->velocity / 127.0f);
                    } else {
                        tsf_channel_note_off(f, msg->channel, msg->key);
                    }
                    break;
                case TML_NOTE_OFF:
                    tsf_channel_note_off(f, msg->channel, msg->key);
                    break;
                case TML_CONTROL_CHANGE:
                    tsf_channel_midi_control(f, msg->channel, msg->control, msg->control_value);
                    break;
                case TML_PITCH_BEND:
                    tsf_channel_set_pitchwheel(f, msg->channel, msg->pitch_bend);
                    break;
                default:
                    break;
            }
        }

        tsf_render_float(f, out.stereo.data() + static_cast<size_t>(done) * 2, n, /*mixing=*/0);
        done += n;
    }

    tml_free(midi);
    tsf_close(f);
    out.valid = true;
    return out;
}

/// RMS de un tramo mono-mezclado `[fromSec, toSec)`, en dB (−inf -> −200).
inline double rmsDb(const Rendered& r, double fromSec, double toSec) {
    if (!r.valid || toSec <= fromSec) return -200.0;
    long long a = static_cast<long long>(fromSec * r.sampleRate);
    long long b = static_cast<long long>(toSec * r.sampleRate);
    if (a < 0) a = 0;
    if (b > r.frames) b = r.frames;
    if (b <= a) return -200.0;
    double acc = 0.0;
    for (long long i = a; i < b; ++i) {
        const double m = 0.5 * (static_cast<double>(r.stereo[static_cast<size_t>(i) * 2]) +
                                static_cast<double>(r.stereo[static_cast<size_t>(i) * 2 + 1]));
        acc += m * m;
    }
    const double rms = std::sqrt(acc / static_cast<double>(b - a));
    return rms > 1e-10 ? 20.0 * std::log10(rms) : -200.0;
}

}  // namespace wma_test::specmidi
