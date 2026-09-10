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
