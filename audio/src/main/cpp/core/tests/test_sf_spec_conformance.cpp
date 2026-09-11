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
 * AC-039.1 — LA TABLA DE HOY: el motor contra la referencia de FluidSynth 2.6.0,
 * prueba por prueba. Es la linea de base contra la que S2 tiene que mejorar.
 *
 * SEGMENTACION — que se DERIVA y que se DECLARA
 * ---------------------------------------------
 * 🔴 El `.mid` **no tiene marcadores** y sus huecos no dan 22 segmentos con ningun
 * umbral (medido: 78 / 43 / 16 / 14 a 1,0 / 1,5 / 2,0 / 3,0 s), asi que el corte no
 * se puede derivar entero. Lo que SI se deriva, y es la mayor parte:
 *
 *   · el archivo tiene 7 pistas y 7 canales, **1 a 1**, y SEIS de ellas llevan el
 *     NOMBRE DE SU PRUEBA adentro del propio archivo (`keynum-to-decay`,
 *     `keynum-to-hold`, `scaleTune`, `initial-FC`, `negative attenuation`,
 *     `panning`). Esos nombres no los escribimos nosotros: salen del artefacto.
 *   · la ventana de cada una son sus propios note-on, re-medidos en cada corrida.
 *
 * Lo unico DECLARADO es el numero de prueba que le corresponde a cada nombre, y lo
 * protege el sha256 del `.mid`: si upstream lo cambia, `fetch-spec-test.sh` se pone
 * rojo ANTES de que este mapeo pueda mentir.
 *
 * EL CRITERIO: NIVELES RELATIVOS, NO ABSOLUTOS
 * --------------------------------------------
 * 🔴 Medido: entre los dos renders hay un sesgo GLOBAL de ~1,4 dB que no es de
 * conformidad sino de convencion de ganancia (tsf en 0 dB contra `fluidsynth -g 1`).
 * Un criterio sobre niveles ABSOLUTOS mediria esa convencion en las 22 pruebas.
 * Por eso se descuenta la mediana de los desvios y se reporta el residuo: lo que
 * queda es lo que NO se explica por la ganancia global. Y no es una comodidad —
 * es lo correcto: las pruebas #13 y #14, que son las que este REQ existe para
 * arreglar, comparan como cambia el nivel/timbre CON LA VELOCITY, o sea una
 * relacion, y una relacion es inmune a la ganancia global.
 */
TEST(SfSpecConformance, TheTableOfTodayAgainstFluidSynth) {
    if (!have()) GTEST_SKIP() << "sin material del spec-test — corre scripts/fetch-spec-test.sh";
    const std::string refPath = dir() + "/reference-fluidsynth-2.6.0.wav";
    struct stat st {};
    if (stat(refPath.c_str(), &st) != 0) {
        GTEST_SKIP() << "sin referencia — corre scripts/render-spec-reference.sh";
    }

    const auto ours = wma_test::specmidi::render(sf2(), mid());
    ASSERT_TRUE(ours.valid);
    const wav::WavData ref = wav::readWav(refPath.c_str());
    ASSERT_GT(ref.numFrames, 0) << "no se pudo leer la referencia";
    ASSERT_EQ(ref.sampleRate, ours.sampleRate) << "los dos renders tienen que estar al mismo rate";

    // ALINEACION: el primer ataque de cada uno. Si no alinean, cualquier criterio
    // por tramos mide desalineacion. Medido el 2026-09-10: 58-63 muestras (1,3 ms).
    auto onset = [](const float* s, int frames, double thr) {
        for (int i = 0; i < frames; ++i)
            if (std::fabs(0.5 * (s[2 * i] + s[2 * i + 1])) > thr) return i;
        return -1;
    };
    const int oA = onset(ours.stereo.data(), ours.frames, 0.01);
    const int oB = onset(ref.buffer.data(), ref.numFrames, 0.01);
    ASSERT_GE(oA, 0);
    ASSERT_GE(oB, 0);
    std::printf("\n  [REQ-039] alineacion: nuestro %d, referencia %d, delta %+d muestras (%.2f ms)\n",
                oA, oB, oA - oB, 1000.0 * (oA - oB) / ours.sampleRate);
    EXPECT_LT(std::abs(oA - oB), 500)
        << "los dos renders no alinean: la comparacion por tramos mediria el desfase";

    struct Ventana { const char* nombre; int prueba; double a, b; const char* nota; };
    // Las seis con nombre en el archivo, mas dos tramos del canal 0 como control.
    const Ventana kVentanas[] = {
        {"volume envelope (ch0)",   1, 0.0,   8.0,  "control, canal 0"},
        {"keynum-to-decay",         3, 17.5,  24.0, ""},
        {"keynum-to-hold",          4, 31.5,  36.5, ""},
        {"scaleTune",               8, 61.0,  63.0, ""},
        {"initial-FC",              9, 65.5,  83.0, "1 nota del ch0 adentro (76,5 s)"},
        {"negative attenuation",   12, 91.0,  94.0, ""},
        {"panning",                22, 266.0, 300.0, "5 notas del ch0 adentro"},
    };
    auto rmsRef = [&](double t0, double t1) {
        long long a = static_cast<long long>(t0 * ref.sampleRate);
        long long b = static_cast<long long>(t1 * ref.sampleRate);
        if (a < 0) a = 0;
        if (b > ref.numFrames) b = ref.numFrames;
        if (b <= a) return -200.0;
        double acc = 0.0;
        for (long long i = a; i < b; ++i) {
            const double m = 0.5 * (static_cast<double>(ref.buffer[static_cast<size_t>(i) * 2]) +
                                    static_cast<double>(ref.buffer[static_cast<size_t>(i) * 2 + 1]));
            acc += m * m;
        }
        const double r = std::sqrt(acc / static_cast<double>(b - a));
        return r > 1e-10 ? 20.0 * std::log10(r) : -200.0;
    };

    std::vector<double> deltas;
    struct Fila { std::string nombre; int prueba; double nuestro, referencia, delta; const char* nota; };
    std::vector<Fila> filas;
    for (const Ventana& v : kVentanas) {
        const double a = wma_test::specmidi::rmsDb(ours, v.a, v.b);
        const double b = rmsRef(v.a, v.b);
        filas.push_back({v.nombre, v.prueba, a, b, a - b, v.nota});
        deltas.push_back(a - b);
    }
    std::vector<double> orden = deltas;
    std::sort(orden.begin(), orden.end());
    const double offset = orden[orden.size() / 2];   // mediana: el sesgo global

    std::printf("  [REQ-039] LA TABLA DE HOY (RMS por tramo; residuo = delta - sesgo global %.2f dB)\n",
                offset);
    std::printf("  %-24s %5s %10s %10s %8s %9s  %s\n", "prueba", "#", "nuestro", "fluidsyn",
                "delta", "residuo", "nota");
    double peor = 0.0;
    std::string peorNombre;
    for (const Fila& f : filas) {
        const double residuo = f.delta - offset;
        std::printf("  %-24s %5d %+9.2f %+10.2f %+8.2f %+9.2f  %s\n", f.nombre.c_str(), f.prueba,
                    f.nuestro, f.referencia, f.delta, residuo, f.nota);
        if (std::fabs(residuo) > std::fabs(peor)) { peor = residuo; peorNombre = f.nombre; }
    }
    std::printf("  sesgo global %.2f dB (mediana) · PEOR RESIDUO %+.2f dB en '%s'\n\n", offset, peor,
                peorNombre.c_str());

    // S1 MIDE, no exige: el trinquete lo pone S2 con este numero como piso. Lo unico
    // que se afirma es que la tabla se PUDO construir y que separa: si todos los
    // residuos fueran cero, el instrumento no distinguiria nada.
    EXPECT_GT(std::fabs(peor), 1.0)
        << "todos los tramos coinciden con la referencia dentro de 1 dB: o el motor ya es "
           "conforme, o la tabla no esta midiendo lo que se cree";
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
 * REQ-039 S3, tarea 3.1 — LA CORRIDA QUE IMPRIME LAS 22. No afirma tolerancias: las
 * tolerancias del trinquete (3.2) salen de ESTA salida, y ninguna se escribe antes de
 * ver el numero (regla de S1). Lo unico que se afirma es que el instrumento derivo lo
 * que se cree: 22 pruebas anunciadas por la tecla `20 + N`, 27 sub-pruebas anunciadas
 * por 51..56 (#5 A-B, #13 A-E, #14 A-E, #17 A-C, #18 A-C, #20 A-C, #22 A-F), y que
 * ninguna ventana quedo sin carga util.
 *
 * Por ventana se imprimen los TRES observables (D4 del stage doc), cada uno como el
 * maximo |motor - referencia| sobre las notas, en unidades RELATIVAS a la primera
 * nota de la ventana (nivel y pitch) — asi la ganancia global de ~1,47 dB no entra:
 *
 *   nivel    RMS de los primeros 0,40 s de cada nota, dB relativo a la primera nota
 *   pitch    cruces por cero, cada 0,25 s a lo largo de la nota, y la diferencia en
 *            cents entre los dos renders EN EL MISMO HOP (asi #2/#6/#20 se ven como
 *            trayectoria). 🔴 La primera version media cents relativos al primer hop
 *            de la ventana, y daba 240 c de artefacto donde los Hz eran identicos
 *            (370,05/370,05): el primer hop caia en el ataque o en la cola de la nota
 *            anterior y cada lado lo estimaba distinto. Comparar hop contra hop no
 *            necesita un ancla.
 *   nivel-t  la TRAYECTORIA del nivel: RMS por hop de 0,25 s a lo largo de la nota,
 *            relativo al ataque de cada lado, y el maximo |motor - referencia|. Sin
 *            esto #1/#2/#3/#4/#21 (envolventes, timing) se median con UN RMS de
 *            0,40 s, que es ciego a la forma. Medido en la primera corrida: #1 daba
 *            0,00 con una sola nota
 *   nivel-on la misma trayectoria pero SOLO hasta el note-off. Medido en 3.1: la
 *            release de tsf difiere de la de FluidSynth en TODAS las pruebas (mas
 *            rapida en #5/#7/#19/#20: -28 contra -17 dB en el primer hop despues del
 *            apagado; mas lenta en #1: -14,5 contra -22,3). Es un hallazgo con dueño
 *            propio (envolventes de tsf contra el spec), y sin esta columna taparia
 *            la tolerancia de cada fila F con un desacuerdo que no es de esa fila.
 *   balance  L - R en dB, absoluto (un pan es un valor, no una relacion)
 *
 * 🔴 El pitch se mide SOLO en hops donde la nota SUENA en los dos lados (a menos de
 * 20 dB de su ataque). Sin la compuerta, el estimador leia 125 Hz sobre la cola de
 * release a -30 dB y las filas de nivel mostraban 240 c de "desacuerdo" con los Hz
 * identicos en todo el tramo sonoro. Y esa cola —el motor a -30 dB donde FluidSynth
 * ya esta a -70— la ve la trayectoria de nivel, que es donde corresponde.
 *
 * `WMA_SPEC_DETAIL=11,20,22` imprime ademas las filas por nota de esas pruebas.
 */
TEST(SfSpecConformance, TheTwentyTwoMeasured) {
    if (!have()) GTEST_SKIP() << "sin material del spec-test — corre scripts/fetch-spec-test.sh";
    const std::string refPath = dir() + "/reference-fluidsynth-2.6.0.wav";
    struct stat st {};
    if (stat(refPath.c_str(), &st) != 0) {
        GTEST_SKIP() << "sin referencia — corre scripts/render-spec-reference.sh";
    }
    using namespace wma_test::specmidi;
    const auto ours = render(sf2(), mid());
    ASSERT_TRUE(ours.valid);
    const wav::WavData ref = wav::readWav(refPath.c_str());
    ASSERT_GT(ref.numFrames, 0);
    ASSERT_EQ(ref.sampleRate, ours.sampleRate);
    const Signal A = view(ours);
    const Signal B{ref.buffer.data(), ref.numFrames, ref.sampleRate};

    const std::vector<Window> windows = deriveWindows(ours);
    int tests = 0, subs = 0, empty = 0;
    for (const Window& w : windows) {
        if (w.sub == 0) ++tests; else ++subs;
        if (w.notes.empty()) ++empty;
    }
    // Control de que se derivo lo que se cree (medido el 2026-09-11 con un parser
    // independiente de tml sobre el .mid; el sha256 lo protege).
    EXPECT_EQ(tests, 22) << "el canal 0 anuncia 22 pruebas con las teclas 21..42";
    EXPECT_EQ(subs, 27) << "#5 A-B, #13 A-E, #14 A-E, #17 A-C, #18 A-C, #20 A-C, #22 A-F";
    EXPECT_EQ(empty, 0) << "una ventana sin carga util no mide nada";
    for (int n = 1; n <= 22; ++n) {
        bool found = false;
        for (const Window& w : windows) found = found || (w.sub == 0 && w.test == n);
        EXPECT_TRUE(found) << "falta la prueba #" << n;
    }

    // Que pruebas van con detalle por nota.
    std::vector<int> detail;
    if (const char* d = std::getenv("WMA_SPEC_DETAIL")) {
        std::string s(d);
        size_t p = 0;
        while (p < s.size()) {
            size_t q = s.find(',', p);
            if (q == std::string::npos) q = s.size();
            if (q > p) detail.push_back(std::atoi(s.substr(p, q - p).c_str()));
            p = q + 1;
        }
    }
    auto wantsDetail = [&](int t) {
        for (int d : detail) if (d == t) return true;
        return false;
    };
    // `WMA_SPEC_HOPS=2,6` imprime la trayectoria de pitch hop por hop de esas pruebas.
    std::vector<int> hopsOf;
    if (const char* d = std::getenv("WMA_SPEC_HOPS")) {
        std::string s(d);
        size_t p = 0;
        while (p < s.size()) {
            size_t q = s.find(',', p);
            if (q == std::string::npos) q = s.size();
            if (q > p) hopsOf.push_back(std::atoi(s.substr(p, q - p).c_str()));
            p = q + 1;
        }
    }
    auto wantsHops = [&](int t) {
        for (int d : hopsOf) if (d == t) return true;
        return false;
    };

    std::printf("\n  [REQ-039 S3] LAS 22 MEDIDAS — max |motor - FluidSynth| por ventana, relativo a su primera nota\n");
    std::printf("  %-8s %5s %10s %10s %10s %10s %10s %10s  %s\n", "ventana", "notas", "nivel dB",
                "rango(m)", "nivel-t", "nivel-on", "pitch c", "balance", "nota");
    for (const Window& w : windows) {
        const double dur = 0.40;
        double lvl0A = 0, lvl0B = 0;
        double maxLvl = 0, maxPitch = 0, maxBal = 0, loA = 0, hiA = 0, maxTraj = 0, maxTrajOn = 0;
        int hops = 0, silentHops = 0;
        std::string note;
        for (size_t i = 0; i < w.notes.size(); ++i) {
            const NoteOn& n = w.notes[i];
            // El limite es el proximo note-on de CUALQUIER clase, anuncios incluidos:
            // la voz que dice "B" suena, y en la primera corrida entraba a -19 dB en la
            // trayectoria de la ultima nota de la sub-prueba A.
            double next = w.t1;
            for (const NoteOn& m : ours.noteOns)
                if (m.sec > n.sec) { next = std::min(next, m.sec); break; }
            const double lA = levelDb(A, n.sec, dur), lB = levelDb(B, n.sec, dur);
            const double bA = balanceDb(A, n.sec, dur), bB = balanceDb(B, n.sec, dur);
            if (i == 0) { lvl0A = lA; lvl0B = lB; }
            const double relA = lA - lvl0A, relB = lB - lvl0B;
            if (relA < loA) loA = relA;
            if (relA > hiA) hiA = relA;
            maxLvl = std::max(maxLvl, std::fabs(relA - relB));
            maxBal = std::max(maxBal, std::fabs(bA - bB));
            // Las trayectorias corren hasta el note-off MAS 1 s de release (la
            // envolvente de #1 tiene una release de 1 s entera), sin pasar de la
            // nota siguiente. El pitch, solo hasta el note-off: la cola no es la nota.
            // ... y 20 ms ANTES de ese note-on: el arnes cuantiza los eventos al bloque
            // (512 muestras, 11,6 ms) y el motor arranca la nota siguiente un bloque
            // antes que la referencia. Un hop que termina justo en el note-on lleva
            // esos 11 ms de la nota siguiente en un lado y no en el otro — medido:
            // -19 dB "de cola" en #17 A que era el ataque de la nota de B.
            const double off = noteOffSec(ours, n, next);
            const double span = std::min(std::min(off + 1.0, next - 0.02) - n.sec, 8.0);
            const double sounding = off - n.sec;
            double notePitchMax = 0;
            const double onA = levelDb(A, n.sec, 0.25), onB = levelDb(B, n.sec, 0.25);
            for (double t = n.sec; t + 0.25 <= n.sec + span; t += 0.25) {
                const double hA = pitchHz(A, t), hB = pitchHz(B, t);
                const double tA = levelDb(A, t, 0.25) - onA, tB = levelDb(B, t, 0.25) - onB;
                // La trayectoria de nivel se compara mientras ALGUNO de los dos suena
                // (-60 dB): asi la cola de release entra, que es donde difieren.
                if (levelDb(A, t, 0.25) > -60.0 || levelDb(B, t, 0.25) > -60.0) {
                    const double d = std::fabs(std::max(tA, -60.0) - std::max(tB, -60.0));
                    maxTraj = std::max(maxTraj, d);
                    if (t + 0.25 <= n.sec + sounding + 1e-9) maxTrajOn = std::max(maxTrajOn, d);
                }
                if (wantsHops(w.test)) {
                    std::printf("        %-8s nota %2zu hop @%7.2fs  %8.2f / %8.2f Hz  Δ %+8.2f c  nivel %+7.2f/%+7.2f\n",
                                w.label().c_str(), i, t, hA, hB, centsBetween(hA, hB), tA, tB);
                }
                const bool inNote = (t + 0.25 <= n.sec + sounding + 1e-9) && tA > -20.0 && tB > -20.0;
                if (hA <= 0 || hB <= 0 || !inNote) { ++silentHops; continue; }
                const double d = centsBetween(hA, hB);
                notePitchMax = std::max(notePitchMax, std::fabs(d));
                ++hops;
            }
            maxPitch = std::max(maxPitch, notePitchMax);
            if (wantsDetail(w.test)) {
                std::printf("      %-8s nota %2zu ch%d k%3d v%3d @%7.2fs  nivel %+7.2f/%+7.2f  bal %+6.2f/%+6.2f"
                            "  pitch %8.2f/%8.2f Hz  maxΔc %6.2f\n",
                            w.label().c_str(), i, n.channel, n.key, n.velocity, n.sec, relA, relB,
                            bA, bB, pitchHz(A, n.sec), pitchHz(B, n.sec), notePitchMax);
            }
        }
        if (hops == 0) note += "sin pitch (ruido/silencio) ";
        else if (silentHops > 0) note += "hops mudos: " + std::to_string(silentHops) + " ";
        std::printf("  %-8s %5zu %10.2f %10.2f %10.2f %10.2f %10.2f %10.2f  %s\n", w.label().c_str(),
                    w.notes.size(), maxLvl, hiA - loA, maxTraj, maxTrajOn, maxPitch, maxBal,
                    note.c_str());
    }
    std::printf("\n");
}
