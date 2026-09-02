/**
 * test_c_api_engine_params.cpp — REQ-028 S1
 *
 * La metadata de parametros de engine saliendo por la C API, y el drift entre
 * lo EXPUESTO y lo IMPLEMENTADO volviendose detectable.
 *
 * EL ARCHIVO TIENE DOS ORACULOS Y NINGUNO SOBRA
 * ---------------------------------------------
 * 1. **Paridad** contra el engine (`TheCApiPublishesWhatEachEngineImplements`):
 *    construye la clase concreta y compara campo por campo contra lo que sale
 *    por la C API. Es AC-028.4 literal — "lo expuesto deja de coincidir con lo
 *    que el engine implementa" — y es lo unico que se pone rojo si la C API deja
 *    de LEER al engine y empieza a inventar.
 *
 * 2. **Catalogo declarado** (`TheCatalogMatchesTheDeclaredValues`): los 15
 *    valores escritos aca, contra la C API. Existe porque el oraculo 1, solo, es
 *    ciego a un cambio de valor: si alguien edita el default de `Decay` en
 *    `KarplusStrongEngine`, los DOS lados se mueven juntos y la paridad sigue
 *    verde. REQ-028 dice explicitamente *"mueve la metadata, no la edita: un
 *    solo valor distinto al terminar es un defecto, no una mejora"*, y esto es
 *    lo que lo hace fallar.
 *
 * 🔴 LA COMPARACION ES NUMERICA, NUNCA TEXTUAL.
 * Al medir la evidencia de partida de este REQ el primer comparador dio 6 de 6
 * en rojo y era del INSTRUMENTO: comparaba `"0.0"` contra `"0"` como texto. Un
 * test de paridad con ese bug es peor que no tenerlo — acusa un drift que no
 * existe y enseña a ignorarlo. Los floats se comparan como floats; solo `name` y
 * `shortName` van por `strcmp`, que es lo que son.
 *
 * LO QUE ESTE ARCHIVO NO CUBRE, Y POR QUE
 * ---------------------------------------
 * Una copia BYTE A BYTE de la tabla dentro de `watermelon_audio.cpp` es
 * indistinguible desde aca: los dos oraculos comparan VALORES, y una copia fiel
 * tiene los mismos. Lo que mata a esa mutacion es `check-mechanism-callers.py`,
 * que es estructural: sin la lectura al engine, las doce virtuales vuelven a no
 * tener llamador de produccion y el lint se pone rojo de a doce. Esta medido en
 * la etapa. Un test de valores no puede contestar una pregunta estructural.
 */

#include "support/CApiFixture.h"

#include "engines/FMEngine.h"
#include "engines/GranularEngine.h"
#include "engines/KarplusStrongEngine.h"
#include "engines/SoundFontEngine.h"
#include "engines/SupersawEngine.h"
#include "engines/SynthEngine.h"
#include "engines/WavetableEngine.h"

#include <cstring>
#include <memory>
#include <string>

#include <gtest/gtest.h>

namespace wma_test {
namespace {

constexpr int kClassic = static_cast<int>(EngineTypeId::CLASSIC);
constexpr int kKarplusStrong = static_cast<int>(EngineTypeId::KARPLUS_STRONG);
constexpr int kFm = static_cast<int>(EngineTypeId::FM_SYNTH);
constexpr int kWavetable = static_cast<int>(EngineTypeId::WAVETABLE);
constexpr int kGranular = static_cast<int>(EngineTypeId::GRANULAR);
constexpr int kSupersaw = static_cast<int>(EngineTypeId::SUPERSAW);
constexpr int kSoundFont = static_cast<int>(EngineTypeId::SOUNDFONT);

/// Los seis tipos que SON un SynthEngine. CLASSIC (0) no lo es: usa los
/// osciladores legacy, y el REQ lo deja fuera de alcance a proposito.
constexpr int kSynthEngineTypes[] = {
    kKarplusStrong, kFm, kWavetable, kGranular, kSupersaw, kSoundFont,
};

/// Una instancia FRESCA de la clase concreta, para preguntarle a la
/// implementacion directamente en vez de a traves de la C API.
std::unique_ptr<SynthEngine> makeEngine(int engineType) {
    switch (engineType) {
        case kKarplusStrong: return std::make_unique<KarplusStrongEngine>();
        case kFm:            return std::make_unique<FMEngine>();
        case kWavetable:     return std::make_unique<WavetableEngine>();
        case kGranular:      return std::make_unique<GranularEngine>();
        case kSupersaw:      return std::make_unique<SupersawEngine>();
        case kSoundFont:     return std::make_unique<SoundFontEngine>();
        default:             return nullptr;
    }
}

/// El catalogo DECLARADO: los 15 parametros de los 6 engines, con sus cinco
/// campos. No es una copia de produccion — es el valor congelado que este REQ
/// promete NO cambiar, y su diff es la revision.
struct ExpectedParam {
    int engineType;
    int index;
    const char* name;
    const char* shortName;
    float minValue;
    float maxValue;
    float defaultValue;
};

constexpr ExpectedParam kExpectedCatalog[] = {
    {kKarplusStrong, 0, "Brightness", "BRIGHT", 0.0f, 1.0f, 0.5f},
    {kKarplusStrong, 1, "Decay",      "DECAY",  0.0f, 1.0f, 0.5f},
    {kKarplusStrong, 2, "Excitation", "EXCITE", 0.0f, 1.0f, 0.3f},
    {kFm,            0, "Mod Index",  "INDEX",  0.0f, 1.0f, 0.3f},
    {kFm,            1, "Ratio",      "RATIO",  0.0f, 1.0f, 0.25f},
    {kFm,            2, "Feedback",   "FDBK",   0.0f, 1.0f, 0.0f},
    {kWavetable,     0, "Position",   "POS",    0.0f, 1.0f, 0.0f},
    {kWavetable,     1, "Morph",      "MORPH",  0.0f, 1.0f, 0.0f},
    {kGranular,      0, "Grain Size", "SIZE",   0.0f, 1.0f, 0.3f},
    {kGranular,      1, "Scatter",    "SCAT",   0.0f, 1.0f, 0.1f},
    {kGranular,      2, "Density",    "DENS",   0.0f, 1.0f, 0.5f},
    {kSupersaw,      0, "Detune",     "DETUNE", 0.0f, 1.0f, 0.3f},
    {kSupersaw,      1, "Voices",     "VOICES", 0.0f, 1.0f, 0.4f},
    {kSupersaw,      2, "Spread",     "SPREAD", 0.0f, 1.0f, 0.5f},
    {kSoundFont,     0, "Expression", "EXPR",   0.0f, 1.0f, 1.0f},
};

constexpr int kExpectedTotalParams = 15;

/// Valores VENENO para los out-params. La firma promete que un rechazo NO los
/// toca (AC-028.5), asi que "sigue siendo el veneno" es la afirmacion.
constexpr const char* kPoisonName = "<<untouched>>";
constexpr float kPoisonFloat = -12345.0f;

struct DefOut {
    const char* name = kPoisonName;
    const char* shortName = kPoisonName;
    float minValue = kPoisonFloat;
    float maxValue = kPoisonFloat;
    float defaultValue = kPoisonFloat;

    void expectUntouched() const {
        EXPECT_EQ(name, kPoisonName);
        EXPECT_EQ(shortName, kPoisonName);
        EXPECT_FLOAT_EQ(minValue, kPoisonFloat);
        EXPECT_FLOAT_EQ(maxValue, kPoisonFloat);
        EXPECT_FLOAT_EQ(defaultValue, kPoisonFloat);
    }
};

bool fetchDef(const WmaEngine* wma, int engineType, int paramIndex, DefOut& out) {
    return wma_engine_get_parameter_def(wma, engineType, paramIndex,
                                        &out.name, &out.shortName,
                                        &out.minValue, &out.maxValue,
                                        &out.defaultValue);
}

using CApiEngineParamsTest = CApiFixture;

// ===========================================================================
// AC-028.2 + AC-028.4 — lo expuesto ES lo que el engine implementa
// ===========================================================================

TEST_F(CApiEngineParamsTest, TheCApiPublishesWhatEachEngineImplements) {
    for (int engineType : kSynthEngineTypes) {
        SCOPED_TRACE("engine_type=" + std::to_string(engineType));
        const std::unique_ptr<SynthEngine> engine = makeEngine(engineType);
        ASSERT_NE(engine, nullptr);

        const int implemented = engine->getParameterCount();
        EXPECT_EQ(wma_engine_get_parameter_count(mWma, engineType), implemented);

        for (int i = 0; i < implemented; ++i) {
            SCOPED_TRACE("param_index=" + std::to_string(i));
            const EngineParameterDef expected = engine->getParameterDef(i);

            DefOut got;
            ASSERT_TRUE(fetchDef(mWma, engineType, i, got));

            EXPECT_STREQ(got.name, expected.name);
            EXPECT_STREQ(got.shortName, expected.shortName);
            // NUMERICO, no textual: ver el encabezado de este archivo.
            EXPECT_FLOAT_EQ(got.minValue, expected.minValue);
            EXPECT_FLOAT_EQ(got.maxValue, expected.maxValue);
            EXPECT_FLOAT_EQ(got.defaultValue, expected.defaultValue);
        }
    }
}

TEST_F(CApiEngineParamsTest, TheCatalogMatchesTheDeclaredValues) {
    int seen = 0;
    for (const ExpectedParam& expected : kExpectedCatalog) {
        SCOPED_TRACE("engine_type=" + std::to_string(expected.engineType) +
                     " param_index=" + std::to_string(expected.index));
        DefOut got;
        ASSERT_TRUE(fetchDef(mWma, expected.engineType, expected.index, got));

        EXPECT_STREQ(got.name, expected.name);
        EXPECT_STREQ(got.shortName, expected.shortName);
        EXPECT_FLOAT_EQ(got.minValue, expected.minValue);
        EXPECT_FLOAT_EQ(got.maxValue, expected.maxValue);
        EXPECT_FLOAT_EQ(got.defaultValue, expected.defaultValue);
        ++seen;
    }
    EXPECT_EQ(seen, kExpectedTotalParams);

    // Y el catalogo declarado es COMPLETO: si un engine gana un parametro, el
    // conteo se despega y este test lo dice, en vez de seguir verde revisando
    // menos.
    int published = 0;
    for (int engineType : kSynthEngineTypes) {
        published += wma_engine_get_parameter_count(mWma, engineType);
    }
    EXPECT_EQ(published, kExpectedTotalParams);
}

// ===========================================================================
// AC-028.1 — la metadata de X sale con Y activo
// ===========================================================================

TEST_F(CApiEngineParamsTest, MetadataOfAnEngineArrivesWhileAnotherIsActive) {
    // El engine activo arranca en CLASSIC y se mueve a Karplus-Strong: en
    // ninguno de los dos momentos el engine consultado es el activo.
    ASSERT_EQ(wma_get_engine_type(mWma), kClassic);

    DefOut fromClassic;
    ASSERT_TRUE(fetchDef(mWma, kGranular, 0, fromClassic));
    EXPECT_STREQ(fromClassic.name, "Grain Size");
    EXPECT_EQ(wma_engine_get_parameter_count(mWma, kGranular), 3);

    wma_set_engine_type(mWma, kKarplusStrong);
    ASSERT_EQ(wma_get_engine_type(mWma), kKarplusStrong);

    DefOut fromKs;
    ASSERT_TRUE(fetchDef(mWma, kGranular, 0, fromKs));
    EXPECT_STREQ(fromKs.name, "Grain Size");
    EXPECT_FLOAT_EQ(fromKs.defaultValue, 0.3f);
    EXPECT_EQ(wma_engine_get_parameter_count(mWma, kGranular), 3);

    // Y sale la de X, no la del activo.
    EXPECT_STRNE(fromKs.name, "Brightness");
}

TEST_F(CApiEngineParamsTest, MetadataArrivesWithoutStartingTheEngine) {
    // El motor nunca se arranco en este test: los engines se construyen en el
    // constructor del dispatcher, asi que la metadata no depende de prepare(),
    // de start(), ni de que haya un backend corriendo.
    for (int engineType : kSynthEngineTypes) {
        SCOPED_TRACE("engine_type=" + std::to_string(engineType));
        EXPECT_GT(wma_engine_get_parameter_count(mWma, engineType), 0);
        DefOut got;
        EXPECT_TRUE(fetchDef(mWma, engineType, 0, got));
    }
}

// ===========================================================================
// AC-028.5 — el rechazo es DISTINGUIBLE de un dato
// ===========================================================================

TEST_F(CApiEngineParamsTest, AnOutOfRangeIndexIsRejectedAndTouchesNothing) {
    for (int engineType : kSynthEngineTypes) {
        SCOPED_TRACE("engine_type=" + std::to_string(engineType));
        const int count = wma_engine_get_parameter_count(mWma, engineType);
        ASSERT_GT(count, 0);

        // El primer indice invalido, el negativo, y uno bien afuera.
        for (int badIndex : {count, -1, -999, 4096}) {
            SCOPED_TRACE("param_index=" + std::to_string(badIndex));
            DefOut got;
            EXPECT_FALSE(fetchDef(mWma, engineType, badIndex, got));
            got.expectUntouched();
        }
    }
}

TEST_F(CApiEngineParamsTest, AnInvalidEngineTypeIsRejectedAndTouchesNothing) {
    // CLASSIC no es un SynthEngine; el resto no existe.
    for (int badType : {kClassic, -1, 7, 999}) {
        SCOPED_TRACE("engine_type=" + std::to_string(badType));
        EXPECT_EQ(wma_engine_get_parameter_count(mWma, badType), 0);

        DefOut got;
        EXPECT_FALSE(fetchDef(mWma, badType, 0, got));
        got.expectUntouched();
    }
}

TEST_F(CApiEngineParamsTest, TheUnknownSentinelNeverCrossesTheBorder) {
    // 🔴 EL PUNTO DE AC-028.5, y la razon por la que no alcanza con mirar el
    // booleano: los seis overrides devuelven un ("Unknown", ...) para un paramId
    // fuera de rango — y NO todos el mismo, SoundFontEngine devuelve
    // {"Unknown", "???", 0, 1, 0.5} contra el {"Unknown", "?", 0, 1, 0} de los
    // otros cinco. Propagado tal cual, un indice invalido sale como una medicion
    // perfectamente formada, que es la clase del (0, largo) de
    // `find_content_bounds` de MINI-016.
    //
    // El chequeo mira el CONTENIDO, no la forma: cualquier "Unknown" que llegue
    // hasta aca es rojo, venga con "?" o con "???".
    for (int engineType : kSynthEngineTypes) {
        const int count = wma_engine_get_parameter_count(mWma, engineType);
        for (int badIndex : {count, count + 1, -1}) {
            SCOPED_TRACE("engine_type=" + std::to_string(engineType) +
                         " param_index=" + std::to_string(badIndex));
            const char* name = nullptr;
            const char* shortName = nullptr;
            float lo = 0.0f, hi = 0.0f, def = 0.0f;
            const bool ok = wma_engine_get_parameter_def(
                mWma, engineType, badIndex, &name, &shortName, &lo, &hi, &def);
            EXPECT_FALSE(ok);
            if (name != nullptr) {
                EXPECT_STRNE(name, "Unknown")
                    << "el centinela del override cruzo la frontera con cara de dato";
            }
        }
    }

    // Y el centinela tampoco puede llegar por un indice VALIDO: ningun parametro
    // publicado se llama "Unknown".
    for (int engineType : kSynthEngineTypes) {
        const int count = wma_engine_get_parameter_count(mWma, engineType);
        for (int i = 0; i < count; ++i) {
            DefOut got;
            ASSERT_TRUE(fetchDef(mWma, engineType, i, got));
            EXPECT_STRNE(got.name, "Unknown");
        }
    }
}

// ===========================================================================
// Contrato de handle nulo — la convencion del resto de la C API
// ===========================================================================

TEST_F(CApiEngineParamsTest, ANullHandleIsRejectedLikeEverywhereElse) {
    EXPECT_EQ(wma_engine_get_parameter_count(nullptr, kKarplusStrong), 0);

    DefOut got;
    EXPECT_FALSE(fetchDef(nullptr, kKarplusStrong, 0, got));
    got.expectUntouched();
}

TEST_F(CApiEngineParamsTest, NullOutParamsAreToleratedIndividually) {
    // Un consumidor que solo quiere el rango no deberia tener que declarar las
    // cinco variables. Mismo criterio que `wma_sf_get_preset_key_range`.
    float lo = kPoisonFloat, hi = kPoisonFloat;
    EXPECT_TRUE(wma_engine_get_parameter_def(mWma, kSupersaw, 0,
                                             nullptr, nullptr, &lo, &hi, nullptr));
    EXPECT_FLOAT_EQ(lo, 0.0f);
    EXPECT_FLOAT_EQ(hi, 1.0f);
}

}  // namespace
}  // namespace wma_test
