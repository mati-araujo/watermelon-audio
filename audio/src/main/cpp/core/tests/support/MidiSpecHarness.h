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
 * Y lo que este REQ mide es el **RENDERIZADOR**, no la fachada. La pregunta es si
 * el renderizador reproduce el font como fue programado. Meter la fachada en el
 * medio mediria **sus** limitaciones —un preset, sin CC— y no la conformidad del
 * renderizador, que es justo lo que hay que medir. Por eso el arnes maneja el
 * renderizador como lo maneja un reproductor de MIDI, que es como el spec-test
 * espera ser tocado.
 *
 * 🔴 Y DESDE S2 EL RENDERIZADOR ES `tsf` MAS LOS MODULADORES DEL ARCHIVO. `tsf`
 * los descarta al cargar; `SoundFontModulatorTable` los recupera de los bytes y
 * `channelNoteOnWithModulators` los aplica por voz al disparar. Ese note-on es el
 * MISMO que cruza `SoundFontEngine::drainEvents` en produccion: el arnes no
 * duplica el cableado, lo comparte. Si estuviera solo en la fachada, este arnes
 * mediria un tsf pelado y el trinquete de las escaleras nunca se habria movido —
 * que es exactamente lo que paso en el primer intento de 2.8.
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

#include <fstream>
#include <iterator>

#include "tml.h"
#include "tsf.h"
#include "../../../engines/SoundFontModulatorTable.h"
#include "../../../engines/SoundFontNoteOn.h"

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
    /// Los apagados (0x80 y 0x90 con velocity 0), en orden temporal. `velocity` = 0.
    /// S3 los necesita para acotar cada nota a su DURACION real: un hop de pitch que
    /// cruza el note-off mide la cola de release, no la nota.
    std::vector<NoteOn> noteOffs;
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

    // Los bytes se leen UNA vez y alimentan a los dos: tsf y la tabla de moduladores.
    // Es lo mismo que hace SoundFontManager::parseFont entre tsf_load_memory y el munmap.
    std::ifstream in(sf2Path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    if (bytes.empty()) return out;
    tsf* f = tsf_load_memory(bytes.data(), static_cast<int>(bytes.size()));
    if (!f) return out;
    wma::sfmod::ModulatorTable modulators;
    modulators.buildFromFontBytes(bytes.data(), bytes.size());
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
                        wma::sfmod::channelNoteOnWithModulators(f, &modulators, msg->channel,
                                                                msg->key, msg->velocity / 127.0f);
                    } else {
                        out.noteOffs.push_back(NoteOn{msg->time / 1000.0, msg->channel, msg->key, 0});
                        tsf_channel_note_off(f, msg->channel, msg->key);
                    }
                    break;
                case TML_NOTE_OFF:
                    out.noteOffs.push_back(NoteOn{msg->time / 1000.0, msg->channel, msg->key, 0});
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

// ---- REQ-039 S3 (3.1): las 22 ventanas DERIVADAS, y los tres observables -------------

/**
 * Una vista sobre un render estereo intercalado: el nuestro (`Rendered`) o la
 * referencia (`wav::WavData`). Los observables se calculan sobre esto para que la
 * MISMA funcion mida los dos lados — si el estimador tiene un sesgo, lo tiene igual
 * en ambos y la diferencia lo cancela.
 */
struct Signal {
    const float* s = nullptr;
    int frames = 0;
    int rate = 0;
};

inline Signal view(const Rendered& r) { return {r.stereo.data(), r.frames, r.sampleRate}; }

/**
 * Una ventana de prueba del spec-test, con sus notas de carga util.
 *
 * 🔴 LAS 22 SE DERIVAN DEL ARCHIVO, NO SE DECLARAN. S1 dejo 7 ventanas porque el
 * `.mid` "no tiene marcadores" y solo 6 pistas llevan nombre. Medido el 2026-09-11:
 * tiene OTRA clase de marcador. El canal 0 ANUNCIA cada prueba N con la tecla
 * `20 + N` (21 en 0,0 s -> #1 ... 42 en 264,0 s -> #22) y las sub-pruebas con las
 * teclas 51..56 (A..F). La ventana de la prueba N es `[anuncio N, anuncio N+1)`; la de
 * una sub-prueba, `[su anuncio, el siguiente anuncio de sub-prueba o de prueba)`.
 * Lo protege el sha256 de `fetch-spec-test.sh`: si upstream mueve un anuncio, el
 * material cambia antes de que este mapeo pueda mentir.
 *
 * Las notas de carga util son TODAS las note-on de cualquier canal dentro de la
 * ventana, MENOS los anuncios (canal 0, teclas 21..42 y 51..56). Las teclas bajas del
 * canal 0 que NO son anuncios (18, 19, 20 en #16 y #21) son carga util: los samples
 * de voz "supported"/"not supported" y el ruido de la clase exclusiva.
 */
struct Window {
    int test = 0;         ///< 1..22
    int sub = 0;          ///< 0 = la prueba entera; 1..6 = A..F
    double t0 = 0.0, t1 = 0.0;
    std::vector<NoteOn> notes;   ///< carga util, en orden temporal
    std::string label() const {
        char buf[32];
        if (sub == 0) std::snprintf(buf, sizeof buf, "#%d", test);
        else std::snprintf(buf, sizeof buf, "#%d %c", test, static_cast<char>('A' + sub - 1));
        return buf;
    }
};

/// El instante del note-off de `n` (mismo canal y tecla, el primero despues de su
/// note-on), o `fallback` si no hay.
inline double noteOffSec(const Rendered& r, const NoteOn& n, double fallback) {
    for (const NoteOn& off : r.noteOffs)
        if (off.sec >= n.sec && off.channel == n.channel && off.key == n.key) return off.sec;
    return fallback;
}

inline bool isTestAnnouncement(const NoteOn& n) {
    return n.channel == 0 && n.key >= 21 && n.key <= 42;
}
inline bool isSubAnnouncement(const NoteOn& n) {
    return n.channel == 0 && n.key >= 51 && n.key <= 56;
}

/**
 * Deriva las ventanas: las 22 pruebas (`sub == 0`) y, para las que tienen
 * sub-pruebas anunciadas, una ventana por sub-prueba. El orden es temporal. La
 * ultima prueba cierra en el fin del render.
 */
inline std::vector<Window> deriveWindows(const Rendered& r) {
    std::vector<Window> out;
    const double end = r.frames / static_cast<double>(r.sampleRate);
    // 1) las pruebas, por anuncio
    for (const NoteOn& n : r.noteOns) {
        if (!isTestAnnouncement(n)) continue;
        if (!out.empty()) out.back().t1 = n.sec;
        Window w;
        w.test = n.key - 20;
        w.t0 = n.sec;
        w.t1 = end;
        out.push_back(w);
    }
    // 2) las sub-pruebas, dentro de cada prueba
    std::vector<Window> subs;
    for (const Window& w : out) {
        for (const NoteOn& n : r.noteOns) {
            if (n.sec < w.t0 || n.sec >= w.t1 || !isSubAnnouncement(n)) continue;
            if (!subs.empty() && subs.back().test == w.test) subs.back().t1 = n.sec;
            Window s;
            s.test = w.test;
            s.sub = n.key - 50;
            s.t0 = n.sec;
            s.t1 = w.t1;
            subs.push_back(s);
        }
    }
    // 3) la carga util de cada ventana, y el orden temporal final
    std::vector<Window> all;
    all.reserve(out.size() + subs.size());
    for (const Window& w : out) {
        all.push_back(w);
        for (const Window& s : subs)
            if (s.test == w.test) all.push_back(s);
    }
    for (Window& w : all) {
        for (const NoteOn& n : r.noteOns) {
            if (n.sec < w.t0 || n.sec >= w.t1) continue;
            if (isTestAnnouncement(n) || isSubAnnouncement(n)) continue;
            w.notes.push_back(n);
        }
    }
    return all;
}

/// RMS en dB de UN canal (0 = L, 1 = R) o de la mezcla mono (`ch = -1`), `[t0, t0+dur)`.
inline double rmsDbOf(const Signal& x, double t0, double dur, int ch = -1) {
    long long a = static_cast<long long>(t0 * x.rate);
    long long b = static_cast<long long>((t0 + dur) * x.rate);
    if (a < 0) a = 0;
    if (b > x.frames) b = x.frames;
    if (b <= a) return -200.0;
    double acc = 0.0;
    for (long long i = a; i < b; ++i) {
        const float* p = x.s + static_cast<size_t>(i) * 2;
        const double m = ch < 0 ? 0.5 * (static_cast<double>(p[0]) + static_cast<double>(p[1]))
                                : static_cast<double>(p[ch]);
        acc += m * m;
    }
    const double rms = std::sqrt(acc / static_cast<double>(b - a));
    return rms > 1e-10 ? 20.0 * std::log10(rms) : -200.0;
}

/// Observable NIVEL: RMS mono de los primeros `dur` s de la nota, en dB.
inline double levelDb(const Signal& x, double t0, double dur = 0.40) { return rmsDbOf(x, t0, dur); }

/**
 * Observable BALANCE: `20·log10(rms_L / rms_R)` de los primeros `dur` s, en dB, con
 * cada canal PISADO en -96 dB (el piso de 16 bits). Sin el piso, un canal en silencio
 * digital (tsf a -50 % de pan) daba -200 contra el piso de la referencia y el balance
 * "media" 140 dB de diferencia donde los dos estan totalmente a la izquierda. Medido
 * en la primera corrida de 3.1.
 */
inline double balanceDb(const Signal& x, double t0, double dur = 0.40) {
    const double l = std::max(rmsDbOf(x, t0, dur, 0), -96.0);
    const double r = std::max(rmsDbOf(x, t0, dur, 1), -96.0);
    return l - r;
}

/**
 * Observable PITCH: frecuencia por CRUCES POR CERO ascendentes de la mezcla mono en
 * `[t0, t0+dur)`, en Hz; 0 si hay menos de tres cruces o el tramo esta en silencio.
 *
 * Es deliberadamente el estimador mas simple que existe: el spec-test usa SENOIDES
 * en las pruebas de pitch (#2, #6, #8, #20), y sobre una senoide limpia el cruce
 * por cero interpolado es exacto al centesimo de cent. NO es el afinador del motor
 * (`analysis/`): no porque sea el sistema bajo prueba —lo es el renderizador—, sino
 * porque un rojo con 17 archivos y un thread de analisis adentro ya no dice de
 * quien es. Sobre ruido (#9, #10) devuelve basura, y esas filas no lo usan.
 */
inline double pitchHz(const Signal& x, double t0, double dur = 0.25) {
    long long a = static_cast<long long>(t0 * x.rate);
    long long b = static_cast<long long>((t0 + dur) * x.rate);
    if (a < 1) a = 1;
    if (b > x.frames) b = x.frames;
    if (b <= a) return 0.0;
    if (rmsDbOf(x, t0, dur) < -60.0) return 0.0;
    auto mono = [&](long long i) {
        const float* p = x.s + static_cast<size_t>(i) * 2;
        return 0.5 * (static_cast<double>(p[0]) + static_cast<double>(p[1]));
    };
    double first = -1.0, last = -1.0;
    int crossings = 0;
    for (long long i = a; i < b; ++i) {
        const double p = mono(i - 1), c = mono(i);
        if (p < 0.0 && c >= 0.0) {
            const double frac = p / (p - c);   // interpolacion lineal del cruce
            const double t = static_cast<double>(i - 1) + frac;
            if (first < 0.0) first = t;
            last = t;
            ++crossings;
        }
    }
    if (crossings < 3 || last <= first) return 0.0;
    return static_cast<double>(crossings - 1) * x.rate / (last - first);
}

/**
 * Observables de PROFUNDIDAD de un LFO: el pico a pico del nivel (dB) o del pitch
 * (cents) en hops de 50 ms sobre `[t0, t1)`. A 0,25 s el hop promedia el ciclo entero
 * de un LFO de 4 Hz y la profundidad desaparece (medido en 3.1: #5 daba 0,00 con
 * ±6 dB declarados); a 50 ms —un quinto de ciclo— se ve, y el sesgo del promedio es
 * el MISMO en los dos renders.
 */
inline double levelPeakToPeakDb(const Signal& x, double t0, double t1, double hop = 0.05) {
    double lo = 1e9, hi = -1e9;
    for (double t = t0; t + hop <= t1 + 1e-9; t += hop) {
        const double l = rmsDbOf(x, t, hop);
        if (l < -100.0) continue;
        lo = std::min(lo, l);
        hi = std::max(hi, l);
    }
    return hi > lo ? hi - lo : 0.0;
}

inline double pitchPeakToPeakCents(const Signal& x, double t0, double t1, double hop = 0.05) {
    double lo = 1e9, hi = -1e9;
    for (double t = t0; t + hop <= t1 + 1e-9; t += hop) {
        const double hz = pitchHz(x, t, hop);
        if (hz <= 0.0) continue;
        lo = std::min(lo, hz);
        hi = std::max(hi, hz);
    }
    return hi > lo ? 1200.0 * std::log2(hi / lo) : 0.0;
}

inline double centsBetween(double hz, double refHz) {
    return (hz > 0.0 && refHz > 0.0) ? 1200.0 * std::log2(hz / refHz) : 0.0;
}

}  // namespace wma_test::specmidi
