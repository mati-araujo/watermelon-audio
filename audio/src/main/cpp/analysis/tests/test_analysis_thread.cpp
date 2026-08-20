/**
 * REQ-001 S1 — el thread de analisis y la publicacion del snapshot.
 *
 * Cubre 1.4 (arranca, para y reinicia sin fugas ni bloquear al thread de
 * captura) y 1.5 (el snapshot nunca entrega un estado a medio escribir).
 */

#include "tests/support/TestWait.h"
#include "../AnalysisRing.h"
#include "../AnalysisSnapshot.h"
#include "../AnalysisThread.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

extern std::atomic<bool> gSnapshotHoldMidPublish;
extern std::atomic<bool> gSnapshotIsMidPublish;
extern std::atomic<bool> gSnapshotHoldMidRead;
extern std::atomic<bool> gSnapshotIsMidRead;

namespace {

using namespace wma::analysis;

constexpr int kRate = 44100;   // NO 48000: es la constante que el motor tenia
                               // hardcodeada, y usarla aca haria que un bug de
                               // propagacion pase inadvertido.

std::vector<float> toneBlock(int frames, float amp) {
    std::vector<float> b(static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const float v = static_cast<float>(
            amp * std::sin(2.0 * M_PI * 187.5 * i / kRate));
        b[static_cast<size_t>(i) * 2] = v;
        b[static_cast<size_t>(i) * 2 + 1] = v;
    }
    return b;
}

/// Espera a que `pred` se cumpla, con techo. Devuelve false si se agoto — asi
/// una falla se ve como una asercion y no como un test colgado.
template <typename Pred>
bool waitFor(Pred pred, std::chrono::milliseconds cap = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + cap;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

}  // namespace

// ---------------------------------------------------------------------------
// 1.4 — ciclo de vida
// ---------------------------------------------------------------------------

TEST(AnalysisThread, StartsStopsAndRestartsWithoutLeakingOrHanging) {
    AnalysisRing ring;
    AnalysisSnapshot snap;
    AnalysisThread th(ring, snap);

    EXPECT_FALSE(th.isRunning());

    for (int cycle = 0; cycle < 5; ++cycle) {
        th.start(kRate);
        EXPECT_TRUE(th.isRunning()) << "ciclo " << cycle;

        // No alcanza con dormir y suponer: se espera a que el lazo DE VUELTAS.
        const uint64_t before = th.ticks();
        ASSERT_TRUE(waitFor([&] { return th.ticks() > before + 1; }))
            << "el thread no avanzo en el ciclo " << cycle;

        th.stop();
        EXPECT_FALSE(th.isRunning()) << "ciclo " << cycle;

        // Y despues de parar, para de verdad: el contador deja de moverse.
        //
        // AUSENCIA. `stop()` joinea, asi que con el codigo sano la propiedad vale
        // POR CONSTRUCCION y la espera no aporta; lo que la espera compra es
        // detectar un `stop()` que dejara el thread vivo — ahi el contador SI se
        // moveria, y sin ventana no habria con que verlo.
        const uint64_t after = th.ticks();
        wma_test::sleepFixed(std::chrono::milliseconds(25));
        EXPECT_EQ(th.ticks(), after) << "sigue girando despues de stop()";
    }

    // Idempotencia en los dos sentidos.
    th.stop();
    th.stop();
    EXPECT_FALSE(th.isRunning());
    th.start(kRate);
    th.start(kRate);
    EXPECT_TRUE(th.isRunning());
    th.stop();
}

/**
 * El thread de captura no se puede quedar esperando al de analisis, ni siquiera
 * mientras este arranca y para en un lazo. Lo unico que comparten es el ring, y
 * ahi el escritor pisa lo viejo y sigue.
 *
 * Se mide el PEOR tiempo de una escritura, no el promedio: un promedio bueno
 * con un pico de 50 ms seguiria siendo un underrun.
 */
TEST(AnalysisThread, TheCaptureWriterIsNeverBlockedWhileTheThreadChurns) {
    AnalysisRing ring;
    AnalysisSnapshot snap;
    AnalysisThread th(ring, snap);

    const int block = 256;
    const auto tone = toneBlock(block, 0.31f);

    std::atomic<bool> stop{false};
    std::atomic<int64_t> worstMicros{0};
    std::atomic<uint64_t> writes{0};

    std::thread writer([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const auto t0 = std::chrono::steady_clock::now();
            ring.writeStereo(tone.data(), block);
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0).count();
            if (us > worstMicros.load(std::memory_order_relaxed)) {
                worstMicros.store(us, std::memory_order_relaxed);
            }
            writes.fetch_add(1, std::memory_order_relaxed);
            // ESTIMULO: marca el paso del escritor RT. La duracion es el experimento.
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    for (int cycle = 0; cycle < 10; ++cycle) {
        th.start(kRate);
        // ESTIMULO: le da al thread un rato de vida antes de pararlo, que es lo que
        // ejercita el ciclo start/stop. No sincroniza nada — lo que se afirma
        // despues son los contadores, no que este sleep haya alcanzado.
        // WAIT-OK: estimulo — le da vida al thread antes de pararlo; es lo que
        //          ejercita el ciclo start/stop, no sincroniza una asercion.
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        th.stop();
    }
    stop.store(true, std::memory_order_release);
    writer.join();

    EXPECT_GT(writes.load(), 50u) << "el escritor casi no llego a escribir";
    // Cota deliberadamente FLOJA: lo que separa es "no espera a nadie" de "se
    // queda trabado", no un presupuesto de latencia. Bajo carga alta el
    // planificador solo ya explica varios ms, y afinar esto lo volveria un test
    // que depende de la maquina — el error que el ring ya me cobro hoy.
    EXPECT_LT(worstMicros.load(), 50000)
        << "una escritura tardo " << worstMicros.load() << " us: parece bloqueo";
}

// ---------------------------------------------------------------------------
// 1.5 — el snapshot nunca se lee a medio escribir
// ---------------------------------------------------------------------------

TEST(AnalysisSnapshotTest, AnUnpublishedSnapshotReportsNoDataAndLeavesTheBufferAlone) {
    AnalysisSnapshot snap;
    float out[kSnapshotValueCount];
    for (int i = 0; i < kSnapshotValueCount; ++i) out[i] = 42.5f;

    EXPECT_FALSE(snap.hasData());
    EXPECT_FALSE(snap.read(out));
    for (int i = 0; i < kSnapshotValueCount; ++i) {
        EXPECT_FLOAT_EQ(out[i], 42.5f)
            << "escribio ceros donde no habia medicion (valor " << i << ")";
    }
}

/**
 * LA PROPIEDAD, forzada con compuerta.
 *
 * El escritor queda detenido con LA MITAD de los campos actualizados. En ese
 * estado el lector no puede llevarse una mezcla: o dice "no hay dato", o
 * devuelve el juego ANTERIOR completo. Nunca mitad y mitad.
 *
 * Los dos juegos son todo-unos y todo-doses justamente para que una mezcla sea
 * inconfundible: alcanza con ver los dos valores presentes a la vez.
 */
TEST(AnalysisSnapshotTest, AHalfWrittenPublishIsNeverHandedToAReader) {
    AnalysisSnapshot snap;

    float a[kSnapshotValueCount];
    float b[kSnapshotValueCount];
    for (int i = 0; i < kSnapshotValueCount; ++i) { a[i] = 1.0f; b[i] = 2.0f; }

    snap.publish(a);
    float out[kSnapshotValueCount];
    ASSERT_TRUE(snap.read(out));
    for (int i = 0; i < kSnapshotValueCount; ++i) ASSERT_FLOAT_EQ(out[i], 1.0f);

    gSnapshotIsMidPublish.store(false);
    gSnapshotHoldMidPublish.store(true);
    std::thread writer([&] { snap.publish(b); });

    ASSERT_TRUE(waitFor([] { return gSnapshotIsMidPublish.load(); }))
        << "la compuerta nunca se activo: el test no llego a la ventana";

    // Con el escritor congelado a mitad de camino, insistir.
    for (int attempt = 0; attempt < 200; ++attempt) {
        float got[kSnapshotValueCount];
        for (int i = 0; i < kSnapshotValueCount; ++i) got[i] = -1.0f;
        if (!snap.read(got)) continue;         // "no hay dato" es respuesta valida
        bool sawOne = false, sawTwo = false;
        for (int i = 0; i < kSnapshotValueCount; ++i) {
            if (got[i] == 1.0f) sawOne = true;
            if (got[i] == 2.0f) sawTwo = true;
        }
        ASSERT_FALSE(sawOne && sawTwo)
            << "lectura DESGARRADA en el intento " << attempt
            << ": mezclo el juego viejo con el nuevo";
    }

    gSnapshotHoldMidPublish.store(false, std::memory_order_release);
    writer.join();

    // Y una vez que el escritor termina, se ve el juego nuevo ENTERO.
    ASSERT_TRUE(snap.read(out));
    for (int i = 0; i < kSnapshotValueCount; ++i) {
        EXPECT_FLOAT_EQ(out[i], 2.0f) << "valor " << i;
    }
}

/**
 * LA OTRA VENTANA, y es la que la validacion del seqlock existe para cubrir.
 *
 * El test de arriba detiene al ESCRITOR, y eso deja el contador en impar: el
 * lector sale por el chequeo de paridad y nunca llega a comparar el contador.
 * Medido: con solo aquel test, un mutante que borra la validacion entera
 * SOBREVIVE — o sea que el mecanismo central quedaba sin medir.
 *
 * Este detiene al LECTOR a mitad de su copia y deja que el escritor complete un
 * publish ENTERO encima. El contador esta PAR en las dos puntas de la ventana,
 * asi que la paridad no ayuda: lo unico que puede detectar la mezcla es que el
 * contador CAMBIO. Sin esa comparacion, el lector devuelve mitad y mitad.
 */
TEST(AnalysisSnapshotTest, APublishThatLandsMidCopyIsCaughtByTheSequenceCheck) {
    AnalysisSnapshot snap;

    float a[kSnapshotValueCount];
    float b[kSnapshotValueCount];
    for (int i = 0; i < kSnapshotValueCount; ++i) { a[i] = 1.0f; b[i] = 2.0f; }
    snap.publish(a);

    gSnapshotIsMidRead.store(false);
    gSnapshotHoldMidRead.store(true);

    std::atomic<bool> readOk{false};
    float got[kSnapshotValueCount];
    for (int i = 0; i < kSnapshotValueCount; ++i) got[i] = -1.0f;

    std::thread reader([&] { readOk.store(snap.read(got)); });

    ASSERT_TRUE(waitFor([] { return gSnapshotIsMidRead.load(); }))
        << "la compuerta del lector nunca se activo";

    // Con el lector congelado a mitad de copia, publicar el juego nuevo ENTERO.
    snap.publish(b);

    gSnapshotHoldMidRead.store(false, std::memory_order_release);
    reader.join();

    ASSERT_TRUE(readOk.load()) << "el reintento tendria que haber conseguido un juego entero";

    bool sawOne = false, sawTwo = false;
    for (int i = 0; i < kSnapshotValueCount; ++i) {
        if (got[i] == 1.0f) sawOne = true;
        if (got[i] == 2.0f) sawTwo = true;
    }
    EXPECT_FALSE(sawOne && sawTwo)
        << "devolvio una MEZCLA: la validacion del contador no la agarro";
    for (int i = 0; i < kSnapshotValueCount; ++i) {
        EXPECT_FLOAT_EQ(got[i], 2.0f) << "valor " << i;
    }
}

// ---------------------------------------------------------------------------
// Lo que el thread publica, y lo que NO inventa
// ---------------------------------------------------------------------------

TEST(AnalysisThread, ItPublishesTheCaptureRateItWasGivenNotAConstant) {
    AnalysisRing ring;
    AnalysisSnapshot snap;
    AnalysisThread th(ring, snap);

    const auto tone = toneBlock(1024, 0.37f);
    ring.writeStereo(tone.data(), 1024);
    th.start(kRate);
    ASSERT_TRUE(waitFor([&] { return snap.hasData(); }));
    th.stop();

    float out[kSnapshotValueCount];
    ASSERT_TRUE(snap.read(out));
    EXPECT_FLOAT_EQ(out[kSnapCaptureSampleRate], static_cast<float>(kRate))
        << "publico un rate distinto del que se le dio";
    EXPECT_GT(out[kSnapLevelRms], 0.0f);
    EXPECT_GT(out[kSnapFramesAnalyzed], 0.0f);
}

/**
 * S1 no tiene estimador: cents, angulo e incertidumbre todavia no existen. Se
 * publican en NaN y NO en cero, porque `0.0` cents es un valor PLAUSIBLE
 * —afinado exacto— que un consumidor mostraria como medicion.
 */
TEST(AnalysisThread, TheFieldsStageTwoWillFillAreNaNNotZero) {
    AnalysisRing ring;
    AnalysisSnapshot snap;
    AnalysisThread th(ring, snap);

    const auto tone = toneBlock(1024, 0.37f);
    ring.writeStereo(tone.data(), 1024);
    th.start(kRate);
    ASSERT_TRUE(waitFor([&] { return snap.hasData(); }));
    th.stop();

    float out[kSnapshotValueCount];
    ASSERT_TRUE(snap.read(out));
    EXPECT_TRUE(std::isnan(out[kSnapCents]))       << "cents salio " << out[kSnapCents];
    EXPECT_TRUE(std::isnan(out[kSnapPhaseAngle]))  << "angulo salio " << out[kSnapPhaseAngle];
    EXPECT_TRUE(std::isnan(out[kSnapUncertainty])) << "incert. salio " << out[kSnapUncertainty];
}
