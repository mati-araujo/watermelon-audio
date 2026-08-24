/**
 * REQ-009 S3 — el eje de CAPTURA llega hasta el afinador.
 *
 * QUE CUBRE, Y POR QUE NO LO CUBRE `test_non_contiguous.cpp`
 * ----------------------------------------------------------
 * Aquel trinquete maneja `StrobeTracker` **en aislamiento** y le inyecta el hueco
 * corriendo el indice de la señal: no hay ring, ni thread, ni backend. O sea que
 * afirma una propiedad del estimador SOLO, **sin que nadie le avise** — y esa
 * propiedad no cambia con esta etapa, porque el estimador sigue sin poder verlo.
 * Lo que S3 construye es que alguien se lo DIGA, y eso solo se ve de punta a
 * punta: backend -> `InputNode` -> `AnalysisRing` -> `AnalysisThread`.
 *
 * Este archivo es ese camino, y por eso vive en `core/tests` y no en
 * `analysis/tests`: necesita el `InputNode` real, que la suite de host compila
 * desde el 2026-08-18.
 *
 * LOS DOS MODOS DE FALLA, Y DE DONDE SALEN
 * ----------------------------------------
 * No son inventados: son lo que los dos backends DECLARAN hacer con su propio
 * ring, el que va entre el callback de entrada y el de salida.
 *
 *   HUECO (overrun)     el productor no entro: el bloque se DESCARTA y esos
 *                       frames de fuente no los ve nadie. El audio que sigue
 *                       queda pegado al anterior — COMPRIMIDO en el tiempo.
 *                       (Oboe `mInputRingOverruns++`; CoreAudio "Overflow drops
 *                       the block instead of blocking")
 *
 *   SILENCIO (underrun) el consumidor llego antes: se entrega un bloque de CEROS
 *                       **de mas**, y el audio de fuente sigue esperando su
 *                       turno. El stream queda DILATADO.
 *                       (`LockFreeRingBuffer::read()` hace memset y devuelve
 *                       false, y los dos backends entregan el buffer igual.)
 *
 * 🔴 La diferencia esta en el INDICE DE FUENTE: en el overrun avanza sin
 * entregar, en el underrun se entrega sin avanzar. La primera version del probe
 * de S1 lo hacia avanzar en los dos —o sea modelaba el underrun como un agujero—
 * y ahi el SILENCIO parecia inofensivo (~1e-3 cents). Corregido el modelo, sube
 * a 0,40. Un agujero conserva la referencia de fase; una insercion la corre.
 *
 * MEDIDO ANTES DE ESCRIBIR ESTE TEST (S1, sobre `88afb10`, o sea CON la guarda
 * de S2 puesta). `droppedFrames` = 0 y marca = 0 en TODAS las filas:
 *
 *     modo        cada    error     sigma    estado
 *     HUECO         64    0,078    0,0035    CONVERGIDO
 *     HUECO         16    0,334    0,0029    CONVERGIDO
 *     HUECO          8    0,730    0,0023    CONVERGIDO
 *     HUECO          4    2,146    0,00098   CONVERGIDO   <- 21x el presupuesto
 *     SILENCIO       8    0,399    0,0041    CONVERGIDO
 *
 * 🔑 σ esta ANTI-CORRELACIONADA con el error: la peor fila tiene la σ mas chica.
 * Cuarta vez que este repo lo mide, y la razon por la que la deteccion NO puede
 * salir del estimador.
 *
 * 🔴 POR QUE EL ARNES TIENE QUE AVISAR, Y NO SOLO ROMPER EL AUDIO
 * ---------------------------------------------------------------
 * La primera version de este archivo inyectaba el daño y nada mas. Nacia roja,
 * pero **se habria quedado roja para siempre**: un bloque que se pierde sin que
 * nadie lo reporte es precisamente lo que S1 midio como INDETECTABLE — el
 * estimador no lo ve (σ anti-correlacionada) y el ring del afinador tampoco
 * (`droppedFrames` = 0). Ningun arreglo posible lo pone verde.
 *
 * Lo que S3 puede arreglar es el caso REAL, que es otro: el backend **ya sabe**
 * que tiro audio —Oboe lo cuenta, CoreAudio puede— y esa noticia hoy se pierde
 * antes de llegar al afinador. Asi que el arnes modela al backend ENTERO: pierde
 * el bloque **y avisa**, que es lo que un backend hace o puede hacer.
 *
 * El caso "se perdio y nadie aviso" no desaparece: sigue siendo indetectable, y
 * eso esta escrito en `NobodyReportedItSoNobodyCanKnow`, abajo — un test que
 * documenta el LIMITE de esta etapa en vez de dejarlo implicito.
 */
#include "../../nodes/InputNode.h"
#include "../../analysis/AnalysisRing.h"
#include "../../analysis/AnalysisSnapshot.h"
#include "../../analysis/AnalysisThread.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using namespace wma::analysis;

constexpr int kRate = 48000;
constexpr int kBlockFrames = 1024;
constexpr double kTargetHz = 440.0;
constexpr double kRealCents = -5.0;
constexpr double kBudgetCents = AnalysisThread::kConvergedUncertaintyCents;

/// Los backends fallan por BLOQUE, no por frame: sus dos rings mueven bloques
/// enteros del tamaño del callback. Modelarlo por frame mediria otra cosa.
enum class Falla { NINGUNA, HUECO, SILENCIO };

/// Cuerda de 4 parciales, con la fase derivada del indice ABSOLUTO de frame: asi
/// saltear un bloque produce una señal genuinamente no contigua, y no un simple
/// reinicio de fase.
void llenarCuerda(float* stereo, int frames, int startFrame, double f0) {
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(startFrame + i) / kRate;
        double v = 0.0;
        for (int p = 1; p <= 4; ++p) {
            v += (0.6 / p) * std::sin(2.0 * M_PI * f0 * p * t);
        }
        const float s = static_cast<float>(v * 0.4);
        stereo[2 * i] = s;
        stereo[2 * i + 1] = s;
    }
}

/**
 * Lo que se vio publicado MIENTRAS la captura estaba fallando.
 *
 * 🔴 SE OBSERVA DURANTE, Y NO AL FINAL. Leer el snapshot despues de dejar de
 * romper mide otra cosa: entre hueco y hueco el motor se recupera con audio
 * limpio —y converger ahi es lo CORRECTO, no un fallo—, asi que un veredicto
 * tomado al final depende de cuanto audio sano hubo despues del ultimo daño.
 * **Medido**: con huecos cada 4 bloques la marca seguia arriba al final, y con
 * huecos cada 16 ya se habia bajado sola. El mismo test, dos veredictos, por una
 * propiedad del arnes y no del motor.
 *
 * Es exactamente la trampa que S2 ya pago con `feedOverrunning`. Dos etapas,
 * misma piedra: **si el daño es intermitente, el veredicto se toma durante**.
 */
struct Lectura {
    int muestras = 0;            ///< publicaciones distintas vistas mientras se rompia
    int convergidas = 0;
    int marcaArriba = 0;
    double peorErrorConvergido = 0.0;
    double sigmaAhi = 0.0;
    double dropped = 0.0;
    // La foto final, para el caso sano y para el limite documentado.
    double cents = 0.0, sigma = 0.0, marca = 0.0;
    int estado = -1;
    bool hubo = false;
};

/// @param cadaCuantos cada cuantos bloques se inyecta la falla. 0 = nunca.
Lectura correr(Falla modo, int cadaCuantos, bool avisa = true, int bloques = 400) {
    AnalysisRing ring;
    AnalysisSnapshot snap;
    AnalysisThread th(ring, snap);
    InputNode node;
    node.prepare(kRate, kBlockFrames);
    node.setInputGain(0.0f);
    node.setMonitoringEnabled(false);
    node.setAnalysisRing(&ring);
    // 🔴 EL RATE VA EN EL NODO, NO SOLO EN EL RING, y no es redundante: el nodo
    // ESTAMPA su `mCaptureSampleRate` sobre el ring en cada bloque (ver
    // `InputNode.cpp`, el ring del afinador), y arranca en **0**. Sembrar solo el
    // ring deja una carrera: si el thread alcanza a preparar antes del primer
    // bloque, mide; si el primer bloque llega antes, el rate queda en 0, el
    // estimador nunca prepara y el test reporta `NO_LOCK` con cents NaN — que se
    // lee como "la guarda apago todo" y no como "el arnes no sembro el rate".
    // Costó un rojo del gemelo para verlo.
    node.setCaptureSampleRate(kRate);
    ring.setCaptureRate(kRate);
    th.setTargetHz(kTargetHz);
    th.start(kRate);

    const double real = kTargetHz * std::pow(2.0, kRealCents / 1200.0);
    std::vector<float> bloque(static_cast<size_t>(kBlockFrames) * 2, 0.0f);

    auto analizados = [&]() -> double {
        float o[kSnapshotValueCount];
        return snap.read(o) ? static_cast<double>(o[kSnapFramesAnalyzed]) : 0.0;
    };
    // 🔴 SE ESPERA LUGAR EN EL RING, y no es opcional: si el alimentador lo
    // desborda, este test deja de medir el eje de CAPTURA y pasa a medir el del
    // RING —que es el que S2 ya cubre— con otro nombre. La premisa se afirma
    // abajo con `dropped == 0`.
    auto esperarLugar = [&]() {
        const auto tope = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (ring.availableFrames() + kBlockFrames > AnalysisRing::kCapacityFrames) {
            if (std::chrono::steady_clock::now() > tope) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    };

    Lectura obs;
    double ultimoFrames = -1.0;
    // Mira el snapshot y guarda cada publicacion NUEVA. Se llama despues de cada
    // bloque para no perderse las vueltas de justo despues del daño.
    auto observar = [&]() {
        float o[kSnapshotValueCount];
        if (!snap.read(o)) return;
        if (o[kSnapFramesAnalyzed] == ultimoFrames) return;
        ultimoFrames = o[kSnapFramesAnalyzed];
        if (!(o[kSnapFramesAnalyzed] > 0.0f)) return;
        ++obs.muestras;
        if (o[kSnapInputDiscontinuity] >= 0.5f) ++obs.marcaArriba;
        if (static_cast<int>(o[kSnapState]) == kStateConverged) {
            ++obs.convergidas;
            const double e = std::fabs(static_cast<double>(o[kSnapCents]) - kRealCents);
            if (e > obs.peorErrorConvergido) {
                obs.peorErrorConvergido = e;
                obs.sigmaAhi = o[kSnapUncertainty];
            }
        }
    };

    int absFrame = 0;
    for (int b = 0; b < bloques; ++b) {
        const bool falla = cadaCuantos > 0 && b > 8 && (b % cadaCuantos == 0);
        if (falla && modo == Falla::HUECO) {
            absFrame += kBlockFrames;      // overrun: se pierde audio, no se entrega
            // ...y el backend AVISA, que es lo unico que hace esto arreglable.
            if (avisa) node.reportCaptureDiscontinuity();
            continue;
        }
        if (!esperarLugar()) break;
        if (falla && modo == Falla::SILENCIO) {
            if (avisa) node.reportCaptureDiscontinuity();
            // underrun: se entrega silencio de MAS. `absFrame` NO avanza — el
            // audio de fuente no se consumio, sigue esperando.
            std::fill(bloque.begin(), bloque.end(), 0.0f);
        } else {
            llenarCuerda(bloque.data(), kBlockFrames, absFrame, real);
            absFrame += kBlockFrames;
        }
        node.feedExternalInput(bloque.data(), kBlockFrames);
        observar();
    }

    const double meta = analizados();
    const auto tope = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (analizados() <= meta && std::chrono::steady_clock::now() < tope) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    float o[kSnapshotValueCount];
    if (snap.read(o)) {
        obs.hubo = true;
        obs.cents = o[kSnapCents];   obs.sigma  = o[kSnapUncertainty];
        obs.dropped = o[kSnapDroppedFrames];
        obs.estado = static_cast<int>(o[kSnapState]);
        obs.marca = o[kSnapInputDiscontinuity];
    }
    th.stop();
    return obs;
}

void afirmar(Falla modo, int cada, const char* nombre) {
    const Lectura r = correr(modo, cada);
    ASSERT_TRUE(r.hubo) << nombre << ": no se publico snapshot; la medicion no ocurrio";
    ASSERT_GT(r.muestras, 0) << nombre << ": no se observo ni una publicacion";
    ASSERT_EQ(r.dropped, 0.0)
        << nombre << ": el alimentador piso " << r.dropped << " frames del ring del "
           "afinador. Esto ya no mide el eje de CAPTURA, mide el del RING — que cubre S2.";

    // AC-009.1 — no se pide "que no converja nunca": entre hueco y hueco el motor
    // se pone al dia con audio limpio y converger ahi es lo CORRECTO. Lo que se
    // pide es que todo lo que declare convergido lo este de verdad.
    EXPECT_LE(r.peorErrorConvergido, kBudgetCents)
        << nombre << ": el motor declaro CONVERGIDA una lectura a " << r.peorErrorConvergido
        << " cents del valor real (presupuesto " << kBudgetCents << "), con sigma="
        << r.sigmaAhi << " y droppedFrames=0. Su integracion cruzo un salto de la CAPTURA.\n"
        << "  Pasó en " << r.convergidas << " de " << r.muestras << " publicaciones.\n"
        << "  🔴 sigma no lo puede atajar y esta MEDIDO: en el peor caso del barrido de S1 "
           "vale 0,00098 con 2,15 cents de error, o sea la mas CHICA de todas las filas con "
           "falla. La deteccion tiene que venir del backend, que ya sabe que tiro audio.";

    // AC-009.3 — y el consumidor tiene que poder distinguirlo de "todavia no".
    EXPECT_GT(r.marcaArriba, 0)
        << nombre << ": la captura perdio continuidad a lo largo de " << r.muestras
        << " publicaciones y el snapshot no lo dijo NUNCA. El consumidor ve un spinner y "
           "espera a que se arregle algo que no se arregla solo (AC-009.3).";
}

}  // namespace

/**
 * AC-009.1 sobre el eje de CAPTURA — el modo que mas duele.
 *
 * `cada 4` es la fila que S1 midio en 2,15 cents, 21x el presupuesto, con el
 * motor diciendo CONVERGIDO.
 */
TEST(CaptureDiscontinuity, ALostInputBlockIsNotPublishedAsConverged) {
    afirmar(Falla::HUECO, 4, "HUECO cada 4");
}

TEST(CaptureDiscontinuity, ASparserLossIsAlsoNotPublishedAsConverged) {
    afirmar(Falla::HUECO, 16, "HUECO cada 16");
}

/**
 * El underrun: silencio INSERTADO. S1 lo midio en 0,40 cents — 4x el
 * presupuesto—, y su primera version lo declaro inofensivo por un error de
 * modelo (ver el encabezado).
 */
TEST(CaptureDiscontinuity, AnInsertedSilenceBlockIsNotPublishedAsConverged) {
    afirmar(Falla::SILENCIO, 8, "SILENCIO cada 8");
}

/**
 * EL GEMELO, y es el que se olvida: sin captura rota, el motor tiene que SEGUIR
 * convergiendo. Sin esto, apagar `CONVERGIDO` para siempre pasaria los tres de
 * arriba sin resolver nada.
 */
TEST(CaptureDiscontinuity, HealthyCaptureStillConverges) {
    // Acá la foto final SÍ corresponde: sin daño no hay recuperación que confunda.
    const Lectura r = correr(Falla::NINGUNA, 0);
    ASSERT_TRUE(r.hubo) << "no se publico snapshot";
    ASSERT_EQ(r.dropped, 0.0) << "premisa rota: el caso sano piso frames";

    EXPECT_EQ(r.estado, kStateConverged)
        << "con la captura intacta el motor dejo de converger. Una guarda que nunca deja "
           "dibujar la aguja cumple los tests de arriba sin resolver nada.";
    EXPECT_NEAR(r.cents, kRealCents, kBudgetCents)
        << "con captura intacta la lectura tiene que caer dentro de presupuesto";
    EXPECT_FLOAT_EQ(static_cast<float>(r.marca), 0.0f)
        << "sin hueco el motor acusa uno: una marca siempre prendida no distingue nada";
}

/**
 * EL LIMITE DE ESTA ETAPA, escrito como test y no como comentario.
 *
 * Si el backend pierde audio y **no avisa**, no hay forma de saberlo: el ring del
 * afinador ve bloques contiguos (`droppedFrames` = 0) y el estimador no puede
 * distinguirlo (σ anti-correlacionada, medido en S1). El motor converge sobre
 * una lectura equivocada, y **eso sigue pasando despues de S3**.
 *
 * No es una laguna que alguien se olvido de tapar: es la razon por la que S3
 * toca los BACKENDS en vez de intentar detectarlo aguas abajo. Este test existe
 * para que ese limite no se descubra por sorpresa — y para que el dia que un
 * backend nuevo entre sin reportar, se sepa exactamente que se pierde.
 */
TEST(CaptureDiscontinuity, NobodyReportedItSoNobodyCanKnow) {
    const Lectura r = correr(Falla::HUECO, 4, /*avisa=*/false);
    ASSERT_TRUE(r.hubo) << "no se publico snapshot";
    ASSERT_EQ(r.dropped, 0.0)
        << "el ring del afinador no puede haber pisado nada: el hueco es de CAPTURA";

    const double error = std::fabs(r.cents - kRealCents);
    EXPECT_GT(error, kBudgetCents)
        << "un hueco de captura NO reportado dejo de producir una lectura fuera de "
           "presupuesto (error " << error << " cents).\n"
        << "  Si el motor se volvio robusto a esto, es una BUENA noticia y este trinquete hay "
           "que actualizarlo — no borrarlo. Si no, dejo de reproducir el limite que declara.";
    EXPECT_EQ(r.estado, kStateConverged)
        << "sin aviso, el motor NO tendria como enterarse — y sin embargo dejo de declarar "
           "convergida la lectura. Algo lo esta detectando por otra via: averiguar cual antes "
           "de tocar este test, porque cambiaria el alcance de S3.";
}

// ===========================================================================
// EL CAMINO DE ANDROID (REQ-009 S3, tarea 3.2b)
// ===========================================================================
//
// Los cinco tests de arriba entran por `feedExternalInput`, que es el camino de
// iOS y USB, y el arnes hace de backend llamando a `reportCaptureDiscontinuity()`
// a mano. Eso deja SIN CUBRIR justo el camino de la plataforma principal: en
// Android el afinador NO pasa por el ring del backend — `wma_tuner_start` hace
// que el `InputNode` abra su PROPIO stream de Oboe — asi que la fuente del aviso
// es el xrun de ESE stream, y quien lo convierte en costura es
// `processInputBlock`. Esa conversion es la que se maneja aca.
//
// Estos NO repiten la medicion de cents: eso ya lo afirman los de arriba sobre el
// mismo `noteInputDiscontinuity()`. Lo que falta probar es el CABLE, y el cable se
// observa donde termina: la posicion de la costura en el `AnalysisRing`.
//
// 🔴 POR QUE HACE FALTA EL GANCHO. `processInputBlock` chequea
// `mInputStreamRunning`, que en host no se puede poner en true por ninguna via
// legitima (lo escribe solo `startInputStream()`, que sin Oboe devuelve false
// antes de tocarlo). Sin el gancho este cable quedaria verificado unicamente por
// leerlo — que es exactamente como el plan de esta etapa llego a apuntar al ring
// equivocado durante dos tareas. Ver la nota de los ganchos en `InputNode.cpp`.
extern std::atomic<bool> gInputNodeForceStreamRunning;

namespace {

/// Deja el gancho prendido mientras dure el bloque, y lo apaga pase lo que pase.
/// Es global: dejarlo prendido contaminaria a los otros tests del binario.
struct StreamCorriendo {
    StreamCorriendo()  { gInputNodeForceStreamRunning.store(true,  std::memory_order_release); }
    ~StreamCorriendo() { gInputNodeForceStreamRunning.store(false, std::memory_order_release); }
};

/// Un nodo con ring conectado, listo para recibir bloques por el camino de Oboe.
struct NodoOboe {
    AnalysisRing ring;
    InputNode node;
    std::vector<float> bloque;

    NodoOboe() : bloque(static_cast<size_t>(kBlockFrames) * 2, 0.0f) {
        node.prepare(kRate, kBlockFrames);
        node.setAnalysisRing(&ring);
        node.setCaptureSampleRate(kRate);
        ring.setCaptureRate(kRate);
    }
    ~NodoOboe() { node.setAnalysisRing(nullptr); }

    /// Un bloque por el camino de Oboe, con el acumulado de xruns que el stream
    /// reportaria en ese momento. Estereo, que es como Oboe abre la captura.
    bool bloqueConXRuns(int32_t xRuns) {
        std::fill(bloque.begin(), bloque.end(), 0.25f);
        const bool ok = node.processInputBlock(bloque.data(), kBlockFrames, 2, xRuns);
        if (ok) escritos += kBlockFrames;
        return ok;
    }

    /// La posicion de escritura del ring, en frames — el mismo sistema de
    /// coordenadas de `captureSeamPosition()`.
    ///
    /// Se lleva a mano y no con `availableFrames()`: ese SATURA en
    /// `kCapacityFrames` y aca nadie lee el ring, asi que pasados 8 bloques
    /// dejaria de decir la verdad. Nadie mas escribe este ring, o sea que la
    /// cuenta es exacta.
    uint64_t frontera() const { return escritos; }

private:
    uint64_t escritos = 0;
};

}  // namespace

/**
 * AC-009.1 sobre el camino de Android — EL CABLE.
 *
 * El stream de Oboe acusa un xrun mas: eso tiene que quedar estampado como
 * costura, y en la frontera EXACTA entre el bloque anterior y el siguiente.
 * La posicion importa tanto como el hecho: un aviso sin posicion ya se probo y
 * dejaba 1 de cada ~80 publicaciones convergida a 0,18 cents (ver el KDoc de
 * `AnalysisRing::reportCaptureDiscontinuity`).
 */
TEST(CaptureDiscontinuity, TheOboePathTurnsItsOwnXRunIntoAPositionedSeam) {
    StreamCorriendo encendido;
    NodoOboe n;

    ASSERT_TRUE(n.bloqueConXRuns(0)) << "el nodo rechazo el primer bloque";
    ASSERT_TRUE(n.bloqueConXRuns(0)) << "el nodo rechazo el segundo bloque";
    ASSERT_EQ(n.ring.captureSeamPosition(), 0u)
        << "sin xruns nuevos no puede haber costura: una marca siempre prendida no distingue nada";

    // Aca el stream perdio audio entre el bloque anterior y este.
    const uint64_t frontera = n.frontera();
    ASSERT_TRUE(n.bloqueConXRuns(1));

    EXPECT_EQ(n.ring.captureSeamPosition(), frontera)
        << "el xrun del stream de captura no llego al ring del afinador, o llego sin la "
           "posicion correcta.\n"
        << "  Se esperaba la costura en " << frontera << " —la frontera entre lo que ya "
           "estaba escrito y el bloque de despues del hueco— y quedo en "
        << n.ring.captureSeamPosition() << ".\n"
        << "  🔴 Estamparla DESPUES de escribir el bloque la corre " << kBlockFrames
        << " frames, y el lector deja pasar audio que cruza el salto.";
}

/**
 * EL GEMELO, y es el que se olvida: sin xruns nuevos, el camino de Oboe no puede
 * inventar costuras. Sin esto, "reportar siempre" pasaria el test de arriba.
 */
TEST(CaptureDiscontinuity, TheOboePathInventsNoSeamWhileTheStreamIsHealthy) {
    StreamCorriendo encendido;
    NodoOboe n;

    for (int b = 0; b < 12; ++b) {
        ASSERT_TRUE(n.bloqueConXRuns(3)) << "bloque " << b << " rechazado";
    }
    EXPECT_EQ(n.ring.captureSeamPosition(), 0u)
        << "el stream reporto SIEMPRE el mismo acumulado de xruns —o sea, ninguno nuevo— y "
           "el nodo estampo una costura igual. Una guarda que reinicia en cada bloque nunca "
           "deja converger, y eso pasa los tests de arriba sin resolver nada.";
}

/**
 * EL BACKEND QUE NO SABE CONTAR. En OpenSL ES `getXRunCount()` devuelve
 * `ErrorUnimplemented`, y el adaptador pasa `kCaptureXRunsUnknown`.
 *
 * No saber NO es "todo bien" — pero tampoco es "hubo un hueco". Lo unico correcto
 * es no afirmar nada: si el desconocido se tratara como un valor mas, el primer
 * bloque de un stream sano ya estamparia una costura, y despues cada alternancia
 * entre conocido y desconocido otra.
 */
TEST(CaptureDiscontinuity, AnUnknownXRunCountAssertsNothingEitherWay) {
    StreamCorriendo encendido;
    NodoOboe n;

    ASSERT_TRUE(n.bloqueConXRuns(InputNode::kCaptureXRunsUnknown));
    ASSERT_TRUE(n.bloqueConXRuns(InputNode::kCaptureXRunsUnknown));
    EXPECT_EQ(n.ring.captureSeamPosition(), 0u)
        << "un backend que no sabe contar xruns termino afirmando que hubo uno";

    // Y cuando empieza a saber, la PRIMERA observacion siembra: no hay audio
    // anterior con el que ese acumulado pudiera ser discontinuo.
    ASSERT_TRUE(n.bloqueConXRuns(9));
    EXPECT_EQ(n.ring.captureSeamPosition(), 0u)
        << "la primera lectura del contador se tomo como un salto. Un stream puede arrancar "
           "con xruns ya acumulados de antes de que el afinador existiera.";

    const uint64_t frontera = n.frontera();
    ASSERT_TRUE(n.bloqueConXRuns(10));
    EXPECT_EQ(n.ring.captureSeamPosition(), frontera)
        << "sembrado el contador, el salto siguiente si tiene que verse";
}

/**
 * EL DESCONOCIDO INTERMITENTE — y este test existe porque un mutante SOBREVIVIO.
 *
 * Sacar la guarda de `xRunCount < 0` no rompia ninguno de los otros tres: con la
 * siembra puesta, una tira de desconocidos al principio se absorbe sola. El caso
 * que si cambia es el que ninguno tocaba — el stream venia contando, deja de
 * saber por un bloque, y vuelve:
 *
 *   sin la guarda, ese `-1` se compara contra el `5` anterior, sale distinto, y
 *   el nodo estampa una costura sobre audio SANO. Y de yapa deja el contador
 *   sembrado en -1, o sea que el bloque siguiente vuelve a sembrar y se pierde
 *   el proximo salto de verdad.
 *
 * "No se" no es "hubo un hueco" — igual que no es "todo bien".
 */
TEST(CaptureDiscontinuity, AnIntermittentlyUnknownCountIsNoSeamAndLosesNoSeed) {
    StreamCorriendo encendido;
    NodoOboe n;

    ASSERT_TRUE(n.bloqueConXRuns(5));
    ASSERT_TRUE(n.bloqueConXRuns(5));
    ASSERT_TRUE(n.bloqueConXRuns(InputNode::kCaptureXRunsUnknown));  // un bloque sin dato
    ASSERT_TRUE(n.bloqueConXRuns(5));
    EXPECT_EQ(n.ring.captureSeamPosition(), 0u)
        << "el stream dejo de saber su cuenta de xruns por un bloque y el nodo lo tomo como "
           "un salto. El audio no se rompio: lo que falto fue el dato.";

    // Y el contador tiene que haber quedado sembrado en 5, no en el desconocido:
    // si el `-1` lo piso, este salto se lee como primera observacion y se pierde.
    const uint64_t frontera = n.frontera();
    ASSERT_TRUE(n.bloqueConXRuns(6));
    EXPECT_EQ(n.ring.captureSeamPosition(), frontera)
        << "despues de un bloque sin dato, el salto siguiente se perdio: el desconocido piso "
           "la semilla y el nodo volvio a arrancar de cero.";
}

/**
 * EL STREAM REABIERTO, que es el caso que un `>` deja pasar.
 *
 * Un stream nuevo arranca su contador en cero, asi que el acumulado BAJA. Eso es
 * una costura tanto como una subida —el audio de antes y el de despues no son
 * contiguos— y compararlo con "aumento" lo dejaria invisible.
 */
TEST(CaptureDiscontinuity, AReopenedStreamResetsTheCounterAndThatIsASeamToo) {
    StreamCorriendo encendido;
    NodoOboe n;

    ASSERT_TRUE(n.bloqueConXRuns(7));
    ASSERT_TRUE(n.bloqueConXRuns(7));
    ASSERT_EQ(n.ring.captureSeamPosition(), 0u) << "premisa: todavia no hubo salto";

    const uint64_t frontera = n.frontera();
    ASSERT_TRUE(n.bloqueConXRuns(0));   // stream reabierto: el contador vuelve a cero

    EXPECT_EQ(n.ring.captureSeamPosition(), frontera)
        << "el contador de xruns BAJO —un stream reabierto— y el nodo no lo tomo como "
           "discontinuidad. Comparar con `>` en vez de con `!=` deja pasar exactamente este "
           "caso, y el audio de los dos streams no es contiguo.";
}
