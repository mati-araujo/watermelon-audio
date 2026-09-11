/**
 * test_sf_spec_conformance.cpp — REQ-039 S1.
 *
 * El motor contra el SoundFont-Spec-Test (MIT, de terceros). Ver
 * `support/MidiSpecHarness.h` para por que esto maneja `tsf` y no
 * `SoundFontEngine`.
 *
 * 🔴 SIN EL MATERIAL, SKIPPED — NUNCA PASSED. Es la regla de REQ-032: una corrida
 * que no verifico no se puede leer como cobertura. Se baja con
 * `bash scripts/fetch-spec-test.sh`.
 */
#include <gtest/gtest.h>

#include "support/MidiSpecHarness.h"
#include "../../looper/WavFile.h"

#include <sys/stat.h>
#include <cstdlib>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

std::string dir() { return std::string(WMA_SPEC_TEST_DIR); }
std::string sf2() { return dir() + "/sf_spec_test.sf2"; }
std::string mid() { return dir() + "/sf_spec_test.mid"; }

bool have() {
    struct stat st {};
    return stat(sf2().c_str(), &st) == 0 && stat(mid().c_str(), &st) == 0;
}

}  // namespace

/**
 * AC-039.1 (instrumento) — el arnes TOCA: carga el font, recorre el MIDI y produce
 * audio. Lo que se afirma aca no es un valor sino que el instrumento existe y que
 * lo que cargo es lo que se creia.
 */
TEST(SfSpecConformance, TheHarnessActuallyPlaysTheSpecTest) {
    if (!have()) {
        GTEST_SKIP() << "sin material del spec-test — corre scripts/fetch-spec-test.sh";
    }
    const auto r = wma_test::specmidi::render(sf2(), mid());
    ASSERT_TRUE(r.valid) << "el arnes no pudo cargar el font o el MIDI";

    // Control de que se cargo el archivo que se cree. Los numeros salen del propio
    // MIDI y estan medidos (2026-09-10) con un parser INDEPENDIENTE de tml; si
    // cambian, el material cambio y el sha256 de `fetch-spec-test.sh` tendria que
    // haberse puesto rojo antes.
    EXPECT_EQ(r.usedPrograms, 7) << "el .mid declara 7 programas distintos";
    EXPECT_EQ(r.usedChannels, 7) << "el .mid usa 7 canales MIDI";

    /**
     * 🔴 LAS DOS CIFRAS SON DISTINTAS Y LAS DOS SON CORRECTAS. `tml_get_info`
     * cuenta TODO mensaje de tipo `TML_NOTE_ON`, **incluidos los de velocity 0**
     * —que en MIDI son note-off disfrazados— porque no los normaliza (se ve en su
     * propio cuerpo: `if (Msg->type != TML_NOTE_ON) continue; ... total_notes++`).
     * O sea que `totalNotes` NO es "cuantas notas suenan".
     *
     * La aritmetica cierra exacta y por eso se afirma entera, en vez de quedarse
     * con el numero que pasa:
     *
     *   284 note-on con velocity > 0   <- notas de verdad, y lo que el arnes despacha
     * + 270 note-on con velocity = 0   <- apagados disfrazados
     *   ---
     *   554 mensajes 0x90              <- lo que `tml_get_info` llama total_notes
     *
     *   270 (vel 0)  +  14 (0x80 explicitos)  =  284 apagados  =  284 notas ✓
     *
     * Afirmar las dos ata el arnes a la semantica real del archivo. Si alguien
     * "arregla" el conteo del arnes para que de 554, esto se pone rojo.
     */
    EXPECT_EQ(r.totalNotes, 554)
        << "tml_get_info cuenta mensajes 0x90 (notas + apagados disfrazados), no notas";
    EXPECT_EQ(static_cast<int>(r.noteOns.size()), 284)
        << "el arnes tiene que despachar SOLO las notas que suenan (velocity > 0)";
    EXPECT_LT(static_cast<int>(r.noteOns.size()), r.totalNotes)
        << "si estos dos coinciden, el arnes esta tratando los note-on de velocity 0 "
           "como notas y va a disparar 270 notas fantasma";

    // Y SUENA. Un arnes que devuelve silencio pasa cualquier comparacion contra si
    // mismo, asi que esto es lo que lo separa de un doble inerte.
    const double db = wma_test::specmidi::rmsDb(r, 0.0, r.frames / double(r.sampleRate));
    std::printf("  [REQ-039] render: %d frames a %d Hz, %d note-on, RMS global %.1f dB\n",
                r.frames, r.sampleRate, static_cast<int>(r.noteOns.size()), db);
    EXPECT_GT(db, -60.0) << "el render es (casi) silencio: RMS " << db << " dB";

    // Volcado opt-in para exploracion (S1). NO corre en el gate: sin la variable
    // no escribe nada. Una corrida que ESCRIBE no puede pasar por una que verifica.
    if (const char* out = std::getenv("WMA_SPEC_DUMP")) {
        std::ofstream f(out, std::ios::binary);
        f.write(reinterpret_cast<const char*>(r.stereo.data()),
                static_cast<std::streamsize>(r.stereo.size() * sizeof(float)));
        std::printf("  volcado crudo (float32 LR) en %s\n", out);
    }
}

/**
 * AC-039.1 / AC-039.2 — LAS ESCALERAS DE VELOCITY: la tabla que de verdad mide lo
 * que este REQ existe para arreglar, y el control que prueba que el instrumento
 * puede cambiar de veredicto.
 *
 * QUE SON — todo DERIVADO del archivo, nada declarado
 * ---------------------------------------------------
 * Las pruebas #13 (velocity -> atenuacion) y #14 (velocity -> corte del filtro)
 * tienen cinco sub-pruebas cada una, y el `.mid` las ANUNCIA con notas: la tecla
 * 33 abre #13 y la 34 abre #14, y las teclas 51..55 abren las sub-pruebas A..E.
 * Cada anuncio va seguido de una ESCALERA de ocho notas en velocity
 * 127/111/95/79/63/47/31/15, espaciadas 0,5 s, en las teclas 70..79.
 *
 * O sea que las diez escaleras salen del archivo. Lo unico que se declara es la
 * etiqueta legible de cada una, y la protege el sha256 del `.mid`.
 *
 * EL CRITERIO: EL RANGO DE LA ESCALERA
 * ------------------------------------
 * De cada escalera se mide el nivel de sus ocho notas **relativo a la de velocity
 * 127**, y de ahi el RANGO en dB. Es una relacion, asi que es inmune al sesgo de
 * ganancia global entre los dos renders (~1,47 dB, ver el test de arriba) — y es
 * exactamente la cantidad que las sub-pruebas estan diseñadas para variar.
 */
TEST(SfSpecConformance, TheVelocityLaddersFollowWhatTheFileDeclares) {
    if (!have()) GTEST_SKIP() << "sin material del spec-test — corre scripts/fetch-spec-test.sh";
    const std::string refPath = dir() + "/reference-fluidsynth-2.6.0.wav";
    struct stat st {};
    if (stat(refPath.c_str(), &st) != 0) {
        GTEST_SKIP() << "sin referencia — corre scripts/render-spec-reference.sh";
    }
    const auto ours = wma_test::specmidi::render(sf2(), mid());
    ASSERT_TRUE(ours.valid);
    const wav::WavData ref = wav::readWav(refPath.c_str());
    ASSERT_GT(ref.numFrames, 0);

    struct Escalera { const char* etiqueta; double t0; };
    // El instante es el de la nota de velocity 127 que abre cada escalera; sale de
    // los note-on del `.mid` (teclas 70..79), no de un tiempo escrito a mano.
    const Escalera kEsc[] = {
        {"#13 A default 96dB concava", 97.5},  {"#13 B 144 dB", 103.5},
        {"#13 C 48 dB", 109.5},                {"#13 D 96 dB LINEAL", 115.5},
        {"#13 E modulador BORRADO", 121.5},    {"#14 A default", 128.5},
        {"#14 B -7200 cents", 134.5},          {"#14 C SoundFont 2.0", 140.5},
        {"#14 D borrado 2.01", 146.5},         {"#14 E borrado 2.04", 152.5},
    };
    // Las ocho velocities de cada escalera, en el orden en que el `.mid` las toca:
    // 127, 111, 95, 79, 63, 47, 31, 15, una cada 0,5 s. El indice j de `rango()` ES
    // esa posicion, y por eso j==1 es la nota de velocity 111 (la del oraculo).

    auto rmsAt = [&](const float* s, int frames, double t0, double dur) {
        long long a = static_cast<long long>(t0 * ours.sampleRate);
        long long b = static_cast<long long>((t0 + dur) * ours.sampleRate);
        if (a < 0) a = 0;
        if (b > frames) b = frames;
        if (b <= a) return -200.0;
        double acc = 0.0;
        for (long long i = a; i < b; ++i) {
            const double m = 0.5 * (static_cast<double>(s[i * 2]) + static_cast<double>(s[i * 2 + 1]));
            acc += m * m;
        }
        const double r = std::sqrt(acc / static_cast<double>(b - a));
        return r > 1e-10 ? 20.0 * std::log10(r) : -200.0;
    };
    auto rango = [&](const float* s, int frames, double t0, double* delta127a111) {
        double v0 = 0.0, lo = 0.0, hi = 0.0;
        for (int j = 0; j < 8; ++j) {
            const double x = rmsAt(s, frames, t0 + 0.5 * j, 0.40);
            if (j == 0) { v0 = x; lo = hi = 0.0; }
            else {
                const double rel = x - v0;
                if (rel < lo) lo = rel;
                if (rel > hi) hi = rel;
                if (j == 1 && delta127a111) *delta127a111 = rel;
            }
        }
        return hi - lo;
    };

    std::printf("\n  [REQ-039] RANGO de cada escalera de velocity (dB, relativo a su nota de 127)\n");
    std::printf("  %-28s %10s %10s\n", "sub-prueba", "motor", "fluidsyn");
    std::vector<double> rangosNuestros;
    for (const Escalera& e : kEsc) {
        double dOurs = 0.0, dRef = 0.0;
        const double a = rango(ours.stereo.data(), ours.frames, e.t0, &dOurs);
        const double b = rango(ref.buffer.data(), ref.numFrames, e.t0, &dRef);
        std::printf("  %-28s %9.2f %10.2f\n", e.etiqueta, a, b);
        rangosNuestros.push_back(a);
    }

    /**
     * 🔴 EL ORACULO EXTERNO, y es lo que hace que la referencia no sea "lo que
     * FluidSynth diga". El README del spec-test documenta que en #13 A la
     * diferencia entre velocity 127 y 111 tiene que ser **2,34 dB**. Ese numero no
     * sale de nuestro motor NI de FluidSynth: sale del spec. Si la referencia no
     * lo reproduce, la referencia esta mal y todo lo demas sobra.
     */
    double refA = 0.0;
    rango(ref.buffer.data(), ref.numFrames, 97.5, &refA);
    std::printf("  oraculo del README — #13 A, 127 -> 111 = -2,34 dB · referencia: %+.2f dB\n", refA);
    EXPECT_NEAR(refA, -2.34, 0.15)
        << "la referencia de FluidSynth no reproduce el numero que el spec documenta";

    /**
     * 🔴 EL TRINQUETE DE S1 SE DIO VUELTA (S2, tarea 2.8), y esto es el contrato nuevo.
     *
     * S1 dejo medido que las diez escaleras daban UNA sola curva (18,5 dB, dispersion
     * 0,13) porque el motor no leia moduladores. Con `channelNoteOnWithModulators` en el
     * note-on del renderizador, las diez salen del archivo. Lo que se afirma ahora es
     * POR SUB-PRUEBA, y contra DOS oraculos distintos — porque la referencia de
     * FluidSynth NO es el spec en tres de las diez, y eso lo dice el README del
     * spec-test por su nombre:
     *
     *   "many SoundFont synths including FluidSynth and BASSMIDI choose not to
     *    implement this default modulator [velocity -> filter cutoff] at all."
     *
     * Asi que #14 A (default), #14 C (sin filtro declarado: el default otra vez) y
     * #14 D (borrado con la identidad 2.01, que NO es la del default 2.04) salen
     * PLANOS en FluidSynth y con filtrado moderado en un synth 2.04 — que es lo que el
     * spec manda: "moderate filtering (-2400 cent curve) as the velocity decreases
     * from 127 to 0 with no sudden jump". Medido en el font con el lector del repo:
     * `veloToFC-deleted2.01` borra con `amtSrc = velocity/switch` (identidad 2.01) y
     * `veloToFC-deleted2.04` con `amtSrc = none` (identidad 2.04, la nuestra). Solo
     * la segunda anula nuestro default #2, y asi tiene que ser.
     */
    auto nivelesRelativos = [&](const float* s, int frames, double t0, double out[8]) {
        double v0 = 0.0;
        for (int j = 0; j < 8; ++j) {
            const double x = rmsAt(s, frames, t0 + 0.5 * j, 0.40);
            if (j == 0) v0 = x;
            out[j] = x - v0;
        }
    };
    auto rangoRef = [&](double t0) { return rango(ref.buffer.data(), ref.numFrames, t0, nullptr); };

    // (1) Las SIETE donde FluidSynth es conforme al spec: al decimo de dB. #13 D lleva
    // mas tolerancia por el residuo del METODO —el rango de un render con envolvente
    // contra 84 dB de atenuacion se acerca al piso—, que 2.2 midio en +1,23 dB y NO es
    // un desacuerdo de formula. Ver `test_soundfont_modulators.cpp`.
    struct Conforme { int idx; double tol; };
    const Conforme kConformes[] = {{0, 0.25}, {1, 0.25}, {2, 0.25}, {3, 1.5}, {4, 0.25},
                                   {6, 0.25}, {9, 0.25}};
    for (const Conforme& c : kConformes) {
        EXPECT_NEAR(rangosNuestros[static_cast<size_t>(c.idx)], rangoRef(kEsc[c.idx].t0), c.tol)
            << kEsc[c.idx].etiqueta << ": el motor se aparto de FluidSynth, que en esta "
            << "sub-prueba SI es conforme al spec";
    }

    // (2) Las TRES del default #2, contra el SPEC 2.04 y no contra FluidSynth.
    const int kDefaultDos[] = {5, 7, 8};  // #14 A, #14 C, #14 D
    const double rangoMenosSieteMil = rangosNuestros[6];  // #14 B: -7200 cents, el techo
    for (int idx : kDefaultDos) {
        const double r = rangosNuestros[static_cast<size_t>(idx)];
        // "moderate filtering": hay filtrado, y es MENOS que el de -7200 cents.
        EXPECT_GT(r, 1.0) << kEsc[idx].etiqueta << ": sin filtrado por velocity — el default #2 "
                          << "(SF2 2.04 §8.4.2) no se esta aplicando";
        EXPECT_LT(r, rangoMenosSieteMil - 3.0)
            << kEsc[idx].etiqueta << ": el filtrado del default (-2400) no puede superar al de -7200";
        // "no sudden jump": la escalera baja de a poco, sin escalon (que era la marca del 2.01
        // en velocity 63) y sin subir.
        double niv[8];
        nivelesRelativos(ours.stereo.data(), ours.frames, kEsc[idx].t0, niv);
        for (int j = 1; j < 8; ++j) {
            EXPECT_LE(niv[j], niv[j - 1] + 0.3)
                << kEsc[idx].etiqueta << ": el nivel SUBE de la velocity " << j - 1 << " a la " << j;
            EXPECT_GT(niv[j], niv[j - 1] - 4.0)
                << kEsc[idx].etiqueta << ": salto de mas de 4 dB entre velocities consecutivas — "
                << "es la marca del default 2.01 (escalon en 63), no del 2.04";
        }
        // 🔴 Y la referencia de FluidSynth es PLANA aca, y el motor NO la sigue. Esta
        // clausula existe para que grite si alguien "arregla" la discrepancia imitando a
        // FluidSynth en vez de al spec: la decision de este REQ es SF2 2.04.
        EXPECT_LT(rangoRef(kEsc[idx].t0), 0.5)
            << kEsc[idx].etiqueta << ": la referencia dejo de ser plana — ¿cambio la referencia?";
        EXPECT_GT(r - rangoRef(kEsc[idx].t0), 2.0)
            << kEsc[idx].etiqueta << ": el motor se acerco a FluidSynth, que NO implementa el "
            << "default #2. El oraculo aca es el spec 2.04, no la referencia";
    }
    // Las tres son el MISMO default 2.04 sin nada que lo pise: tienen que coincidir.
    EXPECT_NEAR(rangosNuestros[5], rangosNuestros[7], 0.5) << "#14 A y #14 C difieren";
    EXPECT_NEAR(rangosNuestros[5], rangosNuestros[8], 0.5) << "#14 A y #14 D difieren";

    double minR = rangosNuestros[0], maxR = rangosNuestros[0];
    for (double r : rangosNuestros) { minR = std::min(minR, r); maxR = std::max(maxR, r); }
    std::printf("  motor: rangos entre %.2f y %.2f dB -> dispersion %.2f dB sobre DIEZ sub-pruebas\n\n",
                minR, maxR, maxR - minR);
    // (3) Y lo que S1 dejo como trinquete, ahora al reves: las diez YA NO son una curva.
    EXPECT_GT(maxR - minR, 30.0)
        << "las diez escaleras volvieron a una sola curva (dispersion " << (maxR - minR)
        << " dB): el motor dejo de leer los moduladores";

    // Y el control positivo del lado de la referencia: #13 E tiene el modulador
    // BORRADO, asi que sus ocho notas tienen que sonar IGUAL. Si esto no fuera
    // plano, la referencia no estaria aplicando moduladores y no serviria de nada.
    const double refE = rango(ref.buffer.data(), ref.numFrames, 121.5, nullptr);
    EXPECT_LT(refE, 0.5) << "#13 E (modulador borrado) no es plano en la referencia: " << refE
                         << " dB — la referencia no aplica moduladores";
}

/**
 * REQ-039 S3 — LA MEDICION POR VENTANA, compartida por la corrida que imprime (3.1) y
 * el trinquete (3.2). Que las dos midan con EL MISMO codigo es lo que hace que el
 * numero que el trinquete afirma sea el que la corrida imprimio.
 *
 * Los observables (D4 del stage doc), todos en unidades RELATIVAS para que la ganancia
 * global de ~1,47 dB entre los dos renders no entre:
 *
 *   levelNotes  RMS de los primeros 0,40 s de cada nota, dB relativo a la primera nota
 *               de la ventana; max |motor - referencia| sobre las notas
 *   levelOn     la TRAYECTORIA del nivel por hop de 0,25 s, relativa al ataque de cada
 *               lado, SOLO hasta el note-off. Medido en 3.1: la release de tsf difiere
 *               de la de FluidSynth en TODAS las pruebas (mas rapida en #5/#7/#19/#20,
 *               mas lenta en #1); es un hallazgo con dueño propio y sin este corte
 *               taparia la tolerancia de cada fila con un desacuerdo que no es suyo
 *   levelTail   la misma trayectoria CON la cola (note-off + 1 s): se imprime, no se
 *               afirma
 *   pitchHop    cruces por cero por hop de 0,25 s, y la diferencia en cents entre los
 *               dos renders EN EL MISMO HOP, solo mientras la nota suena en los dos
 *               (a menos de 20 dB de su ataque). 🔴 La primera version media cents
 *               relativos al primer hop de la ventana y daba 240 c de artefacto con
 *               los Hz identicos: el primer hop caia en el ataque o en la cola. Y sin
 *               la compuerta de "suena" leia 125 Hz sobre la cola de release a -30 dB
 *   balance     L - R en dB por nota, con piso de -96 dB por canal; max |Δ|
 *   levelPP     pico a pico del nivel en hops de 50 ms sobre el sustain de la PRIMERA
 *               nota ([on + 0,5 s, off - 0,4 s]): la profundidad de un LFO de volumen.
 *               A 0,25 s el hop promedia el ciclo de 4 Hz y #5 daba 0,00 con ±6 dB
 *   pitchPP     idem en cents: la profundidad de un vibrato
 *   rangeOurs   el rango (max - min) del nivel por nota en el MOTOR: "todos iguales"
 *   stepOurs    el paso medio entre notas consecutivas en el motor (dB): "exactamente N"
 *   balTones    el balance por nota, por lado, para las relaciones de #22
 *
 * Las trayectorias corren hasta el note-off mas 1 s de release, sin pasar del proximo
 * note-on de CUALQUIER clase (la voz que anuncia "B" suena) y 20 ms ANTES de ese
 * note-on: el arnes cuantiza al bloque (512 muestras, 11,6 ms) y el motor arranca la
 * nota siguiente un bloque antes que la referencia. Medido: -19 dB "de cola" en #17 A
 * que era el ataque de la nota de B.
 */
namespace {

using wma_test::specmidi::Signal;
using wma_test::specmidi::Window;
using wma_test::specmidi::NoteOn;

struct Measured {
    double levelNotes = 0, levelOn = 0, levelTail = 0, pitchHop = 0, balance = 0;
    double levelPP_A = 0, levelPP_B = 0, pitchPP_A = 0, pitchPP_B = 0;
    double rangeOurs = 0, rangeRef = 0, stepOurs = 0, stepRef = 0;
    std::vector<double> balTonesA, balTonesB;
    int hops = 0, silentHops = 0;
};

struct HopPrinter {
    bool on = false;
    const char* label = "";
};

Measured measureWindow(const wma_test::specmidi::Rendered& ours, const Signal& A, const Signal& B,
                       const Window& w, HopPrinter hp = {}) {
    using namespace wma_test::specmidi;
    Measured m;
    const double dur = 0.40;
    double lvl0A = 0, lvl0B = 0, loA = 0, hiA = 0, loB = 0, hiB = 0, prevA = 0, prevB = 0;
    double stepAccA = 0, stepAccB = 0;
    int steps = 0;
    for (size_t i = 0; i < w.notes.size(); ++i) {
        const NoteOn& n = w.notes[i];
        double next = w.t1;
        for (const NoteOn& o : ours.noteOns)
            if (o.sec > n.sec) { next = std::min(next, o.sec); break; }
        const double lA = levelDb(A, n.sec, dur), lB = levelDb(B, n.sec, dur);
        const double bA = balanceDb(A, n.sec, dur), bB = balanceDb(B, n.sec, dur);
        if (i == 0) { lvl0A = lA; lvl0B = lB; }
        const double relA = lA - lvl0A, relB = lB - lvl0B;
        loA = std::min(loA, relA); hiA = std::max(hiA, relA);
        loB = std::min(loB, relB); hiB = std::max(hiB, relB);
        if (i > 0) { stepAccA += relA - prevA; stepAccB += relB - prevB; ++steps; }
        prevA = relA; prevB = relB;
        m.levelNotes = std::max(m.levelNotes, std::fabs(relA - relB));
        m.balance = std::max(m.balance, std::fabs(bA - bB));
        m.balTonesA.push_back(bA);
        m.balTonesB.push_back(bB);

        const double off = noteOffSec(ours, n, next);
        const double span = std::min(std::min(off + 1.0, next - 0.02) - n.sec, 8.0);
        const double sounding = off - n.sec;
        const double onA = levelDb(A, n.sec, 0.25), onB = levelDb(B, n.sec, 0.25);
        for (double t = n.sec; t + 0.25 <= n.sec + span; t += 0.25) {
            const double hA = pitchHz(A, t), hB = pitchHz(B, t);
            const double tA = levelDb(A, t, 0.25) - onA, tB = levelDb(B, t, 0.25) - onB;
            const bool inNote = t + 0.25 <= n.sec + sounding + 1e-9;
            if (levelDb(A, t, 0.25) > -60.0 || levelDb(B, t, 0.25) > -60.0) {
                const double d = std::fabs(std::max(tA, -60.0) - std::max(tB, -60.0));
                m.levelTail = std::max(m.levelTail, d);
                if (inNote) m.levelOn = std::max(m.levelOn, d);
            }
            if (hp.on) {
                std::printf("        %-8s nota %2zu hop @%7.2fs  %8.2f / %8.2f Hz  Δ %+8.2f c  nivel %+7.2f/%+7.2f\n",
                            hp.label, i, t, hA, hB, centsBetween(hA, hB), tA, tB);
            }
            const bool sounds = inNote && tA > -20.0 && tB > -20.0;
            if (hA <= 0 || hB <= 0 || !sounds) { ++m.silentHops; continue; }
            m.pitchHop = std::max(m.pitchHop, std::fabs(centsBetween(hA, hB)));
            ++m.hops;
        }
        if (i == 0 && sounding > 1.0) {
            const double p0 = n.sec + 0.5, p1 = off - 0.4;
            if (p1 > p0 + 0.3) {
                m.levelPP_A = levelPeakToPeakDb(A, p0, p1);
                m.levelPP_B = levelPeakToPeakDb(B, p0, p1);
                m.pitchPP_A = pitchPeakToPeakCents(A, p0, p1);
                m.pitchPP_B = pitchPeakToPeakCents(B, p0, p1);
            }
        }
    }
    m.rangeOurs = hiA - loA;
    m.rangeRef = hiB - loB;
    m.stepOurs = steps ? stepAccA / steps : 0.0;
    m.stepRef = steps ? stepAccB / steps : 0.0;
    return m;
}

std::vector<int> csvEnv(const char* name) {
    std::vector<int> out;
    if (const char* d = std::getenv(name)) {
        std::string s(d);
        size_t p = 0;
        while (p < s.size()) {
            size_t q = s.find(',', p);
            if (q == std::string::npos) q = s.size();
            if (q > p) out.push_back(std::atoi(s.substr(p, q - p).c_str()));
            p = q + 1;
        }
    }
    return out;
}
bool contains(const std::vector<int>& v, int x) {
    for (int d : v) if (d == x) return true;
    return false;
}

struct Loaded {
    wma_test::specmidi::Rendered ours;
    wav::WavData ref;
    std::vector<Window> windows;
    Signal A, B;
};

/// Carga los dos renders y deriva las ventanas, o devuelve `false` (SKIP en el llamador).
bool loadBoth(Loaded& L, std::string* why) {
    if (!have()) { *why = "sin material del spec-test — corre scripts/fetch-spec-test.sh"; return false; }
    const std::string refPath = dir() + "/reference-fluidsynth-2.6.0.wav";
    struct stat st {};
    if (stat(refPath.c_str(), &st) != 0) { *why = "sin referencia — corre scripts/render-spec-reference.sh"; return false; }
    L.ours = wma_test::specmidi::render(sf2(), mid());
    if (!L.ours.valid) { *why = "el arnes no pudo renderizar"; return false; }
    L.ref = wav::readWav(refPath.c_str());
    if (L.ref.numFrames <= 0 || L.ref.sampleRate != L.ours.sampleRate) { *why = "referencia ilegible o a otro rate"; return false; }
    L.A = wma_test::specmidi::view(L.ours);
    L.B = Signal{L.ref.buffer.data(), L.ref.numFrames, L.ref.sampleRate};
    L.windows = wma_test::specmidi::deriveWindows(L.ours);
    return true;
}

const Window* findWindow(const std::vector<Window>& ws, int test, int sub) {
    for (const Window& w : ws) if (w.test == test && w.sub == sub) return &w;
    return nullptr;
}

}  // namespace

/**
 * REQ-039 S3, tarea 3.1 — LA CORRIDA QUE IMPRIME LAS 22. No afirma tolerancias: las
 * tolerancias del trinquete (3.2) salen de ESTA salida, y ninguna se escribe antes de
 * ver el numero (regla de S1). Lo unico que se afirma es que el instrumento derivo lo
 * que se cree: 22 pruebas anunciadas por la tecla `20 + N`, 27 sub-pruebas anunciadas
 * por 51..56 (#5 A-B, #13 A-E, #14 A-E, #17 A-C, #18 A-C, #20 A-C, #22 A-F), y que
 * ninguna ventana quedo sin carga util.
 *
 * `WMA_SPEC_HOPS=2,6` imprime ademas la trayectoria hop por hop de esas pruebas.
 */
TEST(SfSpecConformance, TheTwentyTwoMeasured) {
    Loaded L;
    std::string why;
    if (!loadBoth(L, &why)) GTEST_SKIP() << why;

    int tests = 0, subs = 0, empty = 0;
    for (const Window& w : L.windows) {
        if (w.sub == 0) ++tests; else ++subs;
        if (w.notes.empty()) ++empty;
    }
    // Control de que se derivo lo que se cree (medido el 2026-09-11 con un parser
    // independiente de tml sobre el .mid; el sha256 lo protege).
    EXPECT_EQ(tests, 22) << "el canal 0 anuncia 22 pruebas con las teclas 21..42";
    EXPECT_EQ(subs, 27) << "#5 A-B, #13 A-E, #14 A-E, #17 A-C, #18 A-C, #20 A-C, #22 A-F";
    EXPECT_EQ(empty, 0) << "una ventana sin carga util no mide nada";
    for (int n = 1; n <= 22; ++n) EXPECT_NE(findWindow(L.windows, n, 0), nullptr) << "falta la prueba #" << n;

    const std::vector<int> hops = csvEnv("WMA_SPEC_HOPS");
    std::printf("\n  [REQ-039 S3] LAS 22 MEDIDAS — max |motor - FluidSynth| por ventana, relativo a su primera nota\n");
    std::printf("  %-8s %5s %9s %9s %9s %9s %9s %13s %13s\n", "ventana", "notas", "nivel", "rango(m)",
                "nivel-on", "nivel-t", "pitch c", "nivPP m/ref", "pitPP m/ref");
    for (const Window& w : L.windows) {
        const std::string label = w.label();
        const Measured m = measureWindow(L.ours, L.A, L.B, w, {contains(hops, w.test), label.c_str()});
        std::printf("  %-8s %5zu %9.2f %9.2f %9.2f %9.2f %9.2f %6.2f/%6.2f %6.1f/%6.1f  bal %.2f\n",
                    label.c_str(), w.notes.size(), m.levelNotes, m.rangeOurs, m.levelOn, m.levelTail,
                    m.pitchHop, m.levelPP_A, m.levelPP_B, m.pitchPP_A, m.pitchPP_B, m.balance);
    }
    std::printf("\n");
}

/**
 * AC-039.9 (REQ-039 S3, tarea 3.2) — EL TRINQUETE DE LAS 22, cada una contra SU oraculo.
 *
 * 🔴 POR QUE NO ES "DISTANCIA A FLUIDSYNTH" A SECAS. S2 midio que en #14 A/C/D la
 * distancia a FluidSynth EMPEORA a proposito (0,02 -> 5,1 dB): el motor es SF2 2.04 y
 * FluidSynth eligio no implementar el default #2 (lo dice el README del spec-test por su
 * nombre). Y 3.1 midio que en #1/#2/#3/#4/#8 el motor NO es conforme aunque FluidSynth
 * si lo sea — y S3 no lo arregla. Asi que cada fila declara su clase:
 *
 *   F  FluidSynth es conforme ahi: se afirma la distancia, con la tolerancia que el
 *      observable permite (al decimo donde es un nivel por nota).
 *   S  el spec da el numero o la forma: se afirma contra el spec, y la referencia queda
 *      como CONTROL de que no esta rota.
 *   R  sin oraculo aplicable hoy, o el motor no es conforme y el arreglo tiene OTRO
 *      dueño: se fija el valor de HOY, BIDIRECCIONAL. Si el dueño lo arregla, la fila se
 *      pone roja y se re-declara en ese PR — su diff es la revision. Es "tolerar sin
 *      tapar": #9 a 2,81 dB y #10 a 46 dB estan escritos, no metidos en un margen.
 *
 * 🔴 Y un trinquete sobre la COINCIDENCIA no ve el desplazamiento comun: por eso las
 * R afirman el VALOR (|medido - hoy| <= tol) y no "no empeora".
 *
 * Los valores de HOY y las tolerancias salen de `TheTwentyTwoMeasured` del 2026-09-11
 * (stage doc S3, Notas 3.1), medidos con este mismo codigo. Las escaleras de #13/#14
 * tienen su contrato fino en `TheVelocityLaddersFollowWhatTheFileDeclares`; aca entran
 * como filas para que la tabla este completa, no para duplicar aquel.
 */
namespace {

enum class Obs { LevelNotes, LevelOn, PitchHop, Balance, LevelPP, PitchPP, RangeOurs, StepOurs, BalRelation };
enum class Cls { F, S, R };

struct Row {
    int test, sub;          // sub 0 = la prueba entera
    Obs obs;
    Cls cls;
    double tol;             // F: |Δ| <= tol · R: |Δ - today| <= tol · S: segun obs
    double today;           // R: el valor medido el 2026-09-11 · S: el numero del spec
    const char* why;        // la razon, o el dueño
};

const char* obsName(Obs o) {
    switch (o) {
        case Obs::LevelNotes: return "nivel/nota";
        case Obs::LevelOn: return "nivel-on";
        case Obs::PitchHop: return "pitch hop";
        case Obs::Balance: return "balance";
        case Obs::LevelPP: return "nivel p-p";
        case Obs::PitchPP: return "pitch p-p";
        case Obs::RangeOurs: return "rango(m)";
        case Obs::StepOurs: return "paso(m)";
        case Obs::BalRelation: return "bal B≡A";
    }
    return "?";
}
const char* clsName(Cls c) { return c == Cls::F ? "F" : c == Cls::S ? "S" : "R"; }

// El valor que la fila compara, segun su observable.
double valueOf(const Measured& m, Obs o) {
    switch (o) {
        case Obs::LevelNotes: return m.levelNotes;
        case Obs::LevelOn: return m.levelOn;
        case Obs::PitchHop: return m.pitchHop;
        case Obs::Balance: return m.balance;
        case Obs::LevelPP: return std::fabs(m.levelPP_A - m.levelPP_B);
        case Obs::PitchPP: return std::fabs(m.pitchPP_A - m.pitchPP_B);
        case Obs::RangeOurs: return m.rangeOurs;
        case Obs::StepOurs: return m.stepOurs;
        case Obs::BalRelation: return 0.0;  // se calcula aparte: necesita DOS ventanas
    }
    return 0.0;
}

}  // namespace

TEST(SfSpecConformance, TheTwentyTwoAgainstTheirOracles) {
    Loaded L;
    std::string why;
    if (!loadBoth(L, &why)) GTEST_SKIP() << why;

    // ---- LA TABLA. Un cambio aca es una decision, y su diff es la revision. ----------
    const Row kRows[] = {
        // #1..#4, #8: F por el README, NO conformes medidos en 3.1 -> R con dueño
        {1, 0, Obs::LevelOn, Cls::R, 1.0, 4.94,
         "envolvente de volumen: ataque/decay difieren de FluidSynth; dueño: MINI envolventes de tsf"},
        {2, 0, Obs::PitchHop, Cls::R, 30.0, 600.0,
         "mod env -> pitch: sube igual (+1200 c) pero tsf decae ~1 s antes; dueño: MINI envolventes"},
        {3, 0, Obs::LevelOn, Cls::R, 1.0, 9.63,
         "keynum -> decay: pendiente 20 % mas lenta que FluidSynth; dueño: MINI envolventes"},
        {3, 0, Obs::PitchHop, Cls::R, 3.0, 28.44,
         "tsf escala `tune` por scaleTuning (tsf.h:1213): +23 c; dueño: MINI afinacion fina"},
        {4, 0, Obs::LevelOn, Cls::R, 1.0, 3.21, "keynum -> hold: idem #3; dueño: MINI envolventes"},
        {4, 0, Obs::PitchHop, Cls::R, 3.0, 38.49, "tune x scaleTuning, idem #3; dueño: MINI afinacion fina"},
        // #5: A es velocity 127 y FluidSynth SI limita el boost a 0 dB (7,0 p-p contra 8,8 en B)
        // aunque su README diga que no; tsf no limita (9,0 en las dos). B es la conforme.
        {5, 1, Obs::LevelPP, Cls::R, 0.5, 2.03,
         "LFO -> volumen con boost sobre 0 dB: FluidSynth lo recorta, tsf no; dueño: MINI envolventes/LFO"},
        {5, 2, Obs::LevelPP, Cls::F, 0.5, 0.0, "profundidad del LFO de volumen, ±6 dB, p-p en 50 ms"},
        {6, 0, Obs::PitchPP, Cls::F, 30.0, 0.0,
         "profundidad del vibrato: ~900 c p-p; 30 c es el 3 % que el estimador resuelve sobre un barrido"},
        {7, 0, Obs::PitchPP, Cls::R, 5.0, 74.54,
         "CC1 -> vibrato (default #4, fuente de canal): tsf ignora CC1; dueño: el REQ de superficie CC"},
        {8, 0, Obs::PitchHop, Cls::R, 3.0, 23.0, "scaleTune/rootKey: tune x scaleTuning; dueño: MINI afinacion fina"},
        {9, 0, Obs::LevelNotes, Cls::R, 0.5, 2.81, "corte del low-pass: dueño REQ-041"},
        {10, 0, Obs::LevelNotes, Cls::R, 2.0, 46.22, "resonancia: tsf esta a 46 dB en la Q mas alta; dueño REQ-041"},
        // #11: el spec da el numero (2,00 dB por paso) y el motor da 0,50: hallazgo, con dueño.
        {11, 0, Obs::StepOurs, Cls::R, 0.05, -0.50,
         "initialAttenuation entra a 0,1 dB/dB (tsf.h:652); el spec-quirk es 0,4 = -2,00 dB/paso; dueño: MINI atenuacion"},
        {12, 0, Obs::RangeOurs, Cls::S, 0.5, 0.0, "atenuacion negativa: \"todos al mismo volumen\""},
        {13, 1, Obs::LevelNotes, Cls::F, 0.25, 0.0, "velocity -> atenuacion, default 96 dB concava"},
        {13, 2, Obs::LevelNotes, Cls::F, 0.25, 0.0, "144 dB concava"},
        {13, 3, Obs::LevelNotes, Cls::F, 0.25, 0.0, "48 dB concava"},
        {13, 4, Obs::LevelNotes, Cls::F, 1.5, 0.0, "96 dB lineal: 84 dB de rango contra el piso del render (2.2)"},
        {13, 5, Obs::LevelNotes, Cls::F, 0.25, 0.0, "modulador borrado"},
        {14, 2, Obs::LevelNotes, Cls::F, 0.4, 0.0, "-7200 cents: 0,26 por nota donde el rango da 0,02"},
        {14, 5, Obs::LevelNotes, Cls::F, 0.25, 0.0, "borrado 2.04: plano en los dos"},
        // #14 A/C/D: S, afirmado en detalle en las escaleras. Aca, el control de que la
        // referencia sigue plana y el motor sigue filtrando.
        {14, 1, Obs::RangeOurs, Cls::S, 1.0, 0.0, "default #2 (2.04): hay filtrado; la referencia es plana"},
        {14, 3, Obs::RangeOurs, Cls::S, 1.0, 0.0, "SoundFont 2.0: el default otra vez"},
        {14, 4, Obs::RangeOurs, Cls::S, 1.0, 0.0, "borrado 2.01: no anula al 2.04"},
        {15, 0, Obs::LevelOn, Cls::R, 0.5, 0.78, "CC1 -> corte del filtro: fuente de canal; dueño: el REQ de superficie CC"},
        {16, 0, Obs::LevelNotes, Cls::F, 0.6, 0.0, "sample offset: los dos tocan \"supported\"; sample hablado, 0,43 hoy"},
        {17, 1, Obs::LevelNotes, Cls::R, 0.25, 0.0, "reverb: la referencia es SECA (-R 0), iguales por construccion; dueño REQ-040"},
        {17, 2, Obs::LevelNotes, Cls::R, 0.25, 0.0, "reverb CC91; dueño REQ-040"},
        {17, 3, Obs::LevelNotes, Cls::R, 0.25, 0.0, "reverb ambos; dueño REQ-040"},
        {18, 1, Obs::LevelNotes, Cls::R, 0.25, 0.0, "chorus: idem; dueño REQ-040"},
        {18, 2, Obs::LevelNotes, Cls::R, 0.25, 0.0, "chorus CC93; dueño REQ-040"},
        {18, 3, Obs::LevelNotes, Cls::R, 0.25, 0.0, "chorus ambos; dueño REQ-040"},
        {19, 0, Obs::LevelNotes, Cls::R, 0.25, 0.0, "interpolacion: \"por oido\", este observable no la ve; dueño REQ-041"},
        {20, 1, Obs::PitchHop, Cls::F, 6.0, 0.0, "pitch bend ±2 st: el bend hop a hop; 4,8 c hoy (sample senoide, medido)"},
        {20, 2, Obs::PitchHop, Cls::R, 10.0, 200.5,
         "modulador de rueda BORRADO: exige anular el default #10 por voz en el camino de la rueda, que produccion no tiene; dueño: REQ de superficie CC/rueda"},
        {20, 3, Obs::PitchHop, Cls::R, 10.0, 399.0, "rueda INVERTIDA: idem; dueño: REQ de superficie CC/rueda"},
        {21, 0, Obs::LevelNotes, Cls::F, 1.2, 0.0, "clase exclusiva: los dos cortan; 0,91 hoy por la cuantizacion del corte"},
        {22, 1, Obs::Balance, Cls::R, 2.0, 36.82, "ley de paneo: sqrt en tsf (8,45 dB a -37,5 %) contra sin/cos (14,03); sin dueño aun"},
        {22, 3, Obs::Balance, Cls::R, 0.5, 0.94, "la misma ley sobre el sample estereo"},
        {22, 5, Obs::Balance, Cls::R, 2.0, 31.72, "ley de paneo, -100..100 %"},
        {22, 6, Obs::Balance, Cls::R, 2.0, 48.73, "sobreescribir el default #6 por voz en CC10; dueño: REQ de superficie CC/rueda"},
        // #22 B/D: la RELACION CC10 <-> pan interno, en el motor solo. F sin codigo (3.1).
        {22, 2, Obs::BalRelation, Cls::F, 0.5, 0.0, "CC10 da lo mismo que el pan interno (B ≡ A), tono a tono"},
        {22, 4, Obs::BalRelation, Cls::F, 0.5, 0.0, "idem estereo (D ≡ C)"},
    };

    std::printf("\n  [REQ-039 S3] EL TRINQUETE DE LAS 22 — cada fila contra su oraculo\n");
    std::printf("  %-8s %-11s %3s %9s %9s %6s  %s\n", "ventana", "observable", "cls", "medido", "hoy/spec", "tol", "razon");
    int f = 0, s = 0, r = 0;
    for (const Row& row : kRows) {
        const Window* w = findWindow(L.windows, row.test, row.sub);
        ASSERT_NE(w, nullptr) << "falta la ventana #" << row.test << "/" << row.sub;
        const std::string label = w->label();
        const Measured m = measureWindow(L.ours, L.A, L.B, *w);
        double value = valueOf(m, row.obs);

        if (row.obs == Obs::BalRelation) {
            // B contra A (o D contra C), tono a tono, en el MOTOR. La ultima nota queda
            // afuera: CC10 = 127 es 49,22 % y no 50 % (README), asi que difiere a proposito.
            const Window* base = findWindow(L.windows, row.test, row.sub - 1);
            ASSERT_NE(base, nullptr);
            const Measured mb = measureWindow(L.ours, L.A, L.B, *base);
            ASSERT_EQ(m.balTonesA.size(), mb.balTonesA.size());
            for (size_t i = 0; i + 1 < m.balTonesA.size(); ++i)
                value = std::max(value, std::fabs(m.balTonesA[i] - mb.balTonesA[i]));
        }

        std::printf("  %-8s %-11s %3s %9.2f %9.2f %6.2f  %s\n", label.c_str(), obsName(row.obs),
                    clsName(row.cls), value, row.today, row.tol, row.why);

        switch (row.cls) {
            case Cls::F:
                ++f;
                EXPECT_LE(value, row.tol) << label << " [" << obsName(row.obs) << "] F: el motor se aparto de "
                                          << "FluidSynth, que aqui SI es conforme — " << row.why;
                break;
            case Cls::S:
                ++s;
                if (row.obs == Obs::RangeOurs && row.test == 12) {
                    EXPECT_LT(value, row.tol) << label << " S: \"todos al mismo volumen\" y el motor tiene "
                                              << value << " dB de rango";
                    EXPECT_LT(m.rangeRef, row.tol) << label << " control: la referencia dejo de ser plana";
                } else if (row.obs == Obs::RangeOurs && row.test == 14) {
                    EXPECT_GT(value, row.tol) << label << " S: el default #2 (SF2 2.04 §8.4.2) no filtra";
                    EXPECT_LT(m.rangeRef, 0.5) << label << " control: la referencia (que NO implementa el "
                                               << "default #2) dejo de ser plana — ¿cambio la referencia?";
                }
                break;
            case Cls::R:
                ++r;
                EXPECT_NEAR(value, row.today, row.tol)
                    << label << " [" << obsName(row.obs) << "] R: el valor de hoy CAMBIO (" << row.today
                    << " -> " << value << "). Si mejoro, re-declaralo en el PR que lo arreglo; si empeoro, "
                    << "es una regresion. Dueño: " << row.why;
                break;
        }
    }
    std::printf("  filas: %d F · %d S · %d R = %d\n\n", f, s, r, f + s + r);

    // El control del spec en #11: la referencia da los 2,00 dB por paso que el README
    // exige, asi que el 0,50 del motor es del motor y no del instrumento.
    const Window* w11 = findWindow(L.windows, 11, 0);
    ASSERT_NE(w11, nullptr);
    const Measured m11 = measureWindow(L.ours, L.A, L.B, *w11);
    EXPECT_NEAR(m11.stepRef, -2.00, 0.1) << "#11 control: la referencia no da los 2 dB por paso del spec";
}
