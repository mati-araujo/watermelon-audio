/**
 * test_c_api_tuner_dump.cpp — el volcado a WAV de la entrada del afinador, POR LA C API (REQ-055 S0 · 0.1).
 *
 * AC-055.1 — el corpus se captura por el camino del afinador. Los tests del nucleo
 * (`analysis/tests/test_input_dump.cpp`) prueban el volcado contra un ring; ACA se prueba lo que
 * solo la C API puede: que el volcado esta enganchado AL LUGAR CORRECTO del camino de captura.
 *
 *   FIDELIDAD DE PUNTA A PUNTA. Se alimentan bloques por `AudioEngine::onAudioReady(out, in, n)` con
 *   `in` no nulo (el camino `feedExternalInput` del `InputNode`, el mismo que usa el backend USB), se
 *   lee el WAV de DISCO, y se lo compara BIT A BIT contra un SEGUNDO `InputNode` identico (mismo
 *   prepare, mismo rate) alimentado con los mismos bloques y drenado de SU propio `AnalysisRing`.
 *   Eso mata las dos formas realistas de enganchar mal el volcado:
 *     - un tap ANTES del DSP de entrada (ganancia, DC blocker): el WAV seria la senal cruda del
 *       conversor y no lo que el afinador analiza;
 *     - un tap en otro lado que no sea el ring (el monitoreo, el scratch del analisis).
 *   Para que la comparacion DISCRIMINE se afirma una premisa: la salida del DSP difiere de la
 *   senal cruda (ganancia 12 dB y DC blocker), asi que un tap previo no podria pasar por casualidad.
 *
 * NINGUN RATE DE PRUEBA ES 48000 (la constante que estaba cableada): 44 100 y 32 000.
 *
 * COMO SE SINCRONIZA (REQ-002): el escritor y el analisis son hilos; se espera CONDICIONES
 * (`framesQueued()` alcanzo lo alimentado) y se alimenta de a 1024 frames para que el ring de 8192
 * nunca pise lo que el volcado todavia no copio.
 */

#include "tests/support/TestWait.h"
#include "support/CApiFixture.h"

#include "api/watermelon_audio.h"
#include "api/watermelon_audio_internal.h"
#include "analysis/AnalysisRing.h"
#include "analysis/AnalysisThread.h"
#include "analysis/TunerInputDump.h"
#include "nodes/InputNode.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace wma_test {
namespace {

namespace fs = std::filesystem;

constexpr int kRate44 = 44100;
constexpr int kRate32 = 32000;
constexpr int kBlockFrames = 256;
constexpr int kChunkBlocks = 4;                              // 1024 frames por tanda
constexpr int kChunkFrames = kBlockFrames * kChunkBlocks;
constexpr size_t kHeaderBytes = 106;                         // header fijo del volcado
constexpr int64_t kSentinel64 = -7777777777777LL;            // no es 0 ni un contador plausible

/// Empuja el rate negociado hasta el motor, por el mismo camino que un device (ver
/// test_c_api_tuner.cpp: es una copia deliberada, esa funcion es privada de su archivo).
void negotiateCaptureRate(WmaEngine* e, int hz) {
    watermelon_audio::StreamInfo info{};
    info.sampleRate = hz;
    info.channelCount = 2;
    e->engine->onStreamConfigChanged(info);
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

uint64_t rdLE(const std::vector<uint8_t>& b, size_t off, int bytes) {
    uint64_t v = 0;
    for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | b[off + static_cast<size_t>(i)];
    return v;
}

std::array<int64_t, WMA_TUNER_DUMP_STATS_VALUES> sentinelStats() {
    std::array<int64_t, WMA_TUNER_DUMP_STATS_VALUES> v{};
    v.fill(kSentinel64);
    return v;
}

bool allSentinels(const std::array<int64_t, WMA_TUNER_DUMP_STATS_VALUES>& v) {
    for (int64_t x : v) {
        if (x != kSentinel64) return false;
    }
    return true;
}

/**
 * El fixture: el motor sobre el backend falso, mas un directorio temporal propio y un
 * `InputNode` de REFERENCIA que recibe los mismos bloques que el del motor.
 */
class TunerDumpApiTest : public CApiFixture {
protected:
    void SetUp() override {
        CApiFixture::SetUp();
        if (HasFatalFailure()) return;
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        mDir = fs::temp_directory_path()
               / ("wma-tunerdump-" + std::to_string(tick) + "-"
                  + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        fs::create_directories(mDir, ec);
        ASSERT_FALSE(ec) << "no pude crear el directorio temporal " << mDir;
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(mDir, ec);
        CApiFixture::TearDown();
    }

    std::string pathOf(const std::string& name) const { return (mDir / name).string(); }
    static bool exists(const std::string& p) { return fs::exists(fs::path(p)); }

    /// Afinador arrancado al `rate` negociado, con la fuente pedida ya consumida por el analisis.
    /// Mismo orden que `TheSnapshotCarriesWhatTheCapturePathActuallyMeasured`.
    void bringUpTuner(int rate, int source = 0) {
        startAt(rate, 0);
        negotiateCaptureRate(mWma, rate);
        ASSERT_TRUE(wma_tuner_start(mWma));
        if (source != 0) {
            wma_input_set_source(mWma, source);
            ASSERT_EQ(wma_input_get_source(mWma), source);
        }
        // El cambio de fuente lo atiende el analisis en su proximo tick. Se espera que lo
        // haya hecho ANTES de armar la toma: si no, la fuente cambiada contaria como una
        // discontinuidad DE LA TOMA (y esa es otra prueba, ver ASourceChange...).
        const uint64_t t0 = mWma->analysisThread->ticks();
        ASSERT_TRUE(waitUntil([&] { return mWma->analysisThread->ticks() >= t0 + 3; }))
            << "el hilo de analisis no esta girando";
    }

    /// Un InputNode IDENTICO al del motor (mismo prepare provisional + reconfigure al rate real,
    /// que es lo que `wmaEnsureInputNode` + `AudioEngine::setInputNode` le hacen), con su propio ring.
    void setUpReference(int rate) {
        mRef = std::make_unique<InputNode>();
        mRef->prepare(48000, 4096);
        ASSERT_TRUE(mRef->reconfigureForRate(rate, std::chrono::milliseconds(250)));
        mRef->setAnalysisRing(&mRefRing);
    }

    /// Empuja `blocks` bloques por el camino del motor (y por el de referencia si existe).
    /// El estimulo: seno + DC + ruido, con L != R, amplitud <= 0,5. El DC y la ganancia de
    /// 12 dB son los que hacen que el DSP de entrada CAMBIE la senal (premisa del test).
    void feed(int blocks) {
        std::vector<float> out(static_cast<size_t>(kBlockFrames) * 2);
        std::vector<float> in(static_cast<size_t>(kBlockFrames) * 2);
        std::vector<float> refOut(static_cast<size_t>(kBlockFrames));
        for (int b = 0; b < blocks; ++b) {
            for (int f = 0; f < kBlockFrames; ++f) {
                const double tone = 0.25 * std::sin(mPhase);
                mPhase += 2.0 * M_PI * 187.5 / 44100.0;
                if (mPhase >= 2.0 * M_PI) mPhase -= 2.0 * M_PI;
                const float l = static_cast<float>(tone + 0.10 + 0.05 * noise());
                const float r = static_cast<float>(tone + 0.10 + 0.05 * noise());
                in[static_cast<size_t>(f) * 2] = l;
                in[static_cast<size_t>(f) * 2 + 1] = r;
                mRaw.push_back(0.5f * (l + r));
            }
            std::fill(out.begin(), out.end(), 0.0f);
            mWma->engine->onAudioReady(out.data(), in.data(), kBlockFrames);
            mFed += kBlockFrames;
            if (mRef) {
                mRef->feedExternalInput(in.data(), kBlockFrames);
                const int got = mRefRing.read(refOut.data(), kBlockFrames);
                EXPECT_EQ(got, kBlockFrames) << "el ring de referencia no entrego el bloque";
                mRefOut.insert(mRefOut.end(), refOut.begin(), refOut.begin() + std::max(got, 0));
            }
        }
    }

    /// Alimenta `chunks` tandas de 1024 frames esperando, tras cada una, a que el volcado las
    /// haya encolado: el ring de 8192 nunca pisa lo que el volcado no copio.
    void feedPaced(int chunks) {
        for (int c = 0; c < chunks; ++c) {
            feed(kChunkBlocks);
            ASSERT_TRUE(waitUntil([&] { return mWma->tunerInputDump->framesQueued() >= mFed; }))
                << "el volcado no encolo lo alimentado (" << mWma->tunerInputDump->framesQueued()
                << " de " << mFed << ")";
        }
    }

    /// Cierra la toma por la C API y devuelve los 7 valores.
    std::array<int64_t, WMA_TUNER_DUMP_STATS_VALUES> stopTake() {
        std::array<int64_t, WMA_TUNER_DUMP_STATS_VALUES> v = sentinelStats();
        EXPECT_TRUE(wma_tuner_diag_input_dump_stop(mWma, v.data(), WMA_TUNER_DUMP_STATS_VALUES));
        return v;
    }

    /// Verifica que el `wmad` del archivo repite lo que la C API devolvio.
    static void expectFileHeaderMatches(const std::vector<uint8_t>& bytes,
                                        const std::array<int64_t, WMA_TUNER_DUMP_STATS_VALUES>& v) {
        ASSERT_GE(bytes.size(), kHeaderBytes);
        EXPECT_EQ(std::memcmp(&bytes[0], "RIFF", 4), 0);
        EXPECT_EQ(std::memcmp(&bytes[8], "WAVE", 4), 0);
        EXPECT_EQ(std::memcmp(&bytes[50], "wmad", 4), 0);
        EXPECT_EQ(rdLE(bytes, 20, 2), 3u) << "IEEE float";
        EXPECT_EQ(rdLE(bytes, 22, 2), 1u) << "mono";
        EXPECT_EQ(rdLE(bytes, 24, 4), static_cast<uint64_t>(v[3])) << "rate del fmt";
        EXPECT_EQ(rdLE(bytes, 62, 8), static_cast<uint64_t>(v[0]));
        EXPECT_EQ(rdLE(bytes, 70, 8), static_cast<uint64_t>(v[1]));
        EXPECT_EQ(rdLE(bytes, 78, 8), static_cast<uint64_t>(v[2]));
        EXPECT_EQ(static_cast<int64_t>(static_cast<int32_t>(rdLE(bytes, 86, 4))), v[3]);
        EXPECT_EQ(static_cast<int64_t>(static_cast<int32_t>(rdLE(bytes, 90, 4))), v[4]);
        EXPECT_EQ(static_cast<int64_t>(rdLE(bytes, 94, 4)), v[5]);
        EXPECT_EQ(v[6], 1) << "el archivo existe con su nombre final: finalized";
    }

    /// La fidelidad de punta a punta, al `rate` pedido. Es el cuerpo de dos tests.
    void runFidelity(int rate, int source) {
        SCOPED_TRACE("rate " + std::to_string(rate));
        ASSERT_NO_FATAL_FAILURE(bringUpTuner(rate, source));
        ASSERT_NO_FATAL_FAILURE(setUpReference(rate));

        const std::string path = pathOf("entrada.wav");
        ASSERT_TRUE(wma_tuner_diag_input_dump_start(mWma, path.c_str()));
        ASSERT_NE(mWma->tunerInputDump, nullptr);
        EXPECT_TRUE(wma_tuner_is_running(mWma)) << "el volcado no puede parar al afinador";

        // 12 tandas = 12288 frames: MAS que el ring entero (8192), asi que la toma tiene que
        // haber copiado de forma sostenida y no solo vaciar el ring una vez.
        ASSERT_NO_FATAL_FAILURE(feedPaced(12));
        ASSERT_EQ(mFed, 12288u);

        const auto v = stopTake();
        EXPECT_EQ(v[0], 12288) << "framesWritten";
        EXPECT_EQ(v[1], 0) << "framesLost";
        EXPECT_EQ(v[2], 0) << "discontinuities";
        EXPECT_EQ(v[3], rate) << "sampleRate: el estampado, no una constante";
        EXPECT_EQ(v[4], source) << "inputSource: la que habia al armar";
        EXPECT_EQ(v[5], 1) << "valid";
        EXPECT_EQ(v[6], 1) << "finalized: el archivo quedo cerrado y nombrado";

        // El archivo: en `path`, sin `.part`, con el tamano exacto.
        EXPECT_TRUE(exists(path));
        EXPECT_FALSE(exists(path + ".part"));
        EXPECT_FALSE(exists(pathOf("entrada.invalid.wav")));
        const std::vector<uint8_t> bytes = readFile(path);
        ASSERT_EQ(bytes.size(), kHeaderBytes + 12288u * sizeof(float));
        expectFileHeaderMatches(bytes, v);

        // PREMISA: el DSP de entrada CAMBIA la senal. Sin esto un tap previo al DSP pasaria.
        ASSERT_EQ(mRefOut.size(), 12288u);
        ASSERT_EQ(mRaw.size(), 12288u);
        float maxDiff = 0.0f;
        for (size_t i = 0; i < mRaw.size(); ++i) {
            maxDiff = std::max(maxDiff, std::fabs(mRefOut[i] - mRaw[i]));
        }
        ASSERT_GT(maxDiff, 0.05f) << "el DSP de entrada no altero la senal: el test no discrimina";

        // EL VEREDICTO: el WAV es, bit a bit, lo que sale del ring de un InputNode identico.
        // memcpy y no reinterpret_cast: los datos arrancan en el offset 106, que no es multiplo de 4.
        std::vector<float> got(12288);
        std::memcpy(got.data(), bytes.data() + kHeaderBytes, got.size() * sizeof(float));
        long mismatch = -1;
        for (size_t i = 0; i < got.size(); ++i) {
            if (std::memcmp(&got[i], &mRefOut[i], sizeof(float)) != 0) {
                mismatch = static_cast<long>(i);
                break;
            }
        }
        EXPECT_EQ(mismatch, -1L) << "el WAV difiere de lo que el afinador ve (frame " << mismatch
                                 << "): el tap no esta despues del DSP de entrada, o no es el ring";
    }

    InputNode& ref() { return *mRef; }

    uint64_t mFed = 0;
    std::vector<float> mRaw;      // 0,5(L+R) de la senal CRUDA: solo para la premisa
    std::vector<float> mRefOut;   // lo que sale del ring del nodo de referencia

private:
    float noise() {
        mSeed = mSeed * 1664525u + 1013904223u;
        return static_cast<float>(mSeed >> 8) / 8388608.0f - 1.0f;
    }

    fs::path mDir;
    std::unique_ptr<InputNode> mRef;
    wma::analysis::AnalysisRing mRefRing;
    double mPhase = 0.0;
    uint32_t mSeed = 777u;
};

}  // namespace

// ---------------------------------------------------------------------------
// FIDELIDAD
// ---------------------------------------------------------------------------

/**
 * AC-055.1 — EL WAV ES LO QUE EL AFINADOR VE, A 44 100 Y CON LA FUENTE ARMADA.
 *
 * Bug que atrapa: un volcado enganchado en el lugar equivocado del camino de captura. Mutantes que
 * mata: tap antes del DSP de entrada (ganancia/DC blocker) -> el WAV es la senal cruda; tap en el
 * ring de monitoreo o en el scratch del analisis; rate estampado fijo en 48000; fuente sin registrar
 * (v[4] == 2 y no el default 0); contadores en otro orden (cada indice tiene un valor distinto
 * en el caso de la fuente, ver ASourceChange...).
 */
TEST_F(TunerDumpApiTest, TheWavIsBitForBitWhatTheTunerSeesAt44100) {
    ASSERT_NO_FATAL_FAILURE(runFidelity(kRate44, /*source=*/2));
}

/// AC-055.1 — lo mismo a 32 000: un rate estampado distinto del primero que se probo (un
/// "ultimo rate visto" cacheado en un static pasaria el primer test y no este).
TEST_F(TunerDumpApiTest, TheWavIsBitForBitWhatTheTunerSeesAt32000) {
    ASSERT_NO_FATAL_FAILURE(runFidelity(kRate32, /*source=*/1));
}

// ---------------------------------------------------------------------------
// CONTRATOS DE LA C API
// ---------------------------------------------------------------------------

/**
 * AC-055.1 — LOS RECHAZOS DE `start` NO DEJAN NADA A MEDIAS.
 *
 * Motor nulo, path nulo, path vacio y un directorio inexistente devuelven false, no crean archivo
 * y no dejan una toma activa (el siguiente start tiene que poder andar). Bug que atrapa: un start
 * que devuelve false pero deja la toma armada (el siguiente devuelve false por "toma activa" y el
 * consumidor no puede grabar nunca mas). Mutante que mata: armar el volcado antes de abrir el sink.
 */
TEST_F(TunerDumpApiTest, StartRejectionsLeaveNothingHalfDone) {
    const std::string good = pathOf("ok.wav");
    EXPECT_FALSE(wma_tuner_diag_input_dump_start(nullptr, good.c_str()));
    EXPECT_FALSE(wma_tuner_diag_input_dump_start(mWma, nullptr));
    EXPECT_FALSE(wma_tuner_diag_input_dump_start(mWma, ""));
    const std::string missingDir = pathOf("no/existe/toma.wav");
    EXPECT_FALSE(wma_tuner_diag_input_dump_start(mWma, missingDir.c_str())) << "el sink no abre";
    EXPECT_FALSE(exists(missingDir + ".part"));

    if (mWma->tunerInputDump) {
        EXPECT_FALSE(mWma->tunerInputDump->takeActive()) << "un rechazo dejo una toma armada";
    }
    auto v = sentinelStats();
    EXPECT_FALSE(wma_tuner_diag_input_dump_stop(mWma, v.data(), WMA_TUNER_DUMP_STATS_VALUES))
        << "no hay toma que cerrar";

    // Y tras tantos rechazos, un start valido SI anda.
    EXPECT_TRUE(wma_tuner_diag_input_dump_start(mWma, good.c_str()));
    EXPECT_TRUE(wma_tuner_diag_input_dump_stop(mWma, v.data(), WMA_TUNER_DUMP_STATS_VALUES));
}

/**
 * AC-055.1 — `stop` SIN TOMA, o con argumentos malos, DEJA out_values CON LOS CENTINELAS.
 *
 * Es la regla de `wma_tuner_get_snapshot` (MINI-003): un cero es un dato plausible, y un fallo que
 * escribe ceros se lee como una medicion. Tambien: count mayor que 7 escribe EXACTAMENTE 7 valores
 * (no `count`), y un `stop` rechazado por count chico NO cierra una toma viva.
 * Mutantes que mata: llenar con ceros en el camino de error; escribir `count` valores; cerrar la
 * toma antes de validar `count`.
 */
TEST_F(TunerDumpApiTest, StopContractsLeaveSentinelsAndNeverKillALiveTakeOnBadArguments) {
    auto v = sentinelStats();

    // Sin toma.
    EXPECT_FALSE(wma_tuner_diag_input_dump_stop(mWma, v.data(), WMA_TUNER_DUMP_STATS_VALUES));
    EXPECT_TRUE(allSentinels(v)) << "sin toma, out_values no se toca";

    // Argumentos nulos.
    EXPECT_FALSE(wma_tuner_diag_input_dump_stop(nullptr, v.data(), WMA_TUNER_DUMP_STATS_VALUES));
    EXPECT_TRUE(allSentinels(v));
    EXPECT_FALSE(wma_tuner_diag_input_dump_stop(mWma, nullptr, WMA_TUNER_DUMP_STATS_VALUES));

    // Con una toma VIVA: un count chico (< 7) se rechaza sin tocar nada ni cerrar la toma.
    const std::string path = pathOf("viva.wav");
    ASSERT_TRUE(wma_tuner_diag_input_dump_start(mWma, path.c_str()));
    for (int bad : {6, 1, 0, -1}) {
        v = sentinelStats();
        EXPECT_FALSE(wma_tuner_diag_input_dump_stop(mWma, v.data(), bad)) << "count " << bad;
        EXPECT_TRUE(allSentinels(v)) << "count " << bad << ": no se toca out_values";
        ASSERT_NE(mWma->tunerInputDump, nullptr);
        EXPECT_TRUE(mWma->tunerInputDump->takeActive())
            << "un stop rechazado cerro la toma (count " << bad << ")";
    }

    // count > 7: escribe EXACTAMENTE 7.
    std::array<int64_t, 9> wide;
    wide.fill(kSentinel64);
    ASSERT_TRUE(wma_tuner_diag_input_dump_stop(mWma, wide.data(), 9));
    for (int i = 0; i < WMA_TUNER_DUMP_STATS_VALUES; ++i) {
        EXPECT_NE(wide[static_cast<size_t>(i)], kSentinel64) << "el indice " << i << " no se escribio";
    }
    EXPECT_EQ(wide[7], kSentinel64) << "escribio mas alla de los 7 valores del contrato";
    EXPECT_EQ(wide[8], kSentinel64);
    EXPECT_EQ(WMA_TUNER_DUMP_STATS_VALUES, 7);

    // Un segundo stop ya no tiene toma.
    v = sentinelStats();
    EXPECT_FALSE(wma_tuner_diag_input_dump_stop(mWma, v.data(), WMA_TUNER_DUMP_STATS_VALUES));
    EXPECT_TRUE(allSentinels(v));
}

/**
 * AC-055.1 — DOBLE START: el segundo es false y NO toca la toma en curso ni su archivo.
 *
 * Bug que atrapa: un segundo start que abre otro sink (truncando el `.part` de la toma viva, o
 * dejando uno huerfano). Mutante que mata: chequear la toma activa DESPUES de abrir el sink.
 */
TEST_F(TunerDumpApiTest, ASecondStartIsRejectedAndTheFirstTakeSurvives) {
    const std::string first = pathOf("primera.wav");
    const std::string second = pathOf("segunda.wav");
    ASSERT_TRUE(wma_tuner_diag_input_dump_start(mWma, first.c_str()));
    EXPECT_FALSE(wma_tuner_diag_input_dump_start(mWma, second.c_str())) << "ya hay una toma activa";
    EXPECT_FALSE(exists(second + ".part")) << "el segundo start abrio un sink";
    EXPECT_TRUE(exists(first + ".part")) << "la toma viva perdio su temporal";

    const auto v = stopTake();
    EXPECT_EQ(v[0], 0);
    // Sin audio la primera toma sale invalida, y es SU archivo el que queda nombrado.
    EXPECT_TRUE(exists(pathOf("primera.invalid.wav")));
    EXPECT_FALSE(exists(second));
    EXPECT_FALSE(exists(pathOf("segunda.invalid.wav")));
    EXPECT_FALSE(exists(pathOf("segunda.wav.part")));
}

/**
 * AC-055.1 — `start` NO ARRANCA EL AFINADOR: sin afinador corriendo no entra audio.
 *
 * Es una decision de la interfaz (el volcado es un instrumento, no una funcion del afinador). Se
 * mide por lo observable: tras start el afinador sigue parado, y alimentar audio no produce un
 * solo frame en la toma, que cierra INVALIDA con su archivo nombrado asi.
 * Mutante que mata: que start llame a wma_tuner_start().
 */
TEST_F(TunerDumpApiTest, StartDoesNotStartTheTunerAndWithoutItNoAudioEnters) {
    startAt(kRate44, 0);
    const std::string path = pathOf("sin-afinador.wav");
    ASSERT_TRUE(wma_tuner_diag_input_dump_start(mWma, path.c_str()));
    EXPECT_FALSE(wma_tuner_is_running(mWma)) << "el volcado arranco el afinador";

    feed(kChunkBlocks * 2);   // sin afinador no hay ring enganchado al nodo de entrada

    const auto v = stopTake();
    EXPECT_EQ(v[0], 0) << "entro audio sin afinador corriendo";
    EXPECT_EQ(v[5], 0);
    EXPECT_TRUE(exists(pathOf("sin-afinador.invalid.wav")));
    EXPECT_FALSE(exists(path));
    EXPECT_FALSE(wma_tuner_is_running(mWma));
}

// ---------------------------------------------------------------------------
// TOMAS INDEPENDIENTES Y DISCONTINUIDADES POR EL CAMINO REAL
// ---------------------------------------------------------------------------

/**
 * AC-055.1 — UNA TOMA INVALIDA NO ENVENENA A LA SIGUIENTE (y cada una tiene su archivo).
 *
 * La primera toma no recibe audio (invalida, `.invalid`). La segunda recibe 3 tandas y tiene que
 * salir VALIDA con exactamente sus frames. Despues de cerrar las dos, ambos archivos existen y son
 * lo que cada una dijo. Bug que atrapa: contadores o `valid` que se heredan entre tomas, o un
 * `TunerInputDump` que no se puede reusar tras endTake. Mutante que mata: no resetear en beginTake.
 */
TEST_F(TunerDumpApiTest, AnInvalidTakeDoesNotPoisonTheNextOne) {
    ASSERT_NO_FATAL_FAILURE(bringUpTuner(kRate44));
    const std::string a = pathOf("a.wav");
    const std::string b = pathOf("b.wav");

    ASSERT_TRUE(wma_tuner_diag_input_dump_start(mWma, a.c_str()));
    const auto va = stopTake();   // sin audio
    EXPECT_EQ(va[0], 0);
    EXPECT_EQ(va[5], 0);
    EXPECT_TRUE(exists(pathOf("a.invalid.wav")));
    EXPECT_FALSE(exists(a));

    ASSERT_TRUE(wma_tuner_diag_input_dump_start(mWma, b.c_str())) << "tras cerrar se puede volver a grabar";
    ASSERT_NO_FATAL_FAILURE(feedPaced(3));
    const auto vb = stopTake();
    EXPECT_EQ(vb[0], 3 * kChunkFrames);
    EXPECT_EQ(vb[1], 0);
    EXPECT_EQ(vb[2], 0);
    EXPECT_EQ(vb[3], kRate44);
    EXPECT_EQ(vb[5], 1) << "la toma anterior enveneno a esta";
    EXPECT_TRUE(exists(b));
    EXPECT_FALSE(exists(pathOf("b.invalid.wav")));
    EXPECT_EQ(readFile(b).size(), kHeaderBytes + static_cast<size_t>(3 * kChunkFrames) * sizeof(float));
    EXPECT_TRUE(exists(pathOf("a.invalid.wav"))) << "la segunda toma toco el archivo de la primera";
}

/**
 * AC-055.1 — UN CAMBIO DE FUENTE A MITAD DE TOMA INVALIDA LA TOMA, POR EL CAMINO REAL.
 *
 * `wma_input_set_source` avisa al analisis (`onSourceChanged`), y el analisis se lo anota al
 * volcado. Se mide extremo a extremo: v[2] (discontinuidades) >= 1 mientras v[1] (perdidas) es 0
 * —dos indices con valores DISTINTOS, asi que un orden de campos equivocado se ve—, valid=0 y el
 * archivo con nombre de invalido. La fuente estampada es la del ARMADO (2), no la del cierre (1).
 * Mutantes que mata: no anotar el cambio de fuente en el volcado; estampar la fuente al cerrar.
 */
TEST_F(TunerDumpApiTest, ASourceChangeMidTakeInvalidatesItThroughTheRealPath) {
    ASSERT_NO_FATAL_FAILURE(bringUpTuner(kRate44, /*source=*/2));
    const std::string path = pathOf("cambio-de-fuente.wav");
    ASSERT_TRUE(wma_tuner_diag_input_dump_start(mWma, path.c_str()));

    ASSERT_NO_FATAL_FAILURE(feedPaced(2));
    wma_input_set_source(mWma, 1);
    ASSERT_EQ(wma_input_get_source(mWma), 1);
    ASSERT_NO_FATAL_FAILURE(feedPaced(2));

    const auto v = stopTake();
    EXPECT_GE(v[2], 1) << "el cambio de fuente no se anoto como discontinuidad";
    EXPECT_EQ(v[1], 0) << "no hubo perdidas: no es lo mismo que una discontinuidad";
    EXPECT_EQ(v[4], 2) << "la fuente de la toma es la del armado";
    EXPECT_EQ(v[5], 0);
    EXPECT_GT(v[0], 0);
    EXPECT_TRUE(exists(pathOf("cambio-de-fuente.invalid.wav")));
    EXPECT_FALSE(exists(path)) << "un WAV con dos fuentes mezcladas no puede tener nombre de sano";
    ASSERT_NO_FATAL_FAILURE(expectFileHeaderMatches(readFile(pathOf("cambio-de-fuente.invalid.wav")), v));
}

/**
 * AC-055.1 — UNA TOMA QUE NO SE PUDO NOMBRAR DICE `finalized = 0`, Y EL ARCHIVO ES EL `.part`.
 *
 * El destino pedido es un DIRECTORIO existente: el `rename` del `.part` falla. Kotlin no puede
 * predecir `x.invalid.wav` (no existe): tiene que saber que el archivo quedo como `<pedido>.part`.
 * Bug que atrapa: reportar solo `valid` y dejar que el consumidor apunte a un archivo inexistente.
 * Mutante que mata: out_values[6] fijo en 1 (o ausente).
 */
TEST_F(TunerDumpApiTest, ATakeThatCouldNotBeNamedReportsNotFinalizedAndKeepsThePart) {
    ASSERT_NO_FATAL_FAILURE(bringUpTuner(kRate44));
    const std::string path = pathOf("destino.wav");
    std::error_code ec;
    ASSERT_TRUE(fs::create_directory(fs::path(path), ec)) << "premisa: el destino es un directorio";
    ASSERT_TRUE(wma_tuner_diag_input_dump_start(mWma, path.c_str()));
    ASSERT_NO_FATAL_FAILURE(feedPaced(2));

    const auto v = stopTake();
    EXPECT_EQ(v[0], 2 * kChunkFrames) << "los frames se escribieron";
    EXPECT_EQ(v[5], 0) << "sin archivo nombrado no hay toma sana, aunque cada frame se escribiera";
    EXPECT_EQ(v[6], 0) << "finalized";
    EXPECT_TRUE(exists(path + ".part")) << "el archivo real es el .part";
    EXPECT_FALSE(exists(pathOf("destino.invalid.wav")));
}

// ---------------------------------------------------------------------------
// DESTRUIR EL MOTOR CON UNA TOMA VIVA
// ---------------------------------------------------------------------------

/**
 * AC-055.1 — `wma_engine_destroy` CON UNA TOMA VIVA LA CIERRA Y NOMBRA EL ARCHIVO.
 *
 * Si la app muere o el consumidor destruye el motor sin pedir stop, el archivo no puede quedar
 * como `.part` huerfano ni cortado: se desarma el productor, se cierra la toma y el WAV queda con
 * su nombre (sano, porque no hubo problemas) y su header completo. Se destruye el motor DEL
 * FIXTURE y se anula `mWma` para que TearDown no lo destruya dos veces.
 * Mutantes que mata: destroy que libera el volcado sin endTake (queda el `.part`); endTake sin
 * desarmar antes (use-after-free del volcado desde el hilo de analisis, abort bajo ASan).
 */
TEST_F(TunerDumpApiTest, DestroyingTheEngineWithALiveTakeClosesAndNamesTheFile) {
    ASSERT_NO_FATAL_FAILURE(bringUpTuner(kRate44));
    const std::string path = pathOf("viva.wav");
    ASSERT_TRUE(wma_tuner_diag_input_dump_start(mWma, path.c_str()));
    ASSERT_NO_FATAL_FAILURE(feedPaced(3));

    wma_engine_destroy(mWma);
    mWma = nullptr;
    mBackend = nullptr;

    EXPECT_TRUE(exists(path)) << "la toma viva no quedo nombrada al destruir el motor";
    EXPECT_FALSE(exists(path + ".part")) << "quedo un .part huerfano";
    EXPECT_FALSE(exists(pathOf("viva.invalid.wav")));
    const std::vector<uint8_t> bytes = readFile(path);
    ASSERT_EQ(bytes.size(), kHeaderBytes + static_cast<size_t>(3 * kChunkFrames) * sizeof(float));
    EXPECT_EQ(rdLE(bytes, 62, 8), static_cast<uint64_t>(3 * kChunkFrames)) << "wmad.framesWritten";
    EXPECT_EQ(rdLE(bytes, 94, 4), 1u) << "wmad.valid";
    EXPECT_EQ(rdLE(bytes, 24, 4), static_cast<uint64_t>(kRate44));
}

/// AC-055.1 — lo mismo SIN frames: el archivo queda cerrado y nombrado como INVALIDO (header de
/// 106 bytes, sin datos). Mutante que mata: destroy que solo cierra tomas con datos.
TEST_F(TunerDumpApiTest, DestroyingTheEngineWithAnEmptyLiveTakeLeavesAnInvalidNamedFile) {
    const std::string path = pathOf("vacia.wav");
    ASSERT_TRUE(wma_tuner_diag_input_dump_start(mWma, path.c_str()));

    wma_engine_destroy(mWma);
    mWma = nullptr;
    mBackend = nullptr;

    EXPECT_FALSE(exists(path));
    EXPECT_FALSE(exists(path + ".part"));
    ASSERT_TRUE(exists(pathOf("vacia.invalid.wav")));
    const std::vector<uint8_t> bytes = readFile(pathOf("vacia.invalid.wav"));
    ASSERT_EQ(bytes.size(), kHeaderBytes);
    EXPECT_EQ(rdLE(bytes, 94, 4), 0u) << "wmad.valid";
    EXPECT_EQ(rdLE(bytes, 62, 8), 0u);
}

}  // namespace wma_test
