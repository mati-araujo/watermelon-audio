/**
 * REQ-009 S3, tareas 3.3b y 3.4b — el cruce de threads de iOS y USB.
 *
 * POR QUE ESTE ARCHIVO EXISTE Y NO ALCANZABA CON EL DE ANDROID
 * ------------------------------------------------------------
 * `test_capture_discontinuity.cpp` cubre las dos topologias donde el que avisa
 * ES el que escribe el ring del afinador: Android (el `InputNode` abre su propio
 * stream de Oboe) y el arnes que llama a `reportCaptureDiscontinuity()` a mano.
 *
 * En iOS y en USB no es asi, y es la unica topologia de las tres donde no lo es:
 *
 *   el OVERRUN lo detecta el callback de ENTRADA        <- thread A
 *   el `AnalysisRing` lo escribe el callback de SALIDA  <- thread B
 *
 * De ahi salen las dos cosas que se manejan aca:
 *
 *   1. EL BUZON (`CaptureGapMailbox`). Transporta un NUMERO —cuantos frames de
 *      captura seguian encolados por delante del hueco— y nunca una posicion.
 *   2. LA DISTANCIA (`framesAhead`). Un overrun de `CoreAudioBackend` ocurre con
 *      su `mInputRing` LLENO, o sea con ~1 segundo de estereo encolado: el hueco
 *      no se entrega ahora, se entrega ~48000 frames despues. Avisarlo sin la
 *      distancia deja la costura adelantada 5,9x la capacidad ENTERA del
 *      `AnalysisRing` (8192 frames) y el motor termina publicando CONVERGIDO
 *      sobre una lectura equivocada. Es el hallazgo F del doc de la etapa.
 *
 * LO QUE ESTE ARCHIVO NO PUEDE CUBRIR, DICHO ACA
 * ----------------------------------------------
 * El cableado de `CoreAudioBackend.mm` —quien llama a `note()`, con que numero, y
 * quien llama a `take()`— es Objective-C++ y solo se compila para iOS. Lo unico
 * que lo verifica es el build de iOS. Por eso la REGLA vive en un header
 * (`backends/CaptureGapMailbox.h`) y no adentro del `.mm`: es la misma
 * separacion que 3.2b hizo con el adaptador de Oboe — la plataforma cablea, la
 * regla vive donde un test de host la puede manejar.
 */
#include "../../backends/CaptureGapMailbox.h"
#include "../../core/AudioEngine.h"
#include "../../nodes/InputNode.h"
#include "../../analysis/AnalysisRing.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace {

using wma::analysis::AnalysisRing;
using wma::backends::CaptureGapMailbox;

constexpr int kRate = 48000;
constexpr int kBlockFrames = 512;

}  // namespace

// ===========================================================================
// EL BUZON — el mecanismo, en un thread
// ===========================================================================

TEST(CaptureGapMailbox, AnUntouchedMailboxIsEmpty) {
    CaptureGapMailbox box;
    EXPECT_EQ(box.take(), CaptureGapMailbox::kEmpty)
        << "un buzon recien construido entrego un hueco que nadie dejo";
}

TEST(CaptureGapMailbox, ANotedGapComesBackWithItsDistance) {
    CaptureGapMailbox box;
    box.note(48000);
    EXPECT_EQ(box.take(), 48000u)
        << "el buzon perdio la DISTANCIA, que es lo unico que transporta. Sin ella el "
           "escritor no puede saber donde cae el hueco: el detector la midio cuando lo vio "
           "y el escritor no la puede reconstruir despues.";
}

/**
 * EL HUECO SE POSICIONA UNA VEZ. Dejarlo puesto lo estamparia de nuevo en cada
 * bloque, o sea una guarda trabada que nunca deja converger — que es
 * exactamente lo que AC-009.2 prohibe, y el mismo fallo que el hallazgo A tuvo
 * por otra puerta.
 */
TEST(CaptureGapMailbox, TakingAGapEmptiesTheMailbox) {
    CaptureGapMailbox box;
    box.note(1234);
    ASSERT_EQ(box.take(), 1234u);
    EXPECT_EQ(box.take(), CaptureGapMailbox::kEmpty)
        << "el hueco quedo puesto despues de levantarlo: se volveria a estampar en cada "
           "bloque y la aguja no vuelve nunca";
}

/**
 * GANA EL MAS LEJANO, no el ultimo.
 *
 * Entre dos consumos pueden entrar dos avisos: un overrun con su cola de un
 * segundo por delante, y un underrun de este mismo bloque (distancia 0). El que
 * vale es el lejano — el descarte que provoca ya cubre al cercano, mientras que
 * quedarse con el cercano deja pasar el salto de verdad.
 */
TEST(CaptureGapMailbox, TheFarthestGapWins) {
    CaptureGapMailbox lejanoPrimero;
    lejanoPrimero.note(48000);
    lejanoPrimero.note(0);       // el underrun llega despues y NO puede retirarlo
    EXPECT_EQ(lejanoPrimero.take(), 48000u)
        << "un aviso cercano posterior borro uno lejano. El salto sigue estando alla: el "
           "lector se pone al dia antes de cruzarlo y el motor converge sobre el.";

    CaptureGapMailbox cercanoPrimero;
    cercanoPrimero.note(0);
    cercanoPrimero.note(48000);
    EXPECT_EQ(cercanoPrimero.take(), 48000u) << "y el orden inverso tiene que dar lo mismo";
}

TEST(CaptureGapMailbox, ClearDropsAGapWithoutPositioningIt) {
    CaptureGapMailbox box;
    box.note(7);
    box.clear();
    EXPECT_EQ(box.take(), CaptureGapMailbox::kEmpty)
        << "un hueco de la sesion anterior sobrevivio al stop del stream. No describe al "
           "audio de la sesion nueva.";
}

// ===========================================================================
// LA COSTURA CON DISTANCIA — el ring
// ===========================================================================

TEST(CaptureSeamAhead, AGapAheadLandsPastTheWriterInsteadOfAtIt) {
    AnalysisRing ring;
    std::vector<float> bloque(static_cast<size_t>(kBlockFrames) * 2, 0.1f);
    ring.writeStereo(bloque.data(), kBlockFrames);

    ring.reportCaptureDiscontinuity(48000);
    EXPECT_EQ(ring.captureSeamPosition(), static_cast<uint64_t>(kBlockFrames) + 48000u)
        << "la costura quedo en la posicion de escritura de AHORA en vez de " << 48000
        << " frames mas adelante.\n"
        << "  🔴 Ese es el hallazgo F: un backend con cola propia detecta el overrun con la "
           "cola LLENA, asi que el hueco se entrega mucho despues. Estamparlo aca hace que el "
           "lector descarte audio sano, se ponga al dia, y cruce el salto de verdad SIN "
           "costura pendiente — CONVERGIDO sobre una lectura equivocada.";
}

TEST(CaptureSeamAhead, ANearerSeamDoesNotRetireAFartherOne) {
    AnalysisRing ring;
    std::vector<float> bloque(static_cast<size_t>(kBlockFrames) * 2, 0.1f);
    ring.writeStereo(bloque.data(), kBlockFrames);

    ring.reportCaptureDiscontinuity(48000);
    const uint64_t lejana = ring.captureSeamPosition();
    ring.reportCaptureDiscontinuity(0);          // un underrun, aca mismo

    EXPECT_EQ(ring.captureSeamPosition(), lejana)
        << "una costura cercana retiro a una lejana. El lector deja de descartar antes de "
           "llegar al salto — y ademas la costura RETROCEDE, que el lector interpreta como "
           "un reset del ring y re-sincroniza.";
}

TEST(CaptureSeamAhead, ResetStillClearsASeamThatWasAhead) {
    AnalysisRing ring;
    std::vector<float> bloque(static_cast<size_t>(kBlockFrames) * 2, 0.1f);
    ring.writeStereo(bloque.data(), kBlockFrames);
    ring.reportCaptureDiscontinuity(48000);
    ASSERT_GT(ring.captureSeamPosition(), 0u);

    ring.reset();
    EXPECT_EQ(ring.captureSeamPosition(), 0u)
        << "una costura ADELANTADA sobrevivio al reset. Es el hallazgo A otra vez y peor: "
           "las posiciones vuelven a cero y una costura en 48512 no se alcanza nunca, asi que "
           "el lector descarta todos los bloques PARA SIEMPRE.";
}

// ===========================================================================
// LA COMPUERTA — la unica semantica que solo la concurrencia revela
// ===========================================================================

/**
 * NADIE MAS QUE EL ESCRITOR ESTAMPA, Y LO HACE EN SU PROPIA FRONTERA.
 *
 * 🔑 Es el principio que REQ-009 pago cuatro veces. El modo de falla que este
 * test existe para excluir: que el detector —otro thread— posicione la costura
 * el mismo. Si lo hiciera, la costura caeria en un punto ARBITRARIO de adentro
 * del bloque que el escritor esta armando, en vez de en la frontera anterior al
 * bloque que trae el salto. Y no falla ruidosamente: publica un numero
 * plausible.
 *
 * LA COMPUERTA ES EL PROPIO TEST, y no hace falta un gancho de produccion. El
 * thread principal ES el escritor, asi que "el escritor esta parado en medio de
 * su bloque" se consigue simplemente no llamando al siguiente. El detector corre
 * en otro thread de verdad y se lo espera por CONDICION, nunca por duracion
 * (REQ-002).
 *
 * Agregar iteraciones en vez de compuerta NO sirve, y esta medido en este repo:
 * contra una ventana de microsegundos no pega.
 */
TEST(CaptureGapMailbox, TheDetectorNeverStampsAPositionAndTheWriterDoesAtItsOwnBoundary) {
    AnalysisRing ring;
    CaptureGapMailbox box;
    std::vector<float> bloque(static_cast<size_t>(kBlockFrames) * 2, 0.1f);

    // El escritor (este thread) ya lleva dos bloques puestos.
    ring.writeStereo(bloque.data(), kBlockFrames);
    ring.writeStereo(bloque.data(), kBlockFrames);
    const uint64_t fronteraAntesDelHueco = static_cast<uint64_t>(kBlockFrames) * 2;
    ASSERT_EQ(ring.captureSeamPosition(), 0u) << "premisa: todavia no hay costura";

    // El detector corre en OTRO thread, como el callback de entrada de CoreAudio,
    // y deja la profundidad de la cola en el buzon. Nada mas.
    std::atomic<bool> avisado{false};
    std::thread detector([&] {
        box.note(48000);
        avisado.store(true, std::memory_order_release);
    });

    const auto tope = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!avisado.load(std::memory_order_acquire)) {
        ASSERT_LT(std::chrono::steady_clock::now(), tope) << "el detector nunca aviso";
        std::this_thread::yield();
    }
    detector.join();

    // 🔴 LA MITAD QUE IMPORTA: el detector ya aviso, y el ring NO SE MOVIO. La
    // costura no existe hasta que el escritor la ponga.
    EXPECT_EQ(ring.captureSeamPosition(), 0u)
        << "la costura aparecio sin que el escritor hiciera nada: alguien que no es el "
           "escritor esta estampando posiciones. Esa posicion sale de un instante que no "
           "corresponde al lugar del hueco en el stream.";

    // Ahora el escritor hace su vuelta: levanta el buzon ANTES de escribir, que
    // es el orden que CoreAudioBackend usa, y recien ahi hay costura.
    const uint64_t gap = box.take();
    ASSERT_NE(gap, CaptureGapMailbox::kEmpty);
    ring.reportCaptureDiscontinuity(gap);
    ring.writeStereo(bloque.data(), kBlockFrames);

    EXPECT_EQ(ring.captureSeamPosition(), fronteraAntesDelHueco + gap)
        << "la costura no quedo en la frontera del escritor mas la distancia de la cola";
}

// ===========================================================================
// EL MOTOR REENVIA LA DISTANCIA — el eslabon entre el backend y el nodo
// ===========================================================================

/**
 * `AudioEngine::onCaptureDiscontinuity` corre en el thread de SALIDA, que es el
 * que escribe el ring del afinador unas lineas mas abajo. Lo unico que hace es
 * traducir el aviso del backend a una costura sobre ese ring — y lo unico que
 * puede hacer mal es tirar la distancia, que es el hallazgo F otra vez.
 */
TEST(CaptureGapForwarding, TheEngineHandsTheDistanceThroughToTheTunerRing) {
    AudioEngine engine;
    AnalysisRing ring;
    auto node = std::make_shared<InputNode>();
    node->prepare(kRate, kBlockFrames);
    node->setAnalysisRing(&ring);
    node->setCaptureSampleRate(kRate);
    engine.setInputNode(node);

    std::vector<float> bloque(static_cast<size_t>(kBlockFrames) * 2, 0.1f);
    node->feedExternalInput(bloque.data(), kBlockFrames);

    engine.onCaptureDiscontinuity(48000);

    EXPECT_EQ(ring.captureSeamPosition(), static_cast<uint64_t>(kBlockFrames) + 48000u)
        << "el motor no reenvio la distancia al nodo. Con distancia cero la costura queda "
           "adelantada la cola entera y el motor vuelve a converger sobre el salto.";

    node->setAnalysisRing(nullptr);
    engine.setInputNode(nullptr);
}

/**
 * Y sin afinador escuchando, el aviso no puede reventar nada: el nodo lo
 * descarta solo. El backend no tiene por que enterarse de que existe un
 * afinador.
 */
TEST(CaptureGapForwarding, AGapWithNoTunerListeningIsHarmless) {
    AudioEngine engine;
    auto node = std::make_shared<InputNode>();
    node->prepare(kRate, kBlockFrames);
    engine.setInputNode(node);

    engine.onCaptureDiscontinuity(48000);   // sin ring conectado
    engine.onCaptureDiscontinuity(0);

    SUCCEED() << "no hay que afirmar un valor: lo que se prueba es que no revienta";
    engine.setInputNode(nullptr);
}
