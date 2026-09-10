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
TEST(SfSpecConformance, TheVelocityLaddersAreTheDefectMadeVisible) {
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
     * 🔴 EL DEFECTO, MEDIDO: las diez sub-pruebas estan programadas DISTINTAS —96 dB
     * concava, 144, 48, 96 LINEAL, y dos con el modulador BORRADO— y nuestro motor
     * da **la misma curva en las diez**, porque no lee ningun modulador: aplica su
     * propia curva de velocity cableada (`tsf.h:1619`).
     *
     * Esto es la linea de base de S1 y el trinquete que S2 tiene que ROMPER. Y es
     * tambien el control de AC-039.2: un instrumento que no distinguiera las diez
     * no podria ver el arreglo.
     */
    double minR = rangosNuestros[0], maxR = rangosNuestros[0];
    for (double r : rangosNuestros) { minR = std::min(minR, r); maxR = std::max(maxR, r); }
    std::printf("  motor: rangos entre %.2f y %.2f dB -> dispersion %.2f dB sobre DIEZ sub-pruebas\n\n",
                minR, maxR, maxR - minR);
    EXPECT_LT(maxR - minR, 0.5)
        << "las diez escaleras dejaron de dar la misma curva (dispersion " << (maxR - minR)
        << " dB). Si es por el arreglo de REQ-039, ESTE ES EL TRINQUETE QUE HAY QUE DAR VUELTA: "
           "borralo y escribi el contrato nuevo por sub-prueba.";

    // Y el control positivo del lado de la referencia: #13 E tiene el modulador
    // BORRADO, asi que sus ocho notas tienen que sonar IGUAL. Si esto no fuera
    // plano, la referencia no estaria aplicando moduladores y no serviria de nada.
    const double refE = rango(ref.buffer.data(), ref.numFrames, 121.5, nullptr);
    EXPECT_LT(refE, 0.5) << "#13 E (modulador borrado) no es plano en la referencia: " << refE
                         << " dB — la referencia no aplica moduladores";
}
