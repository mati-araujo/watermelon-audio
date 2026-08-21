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
