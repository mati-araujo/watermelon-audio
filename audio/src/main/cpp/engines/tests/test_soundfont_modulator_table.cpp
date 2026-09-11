/**
 * test_soundfont_modulator_table.cpp — REQ-039 S2, tareas 2.7 y 2.8 (la mitad pura).
 *
 * Lo que se afirma acá NO toca `tsf` ni audio: es la tabla que el thread de control
 * construye y lo que el thread de audio suma al disparar. La otra mitad —que `tsf`
 * reciba esos números por voz y el render cambie— vive en `core/tests`, que sí linkea
 * el renderizador.
 *
 * Los oráculos de la atenuación son los EXTERNOS de 2.2 (README del spec-test y
 * FluidSynth 2.6.0), en centibeles. Usar `applyTransform` como esperado sería
 * preguntarle al acusado.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "../SoundFontModulatorTable.h"
#include "../../core/tests/support/MinimalSoundFont.h"

using wma::sfmod::defaultModulators;
using wma::sfmod::kDestInitialAttenuation;
using wma::sfmod::kDestInitialFilterFc;
using wma::sfmod::ModulatorTable;
using wma::sfmod::noteOnContributionsOf;
using wma::sfmod::RegionModulatorList;
using wma_test::makeMinimalSoundFont;
using wma_test::sf2::kCurveConcave;
using wma_test::sf2::kCurveLinear;
using wma_test::sf2::kGenInitialAttenuation;
using wma_test::sf2::kGenInitialFilterFc;
using wma_test::sf2::kGenPan;
using wma_test::sf2::kSrcNone;
using wma_test::sf2::kSrcVelocity;
using wma_test::sf2::ModulatorPlacement;
using wma_test::sf2::srcOper;

namespace {

/// El default #1 tal como lo escribiría un archivo — misma identidad, otro amount.
wma_test::sf2::Modulator comoElDefaultUno(std::int16_t amount) {
    return {srcOper(kSrcVelocity, false, true, false, kCurveConcave), kGenInitialAttenuation,
            amount, kSrcNone, 0};
}

ModulatorTable tablaDe(const ModulatorPlacement& mods) {
    const auto sf = makeMinimalSoundFont(22050, true, -1, -1, mods);
    ModulatorTable t;
    EXPECT_TRUE(t.buildFromFontBytes(sf.data(), sf.size()));
    return t;
}

const RegionModulatorList* laUnicaRegion(const ModulatorTable& t) {
    const auto* r = t.regionModulators(0, 0);
    EXPECT_NE(r, nullptr) << "el font minimo tiene el preset 0 con la region 0";
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// Los diez por defecto
// ---------------------------------------------------------------------------

TEST(SoundFontModulatorTable, LosDiezPorDefectoSonDiezYElPrimeroEsElDeVelocity) {
    const auto& d = defaultModulators();
    ASSERT_EQ(d.size(), 10u) << "SF2 2.04 §8.4 declara exactamente diez";
    // #1: 0x0502 = velocity (2), sin CC, decreciente, unipolar, concava (1 << 10).
    EXPECT_EQ(d[0].srcOper, 0x0502);
    EXPECT_EQ(d[0].destOper, kDestInitialAttenuation);
    EXPECT_EQ(d[0].amount, 960);
    EXPECT_EQ(d[0].transOper, 0);
    // #2: velocity lineal decreciente al corte del filtro, -2400 cents.
    EXPECT_EQ(d[1].srcOper, 0x0102);
    EXPECT_EQ(d[1].destOper, kDestInitialFilterFc);
    EXPECT_EQ(d[1].amount, -2400);
    // #10 es el unico con fuente secundaria: la sensibilidad de la rueda (indice 16).
    EXPECT_EQ(d[9].amtSrcOper & 0x7F, 16);
}

// ---------------------------------------------------------------------------
// AC-039.5 — la atenuacion por velocity, contra los oraculos EXTERNOS de 2.2
// ---------------------------------------------------------------------------

TEST(SoundFontModulatorTable, SinModuladoresEnElArchivoLaAtenuacionEsLaDelDefaultUno) {
    const auto t = tablaDe({});  // sin moduladores: solo los diez por defecto
    const auto* r = laUnicaRegion(t);

    // Velocity maxima: cero atenuacion. Es lo que hace que el default #1 no toque el
    // nivel de una nota a fondo, y de eso depende todo lo relativo del arnes de S1.
    EXPECT_NEAR(noteOnContributionsOf(r, 60, 127).attenuationCentibels, 0.0f, 1e-4f);

    // README del SoundFont-Spec-Test: v127 -> v111 son 2,34 dB = 23,4 cB.
    EXPECT_NEAR(noteOnContributionsOf(r, 60, 111).attenuationCentibels, 23.4f, 0.2f);

    // FluidSynth 2.6.0 sobre #13 A: v127 -> v15 son 37,11 dB = 371,1 cB.
    EXPECT_NEAR(noteOnContributionsOf(r, 60, 15).attenuationCentibels, 371.1f, 0.5f);
}

TEST(SoundFontModulatorTable, UnModuladorDelArchivoIgualAlDefaultConAmountCeroLoAnula) {
    // Es #13 E del spec-test: FluidSynth da 0,01 dB de rango; el motor daba 18,52.
    ModulatorPlacement m;
    m.instrumentGlobal.push_back(comoElDefaultUno(0));
    const auto t = tablaDe(m);
    const auto* r = laUnicaRegion(t);
    EXPECT_NEAR(noteOnContributionsOf(r, 60, 15).attenuationCentibels, 0.0f, 1e-4f)
        << "el default #1 sigue atenuando: el archivo no lo reemplazo";
}

TEST(SoundFontModulatorTable, UnModuladorDelArchivoConOtroAmountReemplazaAlDefault) {
    // #13 C: 48 dB = 480 cB de rango -> 18,50 dB en FluidSynth a v15.
    ModulatorPlacement m;
    m.instrumentGlobal.push_back(comoElDefaultUno(480));
    const auto t = tablaDe(m);
    EXPECT_NEAR(noteOnContributionsOf(laUnicaRegion(t), 60, 15).attenuationCentibels, 185.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// AC-039.7 (la mitad pura) — el corte del filtro tambien es por velocity
// ---------------------------------------------------------------------------

TEST(SoundFontModulatorTable, ElDefaultDosBajaElCorteDelFiltroConLaVelocity) {
    const auto t = tablaDe({});
    const auto* r = laUnicaRegion(t);
    EXPECT_NEAR(noteOnContributionsOf(r, 60, 127).filterFcCents, 0.0f, 1e-3f);
    // Lineal decreciente: -2400 * (1 - 64/127).
    EXPECT_NEAR(noteOnContributionsOf(r, 60, 64).filterFcCents, -2400.0f * (1.0f - 64.0f / 127.0f), 0.05f);
    // Y dos velocities distintas dan dos cortes distintos — lo que 2.5 mide en el render.
    EXPECT_NE(noteOnContributionsOf(r, 60, 40).filterFcCents,
              noteOnContributionsOf(r, 60, 100).filterFcCents);
}

// ---------------------------------------------------------------------------
// AC-039.8 — el rechazo es CONTABLE, y separa lo del spec de lo que es alcance
// ---------------------------------------------------------------------------

TEST(SoundFontModulatorTable, UnEncadenadoSeDescartaYSeCuenta) {
    ModulatorPlacement m;
    auto encadenado = comoElDefaultUno(300);
    encadenado.destOper = static_cast<std::uint16_t>(0x8000 | 5);  // bit de link: destino = otro mod
    m.instrumentZone.push_back(encadenado);
    const auto t = tablaDe(m);
    EXPECT_EQ(t.rejections().linked, 1u);
    EXPECT_EQ(t.rejections().specRejections(), 1u);
    // 🔴 Y se cuenta UNA vez, en SU contador. Un mutante que contaba el encadenado y
    // seguia clasificando sobrevivio: el destino 0x8005 caia en `destinationUnsupported`
    // y el resultado audible era el mismo. Contar dos veces es mentir sobre cuantos
    // moduladores tiene el archivo fuera de alcance — y AC-039.10 lee ese numero.
    EXPECT_EQ(t.rejections().destinationUnsupported, 0u)
        << "el encadenado se conto tambien como destino no soportado";
    // Y NO cambio nada de lo que suena: la atenuacion sigue siendo la del default.
    EXPECT_NEAR(noteOnContributionsOf(laUnicaRegion(t), 60, 15).attenuationCentibels, 371.1f, 0.5f);
}

TEST(SoundFontModulatorTable, UnTransformNoLinealSeDescartaYSeCuenta) {
    ModulatorPlacement m;
    auto absoluto = comoElDefaultUno(300);
    absoluto.transOper = 2;  // §8.3: valor absoluto
    m.instrumentZone.push_back(absoluto);
    const auto t = tablaDe(m);
    EXPECT_EQ(t.rejections().nonLinearTransform, 1u);
    EXPECT_EQ(t.rejections().linked, 0u);
    // 🔴 Descartado quiere decir que NO reemplazo al default: si lo hubiera hecho, la
    // atenuacion a v15 seria de 300 cB y no de 960.
    EXPECT_NEAR(noteOnContributionsOf(laUnicaRegion(t), 60, 15).attenuationCentibels, 371.1f, 0.5f);
}

TEST(SoundFontModulatorTable, LasFuentesDeCanalSonAlcanceDeS3YNoRechazoDelSpec) {
    // Un modulador del ARCHIVO con fuente de canal (CC1 -> filtro, como el del spec-test)
    // se cuenta como fuera de alcance, no como rechazo del spec. Los defaults #3..#10
    // tambien tienen fuente de canal, pero NO son del archivo y no se cuentan.
    ModulatorPlacement m;
    m.instrumentGlobal.push_back(
        {srcOper(1, true, false, false, kCurveLinear), kGenInitialFilterFc, 7000, kSrcNone, 0});
    const auto t = tablaDe(m);
    EXPECT_EQ(t.declaredInFile(), 1u);
    EXPECT_EQ(t.rejections().specRejections(), 0u);
    EXPECT_EQ(t.rejections().sourceNotAtNoteOn, 1u) << "CC1 es fuente de canal: alcance de S3";
    EXPECT_EQ(t.rejections().destinationUnsupported, 0u);
    // Y por region quedan #1 y #2 (los dos defaults de velocity): el de CC1 no entra.
    ASSERT_EQ(laUnicaRegion(t)->mods.size(), 2u) << "quedan #1 y #2, los dos de velocity";
}

TEST(SoundFontModulatorTable, LosContadoresSonDelArchivoYNoSeMultiplicanPorRegion) {
    // Un font SIN moduladores propios declara CERO en el archivo, aunque cada region
    // resuelva diez defaults. Es lo que sobre GeneralUser separaba 2812 de 63 822.
    const auto t = tablaDe({});
    EXPECT_EQ(t.declaredInFile(), 0u);
    EXPECT_EQ(t.rejections().outOfScope(), 0u) << "los defaults no son del archivo";
    EXPECT_EQ(t.rejections().specRejections(), 0u);
}

TEST(SoundFontModulatorTable, UnDestinoQueS2NoAplicaSeCuentaAparte) {
    ModulatorPlacement m;
    m.instrumentZone.push_back(
        {srcOper(kSrcVelocity, false, false, false, kCurveLinear), kGenPan, 500, kSrcNone, 0});
    const auto t = tablaDe(m);
    EXPECT_EQ(t.rejections().destinationUnsupported, 1u);
    EXPECT_EQ(t.rejections().specRejections(), 0u);
}

// ---------------------------------------------------------------------------
// El thread de audio no puede leer fuera de la tabla
// ---------------------------------------------------------------------------

TEST(SoundFontModulatorTable, UnaRegionQueLaTablaNoConoceDaCeroYNoNullptrDeref) {
    const auto t = tablaDe({});
    EXPECT_EQ(t.regionModulators(0, 7), nullptr);
    EXPECT_EQ(t.regionModulators(3, 0), nullptr);
    EXPECT_EQ(t.regionModulators(-1, 0), nullptr);
    const auto c = noteOnContributionsOf(nullptr, 60, 15);
    EXPECT_EQ(c.attenuationCentibels, 0.0f);
    EXPECT_EQ(c.filterFcCents, 0.0f);
}

TEST(SoundFontModulatorTable, BytesQueNoSonUnSoundFontDejanLaTablaVacia) {
    const std::vector<std::uint8_t> basura(64, 0x42);
    ModulatorTable t;
    EXPECT_FALSE(t.buildFromFontBytes(basura.data(), basura.size()));
    EXPECT_EQ(t.declaredInFile(), 0u);
    EXPECT_EQ(t.regionModulators(0, 0), nullptr);
}

// ---------------------------------------------------------------------------
// AC-039.10 — cero descartes DEL SPEC sobre un font real (tarea 2.9)
// ---------------------------------------------------------------------------

#include <cstdlib>
#include <fstream>
#include <iterator>

namespace {

/**
 * `GeneralUser_GS.sf3` no esta en este repo: vive en el bundle de NoisyPad. Se
 * busca por `WMA_GENERALUSER_SF3` y, si no, en la ruta hermana del checkout. Sin
 * el archivo el test sale SKIPPED y NUNCA passed (regla de REQ-032): una corrida
 * que no verifico no se puede leer como cobertura.
 */
std::vector<std::uint8_t> generalUserBytes() {
    const char* env = std::getenv("WMA_GENERALUSER_SF3");
    const std::string path = env ? env : WMA_GENERALUSER_SF3_DEFAULT;
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

}  // namespace

/**
 * El contador de 2.7 sobre los 2812 moduladores de GeneralUser tiene que dar CERO:
 * si no da cero, la forma que se descarta EXISTE en un font real, y entonces se
 * soporta o se justifica por escrito — no se cuenta y se sigue.
 *
 * 🔴 Lo que NO tiene que ser cero es lo fuera de alcance: GeneralUser modula por CC,
 * presion y rueda (S3) y a destinos que S2 no aplica. Se imprime, para que el
 * numero exista medido y no afirmado, y se afirma que es DISTINTO de cero — un
 * font real sin moduladores de canal seria el lector fallando en silencio.
 */
TEST(SoundFontModulatorTable, GeneralUserNoTieneNingunModuladorQueElSpecMandeRechazar) {
    const auto bytes = generalUserBytes();
    if (bytes.empty()) GTEST_SKIP() << "sin GeneralUser_GS.sf3 — WMA_GENERALUSER_SF3 o el checkout hermano de NoisyPad";

    ModulatorTable t;
    ASSERT_TRUE(t.buildFromFontBytes(bytes.data(), bytes.size()));
    const auto& r = t.rejections();
    std::printf("  [REQ-039] GeneralUser: %u moduladores declarados; rechazados por spec %u "
                "(encadenados %u, transform no lineal %u); fuera de alcance de S2 %u "
                "(fuente de canal %u, destino no aplicado %u)\n",
                t.declaredInFile(), r.specRejections(), r.linked, r.nonLinearTransform,
                r.outOfScope(), r.sourceNotAtNoteOn, r.destinationUnsupported);

    // S1 1.6 conto 2812 en el archivo con un parche a tsf.h; el lector tiene que dar lo mismo.
    EXPECT_EQ(t.declaredInFile(), 2812u) << "el lector no ve los mismos moduladores que S1 conto";
    EXPECT_EQ(r.linked, 0u) << "GeneralUser declara moduladores ENCADENADOS: se soportan o se justifica";
    EXPECT_EQ(r.nonLinearTransform, 0u) << "GeneralUser usa transform no lineal: se soporta o se justifica";
    EXPECT_EQ(r.specRejections(), 0u);
    EXPECT_GT(r.outOfScope(), 0u) << "un font real sin moduladores de canal: el lector fallo mudo";
}
