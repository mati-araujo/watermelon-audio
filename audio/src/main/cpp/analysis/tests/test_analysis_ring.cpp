/**
 * REQ-001 S1 — el ring que lleva la captura al analisis.
 *
 * Cubre las tareas 1.2 (secuencia exacta bajo escritor + lector concurrentes,
 * CON COMPUERTA) y 1.3 (se descarta lo mas viejo, se señala, y el escritor no se
 * bloquea).
 *
 * POR QUE LOS DATOS DE PRUEBA SON UNA RAMPA Y NO RUIDO
 * ---------------------------------------------------
 * Cada frame lleva su numero de secuencia como valor. Con eso "sin huecos ni
 * duplicados" no es una estadistica: es una igualdad exacta, y cuando falla dice
 * EN QUE frame y cuantos se saltearon. Con ruido habria que comparar contra una
 * copia y el diagnostico seria "difieren".
 */

#include "../AnalysisRing.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

// Definidas en AnalysisRing.cpp bajo WMA_TEST_HOOKS.
extern std::atomic<bool> gAnalysisRingHoldAfterCopy;
extern std::atomic<bool> gAnalysisRingIsInCopy;

namespace {

using wma::analysis::AnalysisRing;

constexpr uint32_t kCap = AnalysisRing::kCapacityFrames;

/// Bloque estereo cuyo frame `i` vale `first + i` en LOS DOS canales, de modo
/// que la suma a mono devuelve exactamente ese numero.
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

// ---------------------------------------------------------------------------
// Lo basico, para que lo de abajo signifique algo.
// ---------------------------------------------------------------------------

TEST(AnalysisRing, StereoIsSummedToMonoAndComesBackInOrder) {
    AnalysisRing ring;
    const int n = 512;

    // L y R distintos: si sumara mal, el promedio no daria la rampa.
    std::vector<float> in(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n; ++i) {
        in[static_cast<size_t>(i) * 2]     = static_cast<float>(i) - 0.25f;
        in[static_cast<size_t>(i) * 2 + 1] = static_cast<float>(i) + 0.25f;
    }
    ring.writeStereo(in.data(), n);

    std::vector<float> out(n, -1.0f);
    ASSERT_EQ(ring.read(out.data(), n), n);
    for (int i = 0; i < n; ++i) {
        EXPECT_FLOAT_EQ(out[i], static_cast<float>(i)) << "frame " << i;
    }
    EXPECT_EQ(ring.droppedFrames(), 0u);
}

TEST(AnalysisRing, AnEmptyRingReturnsZeroWithoutTouchingTheBuffer) {
    AnalysisRing ring;
    std::vector<float> out(64, 7.5f);
    EXPECT_EQ(ring.read(out.data(), 64), 0);
    for (float v : out) EXPECT_FLOAT_EQ(v, 7.5f) << "no puede escribir ceros";
}

// ---------------------------------------------------------------------------
// 1.3 — el lector se atrasa: se descarta lo MAS VIEJO y se señala.
// ---------------------------------------------------------------------------

/**
 * Un ring que descarta lo mas NUEVO seria inutil para un afinador: el analisis
 * atrasado no quiere las muestras de hace 200 ms, quiere las de ahora. Este test
 * mide cual de las dos hace.
 */
TEST(AnalysisRing, WhenTheReaderFallsBehindTheOldestIsDroppedAndCounted) {
    AnalysisRing ring;
    const int block = 256;
    const uint32_t blocks = (kCap / block) * 3;   // el triple de la capacidad

    uint64_t written = 0;
    for (uint32_t b = 0; b < blocks; ++b) {
        auto in = rampBlock(written, block);
        ring.writeStereo(in.data(), block);       // el escritor NUNCA falla
        written += block;
    }

    const uint64_t expectedDrop = written - kCap;
    std::vector<float> out(kCap, -1.0f);
    const int got = ring.read(out.data(), static_cast<int>(kCap));

    ASSERT_EQ(got, static_cast<int>(kCap)) << "tiene que entregar la capacidad entera";
    EXPECT_EQ(ring.droppedFrames(), expectedDrop)
        << "el conteo de perdidas tiene que ser EXACTO, no aproximado";

    // Y lo que entrega es la COLA, no la cabeza: el primer frame vivo es
    // `written - kCap`.
    for (uint32_t i = 0; i < kCap; ++i) {
        ASSERT_FLOAT_EQ(out[i], static_cast<float>(expectedDrop + i))
            << "frame " << i << ": se quedo con lo viejo en vez de lo nuevo";
    }
}

/// Un bloque mas grande que el ring entero se recorta a lo ultimo, en vez de
/// dar la vuelta escribiendo trabajo que nadie va a poder leer.
TEST(AnalysisRing, ABlockBiggerThanTheRingKeepsOnlyItsTail) {
    AnalysisRing ring;
    const int huge = static_cast<int>(kCap) * 2 + 123;
    auto in = rampBlock(0, huge);
    ring.writeStereo(in.data(), huge);

    std::vector<float> out(kCap, -1.0f);
    ASSERT_EQ(ring.read(out.data(), static_cast<int>(kCap)), static_cast<int>(kCap));
    const auto first = static_cast<float>(huge - static_cast<int>(kCap));
    EXPECT_FLOAT_EQ(out[0], first);
    EXPECT_FLOAT_EQ(out[kCap - 1], static_cast<float>(huge - 1));

    // Y el recorte queda CONTADO. Sin esta asercion el test no distingue la
    // version con recorte de la que no lo tiene: medido por mutacion, borrar el
    // recorte entero deja el buffer identico —las ultimas `kCap` escrituras
    // pisan todas las ranuras igual— y los seis tests seguian en verde. Lo que
    // el recorte ahorra es trabajo en el thread RT, y el trabajo no aparece en
    // la salida; el contador es lo unico que lo hace visible.
    EXPECT_EQ(ring.oversizedBlocks(), 1u)
        << "el recorte tiene que quedar contado, o es indistinguible de no hacerlo";
}

// ---------------------------------------------------------------------------
// 1.2 — la secuencia es exacta bajo concurrencia. CON COMPUERTA.
// ---------------------------------------------------------------------------

/**
 * LA CARRERA QUE IMPORTA, forzada en vez de buscada.
 *
 * El peligro de un ring que sobreescribe es que el escritor pase por encima de
 * lo que el lector esta copiando: el lector se llevaria mitad viejo y mitad
 * nuevo, sin que nada avise. La ventana dura microsegundos, y este repo ya midio
 * que bombear a ciegas no la pega (40 retiros x 15 corridas, el codigo roto pasó
 * siempre). Por eso hay una compuerta que detiene al lector JUSTO despues de
 * copiar y antes de re-chequear.
 *
 * Lo que se afirma NO es que la lectura salga bien —en ese estado ya es
 * irrecuperable— sino que el ring **la detecta y la descarta** en vez de
 * entregar basura como si fuera senal.
 */
TEST(AnalysisRing, AReadOverrunWhileCopyingIsDetectedInsteadOfHandingBackTornData) {
    AnalysisRing ring;

    // Sembrar media capacidad para que haya algo que leer.
    const int seed = static_cast<int>(kCap) / 2;
    auto seedBlock = rampBlock(0, seed);
    ring.writeStereo(seedBlock.data(), seed);

    gAnalysisRingIsInCopy.store(false);
    gAnalysisRingHoldAfterCopy.store(true);

    std::atomic<int> readResult{-99};
    std::thread reader([&] {
        std::vector<float> out(kCap, -1.0f);
        readResult.store(ring.read(out.data(), seed));
    });

    // Esperar a que el lector este ADENTRO, con la copia hecha.
    while (!gAnalysisRingIsInCopy.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // Ahora, con el lector congelado ahi, pasarle por encima.
    uint64_t written = static_cast<uint64_t>(seed);
    for (uint32_t b = 0; b < (kCap / 256) * 2; ++b) {
        auto in = rampBlock(written, 256);
        ring.writeStereo(in.data(), 256);
        written += 256;
    }

    gAnalysisRingHoldAfterCopy.store(false, std::memory_order_release);
    reader.join();

    EXPECT_EQ(readResult.load(), 0)
        << "la lectura desgarrada tiene que descartarse, no entregarse";
    EXPECT_GE(ring.tornReads(), 1u) << "y quedar contada como desgarro";
    EXPECT_GT(ring.droppedFrames(), 0u);

    // Y despues del episodio el ring sigue sirviendo, desde lo mas nuevo.
    auto after = rampBlock(written, 256);
    ring.writeStereo(after.data(), 256);
    written += 256;
    std::vector<float> out(kCap, -1.0f);
    const int got = ring.read(out.data(), static_cast<int>(kCap));
    ASSERT_GT(got, 0) << "el ring quedo inutilizable despues de un desgarro";
    EXPECT_FLOAT_EQ(out[static_cast<size_t>(got) - 1], static_cast<float>(written - 1))
        << "lo ultimo entregado tiene que ser lo ultimo escrito";
}

/**
 * Sin desgarro: escritor y lector concurrentes, y la secuencia entregada es
 * **exactamente** la escrita, sin huecos ni duplicados. Es el caso de la tarea
 * 1.2, y el que valida que la deteccion del test anterior no sea un falso
 * positivo permanente.
 *
 * EL VEREDICTO NO PUEDE DEPENDER DEL PLANIFICADOR — Y LA PRIMERA VERSION SI
 * -------------------------------------------------------------------------
 * La primera version dejaba al escritor marcando el paso con un `sleep_for` y
 * confiaba en que el lector le siguiera el ritmo. **Fallo en el gate**, en el
 * frame 12928, saltando a 32768: bajo `load average 169` el thread lector se
 * quedo sin CPU por mas de los ~3,2 ms que tarda el escritor en dar la vuelta al
 * ring, y el ring descarto lo viejo — que es lo CORRECTO. El defecto era del
 * test: "el lector sigue el ritmo" es una suposicion sobre el planificador, no
 * una propiedad del codigo, y un test cuyo veredicto depende de la carga de la
 * maquina no prueba nada sobre el ring.
 *
 * La version buena no lo pide: lo GARANTIZA. El escritor no escribe hasta que
 * haya lugar, asi que por construccion el ring nunca pasa su capacidad y no
 * puede haber descarte, corra la maquina vacia o a load 200. Los dos threads
 * siguen siendo concurrentes de verdad y se dan ~6 vueltas completas al ring.
 */
TEST(AnalysisRing, AReaderThatKeepsUpSeesEveryFrameExactlyOnce) {
    AnalysisRing ring;
    const int block = 128;
    const uint64_t total = static_cast<uint64_t>(block) * 400;   // ~6 vueltas

    std::atomic<bool> writerDone{false};
    std::atomic<bool> abort{false};

    std::thread writer([&] {
        uint64_t w = 0;
        while (w < total && !abort.load(std::memory_order_acquire)) {
            // Contrapresion: no escribir si el lector todavia no hizo lugar.
            // Esto es del TEST, no del ring — en produccion el escritor es RT y
            // jamas espera a nadie. Aca existe para que el resultado no dependa
            // de quien gane la CPU.
            while (ring.availableFrames() + static_cast<uint32_t>(block) > kCap) {
                if (abort.load(std::memory_order_acquire)) return;
                std::this_thread::yield();
            }
            auto in = rampBlock(w, block);
            ring.writeStereo(in.data(), block);
            w += block;
        }
        writerDone.store(true, std::memory_order_release);
    });

    uint64_t expected = 0;
    uint64_t badAt = 0;
    float badValue = 0.0f;
    bool ok = true;
    std::vector<float> out(kCap, 0.0f);

    while (expected < total && ok) {
        const int got = ring.read(out.data(), static_cast<int>(kCap));
        for (int i = 0; i < got; ++i) {
            if (out[i] != static_cast<float>(expected)) {
                ok = false; badAt = expected; badValue = out[i];
                break;
            }
            ++expected;
        }
        if (got == 0) std::this_thread::yield();
    }

    // Siempre soltar al escritor y unirlo ANTES de afirmar: un ASSERT que salga
    // del test dejaria el thread girando y el proceso abortaria en su destructor
    // — que es como esta falla se presento la primera vez, tapando el motivo.
    abort.store(true, std::memory_order_release);
    writer.join();

    EXPECT_TRUE(ok) << "hueco o duplicado en el frame " << badAt
                    << ": llego " << badValue;
    EXPECT_EQ(expected, total) << "no se entregaron todos los frames";
    EXPECT_EQ(ring.droppedFrames(), 0u)
        << "con contrapresion el ring no puede desbordar: si esto falla, "
           "desbordo sin que nadie se pasara de capacidad";
    EXPECT_EQ(ring.tornReads(), 0u);
    EXPECT_TRUE(writerDone.load(std::memory_order_acquire))
        << "el escritor no llego a terminar: el test se corto antes";
}

/**
 * REQ-009 S3 — `reset()` tiene que llevarse tambien la COSTURA de captura.
 *
 * 🔴 EL DEFECTO QUE CONGELA ES SILENCIOSO. `reset()` devuelve las posiciones a
 * cero; si la costura se quedara en su valor viejo, apuntaria a un punto que el
 * lector no vuelve a alcanzar nunca — y la guarda de REQ-009 descarta bloques
 * hasta alcanzarla. O sea un afinador MUDO para el resto de la sesion, sin un
 * solo error. Es el mismo modo de falla que AC-009.2 prohibe para el acumulado
 * de `droppedFrames`, entrando por otra puerta.
 *
 * Cuando esto se escribio el defecto era LATENTE —nadie llama a `reset()`; los
 * `analysisRing.reset()` del arbol son del `unique_ptr`— y se arreglo igual: un
 * metodo publico que deja el sistema en un estado del que no se sale no espera a
 * tener un llamador para ser un defecto.
 */
TEST(AnalysisRing, ResetAlsoClearsTheCaptureSeam) {
    AnalysisRing ring;
    const std::vector<float> blk(256 * 2, 0.25f);
    ring.writeStereo(blk.data(), 256);
    ring.reportCaptureDiscontinuity();

    ASSERT_GT(ring.captureSeamPosition(), 0u)
        << "premisa rota: el aviso no dejo costura, asi que no hay nada que ver limpiarse";

    ring.reset();

    EXPECT_EQ(ring.captureSeamPosition(), 0u)
        << "reset() dejo la costura en " << ring.captureSeamPosition()
        << " con las posiciones en cero. El lector no vuelve a alcanzarla nunca, asi que la "
           "guarda de REQ-009 descarta todos los bloques a partir de ahora: afinador mudo, "
           "sin error.";
}
