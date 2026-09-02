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
 * contestarla apareció que **la tasa del SoundFont se fijaba en la carga y nada la
 * volvía a tocar**: `tsf_set_output` tenía un único call site y `prepare()` no
 * llegaba hasta el manager. O sea que el eslabón sin observable no era el problema
 * — el problema era que *cualquier* divergencia posterior quedaba muda.
 *
 * **Arreglado el mismo día.** `SoundFontEngine::prepare()` ahora re-configura vía
 * `SoundFontManager::setOutputSampleRate()`, con la misma disciplina de swap que
 * la carga. Los tests viven en la sección "RE-RATE" de abajo, y el de
 * caracterización que documentaba el defecto se borró en vez de relajarse.
 *
 * ## Y renderizar por primera vez destapó un fixture inválido
 *
 * Esta suite cargaba y leía metadata; ninguno de sus tests había RENDERIZADO. El
 * primero que tocó una nota se llevó un `heap-buffer-overflow` de ASan en
 * `tsf_voice_render`. No era del motor: al fixture le faltaban los **46 sample
 * points en cero** que el spec de SF2 exige después de cada sample, y la
 * interpolación de tsf lee `pos + 1`. Se comprobó que no dependía del re-rate
 * —con la misma tasa, sin copia ni swap, desbordaba igual— y se arregló en
 * `support/MinimalSoundFont.h`, que además dejó de generar puro silencio.
 */

#include "support/MinimalSoundFont.h"

#include "engines/SoundFontEngine.h"
#include "engines/SoundFontManager.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <thread>
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
// RE-RATE — el stream puede cambiar de tasa DESPUÉS de la carga
// ===========================================================================
//
// Hasta 2026-07-28 esta sección era un test de caracterización: `tsf_set_output()`
// se llamaba en un solo lugar (`configurAndSwap`, o sea sólo al cargar) y
// `SoundFontEngine::prepare()` no llegaba al manager, así que la tasa del
// SoundFont quedaba clavada en la de la carga. Si el stream abría o REABRÍA a
// otra tasa, el font quedaba desafinado en silencio — ni error, ni log.
//
// Los dos caminos que lo alcanzaban:
//
//  1. Cargar antes de arrancar. `AudioEngine::currentSampleRate()` cae a 48000 sin
//     stream abierto; en un equipo que después negocia 44100 el font renderiza a
//     48000 dentro de un stream de 44100 — ~1.5 semitonos alto, para siempre.
//  2. Reabrir el stream a otra tasa. `AudioEngine::start()` ya tiene el caso
//     explícito ("Device coerced sample rate X -> Y, re-configuring components"),
//     y en iOS pedir captura reabre la sesión, que con un manos libres Bluetooth
//     puede caer a HFP (16 kHz).
//
// Ahora `prepare()` sí re-configura, con la MISMA disciplina de swap que la carga:
// nunca se muta el `tsf` desde el que está renderizando el hilo de audio.

/** La tasa nueva llega al font por el mismo camino que usa el motor. */
TEST_F(SoundFontLoadTest, PreparingTheEngineAtANewRateReRatesTheFont) {
    ASSERT_TRUE(mManager.loadFromMemory(mSf2.data(), static_cast<int>(mSf2.size()), 48000));
    ASSERT_EQ(mManager.getSampleRate(), 48000);

    // Exactamente lo que hace el motor cuando el stream abre o reabre a otra tasa.
    SoundFontEngine engine;
    engine.setSoundFontManager(&mManager);
    engine.prepare(44100, 256);

    EXPECT_EQ(mManager.getSampleRate(), 44100);
}

/**
 * **El font se reemplaza, no se muta.**
 *
 * Es la mitad de seguridad del arreglo, y la única forma de verla desde afuera es
 * el puntero: `render()` toma `getActiveSF()` UNA vez por bloque, así que cambiar
 * de puntero entre bloques es seguro y escribirle encima al que el hilo de audio
 * está usando es una carrera. Si este test viera el mismo puntero con otra tasa,
 * el arreglo estaría mutando en vivo.
 */
TEST_F(SoundFontLoadTest, ReRatingSwapsTheFontInsteadOfMutatingTheLiveOne) {
    ASSERT_TRUE(mManager.loadFromMemory(mSf2.data(), static_cast<int>(mSf2.size()), 48000));
    tsf* before = mManager.getActiveSF();
    ASSERT_NE(before, nullptr);

    SoundFontEngine engine;
    engine.setSoundFontManager(&mManager);
    engine.prepare(44100, 256);

    EXPECT_NE(mManager.getActiveSF(), before)
        << "la tasa cambió sobre el mismo tsf: eso es mutar lo que el hilo de audio lee";
}

/**
 * **Preparar a la MISMA tasa no puede swapear.**
 *
 * No es una optimización: `prepare()` corre en cada `start()`, y todo swap tira las
 * voces que estuvieran sonando (el `tsf_copy` viene sin voces). Swapear cuando no
 * cambió nada convertiría cada arranque en un corte. El comentario de
 * `AudioEngine::start()` ya declara que reconfigurar es idempotente para tasas
 * iguales; esto lo sostiene.
 */
TEST_F(SoundFontLoadTest, PreparingAtTheSameRateIsANoOp) {
    ASSERT_TRUE(mManager.loadFromMemory(mSf2.data(), static_cast<int>(mSf2.size()), 48000));
    tsf* before = mManager.getActiveSF();

    SoundFontEngine engine;
    engine.setSoundFontManager(&mManager);
    engine.prepare(48000, 256);

    EXPECT_EQ(mManager.getActiveSF(), before) << "swapeó sin que la tasa cambiara";
    EXPECT_EQ(mManager.getSampleRate(), 48000);
}

/**
 * **El font re-rateado todavía SUENA.**
 *
 * `tsf_copy()` devuelve la copia con `voices = NULL` y `voiceNum = 0`: comparte
 * presets y muestras, pero no las voces. Un arreglo que se olvide de re-hacer
 * `tsf_set_max_voices()` deja un SoundFont que carga, reporta sus presets y la
 * tasa correcta... y renderiza silencio. Ninguno de los tests de arriba lo vería.
 *
 * Por eso acá se toca una nota y se mide que salga señal.
 */
TEST_F(SoundFontLoadTest, TheReRatedFontStillProducesAudio) {
    ASSERT_TRUE(mManager.loadFromMemory(mSf2.data(), static_cast<int>(mSf2.size()), 48000));

    SoundFontEngine engine;
    engine.setSoundFontManager(&mManager);
    engine.prepare(44100, 256);

    tsf* sf = mManager.getActiveSF();
    ASSERT_NE(sf, nullptr);
    ASSERT_EQ(tsf_get_presetcount(sf), 1) << "la copia perdió los presets";

    tsf_note_on(sf, 0, 60, 1.0f);

    std::vector<float> block(256 * 2, 0.0f);
    tsf_render_float(sf, block.data(), 256, 0);

    float peak = 0.0f;
    for (float s : block) peak = std::max(peak, std::fabs(s));
    EXPECT_GT(peak, 0.0f) << "el font re-rateado no renderiza: la copia se quedó sin voces";
}

/**
 * **Re-ratear MIENTRAS el hilo de audio renderiza.**
 *
 * Es el único test de esta suite que corre dos hilos, y existe porque la
 * afirmación "reemplaza en vez de mutar" es una afirmación de concurrencia: los
 * tests de puntero de arriba la miran desde un solo hilo y no pueden verla
 * fallar. El hilo lector hace exactamente lo que hace `SoundFontEngine::render()`
 * —`getActiveSF()` una vez, después renderiza con ese puntero— así que un
 * `tsf_set_output()` sobre el font vivo aparece acá como carrera sobre
 * `outSampleRate`, y un retiro mal hecho como use-after-free.
 *
 * **Vale poco sin sanitizers y mucho con ellos**: en una corrida normal esto casi
 * siempre pasa aunque haya una carrera. Los jobs `cpp-tests-asan` y
 * `cpp-tests-tsan` son los que le dan sentido.
 *
 * La cadencia es deliberadamente realista: se renderiza un bloque entre cambios
 * de tasa. En producción un cambio de tasa exige reabrir el stream, así que dos
 * seguidos dentro del mismo bloque de audio no ocurren.
 */
TEST_F(SoundFontLoadTest, ReRatingWhileTheAudioThreadRendersIsSafe) {
    ASSERT_TRUE(mManager.loadFromMemory(mSf2.data(), static_cast<int>(mSf2.size()), 48000));

    SoundFontEngine engine;
    engine.setSoundFontManager(&mManager);

    std::atomic<bool> stop{false};
    std::atomic<int> blocks{0};

    std::thread audio([&] {
        std::vector<float> buf(128 * 2, 0.0f);
        while (!stop.load(std::memory_order_acquire)) {
            // Igual que render(): adquirir con el hazard pointer, usar, soltar.
            // Usar `getActiveSF()` acá sería modelar mal el motor — y de hecho
            // fue así como este test destapó el use-after-free del esquema de
            // retiro viejo, en el TSan de Linux.
            tsf* sf = mManager.acquireActive();
            if (sf) {
                tsf_render_float(sf, buf.data(), 128, 0);
                blocks.fetch_add(1, std::memory_order_relaxed);
            }
            mManager.releaseActive();
        }
    });

    // Esperar a que el lector esté de verdad adentro antes de empezar a swapear.
    while (blocks.load(std::memory_order_relaxed) < 10) { /* spin */ }

    for (int i = 0; i < 20; ++i) {
        engine.prepare(i % 2 == 0 ? 44100 : 48000, 128);
        const int seen = blocks.load(std::memory_order_relaxed);
        while (blocks.load(std::memory_order_relaxed) < seen + 2) { /* un bloque entero */ }
    }

    stop.store(true, std::memory_order_release);
    audio.join();

    EXPECT_TRUE(mManager.isLoaded());
    EXPECT_GT(blocks.load(std::memory_order_relaxed), 0);
}

/** Sin font cargado no hay nada que re-ratear, y no puede explotar. */
TEST_F(SoundFontLoadTest, PreparingWithNoFontLoadedIsHarmless) {
    SoundFontEngine engine;
    engine.setSoundFontManager(&mManager);

    engine.prepare(44100, 256);

    EXPECT_FALSE(mManager.isLoaded());
    EXPECT_EQ(mManager.getActiveSF(), nullptr);
}

}  // namespace

// ===========================================================================
// MINI-017 — el rango de teclas sale de las REGIONES, no del nombre
// ===========================================================================
//
// 🔴 EL DISEÑO DE ESTOS TESTS ES EL PUNTO, no las aserciones.
//
// Antes de MINI-017 el rango lo adivinaba `inferKeyRange` del NOMBRE del preset,
// con una cadena de `strstr`. Un fixture que declarara el rango que la heurística
// habría dado mediría **la heurística contra sí misma** y pasaría en verde con el
// defecto intacto — es lo que le pasaba al fixture del arnés de REQ-024
// ("Cello Uno" -> 36..84, que es exactamente lo que `strstr("cello")` devolvía).
//
// Por eso el fixture escribe un `keyRange` que **CONTRADICE** a la heurística: el
// preset se llama "Test Preset", que no matchea ninguna de sus diez ramas y caía
// en el fallback 21..108. Cualquier valor distinto de 21..108 separa las dos
// hipótesis de una.

TEST_F(SoundFontLoadTest, TheKeyRangeComesFromTheFileNotFromThePresetName) {
    // 40..70 no es lo que la heurística daba para "Test Preset" (21..108), ni el
    // default de región de tsf (0..127). Los tres son distinguibles entre sí.
    mSf2 = makeMinimalSoundFont(kSampleRateInFile, /*looping=*/false,
                                /*keyRangeLo=*/40, /*keyRangeHi=*/70);
    ASSERT_TRUE(mManager.loadFromMemory(mSf2.data(), static_cast<int>(mSf2.size()), 48000));

    int lo = -1, hi = -1;
    ASSERT_TRUE(mManager.getPresetKeyRange(0, lo, hi));
    EXPECT_EQ(lo, 40) << "el rango no salió del generador keyRange del archivo";
    EXPECT_EQ(hi, 70) << "el rango no salió del generador keyRange del archivo";
}

TEST_F(SoundFontLoadTest, ADifferentDeclaredRangeGivesADifferentAnswer) {
    // El gemelo obligatorio: sin él, un getter cableado a 40..70 pasa el test de
    // arriba. Dos rangos distintos sobre el MISMO nombre de preset es lo que
    // prueba que el nombre dejó de decidir.
    mSf2 = makeMinimalSoundFont(kSampleRateInFile, /*looping=*/false, 55, 103);
    ASSERT_TRUE(mManager.loadFromMemory(mSf2.data(), static_cast<int>(mSf2.size()), 48000));

    int lo = -1, hi = -1;
    ASSERT_TRUE(mManager.getPresetKeyRange(0, lo, hi));
    EXPECT_EQ(lo, 55);
    EXPECT_EQ(hi, 103);
}

TEST_F(SoundFontLoadTest, APresetWithoutADeclaredRangeReportsTheRegionDefault) {
    // Sin generador `keyRange`, la región de tsf nace en 0..127
    // (`tsf_region_clear`). Ése es el rango REAL de ese preset — toca todas las
    // teclas— y es lo que hay que publicar. La heurística devolvía 21..108, que
    // era una opinión sobre un archivo que no la pedía.
    ASSERT_TRUE(mManager.loadFromMemory(mSf2.data(), static_cast<int>(mSf2.size()), 48000));

    int lo = -1, hi = -1;
    ASSERT_TRUE(mManager.getPresetKeyRange(0, lo, hi));
    EXPECT_EQ(lo, 0);
    EXPECT_EQ(hi, 127);
    EXPECT_NE(lo, 21) << "21..108 es el fallback de la heurística: no debería poder salir más";
}

TEST_F(SoundFontLoadTest, AnOutOfRangePresetIndexIsStillRejected) {
    // El contrato de rechazo no se movió, y hay que decirlo: el cambio de fuente
    // no puede haber convertido un índice inválido en un dato.
    ASSERT_TRUE(mManager.loadFromMemory(mSf2.data(), static_cast<int>(mSf2.size()), 48000));

    int lo = -1, hi = -1;
    EXPECT_FALSE(mManager.getPresetKeyRange(1, lo, hi));
    EXPECT_FALSE(mManager.getPresetKeyRange(-1, lo, hi));
    EXPECT_EQ(lo, -1) << "un rechazo no puede tocar los out-params";
    EXPECT_EQ(hi, -1);
}

}  // namespace wma_test
