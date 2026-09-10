/**
 * test_soundfont_modulator_reader.cpp — REQ-039 S2, tarea 2.4.
 *
 * Que el lector saque de un `.sf2` los CUATRO ámbitos de cada región, y en el mismo
 * orden en que `tsf` numera esas regiones.
 *
 * ## Los oráculos
 *
 * 1. **Un font que el test construye entero**, con moduladores puestos a propósito
 *    en cada ámbito: acá el esperado se conoce por construcción.
 * 2. **El `SoundFont-Spec-Test`**, con el conteo de regiones que S1 midió contra un
 *    parche a `tsf.h` — 174, con 0 desajustes. Ése es el número que dice si la
 *    reconstrucción del ORDEN sigue siendo fiel, que es la parte frágil.
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../SoundFontModulatorReader.h"
#include "../../core/tests/support/MinimalSoundFont.h"

namespace {

using wma::sfmod::findHydra;
using wma::sfmod::readRegionModulators;
using wma::sfmod::RegionModulators;
using wma_test::makeMinimalSoundFont;
using wma_test::sf2::kCurveConcave;
using wma_test::sf2::kCurveLinear;
using wma_test::sf2::kGenInitialAttenuation;
using wma_test::sf2::kGenInitialFilterFc;
using wma_test::sf2::kSrcNone;
using wma_test::sf2::kSrcVelocity;
using wma_test::sf2::Modulator;
using wma_test::sf2::ModulatorPlacement;
using wma_test::sf2::srcOper;

/** Amounts distintos por ámbito, para poder decir de dónde salió cada uno. */
constexpr std::int16_t kAmountInstGlobal = 960;
constexpr std::int16_t kAmountInstZone = 480;
constexpr std::int16_t kAmountPresetGlobal = 240;
constexpr std::int16_t kAmountPresetZone = 120;

ModulatorPlacement unoPorAmbito() {
    ModulatorPlacement m;
    const auto vel = srcOper(kSrcVelocity, false, true, false, kCurveConcave);
    m.instrumentGlobal.push_back({vel, kGenInitialAttenuation, kAmountInstGlobal, kSrcNone, 0});
    m.instrumentZone.push_back({vel, kGenInitialFilterFc, kAmountInstZone, kSrcNone, 0});
    m.presetGlobal.push_back({vel, kGenInitialAttenuation, kAmountPresetGlobal, kSrcNone, 0});
    m.presetZone.push_back({vel, kGenInitialFilterFc, kAmountPresetZone, kSrcNone, 0});
    return m;
}

/** 🔑 Cada modulador aparece en SU ámbito, y no en otro. */
TEST(SoundFontModulatorReader, CadaModuladorSaleEnElAmbitoQueLeCorresponde) {
    const auto sf = makeMinimalSoundFont(22050, true, -1, -1, unoPorAmbito());
    const auto regiones = readRegionModulators(sf.data(), sf.size());

    ASSERT_EQ(regiones.size(), 1u) << "el font minimo tiene UNA region";
    const auto& s = regiones[0].scopes;

    ASSERT_EQ(s.instrumentGlobal.size(), 1u);
    ASSERT_EQ(s.instrumentZone.size(), 1u);
    ASSERT_EQ(s.presetGlobal.size(), 1u);
    ASSERT_EQ(s.presetZone.size(), 1u);

    // El amount es la etiqueta: dice de qué ámbito salió cada uno. Si el lector los
    // mezclara, los números aparecerían cruzados.
    EXPECT_EQ(s.instrumentGlobal[0].amount, kAmountInstGlobal);
    EXPECT_EQ(s.instrumentZone[0].amount, kAmountInstZone);
    EXPECT_EQ(s.presetGlobal[0].amount, kAmountPresetGlobal);
    EXPECT_EQ(s.presetZone[0].amount, kAmountPresetZone);

    // Y los otros campos viajan enteros, no sólo el amount.
    EXPECT_EQ(s.instrumentGlobal[0].destOper, kGenInitialAttenuation);
    EXPECT_EQ(s.instrumentZone[0].destOper, kGenInitialFilterFc);
    EXPECT_EQ(s.instrumentGlobal[0].srcOper,
              srcOper(kSrcVelocity, false, true, false, kCurveConcave));
}

/**
 * 🔴 El registro TERMINAL de `pmod`/`imod` no es un modulador.
 *
 * Sin el tope, un font sin moduladores declarados devolvería uno de ceros por
 * ámbito — y un modulador de ceros no es inofensivo: tiene la identidad
 * `(0,0,0,0)`, así que **reemplazaría** a cualquier default con esa forma.
 */
TEST(SoundFontModulatorReader, ElRegistroTerminalNoSeCuentaComoModulador) {
    const auto sf = makeMinimalSoundFont(22050, true);  // sin moduladores
    const auto regiones = readRegionModulators(sf.data(), sf.size());

    ASSERT_EQ(regiones.size(), 1u);
    const auto& s = regiones[0].scopes;
    EXPECT_TRUE(s.instrumentGlobal.empty());
    EXPECT_TRUE(s.instrumentZone.empty());
    EXPECT_TRUE(s.presetGlobal.empty());
    EXPECT_TRUE(s.presetZone.empty());
}

/**
 * 🔑 Un font MALFORMADO no puede colar el terminal como modulador.
 *
 * Este test lo trajo un **mutante que sobrevivió**: quitar el tope `count - 1`
 * dejaba los cinco tests en verde, porque en un font **bien formado** el registro
 * terminal nunca cae dentro del rango de una bag — el bag terminal apunta
 * exactamente a él. O sea que el tope no era una regla activa sino una defensa, y
 * una defensa sin test es una promesa sin verificar.
 *
 * El riesgo es real y no cosmético: un modulador de ceros tiene identidad
 * `(0,0,0,0)`, así que **reemplazaría** a cualquier default con esa forma en vez de
 * ser inofensivo. Y los fonts de terceros vienen rotos.
 *
 * Se construye corriendo el `modNdx` del bag terminal un lugar más allá, que es
 * exactamente la forma en que un font mal escrito incluiría al centinela.
 */
TEST(SoundFontModulatorReader, UnBagQueApuntaAlTerminalNoLoDevuelveComoModulador) {
    ModulatorPlacement m;
    m.presetZone.push_back({srcOper(kSrcVelocity, false, true, false, kCurveLinear),
                            kGenInitialAttenuation, 500, kSrcNone, 0});
    auto sf = makeMinimalSoundFont(22050, true, -1, -1, m);

    // Localizar el chunk `pbag` y correr el `modNdx` de su registro TERMINAL.
    const auto h = findHydra(sf.data(), sf.size());
    ASSERT_TRUE(h.complete);
    ASSERT_GE(h.pbag.count, 2u);
    const std::size_t offsetTerminal =
        static_cast<std::size_t>(h.pbag.begin - sf.data()) + (h.pbag.count - 1) * 4 + 2;
    const std::uint16_t antes = static_cast<std::uint16_t>(
        sf[offsetTerminal] | (sf[offsetTerminal + 1] << 8));
    const std::uint16_t despues = static_cast<std::uint16_t>(antes + 1);
    sf[offsetTerminal] = static_cast<std::uint8_t>(despues & 0xFF);
    sf[offsetTerminal + 1] = static_cast<std::uint8_t>((despues >> 8) & 0xFF);

    const auto regiones = readRegionModulators(sf.data(), sf.size());
    ASSERT_EQ(regiones.size(), 1u);
    EXPECT_EQ(regiones[0].scopes.presetZone.size(), 1u)
        << "el bag pide DOS moduladores y el segundo es el terminal: no se devuelve";
    EXPECT_EQ(regiones[0].scopes.presetZone[0].amount, 500) << "y el que se devuelve es el real";
}

/** Un font que no es un SoundFont se rechaza diciéndolo, no devolviendo vacío. */
TEST(SoundFontModulatorReader, UnArchivoQueNoEsSoundFontSeRechaza) {
    const std::vector<std::uint8_t> basura(64, 0xAB);
    EXPECT_FALSE(findHydra(basura.data(), basura.size()).complete);
    EXPECT_TRUE(readRegionModulators(basura.data(), basura.size()).empty());

    // Y un truncado a la mitad tampoco puede pasar por completo.
    const auto sf = makeMinimalSoundFont(22050, true);
    EXPECT_FALSE(findHydra(sf.data(), sf.size() / 2).complete);
}

/** Los ámbitos globales se comparten entre las regiones del mismo instrumento. */
TEST(SoundFontModulatorReader, ElAmbitoGlobalLlegaATodasLasRegiones) {
    // Dos teclas distintas producen dos regiones del mismo instrumento sólo si el
    // font declara dos zonas; el fixture mínimo tiene una, así que lo que se afirma
    // acá es que la global viaja CON la región, no que se pierda por el camino.
    const auto sf = makeMinimalSoundFont(22050, true, 40, 80, unoPorAmbito());
    const auto regiones = readRegionModulators(sf.data(), sf.size());

    ASSERT_FALSE(regiones.empty());
    for (const auto& r : regiones) {
        EXPECT_EQ(r.scopes.instrumentGlobal.size(), 1u)
            << "la zona global del instrumento aplica a TODAS sus regiones";
        EXPECT_EQ(r.scopes.presetGlobal.size(), 1u);
    }
}

// ---------------------------------------------------------------------------
// El oráculo de escala: el SoundFont-Spec-Test.
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> leerArchivo(const std::string& path) {
    std::vector<std::uint8_t> out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size > 0) {
        out.resize(static_cast<std::size_t>(size));
        if (std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
    }
    std::fclose(f);
    return out;
}

/**
 * 🔑 El conteo de regiones tiene que seguir coincidiendo con el de `tsf`.
 *
 * **174** es el número que S1 (1.6) midió con un parche a `tsf.h` usado como
 * oráculo: 0 desajustes contra la reconstrucción, con dos mutantes que probaron que
 * la comparación no era ciega. Es la parte frágil de este lector —reproduce el
 * recorrido de `tsf_load_presets`— y por eso el número se afirma en vez de
 * imprimirse.
 *
 * Sin el material bajado el test sale SKIPPED y nunca passed: una corrida que no
 * verificó no se puede leer como cobertura (regla de REQ-032).
 */
TEST(SoundFontModulatorReader, ElConteoDeRegionesDelSpecTestSigueSiendoElQueMidioS1) {
    const std::string path =
        std::string(WMA_SPEC_TEST_DIR) + "/sf_spec_test.sf2";
    const auto sf = leerArchivo(path);
    if (sf.empty()) {
        GTEST_SKIP() << "sin el material del spec-test (bash scripts/fetch-spec-test.sh)";
    }

    const auto regiones = readRegionModulators(sf.data(), sf.size());
    EXPECT_EQ(regiones.size(), 174u)
        << "S1 midio 174 regiones contra un parche a tsf.h, con 0 desajustes";

    // Y el reparto por ámbito, que es lo que hace a este REQ: la enorme mayoría de
    // los moduladores de este font vive en zonas globales.
    std::size_t globales = 0, deZona = 0;
    for (const auto& r : regiones) {
        globales += r.scopes.instrumentGlobal.size() + r.scopes.presetGlobal.size();
        deZona += r.scopes.instrumentZone.size() + r.scopes.presetZone.size();
    }
    EXPECT_GT(globales, deZona)
        << "en el spec-test MANDAN los globales: 21 de 25 moduladores, medido en S1";
}

}  // namespace
