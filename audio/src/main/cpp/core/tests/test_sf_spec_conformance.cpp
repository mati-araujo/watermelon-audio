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

#include <sys/stat.h>
#include <cstdio>
#include <string>

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
}
