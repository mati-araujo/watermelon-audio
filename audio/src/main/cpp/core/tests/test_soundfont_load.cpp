/**
 * test_soundfont_load.cpp
 *
 * El camino de ÉXITO de los tres cargadores de SoundFont — la deuda que quedó
 * abierta del bug 3 de WA-2.0. Hasta acá sólo estaban cubiertos los caminos
 * negativos (`test_c_api_synth.cpp`), y un loader que falla no configura
 * ninguna tasa: la tasa de salida sólo es observable después de una carga que
 * funcione, y para eso hacía falta un .sf2 de verdad.
 *
 * El fixture se genera en memoria — ver `support/MinimalSoundFont.h` para qué
 * exige `tsf_load` y por qué no se commitea un binario.
 *
 * ## Qué afirma esta suite, y qué NO
 *
 * Afirma que `SoundFontManager` **aplica la tasa que le pasan** por los tres
 * caminos (memoria, path, fd), y que esa tasa es la de SALIDA, distinta de la
 * del sample que viene en el `shdr` del archivo. Confundir esas dos es
 * exactamente el error que el bug 3 hacía fácil.
 *
 * NO afirma que `AudioEngine::loadSoundFont*` le pase `currentSampleRate()`.
 * Ese eslabón no es observable desde afuera: `SoundFontManager::getSampleRate()`
 * existe pero `AudioEngine` no expone el `SynthEngineDispatcher` que lo
 * contiene, y agregar un accesor cuyo único cliente sea este test es
 * precisamente lo que el encabezado de `test_c_api_synth.cpp` desaconseja. Se
 * deja dicho acá en vez de escribir una assertion que no lo cubra: hoy ese
 * eslabón lo sostienen la suite de `currentSampleRate()` y la revisión del
 * diff, y son tres líneas en `AudioEngine.cpp`.
 *
 * ## Actualización 2026-07-28 — mirar esas tres líneas destapó algo más grande
 *
 * La pregunta era si valía exponer la tasa para poder testear el eslabón. Al ir a
 * contestarla apareció que **la tasa del SoundFont se fija en la carga y nada la
 * vuelve a tocar**: `tsf_set_output` tiene un único call site y `prepare()` no
 * llega hasta el manager. O sea que el eslabón sin observable no era el problema
 * — el problema es que *cualquier* divergencia posterior queda muda.
 *
 * Está pinchado abajo, en `PreparingTheEngineAtANewRateDoesNotReRateTheFont`, como
 * test de **caracterización**: pasa hoy y tiene que fallar cuando se arregle.
 */

#include "support/MinimalSoundFont.h"

#include "engines/SoundFontEngine.h"
#include "engines/SoundFontManager.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace wma_test {
namespace {

/** La tasa que va en el `shdr` del archivo. Deliberadamente != a las de salida. */
constexpr uint32_t kSampleRateInFile = 22050;

/** Archivo temporal que se borra solo, con un prefijo opcional adelante. */
class TempSf2File {
public:
    explicit TempSf2File(const std::vector<uint8_t>& sf2, size_t prefixBytes = 0)
        : mOffset(prefixBytes) {
        char tmpl[] = "/tmp/wma_sf2_XXXXXX";
        mFd = ::mkstemp(tmpl);
        mPath = tmpl;
        if (mFd < 0) return;
        if (prefixBytes > 0) {
            // Basura antes del .sf2: modela un asset embebido en un APK, que es
            // el caso real de loadFromFd en Android y el único que ejercita el
            // alineado de la región de mmap de punta a punta.
            std::vector<uint8_t> junk(prefixBytes, 0xAB);
            ::write(mFd, junk.data(), junk.size());
        }
        ::write(mFd, sf2.data(), sf2.size());
        ::lseek(mFd, 0, SEEK_SET);
    }

    ~TempSf2File() {
        if (mFd >= 0) ::close(mFd);
        if (!mPath.empty()) ::unlink(mPath.c_str());
    }

    int fd() const { return mFd; }
    const char* path() const { return mPath.c_str(); }
    int64_t offset() const { return static_cast<int64_t>(mOffset); }

private:
    int mFd = -1;
    std::string mPath;
    size_t mOffset = 0;
};

class SoundFontLoadTest : public ::testing::Test {
protected:
    void SetUp() override {
        wma::setLogCallback([](wma::LogLevel, const char*, const char*) {});
        mSf2 = makeMinimalSoundFont(kSampleRateInFile);
    }
    void TearDown() override { wma::setLogCallback(nullptr); }

    std::vector<uint8_t> mSf2;
    SoundFontManager mManager;
};

// ===========================================================================
// El fixture, antes que nada
// ===========================================================================

TEST_F(SoundFontLoadTest, TheGeneratedFontIsActuallyLoadable) {
    // Si esto falla, todo lo de abajo es ruido: no estaría midiendo el
    // comportamiento del manager sino un archivo mal armado.
    ASSERT_TRUE(mManager.loadFromMemory(mSf2.data(), static_cast<int>(mSf2.size()), 48000));
    EXPECT_TRUE(mManager.isLoaded());
    EXPECT_EQ(mManager.getPresetCount(), 1);
}

// ===========================================================================
// La tasa de salida: lo que el bug 3 dejaba mal
// ===========================================================================

TEST_F(SoundFontLoadTest, LoadFromMemoryAppliesTheRateItIsGiven) {
    ASSERT_TRUE(mManager.loadFromMemory(mSf2.data(), static_cast<int>(mSf2.size()), 44100));

    EXPECT_EQ(mManager.getSampleRate(), 44100);
}

TEST_F(SoundFontLoadTest, LoadFromPathAppliesTheRateItIsGiven) {
    TempSf2File file(mSf2);
    ASSERT_GE(file.fd(), 0);

    ASSERT_TRUE(mManager.loadFromPath(file.path(), 44100));

    EXPECT_EQ(mManager.getSampleRate(), 44100);
    EXPECT_EQ(mManager.getPresetCount(), 1);
}

TEST_F(SoundFontLoadTest, LoadFromFdAppliesTheRateItIsGiven) {
    // Offset deliberadamente NO alineado a página: es el caso que
    // computeSoundFontMmapRegion existe para resolver, y hasta ahora sólo
    // estaba cubierto como aritmética pura (test_soundfont_fd_region.cpp).
    // Acá se recorre entero, con un mmap real.
    TempSf2File file(mSf2, /*prefixBytes=*/1234);
    ASSERT_GE(file.fd(), 0);

    ASSERT_TRUE(mManager.loadFromFd(file.fd(), file.offset(),
                                    static_cast<int64_t>(mSf2.size()), 44100));

    EXPECT_EQ(mManager.getSampleRate(), 44100);
    EXPECT_EQ(mManager.getPresetCount(), 1);
}

/**
 * La que pincha la confusión del bug 3. `shdr.sampleRate` es la tasa a la que
 * se grabó el sample; la de `tsf_set_output` es a la que el motor renderiza.
 * Un cargador que copiara la primera en la segunda pasaría todos los tests de
 * arriba —cargan, tienen preset— y sonaría desafinado.
 */
TEST_F(SoundFontLoadTest, TheOutputRateIsNotTheSampleRateFromTheFile) {
    // 44100 y no 48000, y el motivo es una detección real: con 48000 este test
    // sobrevivía al mutante que hardcodea `tsf_set_output(..., 48000, ...)` —el
    // bug 3 exacto— porque el valor pedido y el hardcodeado coincidían. 48000 es
    // el default de medio mundo; usarlo en una assertion de "aplicó lo que le
    // pedí" es elegir el único número que no distingue.
    ASSERT_TRUE(mManager.loadFromMemory(mSf2.data(), static_cast<int>(mSf2.size()), 44100));

    EXPECT_EQ(mManager.getSampleRate(), 44100);
    EXPECT_NE(mManager.getSampleRate(), static_cast<int32_t>(kSampleRateInFile));
}

/**
 * Recargar tiene que reconfigurar. Es el caso de un device que renegocia la
 * tasa sin reiniciar la app: si el segundo load conservara la tasa del primero,
 * el SoundFont quedaría a la tasa vieja — que es el bug 3 con otra ropa.
 */
TEST_F(SoundFontLoadTest, ReloadingAtANewRateReplacesTheOldOne) {
    ASSERT_TRUE(mManager.loadFromMemory(mSf2.data(), static_cast<int>(mSf2.size()), 48000));
    ASSERT_EQ(mManager.getSampleRate(), 48000);

    ASSERT_TRUE(mManager.loadFromMemory(mSf2.data(), static_cast<int>(mSf2.size()), 44100));

    EXPECT_EQ(mManager.getSampleRate(), 44100);
}

// ===========================================================================
// CARACTERIZACIÓN — la tasa queda clavada en la carga, y nada la re-configura
// ===========================================================================

/**
 * **Esto documenta un defecto latente, no un contrato deseable.**
 *
 * `tsf_set_output()` se llama en **un solo lugar** de todo el motor:
 * `SoundFontManager::configurAndSwap`, o sea únicamente al cargar. Y
 * `SoundFontEngine::prepare()` —que es lo que corre cuando se abre o se REABRE el
 * stream— sólo delega en `SynthEngine::prepare` y **no toca el manager**. O sea que
 * la tasa a la que renderiza el SoundFont se fija en el instante de la carga y no
 * vuelve a moverse nunca.
 *
 * Consecuencia: si la tasa del stream difiere de la que valía al cargar, el
 * SoundFont queda desafinado **en silencio** — ni error, ni log, ni forma de verlo
 * desde afuera. Dos caminos concretos, medidos por lectura:
 *
 * 1. **Cargar antes de arrancar.** `AudioEngine::currentSampleRate()` cae a 48000
 *    cuando no hay stream (`AudioEngine.cpp`, el fallback final). En un dispositivo
 *    que después negocia 44100, el font renderiza 48000 dentro de un stream de
 *    44100: ~1.5 semitonos alto, para siempre.
 * 2. **Reabrir el stream a otra tasa.** Es el caso serio en iOS: pedir captura
 *    reabre la sesión (WA-2.6), y `AVAudioSession` puede caer a HFP —16 kHz— al
 *    conectar un manos libres Bluetooth. `prepare()` re-configura todos los demás
 *    engines con la tasa nueva; el SoundFont se queda con la vieja.
 *
 * **Este test pasa hoy y tiene que FALLAR el día que alguien lo arregle.** Cuando
 * eso pase, borrarlo y poner el inverso — no relajarlo.
 */
TEST_F(SoundFontLoadTest, PreparingTheEngineAtANewRateDoesNotReRateTheFont) {
    ASSERT_TRUE(mManager.loadFromMemory(mSf2.data(), static_cast<int>(mSf2.size()), 48000));
    ASSERT_EQ(mManager.getSampleRate(), 48000);

    // Exactamente lo que hace el motor cuando el stream abre o reabre a otra tasa.
    SoundFontEngine engine;
    engine.setSoundFontManager(&mManager);
    engine.prepare(44100, 256);

    EXPECT_EQ(mManager.getSampleRate(), 48000)
        << "Si esto ahora da 44100, el re-rate se implementó: borrar este test de "
           "caracterización y afirmar el comportamiento nuevo.";
}

}  // namespace
}  // namespace wma_test
