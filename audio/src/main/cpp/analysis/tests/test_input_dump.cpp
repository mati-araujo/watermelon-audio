/**
 * REQ-055 S0 · tarea 0.1 — el volcado a WAV de la ENTRADA del afinador (el nucleo, sin disco).
 *
 * AC-055.1 — el corpus se captura por el camino del afinador: este volcado es su INSTRUMENTO de
 * captura. Un instrumento que miente (otro rate, otra senal, un hueco con cara de sano) fabrica
 * un corpus que mide otra cosa, y todo lo que se afine contra el despues queda mal sin que nada
 * se ponga rojo. Por eso estos tests no verifican "que escribe un archivo": verifican las cuatro
 * cosas que lo vuelven creible.
 *
 *   1. FIDELIDAD. El WAV es EXACTAMENTE lo que el afinador ve: el contenido del AnalysisRing
 *      (mono 0,5f * (L + R) en float), con el rate REAL estampado. Ningun rate de prueba es
 *      48000: es la constante que alguna vez estuvo cableada, y usarla aca haria que un rate
 *      fijo pase por coincidencia.
 *   2. EL ANALISIS NO CAMBIA. Con y sin volcado armado, los snapshots publicados son bit a bit
 *      iguales. Medir al afinador con un instrumento que lo altera mide otro afinador.
 *   3. NUNCA UN WAV CON HUECOS QUE PAREZCA SANO. Perdidas (ring pisado antes de copiar, cola
 *      llena, error del sink) y discontinuidades (costura de captura, reset, cambio de fuente,
 *      cambio de rate) => valid=false y el archivo final se llama invalidDumpPathFor(path).
 *   4. EL PRODUCTOR NUNCA ESPERA AL ESCRITOR. Con el sink trabado, el analisis sigue avanzando.
 *
 * COMO SE SINCRONIZA (REQ-002): el escritor es un hilo, asi que se espera CONDICIONES
 * (`waitUntil` sobre framesWritten()/framesQueued()). Para lo determinista se maneja el analisis
 * a mano con `drainOnce()` sin `start()`: con el hilo parado, `setInputDump` aplica directo.
 *
 * LOS DATOS SON ALEATORIOS (LCG propio, determinista) Y NO UNA RAMPA, a proposito: con una rampa
 * L == R y el promedio "0,5(L+R)" no se distingue de "volcar L". Aca L != R en cada frame y hay
 * muestras con |x| > 1, asi que volcar un canal, cuantizar a int16 o saturar da otro numero.
 */

#include "../AnalysisRing.h"
#include "../AnalysisSnapshot.h"
#include "../AnalysisThread.h"
#include "../TunerInputDump.h"

#include "tests/support/SnapshotRead.h"
#include "tests/support/TestWait.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Definidas en AnalysisRing.cpp bajo WMA_TEST_HOOKS (las mismas que usa test_analysis_ring.cpp).
extern std::atomic<bool> gAnalysisRingHoldAfterCopy;
extern std::atomic<bool> gAnalysisRingIsInCopy;

namespace {

using namespace wma::analysis;
using wma_test::waitUntil;

/// Ninguno de los rates de prueba es 48000: ver el encabezado.
constexpr int kRate = 44100;
constexpr uint32_t kCap = AnalysisRing::kCapacityFrames;
/// La capacidad EFECTIVA del volcado (ADENDA A): el ring menos el bloque en vuelo que el re-chequeo
/// de desgarro no ve. Lo mas atras que esto cuenta como perdido.
constexpr uint32_t kEff = AnalysisRing::kCapacityFrames - AnalysisRing::kDumpTearGuardFrames;
static_assert(AnalysisRing::kDumpTearGuardFrames == 4096, "ADENDA A: el bloque maximo del escritor");
/// El sink de memoria no toca disco: el path es solo un nombre que tiene que viajar intacto.
const char* const kPath = "take/entrada.wav";
/// Una fuente que NO es la de por defecto (0 = MIC): si el nucleo estampara una constante, no pasa.
constexpr int kSource = 2;
/// Cola chica para los tests de escritor trabado: bastante mas chica que lo que se empuja, pero
/// no menor que un trozo de copia (un push todo-o-nada nunca entraria en una cola mas chica que
/// el trozo, y el escritor no recibiria nada que el freno pudiera trabar).
constexpr size_t kSmallQueue = 4096;

// ---------------------------------------------------------------------------
// Datos
// ---------------------------------------------------------------------------

/// LCG propio: determinista entre corridas y entre plataformas (no `random_device`).
struct Lcg {
    uint32_t s = 12345u;
    float next() {   // [-1, 1)
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(s >> 8) / 8388608.0f - 1.0f;
    }
};

/// Estereo intercalado con L y R INDEPENDIENTES, escalado: con `scale` > 1 hay |x| > 1.
std::vector<float> stereoNoise(Lcg& rng, int frames, float scale) {
    std::vector<float> b(static_cast<size_t>(frames) * 2);
    for (float& v : b) v = scale * rng.next();
    return b;
}

/// Lo que el ring guarda por frame. MISMA expresion, en float, que `AnalysisRing::writeStereo`.
float monoOf(float l, float r) { return 0.5f * (l + r); }

/// Primer indice donde dos vectores difieren BIT A BIT, o -1. Compara bits y no `==`: un -0,0
/// frente a un 0,0, o dos NaN, tienen que contar como lo que son.
long firstMismatch(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return -2;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0) return static_cast<long>(i);
    }
    return -1;
}

float maxAbs(const std::vector<float>& v) {
    float m = 0.0f;
    for (float x : v) m = std::max(m, std::fabs(x));
    return m;
}

// ---------------------------------------------------------------------------
// El sink en memoria (el borde de E/S de los tests) y su freno
// ---------------------------------------------------------------------------

/// Estado compartido: el `TunerInputDump` es dueno del sink (unique_ptr), asi que el test lo
/// mira a traves de esto, que sobrevive.
struct SinkState {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<float> samples;
    std::vector<uint8_t> header;
    std::string openedPath;
    size_t openedHeaderBytes = 0;
    int openCalls = 0;
    int appendCalls = 0;
    int finishCalls = 0;
    bool finishValid = false;
    std::string finalPath;
    bool brakeOn = false;     // bajo `mu`
    bool inBrake = false;     // bajo `mu`: el escritor esta CLAVADO adentro de appendSamples
    // Controles de falla: atomicos porque los pone el test y los lee el hilo escritor.
    std::atomic<bool> failOpen{false};
    std::atomic<bool> failAppends{false};
    std::atomic<bool> failFinish{false};

    void setBrake(bool on) {
        {
            std::lock_guard<std::mutex> l(mu);
            brakeOn = on;
        }
        cv.notify_all();
    }
    bool isInBrake() {
        std::lock_guard<std::mutex> l(mu);
        return inBrake;
    }
    std::vector<float> samplesCopy() {
        std::lock_guard<std::mutex> l(mu);
        return samples;
    }
    std::vector<uint8_t> headerCopy() {
        std::lock_guard<std::mutex> l(mu);
        return header;
    }
};

class MemorySink final : public IDumpSink {
public:
    explicit MemorySink(std::shared_ptr<SinkState> s) : mS(std::move(s)) {}

    bool openPart(const std::string& path, size_t headerBytes) override {
        std::lock_guard<std::mutex> l(mS->mu);
        ++mS->openCalls;
        mS->openedPath = path;
        mS->openedHeaderBytes = headerBytes;
        return !mS->failOpen.load();
    }

    bool appendSamples(const float* samples, size_t count) override {
        std::unique_lock<std::mutex> l(mS->mu);
        ++mS->appendCalls;
        if (mS->brakeOn) {
            // El freno: el escritor queda aca ADENTRO, sin aceptar nada, hasta que el test
            // suelte. El techo de 30 s es solo para que un test roto no cuelgue la suite.
            mS->inBrake = true;
            mS->cv.notify_all();
            mS->cv.wait_for(l, std::chrono::seconds(30), [&] { return !mS->brakeOn; });
            mS->inBrake = false;
        }
        if (mS->failAppends.load()) return false;
        mS->samples.insert(mS->samples.end(), samples, samples + count);
        return true;
    }

    std::string finishPart(const uint8_t* header, size_t headerBytes, bool valid) override {
        std::lock_guard<std::mutex> l(mS->mu);
        ++mS->finishCalls;
        mS->finishValid = valid;
        mS->header.assign(header, header + headerBytes);
        if (mS->failFinish.load()) return "";
        // Lo que hace el sink de disco: nombrar segun `valid`. El nucleo es quien decide `valid`.
        mS->finalPath = valid ? mS->openedPath : invalidDumpPathFor(mS->openedPath);
        return mS->finalPath;
    }

private:
    std::shared_ptr<SinkState> mS;
};

/// Una toma con su sink. Soltar el freno al destruir: un test que falla a mitad no cuelga al
/// escritor (el destructor de `TunerInputDump` junta el hilo).
struct Take {
    std::shared_ptr<SinkState> sink = std::make_shared<SinkState>();
    TunerInputDump dump;
    explicit Take(size_t queueFrames = TunerInputDump::kDefaultQueueFrames) : dump(queueFrames) {}
    ~Take() { sink->setBrake(false); }

    bool begin(const std::string& path = kPath, int source = kSource) {
        return dump.beginTake(std::make_unique<MemorySink>(sink), path, source);
    }
};

// ---------------------------------------------------------------------------
// El banco: ring + snapshot + hilo de analisis (sin start salvo que el test lo pida)
// ---------------------------------------------------------------------------

class DumpRig {
public:
    DumpRig() { ring.setCaptureRate(kRate); }

    AnalysisRing ring;
    AnalysisSnapshot snap;
    AnalysisThread thread{ring, snap};
    /// TODO lo escrito al ring desde su ultimo reset, indexado por posicion de escritura: es la
    /// referencia contra la que se compara lo volcado.
    std::vector<float> mono;

    void writeBlock(const std::vector<float>& stereo) {
        const int frames = static_cast<int>(stereo.size() / 2);
        ASSERT_LE(frames, static_cast<int>(kCap)) << "un bloque mayor que el ring se recorta";
        ring.writeStereo(stereo.data(), frames);
        for (int i = 0; i < frames; ++i) {
            mono.push_back(monoOf(stereo[static_cast<size_t>(i) * 2],
                                  stereo[static_cast<size_t>(i) * 2 + 1]));
        }
    }
    void write(int frames, float scale = 3.0f) { writeBlock(stereoNoise(mRng, frames, scale)); }

    /// Reset del ring (cambio de topologia): las posiciones vuelven a cero.
    void resetRing() {
        ring.reset();
        mono.clear();
    }

    /// Lo escrito desde la posicion `from`.
    std::vector<float> since(uint64_t from) const {
        return std::vector<float>(mono.begin() + static_cast<long>(from), mono.end());
    }

private:
    Lcg mRng;
};

/// Cierra la toma con el protocolo del llamador real: primero se desarma el productor, despues
/// `endTake` (su PRECONDICION).
InputDumpStats closeTake(DumpRig& rig, Take& take, std::string* finalPath = nullptr) {
    EXPECT_TRUE(rig.thread.setInputDump(nullptr));
    return take.dump.endTake(finalPath);
}

// ---------------------------------------------------------------------------
// Lectura del header: offsets LITERALES (un parser de WAV no sabe de nuestro codigo)
// ---------------------------------------------------------------------------

uint64_t rdLE(const std::vector<uint8_t>& h, size_t off, int bytes) {
    uint64_t v = 0;
    for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | h[off + static_cast<size_t>(i)];
    return v;
}

bool tagAt(const std::vector<uint8_t>& h, size_t off, const char* tag) {
    return h.size() >= off + 4 && std::memcmp(&h[off], tag, 4) == 0;
}

/// Verifica CADA campo del header contra `s`, por offset. Sirve para el header que arma
/// `writeInputDumpHeader` y para el que el nucleo le entrega al sink al cerrar una toma.
void expectHeaderMatches(const std::vector<uint8_t>& h, const InputDumpStats& s) {
    ASSERT_EQ(h.size(), kInputDumpHeaderBytes);
    ASSERT_EQ(kInputDumpHeaderBytes, size_t{106});
    const uint64_t frames = s.framesWritten;

    // RIFF / WAVE
    EXPECT_TRUE(tagAt(h, 0, "RIFF"));
    // Tamano del RIFF = archivo - 8 = 4 (WAVE) + 26 (fmt) + 12 (fact) + 48 (wmad) + 8 + datos.
    EXPECT_EQ(rdLE(h, 4, 4), 98 + 4 * frames) << "el tamano del RIFF no cierra con el archivo";
    EXPECT_TRUE(tagAt(h, 8, "WAVE"));

    // fmt (18 bytes): formato 3 = IEEE float, 1 canal, rate, byteRate = rate*4, blockAlign 4, 32 bits.
    EXPECT_TRUE(tagAt(h, 12, "fmt "));
    EXPECT_EQ(rdLE(h, 16, 4), 18u);
    EXPECT_EQ(rdLE(h, 20, 2), 3u) << "tiene que ser IEEE float (3), no PCM (1)";
    EXPECT_EQ(rdLE(h, 22, 2), 1u) << "mono";
    EXPECT_EQ(rdLE(h, 24, 4), static_cast<uint64_t>(s.sampleRate)) << "el rate REAL, no uno fijo";
    EXPECT_EQ(rdLE(h, 28, 4), static_cast<uint64_t>(s.sampleRate) * 4u);
    EXPECT_EQ(rdLE(h, 32, 2), 4u);
    EXPECT_EQ(rdLE(h, 34, 2), 32u);
    EXPECT_EQ(rdLE(h, 36, 2), 0u) << "cbSize";

    // fact (4): frames.
    EXPECT_TRUE(tagAt(h, 38, "fact"));
    EXPECT_EQ(rdLE(h, 42, 4), 4u);
    EXPECT_EQ(rdLE(h, 46, 4), frames);

    // wmad (40): version=1, u64 frames, u64 lost, u64 discontinuities, i32 rate, i32 source, u32 valid.
    EXPECT_TRUE(tagAt(h, 50, "wmad"));
    EXPECT_EQ(rdLE(h, 54, 4), 40u);
    EXPECT_EQ(rdLE(h, 58, 4), 1u) << "version";
    EXPECT_EQ(rdLE(h, 62, 8), frames);
    EXPECT_EQ(rdLE(h, 70, 8), s.framesLost) << "u64: un u32 perderia el alto";
    EXPECT_EQ(rdLE(h, 78, 8), s.discontinuities) << "u64: un u32 perderia el alto";
    EXPECT_EQ(static_cast<int32_t>(rdLE(h, 86, 4)), s.sampleRate);
    EXPECT_EQ(static_cast<int32_t>(rdLE(h, 90, 4)), s.inputSource) << "i32 con signo";
    EXPECT_EQ(rdLE(h, 94, 4), s.valid ? 1u : 0u);

    // data.
    EXPECT_TRUE(tagAt(h, 98, "data"));
    EXPECT_EQ(rdLE(h, 102, 4), 4 * frames);
}

}  // namespace

// ===========================================================================
// 1. FIDELIDAD
// ===========================================================================

namespace {

void runFidelity(int rate) {
    SCOPED_TRACE("rate " + std::to_string(rate));
    Take take;
    DumpRig rig;
    rig.ring.setCaptureRate(rate);

    // Antes de armar: estos frames NO tienen que estar en el WAV.
    rig.write(100);
    ASSERT_TRUE(take.begin());
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));
    EXPECT_TRUE(take.dump.takeActive());
    const uint64_t armedAt = rig.ring.writePosition();
    ASSERT_EQ(armedAt, 100u);

    // Tamanos de bloque irregulares: ninguno alineado con kDrainFrames ni con la cola.
    for (int n : {700, 1500, 33, 2048, 1000, 1}) {
        rig.write(n);
        rig.thread.drainOnce();
    }
    const std::vector<float> expected = rig.since(armedAt);
    ASSERT_EQ(expected.size(), 5282u);
    ASSERT_GT(maxAbs(expected), 1.0f) << "la premisa del test: hay muestras con |x| > 1";

    // El contador observable avanza ANTES de cerrar (el escritor es un hilo).
    ASSERT_TRUE(waitUntil([&] { return take.dump.framesWritten() == expected.size(); }))
        << "el escritor no llego a escribir lo encolado";
    EXPECT_EQ(take.dump.framesQueued(), expected.size());

    std::string finalPath;
    const InputDumpStats st = closeTake(rig, take, &finalPath);

    EXPECT_EQ(st.framesWritten, expected.size());
    EXPECT_EQ(st.framesLost, 0u);
    EXPECT_EQ(st.discontinuities, 0u);
    EXPECT_EQ(st.sampleRate, rate) << "el rate estampado, no una constante";
    EXPECT_EQ(st.inputSource, kSource);
    EXPECT_TRUE(st.valid);
    EXPECT_EQ(finalPath, kPath);
    EXPECT_FALSE(take.dump.takeActive());

    // El contenido, bit a bit.
    const std::vector<float> got = take.sink->samplesCopy();
    EXPECT_EQ(firstMismatch(got, expected), -1L)
        << "lo volcado no es 0,5f*(L+R) del ring (indice " << firstMismatch(got, expected) << ")";

    // El borde: lo que se le pidio al sink y lo que recibio al cerrar.
    EXPECT_EQ(take.sink->openCalls, 1);
    EXPECT_EQ(take.sink->openedPath, kPath);
    EXPECT_EQ(take.sink->openedHeaderBytes, kInputDumpHeaderBytes);
    EXPECT_EQ(take.sink->finishCalls, 1);
    EXPECT_TRUE(take.sink->finishValid);
    expectHeaderMatches(take.sink->headerCopy(), st);
}

}  // namespace

/**
 * AC-055.1 — LO VOLCADO ES EL CONTENIDO DEL RING, BIT A BIT, CON EL RATE REAL.
 *
 * Bug que atrapa: un volcado que no es lo que el afinador ve. Tres formas plausibles, cada una
 * con su mutante:
 *   - volcar un solo canal (L) en vez del promedio                    -> L != R mata la comparacion
 *   - cuantizar a int16 / saturar a [-1, 1]                           -> hay |x| > 1 y el memcmp lo ve
 *   - estampar 48000 (la constante vieja) en vez del rate del ring    -> tres rates, ninguno 48000
 * Tambien mata "empezar el cursor en 0" (se colarian los 100 frames previos al armado) y
 * "estampar la fuente con una constante" (kSource = 2, no el default 0).
 */
TEST(InputDumpFidelity, TheDumpIsExactlyTheRingContentWithTheRealRate) {
    for (int rate : {44100, 32000, 96000}) {
        ASSERT_NO_FATAL_FAILURE(runFidelity(rate));
    }
}

/**
 * AC-055.1 — DESARMAR CORTA LA CAPTURA: lo escrito al ring DESPUES del desarme no entra.
 *
 * Bug que atrapa: `setInputDump(nullptr)` que no desengancha al productor. `endTake` tiene como
 * PRECONDICION que el productor ya no escribe; si el desarme no se aplica, el hilo de analisis
 * sigue empujando a una cola cuyo escritor ya se junto (corrupcion o frames de mas).
 * Mutante que mata: ignorar el pedido de desarme (mDump queda puesto).
 */
TEST(InputDumpFidelity, DisarmingStopsTheCaptureBeforeTheTakeCloses) {
    Take take;
    DumpRig rig;
    ASSERT_TRUE(take.begin());
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));

    rig.write(1000);
    rig.thread.drainOnce();
    ASSERT_TRUE(rig.thread.setInputDump(nullptr));
    EXPECT_EQ(rig.thread.inputDumpForTest(), nullptr);

    const uint64_t queuedAtDisarm = take.dump.framesQueued();
    rig.write(1500);          // esto ya NO es de la toma
    rig.thread.drainOnce();
    EXPECT_EQ(take.dump.framesQueued(), queuedAtDisarm);

    const InputDumpStats st = take.dump.endTake();
    EXPECT_EQ(st.framesWritten, 1000u);
    EXPECT_EQ(firstMismatch(take.sink->samplesCopy(),
                            std::vector<float>(rig.mono.begin(), rig.mono.begin() + 1000)),
              -1L);
    EXPECT_TRUE(st.valid);
}

/**
 * AC-055.1 — EL CAMBIO DE OBJETIVO NO AGUJEREA LA TOMA.
 *
 * `setTargetHz` hace que el analisis llame a `skipToNewest()`: descarta lo que habia sin leer en
 * el ring, y esos frames nunca llegan al estimador. El volcado tiene que verlos igual: es lo
 * que el microfono entrego, y un corpus se graba para despues re-analizarlo contra OTRO objetivo.
 *
 * Bug que atrapa: volcar lo que el analisis LEYO (`read()` / mScratch) en vez de lo que el ring
 * CONTIENE. Funciona hasta el primer cambio de cuerda y ahi deja un agujero que no se anuncia.
 * Mutantes que mata: copiar desde `mScratch`; leer con `read()` (comparte cursor con skipToNewest).
 */
TEST(InputDumpFidelity, ChangingTheTargetMidTakeDoesNotPunchAHole) {
    Take take;
    DumpRig rig;
    ASSERT_TRUE(take.begin());
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));
    const uint64_t armedAt = rig.ring.writePosition();

    const double targets[] = {440.0, 523.25, 329.63};
    const int blocks[] = {1000, 1500, 700};
    for (int i = 0; i < 3; ++i) {
        rig.thread.setTargetHz(targets[i]);
        rig.write(blocks[i]);
        rig.thread.drainOnce();
    }
    // PREMISA: el analisis SI aplico los tres objetivos (y con cada uno hizo skipToNewest). Sin
    // esto el test pasaria sin haber ejercido el caso.
    ASSERT_EQ(rig.thread.targetAppliedByUser(), 3u);

    const std::vector<float> expected = rig.since(armedAt);
    ASSERT_EQ(expected.size(), 3200u);
    const InputDumpStats st = closeTake(rig, take);

    EXPECT_EQ(st.framesWritten, 3200u) << "el skipToNewest del analisis agujereo el volcado";
    EXPECT_EQ(st.framesLost, 0u);
    EXPECT_EQ(st.discontinuities, 0u);
    EXPECT_TRUE(st.valid) << "un cambio de cuerda no es una discontinuidad de la senal";
    EXPECT_EQ(firstMismatch(take.sink->samplesCopy(), expected), -1L);
}

// ===========================================================================
// 2. EL ANALISIS DE PRODUCCION NO CAMBIA
// ===========================================================================

namespace {

/// Seno de 220 Hz + armonico + ruido determinista, estereo con L != R. `start` es el frame
/// absoluto: la fase es CONTINUA entre bloques (un salto de fase fabricaria una discontinuidad
/// que el estimador mediria).
std::vector<float> signalBlock(uint64_t start, int frames, Lcg& noise) {
    std::vector<float> b(static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(start + static_cast<uint64_t>(i));
        const double s = 0.30 * std::sin(2.0 * M_PI * 220.0 * t / kRate)
                         + 0.10 * std::sin(2.0 * M_PI * 440.0 * t / kRate);
        b[static_cast<size_t>(i) * 2]     = static_cast<float>(s) + 0.01f * noise.next();
        b[static_cast<size_t>(i) * 2 + 1] = static_cast<float>(s) + 0.01f * noise.next();
    }
    return b;
}

void expectSameAnalysis(DumpRig& plain, DumpRig& armed, AnalysisThread::DrainOutcome op,
                        AnalysisThread::DrainOutcome oa, int step) {
    SCOPED_TRACE("paso " + std::to_string(step));
    EXPECT_EQ(static_cast<int>(op), static_cast<int>(oa)) << "el volcado cambio el resultado del tick";

    float vp[kSnapshotValueCount];
    float va[kSnapshotValueCount];
    const bool rp = plain.snap.read(vp);
    const bool ra = armed.snap.read(va);
    EXPECT_EQ(rp, ra) << "uno publico y el otro no";
    if (rp && ra) {
        for (int i = 0; i < kSnapshotValueCount; ++i) {
            EXPECT_EQ(std::memcmp(&vp[i], &va[i], sizeof(float)), 0)
                << "el snapshot[" << i << "] difiere: " << vp[i] << " vs " << va[i];
        }
    }
    EXPECT_EQ(plain.ring.droppedFrames(), armed.ring.droppedFrames());
    EXPECT_EQ(plain.ring.tornReads(), armed.ring.tornReads());
    EXPECT_EQ(plain.ring.readPosition(), armed.ring.readPosition());
    EXPECT_EQ(plain.thread.ticks(), armed.thread.ticks());
    EXPECT_EQ(plain.thread.targetAppliedByUser(), armed.thread.targetAppliedByUser());
}

}  // namespace

/**
 * AC-055.1 — CON Y SIN VOLCADO ARMADO, TODOS LOS SNAPSHOTS SON BIT A BIT IGUALES.
 *
 * Dos bancos identicos alimentados con la MISMA senal, uno con toma armada y el otro sin. Se
 * compara cada tick, y en el camino se fuerzan los eventos que el volcado toca: cambio de
 * objetivo (skipToNewest), costura de captura, desborde del ring (frames pisados) y cambio de
 * fuente. Si el volcado altera el analisis, aunque sea en un bit, el instrumento mide otro
 * afinador y el corpus que graba deja de ser evidencia.
 *
 * Bugs que atrapa: copiar con `read()` (mueve mRead y cambia que frames ve el analisis), que el
 * volcado cuente en mDropped/mTorn, que consuma el scratch, o que un retorno anticipado cambie el
 * DrainOutcome. Mata: usar el cursor del analisis; bumpear mDropped desde la copia.
 */
TEST(InputDumpProduction, ArmedAndUnarmedAnalysisPublishTheSameSnapshotsBitForBit) {
    Take take;
    DumpRig plain;
    DumpRig armed;
    Lcg noise;

    plain.thread.setTargetHz(220.0);
    armed.thread.setTargetHz(220.0);
    ASSERT_TRUE(take.begin());
    ASSERT_TRUE(armed.thread.setInputDump(&take.dump));

    uint64_t pos = 0;
    constexpr int kBlock = 512;
    for (int b = 0; b < 100; ++b) {
        if (b == 40) {
            plain.thread.setTargetHz(330.0);
            armed.thread.setTargetHz(330.0);
        }
        if (b == 60) {
            plain.ring.reportCaptureDiscontinuity();
            armed.ring.reportCaptureDiscontinuity();
        }
        if (b == 70) {
            // Desborde: mas de la capacidad del ring entre dos ticks, en los dos bancos.
            for (int k = 0; k < 3; ++k) {
                const auto big = signalBlock(pos, 4096, noise);
                pos += 4096;
                plain.writeBlock(big);
                armed.writeBlock(big);
            }
        }
        if (b == 80) {
            plain.thread.onSourceChanged();
            armed.thread.onSourceChanged();
        }
        const auto blk = signalBlock(pos, kBlock, noise);
        pos += kBlock;
        plain.writeBlock(blk);
        armed.writeBlock(blk);
        const auto op = plain.thread.drainOnce();
        const auto oa = armed.thread.drainOnce();
        ASSERT_NO_FATAL_FAILURE(expectSameAnalysis(plain, armed, op, oa, b));
        if (b == 71) {
            ASSERT_GT(armed.ring.droppedFrames(), 0u) << "premisa: el desborde ocurrio de verdad";
        }
    }

    // El volcado no fue letra muerta: capturo, y anoto lo que paso.
    EXPECT_GT(take.dump.framesQueued(), 0u);
    EXPECT_GT(take.dump.framesLost(), 0u) << "el desborde del ring tiene que figurar en la toma";
    EXPECT_FALSE(closeTake(armed, take).valid);
}

// ===========================================================================
// 3. NUNCA UN WAV CON HUECOS QUE PAREZCA SANO
// ===========================================================================

/**
 * AC-055.1 — ESCRITOR TRABADO: la cola se llena, se CUENTA, y la toma queda invalida.
 *
 * Cola chica y sink frenado: el escritor queda clavado dentro de appendSamples, la cola se llena
 * y el productor tiene que descartar. Determinista (drainOnce sin hilo): se empuja de a 512
 * frames hasta ver la perdida, con tope por cantidad de vueltas y no por tiempo.
 *
 * Bugs que atrapa: descartar sin contar (el WAV con hueco queda con nombre de sano) y no marcar la
 * toma invalida al cerrar. Mutantes que mata: `framesLost += n` omitido en la rama de cola
 * llena; `valid` que ignora framesLost; finishPart(valid=true) con perdidas.
 */
TEST(InputDumpLoss, AFullQueueIsCountedAndTheTakeIsInvalid) {
    Take take(kSmallQueue);
    DumpRig rig;
    take.sink->setBrake(true);
    ASSERT_TRUE(take.begin());
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));
    const uint64_t armedAt = rig.ring.writePosition();

    // Con el escritor frenado la cola absorbe a lo sumo ella misma + un lote en manos del
    // escritor: 64 vueltas de 512 frames (32768) es mucho mas que 2 * 4096.
    int rounds = 0;
    for (; rounds < 64 && take.dump.framesLost() == 0; ++rounds) {
        rig.write(512);
        rig.thread.drainOnce();
    }
    ASSERT_GT(take.dump.framesLost(), 0u) << "se empujo mucho mas que la cola y no se conto nada";
    EXPECT_EQ(rig.ring.droppedFrames(), 0u) << "la perdida es de la COLA, no del ring";

    // Soltar el freno: todo lo que entro a la cola se escribe (el escritor vacia).
    take.sink->setBrake(false);
    ASSERT_TRUE(waitUntil([&] { return take.dump.framesWritten() == take.dump.framesQueued(); }));

    std::string finalPath;
    const InputDumpStats st = closeTake(rig, take, &finalPath);
    EXPECT_FALSE(st.valid);
    EXPECT_GT(st.framesLost, 0u);
    EXPECT_GT(st.framesWritten, 0u);
    EXPECT_EQ(st.framesWritten, take.dump.framesQueued());
    EXPECT_FALSE(take.sink->finishValid) << "finishPart tiene que recibir valid=false";
    EXPECT_EQ(finalPath, invalidDumpPathFor(kPath));
    EXPECT_NE(finalPath, kPath);

    // Lo que si se escribio es contenido real y en orden: el primer lote entro entero.
    const std::vector<float> got = take.sink->samplesCopy();
    const std::vector<float> expected = rig.since(armedAt);
    ASSERT_GE(got.size(), 512u);
    EXPECT_EQ(std::memcmp(got.data(), expected.data(), 512 * sizeof(float)), 0);

    // Y el header lo dice: el contador del wmad es el de la toma.
    expectHeaderMatches(take.sink->headerCopy(), st);
}

/**
 * AC-055.1 — EL PRODUCTOR NUNCA ESPERA AL ESCRITOR: con el sink trabado el ANALISIS sigue.
 *
 * Con el hilo de analisis CORRIENDO y el sink frenado, se alimenta el ring a ritmo (esperando
 * LUGAR, que es una condicion) y se exige que `framesAnalyzed` del snapshot siga creciendo y que
 * la toma registre la perdida. Un productor que espera a la cola deja de drenar el ring: el
 * feeder no encuentra lugar y el test FALLA (techo de `waitUntil`), no se cuelga.
 *
 * Bug que atrapa: el hilo de analisis bloqueado por un escritor lento (disco, tarjeta SD, GC).
 * Para un afinador en vivo es la diferencia entre "el volcado perdio audio" y "el afinador se
 * congelo". Mutantes que mata: push bloqueante con espera; mutex en la cola; join/wait del
 * escritor desde captureFromRing.
 */
TEST(InputDumpLoss, AStalledWriterNeverStallsTheAnalysis) {
    Take take(kSmallQueue);
    DumpRig rig;
    take.sink->setBrake(true);
    ASSERT_TRUE(take.begin());
    rig.thread.start(kRate);
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump)) << "el buzon no se aplico";

    constexpr int kBlock = 256;
    auto feed = [&](int blocks) {
        for (int i = 0; i < blocks; ++i) {
            // Una CONDICION, no una duracion: el ring tiene lugar para el bloque sin pisar
            // nada sin leer. Si el analisis esta trabado, esto vence y el test falla.
            if (!waitUntil([&] { return rig.ring.availableFrames() + kBlock <= kCap; })) {
                return false;
            }
            rig.write(kBlock);
        }
        return true;
    };
    auto analyzed = [&] {
        return wma_test::readCoherentValueOr(rig.snap, kSnapFramesAnalyzed, 0.0);
    };

    // 1) Que el escritor quede CLAVADO en el freno.
    ASSERT_TRUE(feed(8));
    ASSERT_TRUE(waitUntil([&] { return take.sink->isInBrake(); })) << "el escritor nunca llego al sink";
    const double analyzed0 = analyzed();

    // 2) Con el escritor clavado, alimentar muchisimo mas que la cola (16384 frames vs 4096).
    ASSERT_TRUE(feed(64)) << "el analisis dejo de drenar el ring con el sink trabado: "
                             "el productor esta esperando al escritor";
    ASSERT_TRUE(waitUntil([&] { return take.dump.framesLost() > 0; })) << "perdida sin contar";
    ASSERT_TRUE(waitUntil([&] { return analyzed() >= analyzed0 + 8192.0; }))
        << "framesAnalyzed no avanzo mientras el sink estaba trabado";
    EXPECT_TRUE(take.sink->isInBrake()) << "premisa: el sink siguio frenado todo el tiempo";

    // 3) Liberar y cerrar.
    take.sink->setBrake(false);
    std::string finalPath;
    const InputDumpStats st = closeTake(rig, take, &finalPath);
    rig.thread.stop();
    EXPECT_FALSE(st.valid);
    EXPECT_GT(st.framesLost, 0u);
    EXPECT_FALSE(take.sink->finishValid);
    EXPECT_EQ(finalPath, invalidDumpPathFor(kPath));
}

/**
 * AC-055.1 — RING PISADO ANTES DE COPIAR: la perdida es EXACTA y la toma, invalida.
 *
 * Mas de la capacidad efectiva (kCapacityFrames - kDumpTearGuardFrames) escritos entre dos ticks:
 * lo mas viejo ya no se acepta. Lo que falta se
 * cuenta frame a frame (no "alguno"), y lo que queda es la COLA del ring, no la cabeza.
 *
 * Bug que atrapa: un volcado que pierde audio sin decirlo, o que lo dice mal (el numero es lo que
 * el humano usa para decidir si re-graba). Mutantes que mata: no contar; contar `w - cursor`
 * entero en vez de `(w - kEff) - cursor`; usar kCapacityFrames como capacidad; conservar la cabeza y descartar la cola.
 */
TEST(InputDumpLoss, ARingOverwrittenBeforeTheCopyIsCountedExactly) {
    Take take;
    DumpRig rig;
    rig.write(100);   // el cursor NO arranca en cero
    ASSERT_TRUE(take.begin());
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));

    for (int i = 0; i < 3; ++i) rig.write(4096);   // 12288 frames sin que nadie copie
    rig.thread.drainOnce();

    const std::vector<float> tail(rig.mono.end() - kEff, rig.mono.end());
    std::string finalPath;
    const InputDumpStats st = closeTake(rig, take, &finalPath);

    EXPECT_EQ(st.framesLost, 12288u - kEff) << "lo pisado: (w - kEff) - cursor";
    EXPECT_EQ(st.framesWritten, static_cast<uint64_t>(kEff));
    EXPECT_FALSE(st.valid);
    EXPECT_FALSE(take.sink->finishValid);
    EXPECT_EQ(finalPath, invalidDumpPathFor(kPath));
    EXPECT_EQ(firstMismatch(take.sink->samplesCopy(), tail), -1L)
        << "lo que sobrevive es la cola del ring";
    expectHeaderMatches(take.sink->headerCopy(), st);
}

/**
 * AC-055.1 — COSTURA DE CAPTURA cruzada dentro de la toma => discontinuidad, toma invalida.
 *
 * Una costura (`reportCaptureDiscontinuity`) es audio perdido ANTES del ring: el ring ve frames
 * contiguos y el hueco esta en el CONTENIDO. Para el volcado es la peor clase de hueco, porque
 * nada del ring la delata. Bug que atrapa: ignorar la costura (el WAV tiene un salto de senal y
 * nombre de sano). Mutante que mata: no consultar captureSeamPosition().
 */
TEST(InputDumpDiscontinuity, ACaptureSeamCrossedInsideTheTakeInvalidatesIt) {
    Take take;
    DumpRig rig;
    ASSERT_TRUE(take.begin());
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));

    rig.write(1000);
    rig.thread.drainOnce();
    rig.ring.reportCaptureDiscontinuity();   // costura en la posicion actual
    rig.write(1000);
    rig.thread.drainOnce();

    std::string finalPath;
    const InputDumpStats st = closeTake(rig, take, &finalPath);
    EXPECT_GE(st.discontinuities, 1u);
    EXPECT_FALSE(st.valid);
    EXPECT_FALSE(take.sink->finishValid);
    EXPECT_EQ(finalPath, invalidDumpPathFor(kPath));
}

/**
 * AC-055.1 — LA MISMA COSTURA, con los frames de los DOS lados copiados en un solo tick.
 *
 * Cuando la costura cae en el medio de un trozo, el cruce se ve en este tick o en el siguiente
 * segun cuando se evalua `cursor >= costura`. Se exige que, tras un tick MAS, la toma este
 * marcada: cualquiera de las dos lecturas razonables de la regla lo cumple.
 */
TEST(InputDumpDiscontinuity, ASeamInsideASingleChunkIsSeenByTheNextTickAtTheLatest) {
    Take take;
    DumpRig rig;
    ASSERT_TRUE(take.begin());
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));

    rig.write(500);
    rig.ring.reportCaptureDiscontinuity();   // la costura cae DENTRO del trozo que se copia
    rig.write(500);
    rig.thread.drainOnce();
    rig.write(500);
    rig.thread.drainOnce();

    const InputDumpStats st = closeTake(rig, take);
    EXPECT_GE(st.discontinuities, 1u);
    EXPECT_FALSE(st.valid);
}

/**
 * AC-055.1 — UNA COSTURA ANTERIOR AL ARMADO NO INVALIDA LA TOMA (el gemelo falso positivo).
 *
 * Una costura vieja, de antes de armar, no pertenece a la toma. Si `armAtRing` no toma la costura
 * vista como base, la primera vuelta la cuenta y TODA toma que arranque despues de un xrun
 * sale invalida: el instrumento deja de grabar en el telefono real.
 * Mutante que mata: lastSeam inicial en 0 en vez de captureSeamPosition() al armar.
 */
TEST(InputDumpDiscontinuity, ASeamBeforeArmingDoesNotInvalidateTheTake) {
    Take take;
    DumpRig rig;
    rig.write(1000);
    rig.ring.reportCaptureDiscontinuity();   // ANTES de armar
    rig.write(500);
    ASSERT_TRUE(take.begin());
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));

    rig.write(1000);
    rig.thread.drainOnce();
    rig.write(1000);
    rig.thread.drainOnce();

    const InputDumpStats st = closeTake(rig, take);
    EXPECT_EQ(st.discontinuities, 0u);
    EXPECT_TRUE(st.valid) << "una costura previa a la toma invalido una toma sana";
}

/**
 * AC-055.1 — UNA COSTURA AVISADA ANTES DE ARMAR PERO ADELANTADA (framesAhead > 0) PERTENECE A LA TOMA.
 *
 * CoreAudioBackend avisa el hueco hasta ~1 s por delante: el hueco todavia no se entrego cuando se
 * arma, y el cursor lo cruza despues. Bug que atrapa: tomar esa costura como "ya vista" al armar
 * (el WAV cruza el salto con nombre de sano). Mutante que mata: armAtRing sin comparar la costura
 * contra el cursor de armado.
 */
TEST(InputDumpDiscontinuity, APendingSeamAheadOfTheCursorWhenArmingInvalidatesTheTake) {
    Take take;
    DumpRig rig;
    rig.write(1000);
    rig.ring.reportCaptureDiscontinuity(3000);   // el hueco esta en la posicion 4000
    ASSERT_TRUE(take.begin());
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));

    for (int i = 0; i < 6; ++i) {   // el cursor cruza 4000
        rig.write(1000);
        rig.thread.drainOnce();
    }

    const InputDumpStats st = closeTake(rig, take);
    EXPECT_GE(st.discontinuities, 1u);
    EXPECT_FALSE(st.valid) << "la toma cruzo un hueco anunciado antes de armar y quedo sana";
}

/**
 * AC-055.1 — DOS COSTURAS SUPERPUESTAS: la lejana no borra a la cercana.
 *
 * `reportCaptureDiscontinuity` se queda con la mas lejana, asi que S2 > S1 pisa a S1 en el ring
 * antes de que el cursor cruce S1. Bug que atrapa: contar solo la costura vigente al cruzarla; el
 * hueco S1 se cruza sin que nadie lo cuente. Mutante que mata: contar la costura recien al cruzarla.
 */
TEST(InputDumpDiscontinuity, ANearSeamSupersededByAFarOneStillInvalidatesTheTake) {
    Take take;
    DumpRig rig;
    ASSERT_TRUE(take.begin());
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));

    rig.write(1000);
    rig.thread.drainOnce();
    rig.ring.reportCaptureDiscontinuity(2000);    // S1 = 3000
    rig.write(1000);
    rig.thread.drainOnce();
    rig.ring.reportCaptureDiscontinuity(20000);   // S2 = 22000, pisa a S1
    for (int i = 0; i < 3; ++i) {                  // el cursor termina en 5000: cruzo S1, no S2
        rig.write(1000);
        rig.thread.drainOnce();
    }

    const InputDumpStats st = closeTake(rig, take);
    EXPECT_GE(st.discontinuities, 1u);
    EXPECT_FALSE(st.valid);
}

/**
 * AC-055.1 — UNA COSTURA AVISADA DENTRO DE LA TOMA Y TODAVIA NO CRUZADA AL CERRAR: INVALIDA, A PROPOSITO.
 *
 * El hueco anunciado esta por delante del cursor; puede que la toma cierre antes de llegar. Se
 * cuenta en cuanto se ve: invalida de mas es aceptable, sana con un hueco al lado no.
 */
TEST(InputDumpDiscontinuity, ASeamAnnouncedButNotYetCrossedAtCloseInvalidatesOnPurpose) {
    Take take;
    DumpRig rig;
    ASSERT_TRUE(take.begin());
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));

    rig.write(1000);
    rig.thread.drainOnce();
    rig.ring.reportCaptureDiscontinuity(40000);
    rig.write(1000);
    rig.thread.drainOnce();

    const InputDumpStats st = closeTake(rig, take);
    EXPECT_EQ(st.discontinuities, 1u);
    EXPECT_FALSE(st.valid);
}

/**
 * AC-055.1 — CAMBIO DE FUENTE dentro de la toma => discontinuidad, toma invalida.
 *
 * Cambiar de microfono a linea mezcla dos senales en el mismo WAV, con un solo rate y un solo
 * header: no hay forma de que el archivo lo diga. El volcado no ve la costura en el ring (el
 * contenido de ambos lados es contiguo); se entera por AnalysisThread::onSourceChanged().
 * Mutante que mata: no llamar a noteInputDiscontinuity() en el camino de cambio de fuente.
 */
TEST(InputDumpDiscontinuity, ASourceChangeInsideTheTakeInvalidatesIt) {
    Take take;
    DumpRig rig;
    ASSERT_TRUE(take.begin());
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));

    rig.write(1000);
    rig.thread.drainOnce();
    rig.thread.onSourceChanged();
    rig.write(1000);
    rig.thread.drainOnce();

    std::string finalPath;
    const InputDumpStats st = closeTake(rig, take, &finalPath);
    EXPECT_GE(st.discontinuities, 1u);
    EXPECT_FALSE(st.valid);
    EXPECT_EQ(finalPath, invalidDumpPathFor(kPath));
}

/// El cambio de fuente SIN toma no rompe nada (el analisis lo atiende igual, sin volcado).
TEST(InputDumpDiscontinuity, ASourceChangeWithoutATakeIsHarmless) {
    DumpRig rig;
    rig.write(1000);
    rig.thread.drainOnce();
    rig.thread.onSourceChanged();
    rig.write(1000);
    rig.thread.drainOnce();
    EXPECT_EQ(rig.thread.inputDumpForTest(), nullptr);
}

/**
 * AC-055.1 — CAMBIO DE RATE a mitad de toma => discontinuidad, toma invalida.
 *
 * El header lleva UN rate. Si la captura pasa de 44 100 a 32 000 a mitad de toma, el WAV tendria
 * la mitad a una velocidad equivocada: sonaria bien y mediria mal. El rate que queda es el
 * PRIMERO (el del primer bloque), no el ultimo.
 * Mutantes que mata: comparar el rate contra una constante; sobreescribir el rate de la toma con
 * el ultimo visto sin marcar la discontinuidad.
 */
TEST(InputDumpDiscontinuity, ARateChangeMidTakeInvalidatesItAndKeepsTheFirstRate) {
    Take take;
    DumpRig rig;
    ASSERT_TRUE(take.begin());
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));

    rig.write(1000);
    rig.thread.drainOnce();
    rig.ring.setCaptureRate(32000);
    rig.write(1000);
    rig.thread.drainOnce();

    const InputDumpStats st = closeTake(rig, take);
    EXPECT_GE(st.discontinuities, 1u);
    EXPECT_FALSE(st.valid);
    EXPECT_EQ(st.sampleRate, kRate) << "el rate de la toma es el del primer bloque";
}

/**
 * AC-055.1 — RESET DEL RING dentro de la toma: las posiciones retroceden => discontinuidad.
 *
 * `AnalysisRing::reset()` (cambio de topologia) vuelve las posiciones a cero: el cursor del
 * volcado queda POR ENCIMA de writePosition(). Sin detectar el retroceso, `w - cursor` da una
 * cantidad enorme (unsigned) y el conteo de perdidas es basura, o el volcado se congela hasta
 * que la posicion vuelva a pasar el cursor. Se exige: marcada discontinua, y lo escrito despues
 * del reset SI entra (el cursor se reubica).
 * Mutante que mata: no comparar cursor contra writePosition().
 */
TEST(InputDumpDiscontinuity, ARingResetInsideTheTakeIsDetectedAndTheCursorRelocates) {
    Take take;
    DumpRig rig;
    rig.write(100);
    ASSERT_TRUE(take.begin());
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));

    rig.write(5000);
    rig.thread.drainOnce();
    rig.resetRing();               // las posiciones vuelven a cero
    rig.write(1000);               // < cursor anterior: el retroceso es visible
    rig.thread.drainOnce();

    const std::vector<float> afterReset = rig.mono;   // exactamente lo escrito tras el reset
    ASSERT_EQ(afterReset.size(), 1000u);
    const InputDumpStats st = closeTake(rig, take);

    EXPECT_GE(st.discontinuities, 1u);
    EXPECT_FALSE(st.valid);
    const std::vector<float> got = take.sink->samplesCopy();
    ASSERT_GE(got.size(), 1000u) << "tras el reset el volcado dejo de capturar";
    const std::vector<float> tail(got.end() - 1000, got.end());
    EXPECT_EQ(firstMismatch(tail, afterReset), -1L) << "lo posterior al reset no entro completo";
}

/**
 * AC-055.1 — SIN RATE CONOCIDO NO HAY WAV SANO.
 *
 * `valid` exige sampleRate > 0: un WAV con rate 0 en el header es ilegible (o peor, un lector lo
 * interpreta con un rate por defecto y mide mal). Aunque haya frames, si el rate nunca se
 * estampo, la toma NO es valida y el archivo final es el `.invalid`.
 * Mutante que mata: `valid` que no mira sampleRate.
 */
TEST(InputDumpLoss, FramesWithoutAKnownRateAreNeverAHealthyTake) {
    Take take;
    DumpRig rig;
    ASSERT_TRUE(take.begin());
    take.dump.armAtRing(rig.ring);
    rig.write(1000);
    take.dump.captureFromRing(rig.ring, 0);   // el ring nunca estampo un rate

    std::string finalPath;
    const InputDumpStats st = take.dump.endTake(&finalPath);
    EXPECT_EQ(st.sampleRate, 0);
    EXPECT_FALSE(st.valid);
    EXPECT_FALSE(take.sink->finishValid);
    EXPECT_EQ(finalPath, invalidDumpPathFor(kPath));
}

/**
 * AC-055.1 — ERROR DEL SINK: lo que sigue se descarta y se CUENTA; la toma queda invalida.
 *
 * El primer bloque se escribe bien; despues el sink empieza a fallar. Contrato: lo que el sink no
 * acepto no es "framesWritten", todo lo que se saca de la cola en adelante se cuenta como
 * perdido, y el productor nunca se traba.
 * Bug que atrapa: un disco lleno que deja un WAV truncado con nombre de sano. Mutantes que mata:
 * ignorar el retorno de appendSamples; contarlo como escrito; no descartar lo posterior.
 */
TEST(InputDumpLoss, ASinkErrorDiscardsTheRestAndCountsItAsLost) {
    Take take;
    DumpRig rig;
    ASSERT_TRUE(take.begin());
    take.dump.armAtRing(rig.ring);

    rig.write(500);
    take.dump.captureFromRing(rig.ring, kRate);
    ASSERT_TRUE(waitUntil([&] { return take.dump.framesWritten() == 500; }));

    take.sink->failAppends.store(true);
    rig.write(500);
    take.dump.captureFromRing(rig.ring, kRate);
    rig.write(500);
    take.dump.captureFromRing(rig.ring, kRate);

    std::string finalPath;
    const InputDumpStats st = take.dump.endTake(&finalPath);
    EXPECT_EQ(st.framesWritten, 500u) << "lo que el sink rechazo no es 'escrito'";
    EXPECT_EQ(st.framesLost, 1000u) << "todo lo que se saco de la cola tras el error es perdida";
    EXPECT_FALSE(st.valid);
    EXPECT_FALSE(take.sink->finishValid);
    EXPECT_EQ(finalPath, invalidDumpPathFor(kPath));
    EXPECT_EQ(take.sink->samplesCopy().size(), 500u);
}

/**
 * AC-055.1 — ERROR AL CERRAR (finishPart devuelve ""): la toma NO se declara valida.
 *
 * Aunque cada frame se haya escrito, si el sink no pudo cerrar/nombrar el archivo no hay WAV sano
 * que ofrecer: `valid` incluye "el sink no fallo (abrir/escribir/cerrar)" y el path final es "".
 * Mutante que mata: ignorar el retorno de finishPart.
 */
TEST(InputDumpLoss, AFailedCloseIsNeverReportedAsAHealthyTake) {
    Take take;
    DumpRig rig;
    ASSERT_TRUE(take.begin());
    take.dump.armAtRing(rig.ring);
    rig.write(800);
    take.dump.captureFromRing(rig.ring, kRate);

    take.sink->failFinish.store(true);
    std::string finalPath = "basura";
    const InputDumpStats st = take.dump.endTake(&finalPath);
    EXPECT_EQ(st.framesWritten, 800u);
    EXPECT_EQ(st.framesLost, 0u);
    EXPECT_FALSE(st.valid);
    EXPECT_EQ(finalPath, "") << "sin archivo nombrado no hay path final";
}

// ===========================================================================
// 4. EL BORDE DEL NUCLEO: header, nombres, ciclo de vida de la toma
// ===========================================================================

/**
 * AC-055.1 — EL HEADER: cada campo en su offset, con valores que delatan un ancho equivocado.
 *
 * Valores elegidos para que cada error de ancho o signo cambie algo: perdidas y
 * discontinuidades por encima de 2^32 (un u32 las trunca), fuente -1 (un campo sin signo la
 * lee como 4294967295), rate 44100 (byteRate 176400 delata 'rate*2'). Un guardia de 16 bytes
 * detras del buffer detecta un header que escribe de mas.
 * Mutantes que mata: formato 1 en vez de 3; blockAlign/byteRate mal; u32 en los contadores;
 * offsets corridos; escribir mas de 106 bytes.
 */
TEST(InputDumpHeader, EveryFieldSitsAtItsOffsetWithItsWidth) {
    InputDumpStats s;
    s.framesWritten = 123456;
    s.framesLost = (uint64_t{1} << 32) + 5;
    s.discontinuities = (uint64_t{1} << 33) + 7;
    s.sampleRate = 44100;
    s.inputSource = -1;
    s.valid = false;

    std::vector<uint8_t> buf(kInputDumpHeaderBytes + 16, 0xAB);
    writeInputDumpHeader(buf.data(), s);
    for (size_t i = kInputDumpHeaderBytes; i < buf.size(); ++i) {
        ASSERT_EQ(buf[i], 0xAB) << "el header escribio mas alla de sus 106 bytes (offset " << i << ")";
    }
    buf.resize(kInputDumpHeaderBytes);
    ASSERT_NO_FATAL_FAILURE(expectHeaderMatches(buf, s));

    // Y el caso sano, con otro rate y la fuente 3.
    InputDumpStats ok;
    ok.framesWritten = 1000000007;   // 4 * esto sigue entrando en u32
    ok.sampleRate = 32000;
    ok.inputSource = 3;
    ok.valid = true;
    std::vector<uint8_t> buf2(kInputDumpHeaderBytes, 0);
    writeInputDumpHeader(buf2.data(), ok);
    ASSERT_NO_FATAL_FAILURE(expectHeaderMatches(buf2, ok));
}

/**
 * AC-055.1 — EL NOMBRE DEL ARCHIVO INVALIDO: "x.wav" -> "x.invalid.wav".
 *
 * Es lo que le dice al humano (y al script de corpus) que ese archivo no se usa. Bug que atrapa:
 * un nombre que no cambia, o que cambia el directorio (un punto en el path de un directorio).
 * Mutante que mata: reemplazar el primer '.', o buscar el punto en todo el path en vez de al final.
 */
TEST(InputDumpHeader, TheInvalidNameInsertsInvalidBeforeTheExtension) {
    EXPECT_EQ(invalidDumpPathFor("a/b.wav"), "a/b.invalid.wav");
    EXPECT_EQ(invalidDumpPathFor("x.wav"), "x.invalid.wav");
    EXPECT_EQ(invalidDumpPathFor("a/b"), "a/b.invalid.wav") << "sin .wav se agrega el sufijo entero";
    EXPECT_EQ(invalidDumpPathFor("dir.wav/take"), "dir.wav/take.invalid.wav")
        << "el '.wav' de un DIRECTORIO no es la extension";
    EXPECT_EQ(invalidDumpPathFor("x.wav.wav"), "x.wav.invalid.wav");
    EXPECT_EQ(invalidDumpPathFor("a.b.c.wav"), "a.b.c.invalid.wav");
}

/**
 * AC-055.1 — UNA TOMA SIN FRAMES NO ES VALIDA (y su header lo dice).
 *
 * Un WAV vacio con nombre de sano le hace creer al humano que grabo. valid exige framesWritten >
 * 0 y sampleRate > 0; el rate queda en 0 ("nunca llego audio").
 * Mutante que mata: valid = (framesLost == 0 && discontinuities == 0) sin mirar los frames.
 */
TEST(InputDumpLifecycle, ATakeWithoutFramesIsInvalidAndStillNamedAndClosed) {
    Take take;
    DumpRig rig;
    ASSERT_TRUE(take.begin());
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));
    rig.thread.drainOnce();   // un tick sin audio

    std::string finalPath;
    const InputDumpStats st = closeTake(rig, take, &finalPath);
    EXPECT_EQ(st.framesWritten, 0u);
    EXPECT_EQ(st.sampleRate, 0) << "0 = nunca llego audio";
    EXPECT_EQ(st.inputSource, kSource);
    EXPECT_FALSE(st.valid);
    EXPECT_EQ(take.sink->finishCalls, 1) << "el archivo se cierra y se nombra igual";
    EXPECT_FALSE(take.sink->finishValid);
    EXPECT_EQ(finalPath, invalidDumpPathFor(kPath));
    expectHeaderMatches(take.sink->headerCopy(), st);
}

/**
 * AC-055.1 — beginTake: los rechazos, y que un rechazo NO toca la toma viva.
 *
 * Un segundo beginTake con la toma activa devuelve false SIN abrir el segundo sink: con el sink de
 * disco, abrir el mismo `<path>.part` truncaria el archivo de la toma en curso. Null y path vacio
 * tambien se rechazan. Mutantes que mata: abrir antes de chequear la toma activa; aceptar sink null.
 */
TEST(InputDumpLifecycle, BeginTakeRejectsBadArgumentsAndNeverDisturbsALiveTake) {
    Take take;
    DumpRig rig;
    EXPECT_FALSE(take.dump.beginTake(nullptr, kPath, 0)) << "sink null";
    EXPECT_FALSE(take.dump.takeActive());
    EXPECT_FALSE(take.dump.beginTake(std::make_unique<MemorySink>(take.sink), "", 0)) << "path vacio";
    EXPECT_FALSE(take.dump.takeActive());

    ASSERT_TRUE(take.begin());
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));
    rig.write(600);
    rig.thread.drainOnce();

    auto second = std::make_shared<SinkState>();
    EXPECT_FALSE(take.dump.beginTake(std::make_unique<MemorySink>(second), "otra/toma.wav", 1))
        << "ya hay una toma activa";
    EXPECT_EQ(second->openCalls, 0) << "el segundo sink no se abre: truncaria el archivo vivo";
    EXPECT_TRUE(take.dump.takeActive());

    rig.write(400);
    rig.thread.drainOnce();
    const InputDumpStats st = closeTake(rig, take);
    EXPECT_EQ(st.framesWritten, 1000u) << "el intento rechazado reinicio los contadores";
    EXPECT_TRUE(st.valid);
    EXPECT_EQ(st.inputSource, kSource) << "el intento rechazado piso la fuente";
}

/**
 * AC-055.1 — openPart que falla: beginTake devuelve false, no queda toma, y se puede reintentar.
 *
 * Bug que atrapa: una toma "activa" sin sink (el siguiente endTake desreferencia null) o un hilo
 * escritor vivo sin toma. Mutante que mata: marcar la toma activa antes de abrir el sink.
 */
TEST(InputDumpLifecycle, AFailingOpenLeavesNoTakeAndTheNextOneWorks) {
    Take take;
    DumpRig rig;
    take.sink->failOpen.store(true);
    EXPECT_FALSE(take.begin());
    EXPECT_FALSE(take.dump.takeActive());
    EXPECT_EQ(take.sink->openCalls, 1);

    take.sink->failOpen.store(false);
    ASSERT_TRUE(take.begin()) << "tras un open fallido tiene que poder empezar otra toma";
    EXPECT_TRUE(take.dump.takeActive());
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));
    rig.write(300);
    rig.thread.drainOnce();
    const InputDumpStats st = closeTake(rig, take);
    EXPECT_EQ(st.framesWritten, 300u);
    EXPECT_TRUE(st.valid);
}

/**
 * AC-055.1 — endTake SIN TOMA: stats vacias, valid=false, path "" y no cierra nada dos veces.
 *
 * Bug que atrapa: un segundo endTake (doble stop del consumidor) que re-cierra el sink o devuelve
 * los contadores de la toma anterior como si fueran de una nueva.
 */
TEST(InputDumpLifecycle, EndTakeWithoutATakeIsEmptyAndNeverClosesTwice) {
    Take take;
    std::string finalPath = "basura";
    InputDumpStats none = take.dump.endTake(&finalPath);
    EXPECT_FALSE(none.valid);
    EXPECT_EQ(none.framesWritten, 0u);
    EXPECT_EQ(none.framesLost, 0u);
    EXPECT_EQ(none.discontinuities, 0u);
    EXPECT_EQ(finalPath, "");

    DumpRig rig;
    ASSERT_TRUE(take.begin());
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));
    rig.write(100);
    rig.thread.drainOnce();
    EXPECT_TRUE(closeTake(rig, take).valid);
    ASSERT_EQ(take.sink->finishCalls, 1);

    finalPath = "basura";
    none = take.dump.endTake(&finalPath);
    EXPECT_FALSE(none.valid);
    EXPECT_EQ(none.framesWritten, 0u) << "devolvio los contadores de la toma anterior";
    EXPECT_EQ(finalPath, "");
    EXPECT_EQ(take.sink->finishCalls, 1) << "cerro el sink dos veces";
}

/**
 * AC-055.1 — CADA TOMA ARRANCA DE CERO: ni contadores ni invalidez se heredan.
 *
 * La primera toma sale invalida (perdidas); la segunda es limpia y tiene que salir VALIDA y con
 * sus propios contadores. Bug que atrapa: `valid`/contadores que sobreviven a endTake (el
 * instrumento queda "envenenado" tras la primera toma mala hasta reiniciar la app).
 * Mutante que mata: beginTake que no resetea framesLost/discontinuities/sampleRate.
 */
TEST(InputDumpLifecycle, ASecondTakeStartsFromZeroAndIsNotPoisonedByTheFirst) {
    Take take;
    DumpRig rig;
    ASSERT_TRUE(take.begin());
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));
    for (int i = 0; i < 3; ++i) rig.write(4096);   // pisa el ring: perdida
    rig.thread.drainOnce();
    const InputDumpStats first = closeTake(rig, take);
    ASSERT_FALSE(first.valid);
    ASSERT_GT(first.framesLost, 0u);

    auto sink2 = std::make_shared<SinkState>();
    ASSERT_TRUE(take.dump.beginTake(std::make_unique<MemorySink>(sink2), "segunda.wav", 1));
    EXPECT_EQ(take.dump.framesQueued(), 0u);
    EXPECT_EQ(take.dump.framesLost(), 0u);
    EXPECT_EQ(take.dump.framesWritten(), 0u);
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));
    const uint64_t armedAt = rig.ring.writePosition();
    rig.write(700);
    rig.thread.drainOnce();
    const std::vector<float> expected = rig.since(armedAt);

    std::string finalPath;
    const InputDumpStats second = closeTake(rig, take, &finalPath);
    EXPECT_EQ(second.framesWritten, 700u);
    EXPECT_EQ(second.framesLost, 0u);
    EXPECT_EQ(second.discontinuities, 0u);
    EXPECT_EQ(second.inputSource, 1);
    EXPECT_TRUE(second.valid) << "la toma anterior enveneno a esta";
    EXPECT_EQ(finalPath, "segunda.wav");
    EXPECT_EQ(firstMismatch(sink2->samplesCopy(), expected), -1L);
}

/**
 * AC-055.1 — DESTRUIR LA TOMA VIVA LA CIERRA (no deja un `.part` huerfano ni un hilo colgado).
 *
 * Si el consumidor se va sin pedir stop (la app muere, el motor se destruye), el destructor hace
 * endTake: junta el escritor y nombra el archivo. Con frames y sin problemas, valido.
 * Mutante que mata: destructor que solo junta el hilo sin cerrar el sink (queda el `.part`).
 */
TEST(InputDumpLifecycle, DestroyingALiveTakeClosesAndNamesTheFile) {
    auto sink = std::make_shared<SinkState>();
    DumpRig rig;
    {
        TunerInputDump dump;
        ASSERT_TRUE(dump.beginTake(std::make_unique<MemorySink>(sink), kPath, kSource));
        dump.armAtRing(rig.ring);
        rig.write(300);
        dump.captureFromRing(rig.ring, kRate);
    }   // destruye con la toma viva
    EXPECT_EQ(sink->finishCalls, 1);
    EXPECT_TRUE(sink->finishValid);
    EXPECT_EQ(sink->samplesCopy().size(), 300u);
}

// ===========================================================================
// 5. EL BUZON DE AnalysisThread con el hilo CORRIENDO
// ===========================================================================

/**
 * AC-055.1 — EL BUZON: con el hilo corriendo, armar y desarmar SE APLICAN, y lo que sigue captura.
 *
 * `setInputDump` lo llama el control; el estado lo toca solo el hilo de analisis (unico
 * productor), asi que con el hilo vivo viaja por un buzon y el control espera a que se aplique.
 * Se exige que devuelva true, que `inputDumpForTest()` lo refleje, y que lo escrito DESPUES del
 * armado entre completo (el cursor se fija en el momento en que el hilo aplica el pedido).
 * Bug que atrapa: un pedido que nunca se aplica (false tras el techo, o true sin haber hecho nada)
 * o que se aplica sin `armAtRing` (el cursor queda en cero y vuelca historia ajena).
 * Mutantes que mata: buzon sin lectura en el tope de drainOnce; armar sin armAtRing.
 */
TEST(InputDumpMailbox, ARunningAnalysisThreadAppliesArmAndDisarm) {
    Take take;
    DumpRig rig;
    rig.write(300);   // historia previa: no es de la toma
    ASSERT_TRUE(take.begin());
    rig.thread.start(kRate);

    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));
    EXPECT_EQ(rig.thread.inputDumpForTest(), &take.dump) << "el buzon se aplico";

    const uint64_t armedAt = rig.ring.writePosition();
    rig.write(1000);
    ASSERT_TRUE(waitUntil([&] { return take.dump.framesQueued() >= 1000; }))
        << "el hilo vivo no capturo lo escrito tras el armado";
    const std::vector<float> expected = rig.since(armedAt);

    ASSERT_TRUE(rig.thread.setInputDump(nullptr));
    EXPECT_EQ(rig.thread.inputDumpForTest(), nullptr) << "el desarme se aplico";
    rig.thread.stop();

    const InputDumpStats st = take.dump.endTake();
    EXPECT_EQ(st.framesWritten, 1000u);
    EXPECT_TRUE(st.valid);
    EXPECT_EQ(firstMismatch(take.sink->samplesCopy(), expected), -1L);
}

/// Con el hilo PARADO, `setInputDump` aplica directo (sin esperar a nadie) y desarmar sin armar no rompe.
TEST(InputDumpMailbox, WithTheThreadStoppedSetInputDumpAppliesAtOnce) {
    Take take;
    DumpRig rig;
    EXPECT_TRUE(rig.thread.setInputDump(nullptr));   // no hay nada armado: inocuo
    ASSERT_TRUE(take.begin());
    ASSERT_FALSE(rig.thread.isRunning());
    EXPECT_TRUE(rig.thread.setInputDump(&take.dump));
    EXPECT_EQ(rig.thread.inputDumpForTest(), &take.dump);
    EXPECT_TRUE(rig.thread.setInputDump(nullptr));
    EXPECT_EQ(rig.thread.inputDumpForTest(), nullptr);
    take.dump.endTake();
}

/**
 * AC-055.1 — PARAR Y VOLVER A ARRANCAR EL ANALISIS A MITAD DE TOMA LA INVALIDA.
 *
 * Con el hilo parado nadie escribe al ring: el cursor del volcado seguiria como si nada y el WAV
 * empalmaria dos tramos separados por un silencio de reloj que ningun contador del ring delata.
 * Mutante que mata: `start()` que no anota la discontinuidad de una toma con frames.
 */
TEST(InputDumpMailbox, RestartingTheAnalysisMidTakeInvalidatesIt) {
    Take take;
    DumpRig rig;
    ASSERT_TRUE(take.begin());
    rig.thread.start(kRate);
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));
    rig.write(1000);
    ASSERT_TRUE(waitUntil([&] { return take.dump.framesQueued() >= 1000; }));

    rig.thread.stop();
    rig.thread.start(kRate);
    ASSERT_TRUE(rig.thread.setInputDump(nullptr));
    rig.thread.stop();

    const InputDumpStats st = take.dump.endTake();
    EXPECT_GE(st.discontinuities, 1u);
    EXPECT_FALSE(st.valid);
}

/// El gemelo: armar ANTES del primer `start()` (el orden natural, sin frames todavia) no invalida.
/// Mutante que mata: anotar la discontinuidad en todo `start()`, tenga la toma frames o no.
TEST(InputDumpMailbox, ArmingBeforeTheFirstStartDoesNotInvalidateTheTake) {
    Take take;
    DumpRig rig;
    ASSERT_TRUE(take.begin());
    ASSERT_TRUE(rig.thread.setInputDump(&take.dump));   // parado: aplica directo
    rig.thread.start(kRate);
    rig.write(1000);
    ASSERT_TRUE(waitUntil([&] { return take.dump.framesQueued() >= 1000; }));
    ASSERT_TRUE(rig.thread.setInputDump(nullptr));
    rig.thread.stop();

    const InputDumpStats st = take.dump.endTake();
    EXPECT_EQ(st.discontinuities, 0u);
    EXPECT_TRUE(st.valid);
    EXPECT_EQ(st.framesWritten, 1000u);
}

// ===========================================================================
// 6. AnalysisRing: writePosition() y copyForInputDump()
// ===========================================================================

namespace {

/// Estereo cuyo frame `i` vale `first + i` en los dos canales: el mono es EXACTAMENTE esa
/// posicion (hasta 2^24), asi que "esta copia viene de aca" es una igualdad y no una estadistica.
std::vector<float> rampBlock(uint64_t first, int frames) {
    std::vector<float> b(static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const float v = static_cast<float>(first + static_cast<uint64_t>(i));
        b[static_cast<size_t>(i) * 2] = v;
        b[static_cast<size_t>(i) * 2 + 1] = v;
    }
    return b;
}

}  // namespace

/**
 * AC-055.1 — writePosition() cuenta frames escritos desde el ultimo reset.
 * Mutante que mata: devolver mRead; no volver a cero en reset().
 */
TEST(InputDumpRing, WritePositionCountsFramesSinceTheLastReset) {
    AnalysisRing ring;
    EXPECT_EQ(ring.writePosition(), 0u);
    auto a = rampBlock(0, 300);
    ring.writeStereo(a.data(), 300);
    EXPECT_EQ(ring.writePosition(), 300u);
    auto b = rampBlock(300, 5000);
    ring.writeStereo(b.data(), 5000);
    EXPECT_EQ(ring.writePosition(), 5300u);
    ring.reset();
    EXPECT_EQ(ring.writePosition(), 0u);
}

/**
 * AC-055.1 — copyForInputDump: copia en orden, de a `maxFrames`, y NO TOCA al lector del analisis.
 *
 * Es la propiedad de la que cuelga "el analisis no cambia": si la copia moviera mRead o contara en
 * mDropped/mTorn, el volcado alteraria lo que el afinador ve. Se verifica ademas el contrato de
 * "ring vacio => 0 y no escribe en `out`".
 * Mutantes que mata: copiar via read() (mueve mRead); no avanzar el cursor; escribir en el out
 * cuando no hay nada.
 */
TEST(InputDumpRing, CopyReturnsFramesInOrderInChunksAndLeavesTheAnalysisReaderAlone) {
    AnalysisRing ring;
    auto blk = rampBlock(0, 1000);
    ring.writeStereo(blk.data(), 1000);

    uint64_t cursor = 0;
    uint64_t lost = 0;
    std::vector<float> out(300, -1.0f);
    const int expectedChunks[] = {300, 300, 300, 100};
    uint64_t next = 0;
    for (int want : expectedChunks) {
        ASSERT_EQ(ring.copyForInputDump(cursor, out.data(), 300, lost), want);
        for (int i = 0; i < want; ++i) {
            ASSERT_EQ(out[static_cast<size_t>(i)], static_cast<float>(next + static_cast<uint64_t>(i)));
        }
        next += static_cast<uint64_t>(want);
        ASSERT_EQ(cursor, next);
    }
    EXPECT_EQ(lost, 0u);

    // Vacio: devuelve 0, no escribe, no mueve el cursor.
    std::fill(out.begin(), out.end(), 7.5f);
    EXPECT_EQ(ring.copyForInputDump(cursor, out.data(), 300, lost), 0);
    for (float v : out) EXPECT_EQ(v, 7.5f) << "no puede escribir cuando no hay nada";
    EXPECT_EQ(cursor, 1000u);

    // El lector del analisis ni se enteró.
    EXPECT_EQ(ring.readPosition(), 0u);
    EXPECT_EQ(ring.droppedFrames(), 0u);
    EXPECT_EQ(ring.tornReads(), 0u);
    EXPECT_EQ(ring.availableFrames(), 1000u);
}

/**
 * AC-055.1 — copyForInputDump con el ring PISADO: la cuenta es exacta y lo que queda es la cola.
 *
 * Tambien los bordes de la capacidad EFECTIVA (ADENDA A, kEff = kCapacityFrames -
 * kDumpTearGuardFrames): exactamente kEff de distancia NO es perdida (el `>` del contrato, no
 * `>=`) y kEff + 1 pierde exactamente 1. Con kCapacityFrames de distancia ya se pierde: es la
 * guarda contra el bloque en vuelo.
 * Mutantes que mata: `>=` en vez de `>` en el RE-CHEQUEO posterior a la copia (devolveria 0 con el
 * ring justo lleno; el mismo cambio en el chequeo previo es equivalente: no mueve nada); contar mal
 * la cantidad pisada; empezar la copia en el cursor viejo (datos sobrescritos con cara de validos);
 * quitar la guarda (capacidad efectiva = kCapacityFrames).
 */
TEST(InputDumpRing, CopyAfterOverwriteCountsExactlyAndKeepsTheTail) {
    {   // Borde: justo kEff de distancia => sin perdida.
        AnalysisRing ring;
        auto blk = rampBlock(0, static_cast<int>(kEff));
        ring.writeStereo(blk.data(), static_cast<int>(kEff));
        uint64_t cursor = 0, lost = 0;
        std::vector<float> out(kCap);
        ASSERT_EQ(ring.copyForInputDump(cursor, out.data(), static_cast<int>(kCap), lost),
                  static_cast<int>(kEff));
        EXPECT_EQ(lost, 0u);
        EXPECT_EQ(cursor, static_cast<uint64_t>(kEff));
        EXPECT_EQ(out.front(), 0.0f);
        EXPECT_EQ(out[kEff - 1], static_cast<float>(kEff - 1));
    }
    {   // kEff + 1 => pierde exactamente 1.
        AnalysisRing ring;
        auto blk = rampBlock(0, 2048);
        ring.writeStereo(blk.data(), 2048);
        blk = rampBlock(2048, static_cast<int>(kEff) + 1 - 2048);
        ring.writeStereo(blk.data(), static_cast<int>(kEff) + 1 - 2048);
        uint64_t cursor = 0, lost = 0;
        std::vector<float> out(kCap);
        ASSERT_EQ(ring.copyForInputDump(cursor, out.data(), static_cast<int>(kCap), lost),
                  static_cast<int>(kEff));
        EXPECT_EQ(lost, 1u);
        EXPECT_EQ(out.front(), 1.0f);
    }
    {   // kCapacityFrames de distancia: sin la guarda no se perderia nada. Con ella, kCap - kEff.
        AnalysisRing ring;
        auto blk = rampBlock(0, static_cast<int>(kCap));
        ring.writeStereo(blk.data(), static_cast<int>(kCap));
        uint64_t cursor = 0, lost = 0;
        std::vector<float> out(kCap);
        ASSERT_EQ(ring.copyForInputDump(cursor, out.data(), static_cast<int>(kCap), lost),
                  static_cast<int>(kEff));
        EXPECT_EQ(lost, static_cast<uint64_t>(kCap - kEff)) << "la guarda del bloque en vuelo";
        EXPECT_EQ(out.front(), static_cast<float>(kCap - kEff));
        EXPECT_EQ(out[kEff - 1], static_cast<float>(kCap - 1));
    }
    {   // El triple de la capacidad, con maxFrames MENOR que lo disponible.
        AnalysisRing ring;
        uint64_t pos = 0;
        for (int i = 0; i < 6; ++i) {
            auto blk = rampBlock(pos, 4096);
            ring.writeStereo(blk.data(), 4096);
            pos += 4096;
        }
        uint64_t cursor = 100, lost = 0;   // cursor lejos del inicio
        std::vector<float> out(1000);
        ASSERT_EQ(ring.copyForInputDump(cursor, out.data(), 1000, lost), 1000);
        const uint64_t tailStart = pos - kEff;
        EXPECT_EQ(lost, tailStart - 100) << "(w - kEff) - cursor";
        EXPECT_EQ(cursor, tailStart + 1000);
        for (int i = 0; i < 1000; ++i) {
            ASSERT_EQ(out[static_cast<size_t>(i)], static_cast<float>(tailStart + static_cast<uint64_t>(i)));
        }
        EXPECT_EQ(ring.droppedFrames(), 0u) << "la perdida es del volcado, no del lector del analisis";
        EXPECT_EQ(ring.tornReads(), 0u);
    }
}

/**
 * AC-055.1 — ESCRITOR Y COPIADOR CONCURRENTES, SIN DESBORDE: la secuencia copiada es EXACTAMENTE la
 * escrita, sin huecos, sin duplicados y sin una sola perdida contada.
 *
 * Es el gemelo del test de desgarro de abajo y el que impide que la deteccion sea un falso positivo
 * permanente: un copiador que "detecta" desgarros que no existen cuenta perdidas de mas y marca
 * invalida TODA toma. El escritor no escribe hasta que haya lugar respecto del cursor del copiador
 * (misma disciplina que `test_analysis_ring.cpp`: el veredicto no depende del planificador, el ring
 * nunca pasa la capacidad efectiva del volcado por construccion), y los dos hilos siguen siendo concurrentes de verdad.
 * Los valores son la POSICION: una copia con datos de otra vuelta del ring se ve como un valor que
 * no es su posicion. Mutantes que mata: copiar antes de mirar la posicion de escritura; contar
 * perdida cuando `w2 - cursor` es exactamente kEff.
 */
TEST(InputDumpRing, AConcurrentCopierSeesTheExactSequenceWhenTheWriterNeverLapsIt) {
    AnalysisRing ring;
    constexpr uint64_t kTotal = 400000;   // ~48 vueltas al ring; < 2^24: la rampa es exacta en float
    std::atomic<uint64_t> copierCursor{0};
    std::atomic<bool> abortWriter{false};

    std::thread writer([&] {
        std::vector<float> st(2 * 64);
        uint64_t pos = 0;
        while (pos < kTotal && !abortWriter.load(std::memory_order_acquire)) {
            // Una CONDICION, no una duracion: hay lugar para el bloque sin pisar lo no copiado.
            if (pos + 64 - copierCursor.load(std::memory_order_acquire) > kEff) {
                std::this_thread::yield();
                continue;
            }
            for (int i = 0; i < 64; ++i) {
                const float v = static_cast<float>(pos + static_cast<uint64_t>(i));
                st[static_cast<size_t>(i) * 2] = v;
                st[static_cast<size_t>(i) * 2 + 1] = v;
            }
            ring.writeStereo(st.data(), 64);
            pos += 64;
        }
    });

    uint64_t cursor = 0, lost = 0, bad = 0, firstBad = 0;
    std::vector<float> out(2048);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (cursor < kTotal && std::chrono::steady_clock::now() < deadline) {
        const int n = ring.copyForInputDump(cursor, out.data(), 2048, lost);
        copierCursor.store(cursor, std::memory_order_release);
        if (n <= 0) {
            std::this_thread::yield();
            continue;
        }
        const uint64_t start = cursor - static_cast<uint64_t>(n);
        for (int i = 0; i < n; ++i) {
            if (out[static_cast<size_t>(i)] != static_cast<float>(start + static_cast<uint64_t>(i))) {
                if (bad++ == 0) firstBad = start + static_cast<uint64_t>(i);
            }
        }
    }
    abortWriter.store(true, std::memory_order_release);
    writer.join();

    EXPECT_EQ(cursor, kTotal) << "el copiador no llego al final (techo de 20 s)";
    EXPECT_EQ(lost, 0u) << "con el escritor sin pisar nunca, ninguna perdida es real";
    EXPECT_EQ(bad, 0u) << "una copia entrego un valor que no es su posicion (primera: " << firstBad << ")";
    EXPECT_EQ(ring.readPosition(), 0u);
    EXPECT_EQ(ring.droppedFrames(), 0u);
    EXPECT_EQ(ring.tornReads(), 0u);
}

/**
 * AC-055.1 — COPIA DESGARRADA: si el escritor pasa por encima MIENTRAS se copia, la copia se
 * DESCARTA, se cuenta exacta como perdida, y el cursor se reubica en lo mas nuevo que sigue vivo.
 *
 * Determinista, con la misma compuerta que el test gemelo de `read()`
 * (`AReadOverrunWhileCopyingIsDetectedInsteadOfHandingBackTornData`): el copiador se congela justo
 * DESPUES de copiar, el escritor da dos vueltas al ring, y al soltar el copiador tiene que haber
 * detectado el desgarro. Esta es la mitad del contrato que ningun test sin compuerta puede forzar.
 *
 * 🔴 REQUIERE QUE `copyForInputDump` HONRE `gAnalysisRingHoldAfterCopy` / `gAnalysisRingIsInCopy`
 * (bajo `WMA_TEST_HOOKS`, igual que `read()`). La interfaz fijada no lo menciona: es el unico
 * agregado de prueba que este archivo le pide al codigo de produccion. Sin el, este test FALLA con
 * un mensaje que lo dice, no se cuelga.
 *
 * Bug que atrapa: entregar frames pisados con cara de validos (un WAV con 170 ms de basura y
 * nombre de sano). Mutantes que mata: omitir el re-chequeo de `mWritten` tras copiar; contar mal lo
 * perdido; contarlo en tornReads/droppedFrames del lector del analisis.
 */
TEST(InputDumpRing, AWriterLappingTheCopyMidFlightMakesTheCopyBeDiscardedAndCounted) {
    AnalysisRing ring;
    const int seed = static_cast<int>(kCap) / 2;
    auto seedBlock = rampBlock(0, seed);
    ring.writeStereo(seedBlock.data(), seed);

    gAnalysisRingIsInCopy.store(false);
    gAnalysisRingHoldAfterCopy.store(true);

    std::atomic<int> result{-99};
    uint64_t cursor = 0;
    uint64_t lost = 0;
    std::thread copier([&] {
        std::vector<float> out(kCap, -1.0f);
        result.store(ring.copyForInputDump(cursor, out.data(), seed, lost));
    });

    const bool inCopy = waitUntil([] { return gAnalysisRingIsInCopy.load(std::memory_order_acquire); });
    if (!inCopy) {
        // Sin el gancho el copiador no se congela: soltar y fallar con la causa, sin colgar.
        gAnalysisRingHoldAfterCopy.store(false, std::memory_order_release);
        copier.join();
        FAIL() << "copyForInputDump no honra gAnalysisRingHoldAfterCopy: el gancho de prueba "
                  "(bajo WMA_TEST_HOOKS, como en read()) hace falta para forzar el desgarro";
    }

    // Con el copiador congelado ADENTRO, pasarle por encima: dos vueltas completas al ring.
    uint64_t written = static_cast<uint64_t>(seed);
    for (uint32_t b = 0; b < (kCap / 256) * 2; ++b) {
        auto in = rampBlock(written, 256);
        ring.writeStereo(in.data(), 256);
        written += 256;
    }
    gAnalysisRingHoldAfterCopy.store(false, std::memory_order_release);
    copier.join();

    EXPECT_EQ(result.load(), 0) << "la copia desgarrada tiene que descartarse, no entregarse";
    EXPECT_EQ(lost, written - kEff) << "(w2 - cursorDeLaCopia) - kEff, exacto";
    EXPECT_EQ(cursor, written - kEff) << "el cursor se reubica en lo mas viejo que sigue vivo";
    // Las perdidas del VOLCADO no son del lector del analisis.
    EXPECT_EQ(ring.tornReads(), 0u);
    EXPECT_EQ(ring.droppedFrames(), 0u);
    EXPECT_EQ(ring.readPosition(), 0u);

    // Y despues del episodio el ring sigue sirviendo, desde donde quedo el cursor.
    std::vector<float> out(kCap, -1.0f);
    const int got = ring.copyForInputDump(cursor, out.data(), static_cast<int>(kCap), lost);
    ASSERT_EQ(got, static_cast<int>(kEff));
    EXPECT_EQ(out.front(), static_cast<float>(written - kEff));
    EXPECT_EQ(out[kEff - 1], static_cast<float>(written - 1));
    EXPECT_EQ(cursor, written);
}
