/**
 * REQ-055 S0 · tarea 0.6 — EL MAPA DE PERDIDAS (D16, D25, D26): el `wmad` v2 del volcado.
 *
 * AC-055.1 — una toma con perdidas UBICADAS sale con nombre de sana y las declara; una perdida NO
 * ubicada la deja `.invalid.wav`. Lo que estos tests protegen es que el mapa NO MIENTA: si un corte
 * queda un frame corrido, el oraculo y el instrumento miden a traves del hueco con la toma sana, y
 * eso es exactamente el WAV con huecos que parece sano que el volcado existe para impedir.
 *
 * La coordenada de un corte es el frame DEL ARCHIVO (indice en `data`). Por eso cada toma arma con
 * el ring ya avanzado: un corte anotado en la coordenada del ring da otro numero.
 *
 * COMO SE SINCRONIZA (REQ-002): el productor se maneja a mano (`armAtRing`/`captureFromRing` desde
 * el hilo del test, sin `AnalysisThread`), asi que el estado del productor es determinista; al
 * escritor se lo espera por CONDICION (`waitUntil` sobre `framesWritten()`), nunca por duracion.
 */

#include "../AnalysisRing.h"
#include "../TunerInputDump.h"

#include "tests/support/TestWait.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Definidas en AnalysisRing.cpp bajo WMA_TEST_HOOKS.
extern std::atomic<bool> gAnalysisRingHoldAfterCopy;
extern std::atomic<bool> gAnalysisRingIsInCopy;

namespace {

using namespace wma::analysis;
using wma_test::waitUntil;

constexpr int kRate = 44100;   // no 48000: la constante que alguna vez estuvo cableada
constexpr uint32_t kCap = AnalysisRing::kCapacityFrames;
constexpr uint32_t kEff = AnalysisRing::kDumpWindowFrames;
static_assert(kEff == 4096, "estos tests cuentan con la ventana efectiva del volcado de 4096");
const char* const kPath = "cortes/toma.wav";

struct Lcg {
    uint32_t s = 777u;
    float next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(s >> 8) / 8388608.0f - 1.0f;
    }
};

long firstMismatch(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return -2;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0) return static_cast<long>(i);
    }
    return -1;
}

// ---- sink en memoria con freno (el borde de E/S) ----

struct SinkState {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<float> samples;
    std::vector<uint8_t> header;
    std::string openedPath;
    bool finishValid = false;
    bool brakeOn = false;
    void setBrake(bool on) {
        {
            std::lock_guard<std::mutex> l(mu);
            brakeOn = on;
        }
        cv.notify_all();
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
    bool openPart(const std::string& path, size_t) override {
        std::lock_guard<std::mutex> l(mS->mu);
        mS->openedPath = path;
        return true;
    }
    bool appendSamples(const float* samples, size_t count) override {
        std::unique_lock<std::mutex> l(mS->mu);
        // Techo de 30 s solo para que un test roto no cuelgue la suite.
        mS->cv.wait_for(l, std::chrono::seconds(30), [&] { return !mS->brakeOn; });
        mS->samples.insert(mS->samples.end(), samples, samples + count);
        return true;
    }
    std::string finishPart(const uint8_t* header, size_t headerBytes, bool valid) override {
        std::lock_guard<std::mutex> l(mS->mu);
        mS->finishValid = valid;
        mS->header.assign(header, header + headerBytes);
        return valid ? mS->openedPath : invalidDumpPathFor(mS->openedPath);
    }
    std::string partPath() const override { return mS->openedPath + ".part"; }
    bool abortPart() override { return true; }

private:
    std::shared_ptr<SinkState> mS;
};

/// Ring + volcado manejados a mano. `fed` = todo lo escrito al ring desde el armado.
struct Rig {
    std::shared_ptr<SinkState> sink = std::make_shared<SinkState>();
    AnalysisRing ring;
    TunerInputDump dump;
    std::vector<float> fed;
    Lcg rng;

    explicit Rig(size_t queueFrames = TunerInputDump::kDefaultQueueFrames) : dump(queueFrames) {
        ring.setCaptureRate(kRate);
    }
    ~Rig() { sink->setBrake(false); }

    /// Escribe `frames` al ring; si `record`, los anota en `fed` (despues del armado).
    void write(int frames, bool record = true) {
        std::vector<float> st(static_cast<size_t>(frames) * 2);
        for (float& v : st) v = 2.0f * rng.next();
        for (int done = 0; done < frames;) {
            const int n = std::min(frames - done, static_cast<int>(kCap));
            ring.writeStereo(st.data() + static_cast<size_t>(done) * 2, n);
            done += n;
        }
        if (!record) return;
        for (int i = 0; i < frames; ++i) {
            fed.push_back(0.5f * (st[static_cast<size_t>(i) * 2] + st[static_cast<size_t>(i) * 2 + 1]));
        }
    }
    /// Arma con el ring ya avanzado `preroll` frames (no anotados): archivo != ring.
    void arm(int preroll = 300) {
        write(preroll, false);
        ASSERT_TRUE(dump.beginTake(std::make_unique<MemorySink>(sink), kPath, 1));
        dump.armAtRing(ring);
    }
    void drain() { dump.captureFromRing(ring, kRate); }
    /// Drena y espera a que el escritor alcance a lo encolado (la cola nunca se llena sola).
    void drainAndFlush() {
        drain();
        ASSERT_TRUE(waitUntil([&] { return dump.framesWritten() == dump.framesQueued(); }));
    }
    InputDumpStats close(std::string* finalPath = nullptr) { return dump.endTake(finalPath); }
};

/// Lo alimentado sin los frames que declaran los cortes de perdida, cada uno en su lugar.
std::vector<float> fedMinusLossCuts(const std::vector<float>& fed, const InputDumpStats& s) {
    std::vector<float> out;
    size_t src = 0;
    for (uint32_t i = 0; i < s.cutCount; ++i) {
        if (s.cuts[i].kind != kInputDumpCutLoss) continue;
        while (out.size() < s.cuts[i].fileFrame && src < fed.size()) out.push_back(fed[src++]);
        src += static_cast<size_t>(s.cuts[i].lostFrames);
    }
    while (src < fed.size()) out.push_back(fed[src++]);
    return out;
}

uint64_t rdLE(const std::vector<uint8_t>& h, size_t off, int bytes) {
    uint64_t v = 0;
    for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | h[off + static_cast<size_t>(i)];
    return v;
}

}  // namespace

// ===========================================================================
// PERDIDAS UBICADAS
// ===========================================================================

/**
 * AC-055.1 — RING PISADO A MITAD DE TOMA: corte exacto en el frame del archivo, toma sana, y el
 * archivo es los DOS tramos bit a bit.
 *
 * Mutantes que mata: coordenada del ring en vez del archivo (el armado en 300 la corre 300 + ...);
 * `fileFrame +- 1`; perdida ubicada que invalida (`valid` por totales).
 */
TEST(InputDumpCuts, ARingOverwrittenMidTakeIsALocatedCutAndTheTakeIsHealthy) {
    Rig rig;
    rig.arm();
    rig.write(1000);
    rig.drainAndFlush();
    rig.write(3 * 4096);   // 12288 sin copiar: lo mas viejo que la ventana se pierde
    rig.drainAndFlush();
    rig.write(700);
    rig.drainAndFlush();

    std::string finalPath;
    const InputDumpStats st = rig.close(&finalPath);
    ASSERT_EQ(st.cutCount, 1u);
    EXPECT_EQ(st.cuts[0].kind, kInputDumpCutLoss);
    EXPECT_EQ(st.cuts[0].fileFrame, 1000u) << "el corte va en el frame del ARCHIVO donde falta";
    EXPECT_EQ(st.cuts[0].lostFrames, 12288u - kEff);
    EXPECT_EQ(st.cuts[0].fileSpan, 0u);
    EXPECT_EQ(st.framesLost, 12288u - kEff);
    EXPECT_EQ(st.unlocatedFrames, 0u);
    EXPECT_TRUE(st.valid);
    EXPECT_TRUE(rig.sink->finishValid);
    EXPECT_EQ(finalPath, kPath) << "una perdida ubicada sale con nombre de sana";

    // Los dos tramos, bit a bit: [0, 1000) del principio y lo que sigue al hueco.
    const std::vector<float> got = rig.sink->samplesCopy();
    ASSERT_EQ(got.size(), 1000u + kEff + 700u);
    std::vector<float> want(rig.fed.begin(), rig.fed.begin() + 1000);
    want.insert(want.end(), rig.fed.begin() + 1000 + (12288 - kEff), rig.fed.end());
    EXPECT_EQ(firstMismatch(got, want), -1L);

    // Y el header lo declara en su slot.
    const std::vector<uint8_t> h = rig.sink->headerCopy();
    EXPECT_EQ(rdLE(h, 114, 4), 1u);
    EXPECT_EQ(rdLE(h, 122, 8), 1000u);
    EXPECT_EQ(rdLE(h, 130, 8), 12288u - kEff);
    EXPECT_EQ(rdLE(h, 142, 4), kInputDumpCutLoss);
    EXPECT_EQ(rdLE(h, 94, 4), 1u) << "wmad.valid";
}

/**
 * AC-055.1 — COLA LLENA: deterministica (escritor frenado ADENTRO de append, la cola no se vacia).
 * Dos trozos rechazados seguidos (y el ring pisado en el mismo frame) son UNA entrada.
 *
 * Mutantes que mata: sin fusion (tres entradas en el mismo frame); `fileFrame +- 1`; la perdida de
 * cola sin ubicar (toma invalida).
 */
TEST(InputDumpCuts, AFullQueueIsLocatedAndContiguousLossesAreOneEntry) {
    Rig rig(4096);
    rig.sink->setBrake(true);
    rig.arm();
    rig.write(4096);
    rig.drain();               // entra entero: la cola (4096) queda llena hasta soltar el freno
    rig.write(4096 + 4096);    // ventana 4096: 4096 pisados en el ring + 4096 que no entran
    rig.drain();
    rig.write(4096);           // otro trozo que no entra, en el MISMO frame del archivo
    rig.drain();
    EXPECT_EQ(rig.dump.framesQueued(), 4096u) << "premisa: la cola siguio llena";

    rig.sink->setBrake(false);
    ASSERT_TRUE(waitUntil([&] { return rig.dump.framesWritten() == 4096u; }));
    rig.write(500);
    rig.drainAndFlush();

    std::string finalPath;
    const InputDumpStats st = rig.close(&finalPath);
    ASSERT_EQ(st.cutCount, 1u) << "perdidas contiguas en el mismo frame: una sola entrada";
    EXPECT_EQ(st.cuts[0].fileFrame, 4096u);
    EXPECT_EQ(st.cuts[0].lostFrames, 3u * 4096u);
    EXPECT_EQ(st.framesLost, 3u * 4096u);
    EXPECT_TRUE(st.valid);
    EXPECT_EQ(finalPath, kPath);
    EXPECT_EQ(firstMismatch(rig.sink->samplesCopy(), fedMinusLossCuts(rig.fed, st)), -1L);
}

/**
 * AC-055.1 — EL MAPA LLENO: el corte 257 ya no se puede ubicar: va a `unlocatedFrames` y la toma
 * queda invalida, CON los 256 primeros intactos en el mapa.
 *
 * Mutantes que mata: desborde que pisa la ultima entrada; desborde descartado sin contar (toma
 * sana con un hueco sin declarar); capacidad distinta de 256.
 */
TEST(InputDumpCuts, The257thCutCannotBeLocatedAndInvalidatesKeepingTheFirst256) {
    Rig rig;
    rig.arm();
    // Cada vuelta escribe un ring entero sin drenar: 4096 pisados + 4096 copiados.
    for (uint32_t i = 0; i < kInputDumpCutCapacity + 1; ++i) {
        rig.write(static_cast<int>(kCap));
        rig.drainAndFlush();
    }

    std::string finalPath;
    const InputDumpStats st = rig.close(&finalPath);
    ASSERT_EQ(st.cutCount, kInputDumpCutCapacity);
    for (uint32_t i = 0; i < kInputDumpCutCapacity; ++i) {
        ASSERT_EQ(st.cuts[i].fileFrame, uint64_t{i} * kEff) << "corte " << i;
        ASSERT_EQ(st.cuts[i].lostFrames, kEff) << "corte " << i;
    }
    EXPECT_EQ(st.unlocatedFrames, kEff) << "lo del corte 257, sin ubicar";
    EXPECT_EQ(st.framesLost, uint64_t{kInputDumpCutCapacity + 1} * kEff) << "el total sigue entero";
    EXPECT_FALSE(st.valid);
    EXPECT_EQ(finalPath, invalidDumpPathFor(kPath));
}

/// El mapa lleno tambien deja sin ubicar a una COSTURA: `unlocatedDiscontinuities`, invalida.
/// Mutante que mata: una costura con el mapa lleno descartada en silencio.
TEST(InputDumpCuts, ASeamWithTheMapFullCannotBeLocated) {
    Rig rig;
    rig.arm();
    for (uint32_t i = 0; i < kInputDumpCutCapacity; ++i) {
        rig.write(static_cast<int>(kCap));
        rig.drainAndFlush();
    }
    rig.ring.reportCaptureDiscontinuity();
    rig.write(100);
    rig.drainAndFlush();
    const InputDumpStats st = rig.close();
    EXPECT_EQ(st.cutCount, kInputDumpCutCapacity);
    EXPECT_EQ(st.unlocatedFrames, 0u);
    EXPECT_EQ(st.unlocatedDiscontinuities, 1u);
    EXPECT_FALSE(st.valid);
}

// ===========================================================================
// COSTURAS (D25: corte ubicado de largo desconocido)
// ===========================================================================

/**
 * AC-055.1 — COSTURA ADELANTADA (`framesAhead`): el tramo cierra EXACTO en f(S), y la copia topada
 * en S no cambia un bit de lo escrito.
 *
 * Armado en 300; 1000 copiados; aviso con 2500 por delante (S = 3800 en el ring = 3500 en el
 * archivo); 4000 mas (dentro de la ventana) en una sola vuelta. Mutantes que mata: sin tope en S (cierra en el borde de un
 * trozo, 1000 + 4096); costura cerrada al verse (en 1000); coordenada del ring (3800).
 */
TEST(InputDumpCuts, ASeamAheadOfTheCursorClosesExactlyAtItsFileFrame) {
    Rig rig;
    rig.arm();
    rig.write(1000);
    rig.drainAndFlush();
    rig.ring.reportCaptureDiscontinuity(2500);
    rig.write(4000);
    rig.drainAndFlush();

    std::string finalPath;
    const InputDumpStats st = rig.close(&finalPath);
    ASSERT_EQ(st.cutCount, 1u);
    EXPECT_EQ(st.cuts[0].kind, kInputDumpCutSeam);
    EXPECT_EQ(st.cuts[0].fileFrame, 1000u) << "lo = el frame de la lectura anterior";
    EXPECT_EQ(st.cuts[0].fileFrame + st.cuts[0].fileSpan, 3500u) << "f(S) exacto";
    EXPECT_EQ(st.cuts[0].lostFrames, 0u) << "largo desconocido";
    EXPECT_EQ(st.discontinuities, 1u);
    EXPECT_TRUE(st.valid);
    EXPECT_EQ(finalPath, kPath);
    EXPECT_EQ(firstMismatch(rig.sink->samplesCopy(), rig.fed), -1L) << "el tope cambio los bits";
}

/**
 * AC-055.1 — DOS AVISOS MIENTRAS SE COPIA: el ring solo guarda el mas lejano; el OCULTO tiene que
 * caer adentro del tramo. El lo del tramo es el frame de la lectura ANTERIOR (antes de la copia).
 *
 * Con el copiador congelado adentro de `copyForInputDump` (compuerta de test), se escriben 100,
 * aviso (S1 = 1100), 100, aviso (S2 = 1200): la lectura del final de la vuelta ve solo S2.
 * Mutante que mata: `lo` = la lectura actual (el tramo seria [1200, 1200] y dejaria S1 afuera).
 */
TEST(InputDumpCuts, AHiddenSeamBetweenTwoReadsFallsInsideTheSpan) {
    Rig rig;
    rig.arm(0);
    rig.write(1000);

    gAnalysisRingIsInCopy.store(false);
    gAnalysisRingHoldAfterCopy.store(true);
    std::thread producer([&] { rig.drain(); });
    const bool inCopy =
        waitUntil([] { return gAnalysisRingIsInCopy.load(std::memory_order_acquire); });
    if (inCopy) {
        rig.write(100);
        rig.ring.reportCaptureDiscontinuity();   // S1 = 1100: lo pisa S2
        rig.write(100);
        rig.ring.reportCaptureDiscontinuity();   // S2 = 1200
    }
    gAnalysisRingHoldAfterCopy.store(false, std::memory_order_release);
    producer.join();
    ASSERT_TRUE(inCopy) << "copyForInputDump no honro la compuerta de test";
    ASSERT_TRUE(waitUntil([&] { return rig.dump.framesWritten() == rig.dump.framesQueued(); }));

    const InputDumpStats st = rig.close();
    ASSERT_EQ(st.cutCount, 1u);
    const uint64_t lo = st.cuts[0].fileFrame;
    const uint64_t hi = lo + st.cuts[0].fileSpan;
    EXPECT_LE(lo, 1100u) << "el aviso oculto (S1) quedo antes del tramo";
    EXPECT_GE(hi, 1200u) << "S2 quedo despues del tramo";
    EXPECT_LE(hi, 1200u) << "el tramo no pasa de lo copiado";
    EXPECT_TRUE(st.valid);
}

/**
 * AC-055.1 — UNA PERDIDA QUE SALTA LA COSTURA PENDIENTE cierra el tramo AHI (en el frame de la
 * perdida), no despues de lo que se copia a continuacion.
 *
 * Mutante que mata: no cerrar en el salto (el tramo se estiraria hasta 1000 + 500 copiados).
 */
TEST(InputDumpCuts, ALossThatJumpsThePendingSeamClosesTheSpanThere) {
    Rig rig;
    rig.arm();
    rig.write(1000);
    rig.drainAndFlush();
    rig.ring.reportCaptureDiscontinuity(500);   // S 500 frames delante del cursor
    rig.write(static_cast<int>(kCap));          // pisa el ring mas alla de S
    rig.drainAndFlush();

    const InputDumpStats st = rig.close();
    ASSERT_EQ(st.cutCount, 2u);
    // Mismo frame: la perdida se anoto primero, el orden estable la deja primero.
    EXPECT_EQ(st.cuts[0].kind, kInputDumpCutLoss);
    EXPECT_EQ(st.cuts[0].fileFrame, 1000u);
    EXPECT_EQ(st.cuts[0].lostFrames, kCap - kEff);
    EXPECT_EQ(st.cuts[1].kind, kInputDumpCutSeam);
    EXPECT_EQ(st.cuts[1].fileFrame, 1000u);
    EXPECT_EQ(st.cuts[1].fileSpan, 0u) << "la perdida salto S: el tramo cierra en ella";
    EXPECT_TRUE(st.valid);
    EXPECT_EQ(firstMismatch(rig.sink->samplesCopy(), fedMinusLossCuts(rig.fed, st)), -1L);
}

/**
 * AC-055.1 — EL MAPA SALE ORDENADO por frame del archivo: un tramo de costura se anota al CERRARSE,
 * despues de una perdida que cae adentro de el, pero va antes en el mapa.
 *
 * Mutante que mata: el mapa en orden de anotacion (el oraculo lo rechaza por desordenado).
 */
TEST(InputDumpCuts, TheMapIsSortedByFileFrame) {
    Rig rig;
    rig.arm();
    rig.write(1000);
    rig.drainAndFlush();
    rig.ring.reportCaptureDiscontinuity(20000);   // lejos: pendiente hasta el cierre
    rig.write(1000);
    rig.drainAndFlush();
    rig.write(static_cast<int>(kCap));            // perdida en el frame 2000, adentro del tramo
    rig.drainAndFlush();

    const InputDumpStats st = rig.close();
    ASSERT_EQ(st.cutCount, 2u);
    EXPECT_EQ(st.cuts[0].kind, kInputDumpCutSeam);
    EXPECT_EQ(st.cuts[0].fileFrame, 1000u);
    EXPECT_EQ(st.cuts[0].fileFrame + st.cuts[0].fileSpan, st.framesWritten)
        << "pendiente al cerrar: cierra en lo escrito";
    EXPECT_EQ(st.cuts[1].kind, kInputDumpCutLoss);
    EXPECT_EQ(st.cuts[1].fileFrame, 2000u);
    const std::vector<uint8_t> h = rig.sink->headerCopy();
    EXPECT_EQ(rdLE(h, 122, 8), 1000u) << "el header tambien va ordenado";
    EXPECT_EQ(rdLE(h, 146, 8), 2000u);
    EXPECT_TRUE(st.valid);
}

/**
 * AC-055.1 — DOS COSTURAS YA COPIADAS QUE SE TOCAN se fusionan en un tramo; dos separadas quedan dos.
 *
 * Mutante que mata: sin fusion de tramos solapados (dos entradas para el mismo intervalo).
 */
TEST(InputDumpCuts, OverlappingSeamSpansFuseAndSeparateOnesDoNot) {
    Rig rig;
    rig.arm();
    rig.write(1000);
    rig.drainAndFlush();
    rig.ring.reportCaptureDiscontinuity();   // en el cursor (1000): tramo [1000, 1000]
    rig.drainAndFlush();
    rig.ring.reportCaptureDiscontinuity(1);  // S = 1001, apenas delante: tramo [1000, 1001]
    rig.write(10);
    rig.drainAndFlush();
    rig.write(3000);
    rig.drainAndFlush();
    rig.ring.reportCaptureDiscontinuity();   // en 4010, lejos del anterior
    rig.write(10);
    rig.drainAndFlush();

    const InputDumpStats st = rig.close();
    ASSERT_EQ(st.cutCount, 2u);
    EXPECT_EQ(st.cuts[0].fileFrame, 1000u);
    EXPECT_EQ(st.cuts[0].fileSpan, 1u) << "los dos primeros, fusionados";
    EXPECT_EQ(st.cuts[1].fileFrame, 4010u);
    EXPECT_EQ(st.cuts[1].fileSpan, 0u);
    EXPECT_EQ(st.discontinuities, 3u) << "el total cuenta los avisos";
    EXPECT_TRUE(st.valid);
}

// ===========================================================================
// LO QUE NO SE UBICA SIGUE INVALIDANDO (D26)
// ===========================================================================

/**
 * AC-055.1 — `noteInputDiscontinuity` (cambio de fuente, reinicio del afinador con la toma viva,
 * D26) NO va al mapa: invalida. Mutante que mata: fuente/reinicio anotados como corte ubicado.
 */
TEST(InputDumpCuts, AnUnlocatedDiscontinuityNeverEntersTheMapAndInvalidates) {
    Rig rig;
    rig.arm();
    rig.write(1000);
    rig.drainAndFlush();
    rig.dump.noteInputDiscontinuity();
    rig.write(1000);
    rig.drainAndFlush();

    std::string finalPath;
    const InputDumpStats st = rig.close(&finalPath);
    EXPECT_EQ(st.cutCount, 0u);
    EXPECT_EQ(st.unlocatedDiscontinuities, 1u);
    EXPECT_EQ(st.discontinuities, 1u);
    EXPECT_FALSE(st.valid);
    EXPECT_EQ(finalPath, invalidDumpPathFor(kPath));
    EXPECT_EQ(rdLE(rig.sink->headerCopy(), 106, 8), 1u) << "wmad.unlocatedDiscontinuities";
}

/// El rate que cambia o que no se conoce no se ubica: invalida, sin entrar al mapa.
TEST(InputDumpCuts, ARateChangeOrAnUnknownRateIsUnlocated) {
    {
        Rig rig;
        rig.arm();
        rig.write(500);
        rig.drainAndFlush();
        rig.write(500);
        rig.dump.captureFromRing(rig.ring, 32000);
        const InputDumpStats st = rig.close();
        EXPECT_EQ(st.cutCount, 0u);
        EXPECT_EQ(st.unlocatedDiscontinuities, 1u);
        EXPECT_FALSE(st.valid);
    }
    {
        Rig rig;
        rig.arm();
        rig.write(500);
        rig.dump.captureFromRing(rig.ring, 0);
        const InputDumpStats st = rig.close();
        EXPECT_EQ(st.cutCount, 0u);
        EXPECT_EQ(st.unlocatedDiscontinuities, 1u);
        EXPECT_FALSE(st.valid);
    }
}
