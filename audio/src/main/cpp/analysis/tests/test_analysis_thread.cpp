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

#include <array>
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


/**
 * Cuerda de 4 parciales armonicos a `f0`, en estereo, para alimentar el ring.
 * REQ-003: hace falta contenido armonico real — un seno puro no ejercita el
 * descarte por dominio, que es una decision POR PARCIAL.
 *
 * 🔴 `startFrame` NO ES OPCIONAL, Y COSTO UN FALSO ROJO. El estimador integra
 * FASE a lo largo de ventanas de 4096 frames. Si cada bloque se genera
 * arrancando en fase 0, la señal lleva una discontinuidad artificial cada
 * `frames` muestras y el estimador mide ESO: medido, publicaba +5,62 cents con
 * la cuerda 5 cents ABAJO, y el defecto era del test, no del motor. El llamador
 * tiene que ir corriendo el offset.
 */
std::vector<float> stringBlock(double f0, int frames, int startFrame = 0,
                               float amp = 0.5f) {
    std::vector<float> b(static_cast<size_t>(frames) * 2, 0.0f);
    for (int i = 0; i < frames; ++i) {
        double s = 0.0;
        const double tIdx = static_cast<double>(startFrame + i);
        for (int n = 1; n <= 4; ++n) {
            s += (amp / n) * std::sin(2.0 * M_PI * f0 * n * tIdx / kRate);
        }
        b[static_cast<size_t>(i) * 2]     = static_cast<float>(s);
        b[static_cast<size_t>(i) * 2 + 1] = static_cast<float>(s);
    }
    return b;
}

/// Ruido audible SIN altura: la gruesa no engancha, asi que el strobe se queda
/// sin control. Determinista a proposito (LCG propio, no `random_device`).
std::vector<float> noiseBlock(int frames, float amp = 0.4f) {
    std::vector<float> b(static_cast<size_t>(frames) * 2, 0.0f);
    unsigned st = 12345u;
    for (int i = 0; i < frames; ++i) {
        st = st * 1664525u + 1013904223u;
        const float v = amp * (static_cast<float>(st >> 8) / 8388608.0f - 1.0f);
        b[static_cast<size_t>(i) * 2]     = v;
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


/**
 * Espera a que el ring tenga lugar para `frames` **sin pisar nada sin leer**.
 *
 * Es una CONDICIÓN, no una duración: no depende de la velocidad de la máquina,
 * así que no se puede quedar corta en un runner cargado. El techo sólo existe
 * para que un consumidor muerto se vea como aserción y no como test colgado.
 *
 * Está separada de `feedAtAnalysisPace` para que se pueda probar sola contra un
 * consumidor lento — ver `TheFeederNeverOverrunsTheRingWithASlowConsumer`, que
 * es lo que impide que esta compuerta se vuelva decorativa.
 */
inline bool waitForRoom(AnalysisRing& ring, int frames,
                        std::chrono::milliseconds cap = std::chrono::seconds(5)) {
    return waitFor([&] {
        return ring.availableFrames() + static_cast<uint32_t>(frames)
               <= AnalysisRing::kCapacityFrames;
    }, cap);
}

/**
 * Alimenta el ring **al ritmo del ANÁLISIS**, no al del reloj.
 *
 * 🔴 ESTO NO ES UN DETALLE DE ESTILO, Y COSTÓ UN ROJO EN EL TSAN DEL CI.
 * La versión anterior escribía N bloques con un `sleep` fijo entre medio. Con
 * el thread de análisis a velocidad normal alcanzaba; **bajo TSan, que lo hace
 * ~10x más lento, el productor le gana al consumidor y el ring PISA frames**.
 * El estimador entonces integra fase sobre muestras no contiguas, la fase salta,
 * y la lectura sale fuera de presupuesto: `EXPECT_NEAR(cents, -5, 0.1)` en rojo.
 * Verde en esta máquina, rojo en el runner — el defecto que REQ-002 persigue.
 *
 * Y agrandar el sleep NO lo arregla: sigue siendo una duración adivinada contra
 * una máquina de velocidad desconocida. Lo que lo arregla es **esperar a que el
 * análisis consuma** antes de escribir el bloque siguiente, que no depende de
 * ninguna velocidad.
 *
 * 🔴 **REQ-005 S3: la primera versión de ese arreglo todavía tenía escapatoria.**
 * Esperaba hasta 200 ms y, si el análisis no había avanzado, **escribía igual** —
 * o sea que bajo un consumidor lento seguía pisando el ring, sólo que más tarde.
 * La condición de ahora es que el ring **tenga lugar**, que no es una duración y
 * no se puede quedar corta en una máquina más lenta.
 *
 * Devuelve false en los dos casos en que **la muestra salió corta** —nunca hubo
 * lugar, o no se llegó a la meta dentro del techo— y nunca escribiendo audio no
 * contiguo. Que el llamador no pueda emitir un veredicto de exactitud sobre una
 * muestra corta es justamente AC-005.4: "la muestra salió corta" y "el sistema
 * cambió" son dos fallas distintas y tienen que verse distintas.
 */
template <typename MakeBlock>
bool feedAtAnalysisPace(AnalysisRing& ring, AnalysisSnapshot& snap,
                        MakeBlock makeBlock, int blocks, int frames = 1024) {
    auto analysed = [&]() -> double {
        float o[kSnapshotValueCount];
        return snap.read(o) ? static_cast<double>(o[kSnapFramesAnalyzed]) : 0.0;
    };
    const double goal = static_cast<double>(blocks) * frames;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    int written = 0;
    while (analysed() < goal && std::chrono::steady_clock::now() < deadline) {
        // 🔴 SE ESPERA LUGAR EN EL RING, Y ESO NO ES UNA DURACIÓN (REQ-005 S3).
        //
        // La versión anterior esperaba HASTA 200 ms a que el análisis avanzara y
        // después **escribía igual**. Esa escapatoria convierte "el consumidor
        // viene lento" en "el estimador integra fase sobre muestras no
        // contiguas", y eso no da un rojo honesto: da un número BIEN FORMADO Y
        // EQUIVOCADO que el motor encima declara CONVERGIDO.
        //
        // Medido (REQ-005 S3, tarea 3.1): con un hueco sostenido de 64 frames la
        // lectura sale a 1,75 cents del valor real —35x el presupuesto de 0,1—
        // y σ publica 0,076, POR DEBAJO del umbral de convergencia. La
        // incertidumbre no ve la discontinuidad, así que ninguna guarda basada
        // en σ puede atajar esto.
        //
        // La condición correcta no tiene techo adivinado: es que el ring TENGA
        // LUGAR para el bloque siguiente. Con el consumidor al día se cumple
        // siempre y no cuesta nada; con el consumidor lento se espera en vez de
        // pisar. El único techo es el de 30 s de abajo, que es "el análisis se
        // murió", no "todavía no llegó".
        if (!waitForRoom(ring, frames)) {
            return false;   // la muestra salió corta: nunca hubo lugar
        }
        const auto blk = makeBlock(written * frames);
        ring.writeStereo(blk.data(), frames);
        ++written;
    }
    return analysed() >= goal;
}
}  // namespace

// ---------------------------------------------------------------------------
// 1.4 — ciclo de vida
// ---------------------------------------------------------------------------

/**
 * REQ-005 S3 — **la compuerta de capacidad no puede ser decorativa**.
 *
 * `feedAtAnalysisPace` existe para que el test nunca alimente al estimador con
 * audio no contiguo. La versión anterior esperaba 200 ms y **escribía igual**;
 * ésta espera a que haya lugar. La diferencia sólo se ve con un consumidor
 * LENTO, que en la suite normal nunca aparece — así que se fabrica uno.
 *
 * 🔴 POR QUÉ ESTO IMPORTA MÁS QUE UN DROP CONTADO. Medido en la tarea 3.1: con
 * un hueco sostenido de 64 frames la lectura sale a 1,75 cents del valor real
 * —35x el presupuesto— y σ publica 0,076, **por debajo** del umbral de
 * convergencia. O sea que el motor lo declara CONVERGIDO y ninguna guarda
 * basada en σ lo puede atajar. La única defensa es no producir el hueco.
 *
 * EL MUTANTE QUE ESTE TEST MATA: volver la compuerta al techo de 200 ms que
 * escribe igual. Con este consumidor, esa versión pisa el ring y `droppedFrames`
 * se va a miles.
 */
TEST(AnalysisThread, TheFeederNeverOverrunsTheRingWithASlowConsumer) {
    AnalysisRing ring;
    std::atomic<bool> draining{true};
    std::atomic<uint64_t> consumed{0};

    // Un consumidor MUCHO más lento que el productor: 256 frames por vuelta con
    // una pausa, contra bloques de 1024 que el alimentador querría meter sin
    // parar. Es el runner cargado, sin depender de que el runner esté cargado.
    std::thread slow([&] {
        std::vector<float> scratch(AnalysisRing::kCapacityFrames, 0.0f);
        while (draining.load(std::memory_order_acquire)) {
            const int got = ring.read(scratch.data(), 256);
            if (got > 0) consumed.fetch_add(static_cast<uint64_t>(got));
            // WAIT-OK: estimulo — la lentitud del consumidor ES el experimento.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    const auto tone = toneBlock(1024, 0.3f);
    int written = 0;
    for (int i = 0; i < 60; ++i) {
        ASSERT_TRUE(waitForRoom(ring, 1024))
            << "no hubo lugar en 5 s con el consumidor drenando: la muestra "
               "salio corta (bloque " << i << ")";
        ring.writeStereo(tone.data(), 1024);
        ++written;
    }

    draining.store(false, std::memory_order_release);
    slow.join();

    EXPECT_EQ(ring.droppedFrames(), 0u)
        << "el alimentador piso " << ring.droppedFrames() << " frames con un "
           "consumidor lento. El estimador integraria fase sobre muestras no "
           "contiguas y publicaria un numero bien formado y equivocado — que "
           "ademas declararia CONVERGIDO, porque sigma no ve la discontinuidad.";
    EXPECT_EQ(written, 60) << "el alimentador no llego a escribir todo";
}

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

// ---------------------------------------------------------------------------
// REQ-003 S1 — lo que se publica cuando la lectura fina no se puede sostener
// ---------------------------------------------------------------------------

/**
 * AC-003.3 — con la fina ausente, **la gruesa se sigue publicando**.
 *
 * Es lo que le deja al usuario "que nota es y de que lado estoy" cuando pierde
 * "cuanto exactamente". Un afinador que se apaga entero al salirse del rango
 * fino es peor que uno que degrada: el que degrada todavia sirve para llegar.
 *
 * Se verifica ACA y no sobre la primitiva a proposito: la propiedad es del
 * SNAPSHOT —de lo que el consumidor recibe— y a nivel de `StrobeTracker` no
 * existe la deteccion gruesa con la que compararla.
 */
TEST(AnalysisThread, WithTheFineReadingAbsentTheCoarseDetectionIsStillPublished) {
    AnalysisRing ring;
    AnalysisSnapshot snap;
    AnalysisThread th(ring, snap);

    // A4 con el objetivo puesto en A4, pero la cuerda 80 cents abajo: bien
    // afuera del rango de captura del fundamental a 44,1 kHz (~21 cents).
    constexpr double kTarget = 440.0;
    const double real = kTarget * std::pow(2.0, -80.0 / 1200.0);

    th.setTargetHz(kTarget);
    th.start(kRate);
    ASSERT_TRUE(feedAtAnalysisPace(ring, snap,
        [&](int off) { return stringBlock(real, 1024, off); }, 200))
        << "el analisis dejo de consumir";
    ASSERT_TRUE(waitFor([&] {
        float o[kSnapshotValueCount];
        return snap.read(o) && o[kSnapDetectedHz] > 0.0f;
    }));
    th.stop();

    float out[kSnapshotValueCount];
    ASSERT_TRUE(snap.read(out));

    EXPECT_TRUE(std::isnan(out[kSnapCents]))
        << "publico una lectura fina fuera de rango: " << out[kSnapCents];
    EXPECT_GT(out[kSnapDetectedHz], 0.0f) << "se llevo puesta la deteccion gruesa";
    EXPECT_NEAR(out[kSnapDetectedHz], real, 5.0);
    EXPECT_NE(static_cast<int>(out[kSnapState]), kStateConverged)
        << "no puede declarar CONVERGIDO sin lectura fina";
}

/**
 * AC-003.8 — **sin control no se publica lectura fina**.
 *
 * Con ruido audible la deteccion gruesa no engancha, asi que no hay con que
 * verificar si los parciales estan en su dominio. Publicar igual es exactamente
 * por donde reentra el defecto que este REQ cierra: el estimador de fase
 * devuelve un numero con sigma ~ 0 tambien cuando lo alimentan con basura.
 *
 * El nivel esta MUY por encima del piso de silencio a proposito: si el test
 * pasara por quedarse sin señal, estaria midiendo otra cosa.
 */
TEST(AnalysisThread, WithoutACoarseDetectionNoFineReadingIsPublished) {
    AnalysisRing ring;
    AnalysisSnapshot snap;
    AnalysisThread th(ring, snap);

    th.setTargetHz(440.0);
    th.start(kRate);
    ASSERT_TRUE(feedAtAnalysisPace(ring, snap,
        [&](int) { return noiseBlock(1024); }, 200))
        << "el analisis dejo de consumir";
    th.stop();

    float out[kSnapshotValueCount];
    ASSERT_TRUE(snap.read(out));

    ASSERT_GT(out[kSnapLevelRms], 0.01f)
        << "el test se quedo sin señal: estaria pasando por la razon equivocada";
    EXPECT_TRUE(std::isnan(out[kSnapCents]))
        << "publico una lectura fina sin control: " << out[kSnapCents];
    EXPECT_NE(static_cast<int>(out[kSnapState]), kStateConverged);
}

// ---------------------------------------------------------------------------
// REQ-003 S2 — el rango publicado, en la unidad en la que se dibuja
// ---------------------------------------------------------------------------

/**
 * AC-003.4 — el rango se publica **en cents**, contra el objetivo y el rate
 * vigentes.
 *
 * 🔑 EL TEST AFIRMA LA PROPIEDAD, NO EL NUMERO (tarea 2.5). Comprobar que el
 * indice 14 vale "30,5" seria fijar una constante y no un contrato: se
 * mantendria verde con la guarda apuntando a otro lado. Lo que se afirma es la
 * relacion entre las DOS etapas — **dentro del rango publicado la lectura fina
 * existe y cumple el presupuesto; fuera esta ausente** —, que es lo unico que
 * impide que S1 y S2 diverjan en silencio.
 *
 * Se prueba en los dos lados del borde con la MISMA cuerda, para que la unica
 * variable sea el desajuste.
 */
TEST(AnalysisThread, ThePublishedRangePredictsWhereTheFineReadingExists) {
    constexpr double kTarget = 440.0;

    // 🔴 DEVUELVE bool Y NO EL SNAPSHOT, Y ESO ES AC-005.4 (REQ-005 S3).
    //
    // Antes esto era `EXPECT_TRUE(feedAtAnalysisPace(...))` adentro de un lambda
    // que devolvía el snapshot igual. Un `EXPECT` no corta: con la alimentación
    // corta, el test seguía y emitía un veredicto de EXACTITUD sobre una muestra
    // que nunca se completó — y entonces las dos fallas se veían iguales.
    //
    // Ahora "la muestra salió corta" sale por el valor de retorno y el llamador
    // lo ASSERTea, así que la comparación de exactitud NO LLEGA A CORRERSE.
    // Lo que quede en rojo después de eso significa una sola cosa: el sistema
    // cambió.
    auto runAt = [&](double detuneCents,
                     std::array<float, kSnapshotValueCount>& out) -> bool {
        AnalysisRing ring;
        AnalysisSnapshot snap;
        AnalysisThread th(ring, snap);
        const double real = kTarget * std::pow(2.0, detuneCents / 1200.0);
        th.setTargetHz(kTarget);
        th.start(kRate);
        if (!feedAtAnalysisPace(ring, snap,
                [&](int off) { return stringBlock(real, 1024, off); }, 200)) {
            th.stop();
            return false;
        }
        waitFor([&] {
            float o[kSnapshotValueCount];
            return snap.read(o) && o[kSnapDetectedHz] > 0.0f;
        });
        th.stop();
        return snap.read(out.data());
    };

    // 1. El rango se publica y es un numero util.
    std::array<float, kSnapshotValueCount> inside{};
    ASSERT_TRUE(runAt(-5.0, inside))
        << "LA MUESTRA SALIO CORTA: el analisis nunca hizo lugar en el ring o no "
           "llego a la meta. No es un veredicto sobre el motor — no se midio nada.";
    // Si el ring pisó frames, la integración de fase vio muestras no contiguas y
    // cualquier veredicto de exactitud de abajo mide ESO. Se afirma explícito.
    //
    // Con la compuerta de capacidad de `feedAtAnalysisPace` esto ya no debería
    // poder dispararse desde el test; se deja porque también cubre un drop de
    // origen distinto —el motor descartando por su cuenta— y esa sí sería una
    // señal real.
    ASSERT_FLOAT_EQ(inside[kSnapDroppedFrames], 0.0f)
        << "el ring pisó frames: el test estaría midiendo el drop, no el rango";

    const float range = inside[kSnapUsableRangeCents];
    ASSERT_FALSE(std::isnan(range)) << "no publico el rango teniendo objetivo";
    ASSERT_GT(range, 0.0f);

    // 2. DENTRO del rango publicado: la lectura existe y cumple el presupuesto.
    ASSERT_LT(5.0f, range) << "el caso 'adentro' quedo fuera: revisar el test";
    EXPECT_FALSE(std::isnan(inside[kSnapCents]))
        << "el rango dice " << range << " y a -5 cents no publico lectura";
    EXPECT_NEAR(inside[kSnapCents], -5.0f, 0.1f);

    // 2b. 🔴 EL BORDE INTERIOR, y sin esto el test NO SIRVE.
    //
    // Los puntos de afuera se calculan DESDE el rango publicado, asi que un
    // rango INFLADO los empuja mas lejos y sigue cumpliendo "afuera esta
    // ausente". Medido: un mutante que publicaba `150.0f` fijo —un rango
    // inventado, siete veces el real— pasaba los 8 tests. Lo que lo mata es
    // exigir que CERCA DEL BORDE INTERIOR la lectura todavia exista: con 150
    // inventado, a 135 cents no hay ninguna.
    std::array<float, kSnapshotValueCount> nearEdge{};
    ASSERT_TRUE(runAt(-0.90 * range, nearEdge)) << "LA MUESTRA SALIO CORTA en el borde interior";
    EXPECT_FALSE(std::isnan(nearEdge[kSnapCents]))
        << "el rango dice " << range << " y a " << (-0.90 * range)
        << " cents —adentro— no publico nada: el rango publicado es mas grande "
           "que el real";

    // 3. FUERA del rango publicado: ausente. Se toma 1,5x para no medir el borde.
    std::array<float, kSnapshotValueCount> outside{};
    ASSERT_TRUE(runAt(-1.5 * range, outside)) << "LA MUESTRA SALIO CORTA fuera del rango";
    EXPECT_TRUE(std::isnan(outside[kSnapCents]))
        << "el rango dice " << range << " y a " << (-1.5 * range)
        << " cents igual publico " << outside[kSnapCents];
}

/**
 * 2.2 — **sin objetivo no hay rango**, y se dice con NaN.
 *
 * Cero seria un rango plausible (nulo) y un consumidor lo dibujaria como "nunca
 * confies", que es una afirmacion distinta de "no hay contra que medir".
 */
TEST(AnalysisThread, WithoutATargetTheRangeIsAbsentInsteadOfZero) {
    AnalysisRing ring;
    AnalysisSnapshot snap;
    AnalysisThread th(ring, snap);

    const auto tone = toneBlock(1024, 0.37f);
    ring.writeStereo(tone.data(), 1024);
    th.start(kRate);                       // sin setTargetHz()
    ASSERT_TRUE(waitFor([&] { return snap.hasData(); }));
    th.stop();

    float out[kSnapshotValueCount];
    ASSERT_TRUE(snap.read(out));
    EXPECT_TRUE(std::isnan(out[kSnapUsableRangeCents]))
        << "sin objetivo publico un rango: " << out[kSnapUsableRangeCents];
}

// ---------------------------------------------------------------------------
// REQ-009 S2 — el motor deja de decir CONVERGIDO sobre una ventana que perdio
// frames. Los tests van EN PAR a proposito: uno afirma "no mientas" y el otro
// "no calles de mas". Sin el segundo, un apagado total pasa.
// ---------------------------------------------------------------------------
namespace {

constexpr double kReq009Target = 440.0;
constexpr double kReq009RealCents = -5.0;
const double kReq009Real = kReq009Target * std::pow(2.0, kReq009RealCents / 1200.0);

/**
 * Lo que se vio publicado MIENTRAS el ring se estaba pisando.
 *
 * 🔴 POR QUE SE OBSERVA DURANTE Y NO AL FINAL, QUE ERA LA PRIMERA VERSION.
 * Leer el snapshot DESPUES de dejar de desbordar mide otra cosa: cuando el
 * escritor para, el ring todavia tiene ~`kCapacityFrames` de audio CONTIGUO, el
 * analisis lo drena sin perder un frame y el estimador vuelve a tener medicion
 * — o sea que la marca se baja y la lectura converge, que es la recuperacion
 * que `TheMarkClears...` exige mas abajo. **Medido**: esa version paso sola y
 * fallo dentro de la suite completa, con `dropped = 497.664` y la marca en 0.
 * El veredicto dependia de si el lector llegaba antes que la recuperacion, o sea
 * del scheduler — el defecto que REQ-002 persigue.
 *
 * 🔴 Y POR QUE NO SE EXIGE "NUNCA CONVERGE MIENTRAS PISA", que era la segunda
 * version. **Tambien esta medido que eso es falso, y ademas seria incorrecto
 * pedirlo**: el desbordador escribe a rafagas, y entre rafaga y rafaga el
 * analisis se pone al dia con audio contiguo. En esos tramos converger es lo
 * CORRECTO, y prohibirlo chocaria de frente con AC-009.2. Todas las muestras
 * convergidas que se observaron tenian `Δdropped = 0`, o sea que eran
 * recuperaciones legitimas.
 *
 * Lo que si vale, y es lo que estos tests afirman: **todo lo que el motor
 * declare convergido tiene que estarlo de verdad**.
 */
struct OverrunObservation {
    int samples = 0;              ///< vueltas en las que el ring ya habia pisado
    int convergedSamples = 0;     ///< de esas, cuantas publicaron CONVERGIDO
    int markUpSamples = 0;        ///< cuantas traian la marca de hueco levantada
    int markedAndConverged = 0;   ///< CONVERGIDO **y** marcado: contradiccion pura
    double worstCents = 0.0;      ///< peor error entre las convergidas
    double sigmaThere = 0.0;      ///< y la σ que lo acompañaba
    double dropped = 0.0;         ///< acumulado al terminar
    int blocksWritten = 0;
};

/**
 * Desborda el ring a proposito: escribe `k` bloques por cada tick de analisis,
 * SIN esperar lugar. Es lo contrario de `feedAtAnalysisPace`, y reproduce el
 * mecanismo que la spec de REQ-009 declara real.
 */
OverrunObservation feedOverrunning(AnalysisRing& ring, AnalysisSnapshot& snap,
                                   double f0, int k, int iterations) {
    OverrunObservation obs;
    auto analysed = [&]() -> double {
        float o[kSnapshotValueCount];
        return snap.read(o) ? static_cast<double>(o[kSnapFramesAnalyzed]) : 0.0;
    };
    for (int i = 0; i < iterations; ++i) {
        const double before = analysed();
        for (int j = 0; j < k; ++j) {
            const auto blk = stringBlock(f0, 1024, obs.blocksWritten * 1024);
            ring.writeStereo(blk.data(), 1024);
            ++obs.blocksWritten;
        }
        waitFor([&] { return analysed() > before; }, std::chrono::milliseconds(500));

        float o[kSnapshotValueCount];
        if (!snap.read(o)) continue;
        // Solo cuentan las vueltas en las que el ring YA piso algo: antes del
        // primer desborde no hay nada que juzgar, y contarlas dejaria el
        // veredicto a merced de cuanto tarda el ring en llenarse.
        if (!(o[kSnapDroppedFrames] > 0.0f)) continue;
        ++obs.samples;
        obs.dropped = o[kSnapDroppedFrames];

        const bool marked = o[kSnapInputDiscontinuity] >= 0.5f;
        const bool converged = static_cast<int>(o[kSnapState]) == kStateConverged;
        if (marked) ++obs.markUpSamples;
        if (marked && converged) ++obs.markedAndConverged;
        if (converged) {
            ++obs.convergedSamples;
            const double err =
                std::fabs(static_cast<double>(o[kSnapCents]) - kReq009RealCents);
            if (err > obs.worstCents) {
                obs.worstCents = err;
                obs.sigmaThere = o[kSnapUncertainty];
            }
        }
    }
    return obs;
}

/**
 * Alimenta audio CONTIGUO al ritmo del analisis, con una meta RELATIVA a lo ya
 * analizado.
 *
 * `feedAtAnalysisPace` mide contra el acumulado desde el arranque, asi que
 * despues de una tanda previa su meta ya esta cumplida y devuelve sin escribir
 * un solo bloque. Un test de RECUPERACION que la usara tal cual mediria el
 * estado viejo y saldria verde sin haber alimentado nada.
 */
bool feedContiguousMore(AnalysisRing& ring, AnalysisSnapshot& snap, double f0,
                        int blocks, int startBlock) {
    auto analysed = [&]() -> double {
        float o[kSnapshotValueCount];
        return snap.read(o) ? static_cast<double>(o[kSnapFramesAnalyzed]) : 0.0;
    };
    const double goal = analysed() + static_cast<double>(blocks) * 1024.0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    int written = startBlock;
    while (analysed() < goal && std::chrono::steady_clock::now() < deadline) {
        if (!waitForRoom(ring, 1024)) return false;
        const auto blk = stringBlock(f0, 1024, written * 1024);
        ring.writeStereo(blk.data(), 1024);
        ++written;
    }
    return analysed() >= goal;
}

}  // namespace

/**
 * AC-009.1 — con el ring pisando, lo que el motor declara CONVERGIDO lo esta.
 *
 * 🔴 MEDIDO ANTES DE LA GUARDA, con este mismo arnes: el motor publicaba
 * CONVERGIDO con la lectura a **1,04 cents** del valor real —10x el presupuesto
 * de 0,1— y σ en **0,024**, muy por debajo del umbral. Mirar σ no alcanza, y ese
 * es el hallazgo entero de REQ-009.
 *
 * DESPUES de la guarda, sobre 20 corridas de 150 vueltas: el peor error entre
 * TODAS las muestras convergidas es de **3,8·10⁻⁶ cents**. Las que sobreviven son
 * recuperaciones legitimas —tramos con `Δdropped = 0` entre rafagas— y converger
 * ahi es lo correcto.
 *
 * EL GEMELO DE ESTE TEST VIVE APARTE: `HealthyContiguousAudioStillConverges`.
 * Sin el, un motor que no convergiera NUNCA pasaria este por vacio.
 */
TEST(AnalysisThreadReq009, WhatIsPublishedAsConvergedWhileTheRingOverrunsActuallyIs) {
    AnalysisRing ring;
    AnalysisSnapshot snap;
    AnalysisThread th(ring, snap);
    th.setTargetHz(kReq009Target);
    th.start(kRate);

    const OverrunObservation obs = feedOverrunning(ring, snap, kReq009Real, 8, 150);
    th.stop();

    ASSERT_GT(obs.samples, 0)
        << "el test no reprodujo su propia premisa: el ring no piso NI UN frame en "
           "ninguna vuelta, asi que no hay ventana rota que juzgar. Si esto salta, el "
           "desbordador dejo de desbordar (crecio kCapacityFrames? bajo kDrainFrames?) "
           "y los EXPECT de abajo serian verdes por vacio.";

    EXPECT_LE(obs.worstCents, AnalysisThread::kConvergedUncertaintyCents)
        << "el motor declaro CONVERGIDA una lectura que esta a " << obs.worstCents
        << " cents del valor real, con sigma=" << obs.sigmaThere << " — o sea POR "
        << "DEBAJO del umbral de " << AnalysisThread::kConvergedUncertaintyCents
        << ". Pasó en " << obs.convergedSamples << " de " << obs.samples
        << " vueltas con el ring pisando (acumulado " << obs.dropped << " frames).\n"
        << "  🔴 Mirar sigma no alcanza: ESE es REQ-009. La guarda tiene que tirar la "
           "integracion que cruzo el hueco — y consultar la perdida DESPUES de leer del "
           "ring, que es donde el lector la cuenta. Lo especifico lo cubre "
           "`ABurstOverrunIsNeverPublishedAsConverged`.";

    EXPECT_EQ(obs.markedAndConverged, 0)
        << "el motor publico CONVERGIDO y la marca de hueco A LA VEZ, en "
        << obs.markedAndConverged << " vueltas. Son mutuamente excluyentes por "
           "construccion: la marca solo se baja cuando hay medicion propia.";
}

/**
 * AC-009.2 — EL GEMELO, y es el que se olvida.
 *
 * Sin este, un apagado total del estado pasa el test de arriba: no mentir es
 * trivial si no decis nada. Este exige que el motor SIGA publicando CONVERGIDO
 * con senial contigua y limpia.
 */
TEST(AnalysisThreadReq009, HealthyContiguousAudioStillConverges) {
    AnalysisRing ring;
    AnalysisSnapshot snap;
    AnalysisThread th(ring, snap);
    th.setTargetHz(kReq009Target);
    th.start(kRate);

    const bool fed = feedAtAnalysisPace(
        ring, snap,
        [&](int startFrame) { return stringBlock(kReq009Real, 1024, startFrame); }, 150);

    float o[kSnapshotValueCount];
    ASSERT_TRUE(snap.read(o));
    const int state = static_cast<int>(o[kSnapState]);
    const double dropped = o[kSnapDroppedFrames];
    const double cents = o[kSnapCents];
    th.stop();

    ASSERT_TRUE(fed) << "la muestra salio corta: el analisis no consumio lo pedido";
    ASSERT_EQ(dropped, 0.0)
        << "premisa rota: alimentando al ritmo del analisis no se puede pisar un solo "
        << "frame. Con " << dropped << " pisados este test dejo de medir el caso SANO.";

    EXPECT_EQ(state, kStateConverged)
        << "la guarda de REQ-009 apago CONVERGIDO en regimen SANO, que es lo que "
           "AC-009.2 prohibe. Una guarda que nunca deja converger cumple AC-009.1 sin "
           "resolver nada: la aguja no se dibuja nunca.";
    EXPECT_NEAR(cents, kReq009RealCents, AnalysisThread::kConvergedUncertaintyCents)
        << "con audio contiguo la lectura tiene que caer dentro de presupuesto";
}

/**
 * AC-009.3 — el consumidor distingue "todavia no" de "la entrada llego rota".
 *
 * Las dos situaciones publican el MISMO estado (`kStateMeasuring`) y piden del
 * usuario acciones OPUESTAS: una se resuelve esperando y la otra revisando el
 * cable. Sin el slot, la unica salida del consumidor es esperar — que frente a
 * un problema que no se arregla solo es la peor de las dos.
 *
 * Este test no vuelve a preguntar si converge (eso es el de arriba): pregunta si
 * la marca **discrimina**, y por eso afirma los dos lados con la misma medicion.
 */
TEST(AnalysisThreadReq009, AGapIsDistinguishableFromNotConvergedYet) {
    // --- lado roto: se observa MIENTRAS pisa -------------------------------
    OverrunObservation broken;
    {
        AnalysisRing ring;
        AnalysisSnapshot snap;
        AnalysisThread th(ring, snap);
        th.setTargetHz(kReq009Target);
        th.start(kRate);
        broken = feedOverrunning(ring, snap, kReq009Real, 8, 150);
        th.stop();
    }

    // --- lado sano ---------------------------------------------------------
    float healthy[kSnapshotValueCount];
    {
        AnalysisRing ring;
        AnalysisSnapshot snap;
        AnalysisThread th(ring, snap);
        th.setTargetHz(kReq009Target);
        th.start(kRate);
        ASSERT_TRUE(feedAtAnalysisPace(
            ring, snap,
            [&](int startFrame) { return stringBlock(kReq009Real, 1024, startFrame); },
            150));
        ASSERT_TRUE(snap.read(healthy));
        th.stop();
    }

    ASSERT_GT(broken.samples, 0)
        << "premisa rota: el desbordador no desbordo, asi que no hay lado ROTO";
    ASSERT_FLOAT_EQ(healthy[kSnapDroppedFrames], 0.0f)
        << "premisa rota: el lado SANO piso frames, asi que los dos lados son el mismo";

    EXPECT_GT(broken.markUpSamples, 0)
        << "el ring piso " << broken.dropped << " frames a lo largo de "
        << broken.samples << " vueltas y el snapshot no lo dijo NUNCA: el consumidor "
           "ve un spinner y espera a que se arregle algo que no se arregla solo.";
    EXPECT_FLOAT_EQ(healthy[kSnapInputDiscontinuity], 0.0f)
        << "con la entrada intacta el motor acusa un hueco. Una marca que este "
           "siempre prendida no distingue nada: es lo mismo que no publicarla — y es "
           "exactamente como se comporta el acumulado `droppedFrames`.";
}

/**
 * AC-009.2 + AC-009.3 — LA MARCA SE BAJA SOLA cuando la entrada vuelve a estar
 * entera, y este es el gemelo de la marca (no el de la guarda).
 *
 * 🔴 Sin este test, la marca podria quedarse prendida PARA SIEMPRE despues del
 * primer hueco y los tres de arriba seguirian verdes — que es exactamente el
 * fallo que la spec le imputa al acumulado `droppedFrames`. Un afinador que dice
 * "revisa el cable" el resto de la sesion es tan inutil como uno que nunca lo
 * dice.
 */
TEST(AnalysisThreadReq009, TheMarkClearsAndTheReadingConvergesOnceTheInputIsWholeAgain) {
    AnalysisRing ring;
    AnalysisSnapshot snap;
    AnalysisThread th(ring, snap);
    th.setTargetHz(kReq009Target);
    th.start(kRate);

    const OverrunObservation obs = feedOverrunning(ring, snap, kReq009Real, 8, 150);
    ASSERT_GT(obs.samples, 0) << "el desbordador no desbordo";
    ASSERT_GT(obs.markUpSamples, 0)
        << "la premisa de este test es que la marca llego a estar ARRIBA mientras se "
           "pisaba; si no, no hay nada que ver bajar.";

    // Se deja de pisar y se sigue alimentando CONTIGUO desde donde quedo.
    ASSERT_TRUE(feedContiguousMore(ring, snap, kReq009Real, 200, obs.blocksWritten))
        << "la muestra de recuperacion salio corta";

    float after[kSnapshotValueCount];
    ASSERT_TRUE(snap.read(after));
    const double dropped = after[kSnapDroppedFrames];
    th.stop();

    EXPECT_GT(dropped, 0.0)
        << "el acumulado tendria que seguir contando los frames que SI se perdieron: "
           "la marca describe la lectura viva, no borra la historia.";
    EXPECT_FLOAT_EQ(after[kSnapInputDiscontinuity], 0.0f)
        << "la entrada volvio a estar entera y la marca sigue arriba. Asi se comporta "
           "el ACUMULADO `droppedFrames` (" << dropped << ", monotono), que es justo "
           "lo que este slot existe para no ser.";
    EXPECT_EQ(static_cast<int>(after[kSnapState]), kStateConverged)
        << "despues de recuperarse la aguja no vuelve nunca: la guarda se traba, que "
           "es el fallo que AC-009.2 prohibe.";
    EXPECT_NEAR(after[kSnapCents], kReq009RealCents,
                AnalysisThread::kConvergedUncertaintyCents)
        << "convergio, pero sobre una integracion que todavia arrastra el hueco";
}

/**
 * AC-009.1 — EL CASO SE FUERZA, NO SE ESPERA. Un desborde de UNA rafaga sobre
 * una lectura que YA estaba convergida.
 *
 * 🔴 POR QUE HACE FALTA ADEMAS DEL DE DESBORDE SOSTENIDO. Aquel mira si algo
 * malo *aparece*, y eso deja el veredicto a merced del azar: la variante rota
 * —muestrear `droppedFrames()` ANTES de `read()`— cruzaba el presupuesto en
 * **1 de 20 corridas**, o sea que ese test la dejaba pasar 19 veces de 20. Es la
 * leccion que este repo ya pago: contra una ventana angosta no se agregan
 * iteraciones, se ARMA la situacion.
 *
 * Y se puede armar porque el mecanismo es ESTRUCTURAL, no una carrera: los dos
 * `mDropped.bump()` de `AnalysisRing` viven adentro de `read()` —el escritor no
 * toca ese contador nunca—, asi que el desborde lo cuenta el lector en la MISMA
 * llamada que devuelve el bloque de despues del hueco. Alcanza con:
 *
 *   1. dejar que converja con audio limpio (asi hay una medicion viva que
 *      contaminar — sin eso no hay nada que arruinar y el test saldria verde por
 *      vacio, que es lo que vigila el ASSERT de premisa),
 *   2. **vaciar el ring**, para que el lector quede en su siesta y la rafaga
 *      entera caiga entre dos vueltas,
 *   3. tirar de golpe mas de un ring entero,
 *   4. mirar la PRIMERA publicacion que ya cuenta el desborde.
 *
 * Con la guarda: esa vuelta ve Δ > 0, tira la integracion y publica "midiendo".
 * Muestreando antes de `read()`: el Δ da 0, el salto entra a una integracion que
 * seguia convergida, y el motor publica CONVERGIDO sobre dos trozos distintos.
 *
 * 🔴 LA CAPTURA NO PUEDE USAR `waitFor`: duerme 1 ms entre sondeos y una vuelta
 * del analisis dura mucho menos, asi que se saltearia justo la publicacion
 * contaminada y veria la siguiente, ya recuperada. **Medido**: con `waitFor`
 * este test no mataba al mutante del orden. Por eso el sondeo es apretado y
 * guarda TODAS las publicaciones distintas.
 */
TEST(AnalysisThreadReq009, ABurstOverrunIsNeverPublishedAsConverged) {
    AnalysisRing ring;
    AnalysisSnapshot snap;
    AnalysisThread th(ring, snap);
    th.setTargetHz(kReq009Target);
    th.start(kRate);

    // 1 · converger con audio limpio.
    int written = 0;
    waitFor([&] {
        for (int i = 0; i < 4; ++i) {
            if (!waitForRoom(ring, 1024)) return true;   // lo juzga el ASSERT de abajo
            const auto blk = stringBlock(kReq009Real, 1024, written * 1024);
            ring.writeStereo(blk.data(), 1024);
            ++written;
        }
        float o[kSnapshotValueCount];
        return snap.read(o) && static_cast<int>(o[kSnapState]) == kStateConverged;
    }, std::chrono::seconds(10));

    float before[kSnapshotValueCount];
    ASSERT_TRUE(snap.read(before) &&
                static_cast<int>(before[kSnapState]) == kStateConverged)
        << "premisa rota: el motor nunca llego a converger con audio limpio, asi que no hay "
           "medicion viva que el hueco pueda contaminar y el EXPECT de abajo seria verde por "
           "vacio.";
    const double droppedBefore = before[kSnapDroppedFrames];
    ASSERT_EQ(droppedBefore, 0.0) << "el tramo limpio no puede haber pisado nada";

    // 2 · vaciar el ring: el lector queda en la siesta, no adentro de read().
    ASSERT_TRUE(waitFor([&] { return ring.availableFrames() == 0; }))
        << "el analisis no llego a drenar el ring: la rafaga no caeria entre dos vueltas";

    // 3 · UNA rafaga de mas de un ring entero, sin esperar lugar.
    const int burst = 2 * AnalysisRing::kCapacityFrames / 1024;
    for (int i = 0; i < burst; ++i) {
        const auto blk = stringBlock(kReq009Real, 1024, written * 1024);
        ring.writeStereo(blk.data(), 1024);
        ++written;
    }

    // 4 · sondeo APRETADO: guarda cada publicacion distinta hasta ver la primera
    //     que ya cuenta el desborde, mas un par mas de margen.
    int badState = -1;
    double badCents = 0.0, badDropped = 0.0, badMark = 0.0;
    bool sawDrop = false;
    double lastFrames = -1.0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        float o[kSnapshotValueCount];
        if (!snap.read(o)) continue;
        if (o[kSnapFramesAnalyzed] == lastFrames) continue;
        lastFrames = o[kSnapFramesAnalyzed];
        if (!(o[kSnapDroppedFrames] > droppedBefore)) continue;
        if (!sawDrop) {
            sawDrop = true;
            badState   = static_cast<int>(o[kSnapState]);
            badCents   = o[kSnapCents];
            badDropped = o[kSnapDroppedFrames];
            badMark    = o[kSnapInputDiscontinuity];
            break;
        }
    }
    th.stop();

    ASSERT_TRUE(sawDrop)
        << "premisa rota: la rafaga escribio " << burst << " bloques de golpe sobre un ring de "
        << AnalysisRing::kCapacityFrames << " frames y ninguna publicacion conto un solo frame "
           "pisado.";

    EXPECT_NE(badState, kStateConverged)
        << "el motor siguio diciendo CONVERGIDO en la primera ventana despues de un hueco de "
        << (badDropped - droppedBefore) << " frames. cents=" << badCents << " — la integracion "
           "que sostiene ese numero arranco ANTES del hueco y sigue viva.\n"
        << "  🔴 El sintoma tipico es muestrear `droppedFrames()` ANTES de `read()`: los dos "
           "bump del contador viven ADENTRO de read(), asi que ahi el Δ da 0 y el salto entra "
           "sin que nadie lo vea.";
    EXPECT_FLOAT_EQ(static_cast<float>(badMark), 1.0f)
        << "hubo hueco y el snapshot no lo dice: el consumidor no puede distinguir esto de "
           "'todavia no' (AC-009.3).";
}
