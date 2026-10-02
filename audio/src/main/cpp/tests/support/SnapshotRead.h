#pragma once

/**
 * SnapshotRead.h — como lee un TEST el snapshot del analisis (MINI-043).
 *
 * 🔴 UNA LECTURA ROTA NO ES UN VALOR
 * ----------------------------------
 * `AnalysisSnapshot::read` es un seqlock con 8 reintentos SIN `yield`, a
 * proposito: el lector de produccion puede ser la UI y no tiene que quedarse
 * girando. La contracara es que devuelve `false` tambien cuando el escritor esta
 * a mitad de un `publish` y los 8 reintentos caen todos adentro de la ventana
 * impar. Bajo sanitizers el `publish` instrumentado dura mas que esos 8
 * reintentos, y pasa: 0,027 % de las lecturas con la maquina ociosa (medido en
 * #284), el doble bajo carga.
 *
 * Un helper de test que traduce ese `false` a un numero —`-1`, `0.0`— fabrica
 * una medicion que nadie hizo. Le costo un rojo al CI de #385:
 * `DiscontinuityCount.ASustainedBreakCountsOnce` fallo con `before = -1` recien
 * convergido, y el `-1` no salia del motor sino de `Bench::discontinuityCount()`.
 * Es la misma clase que #284 arreglo en `AnalysedFrames`, en un archivo que quedo
 * afuera de ese arreglo — por eso esto vive en `tests/support/` y no copiado.
 *
 * LO QUE HACE
 * -----------
 * Reintenta CEDIENDO EL HILO (el `publish` termina en microsegundos) hasta que la
 * lectura salga coherente, con techo de tiempo. Distingue tres respuestas, porque
 * son tres cosas distintas y un llamador tiene que poder tratarlas distinto:
 *
 *   kCoherent       `out` tiene un juego entero, de UN solo publish.
 *   kNeverPublished no se publico nunca. Para un contador monotono eso es 0 DE
 *                   VERDAD (no hubo nada que contar); para cualquier otra cosa es
 *                   "no hay dato", y el llamador decide.
 *   kTimedOut       el escritor no termino un publish en todo el techo. No es una
 *                   carrera: es un escritor trabado, y el test tiene que FALLAR
 *                   diciendolo, no seguir con un valor.
 *
 * Es polling con deadline sobre `yield`, sin `sleep_for`: no es una espera ciega
 * y `check-test-waits.py` no tiene nada que clasificar. El techo es generoso a
 * proposito, igual que el de `waitUntil`: no es un presupuesto de latencia, es el
 * punto donde se deja de esperar para poder fallar con un mensaje propio.
 *
 * Es plantilla sobre el tipo del snapshot (pide `read(float*)` y `hasData()`)
 * para que el test de MINI-043 pueda contar los intentos fallidos con un
 * envoltorio, sin tocar produccion.
 */

#include "analysis/AnalysisSnapshot.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <thread>

namespace wma_test {

enum class SnapshotRead {
    kCoherent,
    kNeverPublished,
    kTimedOut,
};

inline const char* describe(SnapshotRead r) {
    switch (r) {
        case SnapshotRead::kCoherent:       return "lectura coherente";
        case SnapshotRead::kNeverPublished: return "nunca se publico";
        case SnapshotRead::kTimedOut:       return "techo vencido con el escritor a mitad de un publish";
    }
    return "?";
}

/// Techo por defecto. Mismo orden que el de `waitUntil`.
inline constexpr std::chrono::milliseconds kSnapshotReadCap{2000};

/**
 * Lee un juego COHERENTE o dice por que no. Nunca deja en `out` algo que no sea
 * un publish entero: si no devuelve `kCoherent`, `out` queda como estaba (es la
 * garantia de `AnalysisSnapshot::read`, que esto no toca).
 */
template <typename Snapshot>
SnapshotRead readCoherent(const Snapshot& snap, float* out,
                          std::chrono::milliseconds cap = kSnapshotReadCap) {
    const auto deadline = std::chrono::steady_clock::now() + cap;
    while (true) {
        if (snap.read(out)) return SnapshotRead::kCoherent;
        // Despues del read, no antes: si se publico entre los dos, hasData() ya
        // es true y se reintenta, que es lo correcto.
        if (!snap.hasData()) return SnapshotRead::kNeverPublished;
        if (std::chrono::steady_clock::now() >= deadline) return SnapshotRead::kTimedOut;
        std::this_thread::yield();   // el escritor esta a mitad de un publish
    }
}

/**
 * Un valor del snapshot, leido coherente. Si no se puede leer —nunca se publico,
 * o vencio el techo— el test FALLA con "no se pudo leer" y esto devuelve NaN.
 *
 * NaN y no -1 ni 0: no es un valor que el motor pueda haber publicado para estos
 * indices, y cualquier comparacion contra el sale falsa, asi que ningun
 * `EXPECT_*` posterior puede quedar verde por accidente sobre el.
 */
template <typename Snapshot>
double readCoherentValue(const Snapshot& snap, int index,
                         std::chrono::milliseconds cap = kSnapshotReadCap) {
    float o[wma::analysis::kSnapshotValueCount];
    const SnapshotRead r = readCoherent(snap, o, cap);
    if (r != SnapshotRead::kCoherent) {
        ADD_FAILURE() << "no se pudo leer el snapshot (indice " << index << "): "
                      << describe(r);
        return NAN;
    }
    return static_cast<double>(o[index]);
}

/**
 * Igual, para los valores que SI tienen respuesta sin publicacion: un contador
 * monotono (`framesAnalyzed`, `droppedFrames`, `discontinuityCount`) vale
 * `ifNeverPublished` de verdad si el analisis nunca publico. El techo vencido
 * sigue siendo una falla: un escritor trabado no es "cero".
 */
template <typename Snapshot>
double readCoherentValueOr(const Snapshot& snap, int index, double ifNeverPublished,
                           std::chrono::milliseconds cap = kSnapshotReadCap) {
    float o[wma::analysis::kSnapshotValueCount];
    const SnapshotRead r = readCoherent(snap, o, cap);
    if (r == SnapshotRead::kNeverPublished) return ifNeverPublished;
    if (r != SnapshotRead::kCoherent) {
        ADD_FAILURE() << "no se pudo leer el snapshot (indice " << index << "): "
                      << describe(r);
        return NAN;
    }
    return static_cast<double>(o[index]);
}

}  // namespace wma_test
