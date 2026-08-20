#pragma once

/**
 * TestWait.h — las dos formas de esperar en un test, y la que se puede medir.
 *
 * POR QUE EXISTE (REQ-002 · S1)
 * -----------------------------
 * Tres de cinco pushes a `master` fallaron el CI el 2026-08-20, siempre en
 * `cpp-tests-macos`, siempre por lo mismo: un test que sincroniza con el reloj
 * de pared —`sleep_for(120ms)` contra un worker que polea cada 15— y afirma
 * despues. Verde en una maquina ociosa, rojo en un runner con siete jobs.
 *
 * LO QUE NO FUNCIONA COMO INSTRUMENTO, Y ESTA MEDIDO
 * --------------------------------------------------
 * Reproducirlo cargando la maquina NO anda. Medido en S1:
 *
 *     40 quemadores sobre 10 nucleos ................  0/10
 *     `taskpolicy -c background` + 20 quemadores ....  1/10, y ese uno fue TIMEOUT
 *     comprimir la espera a 0 ms ....................  10/10, en 2 segundos
 *
 * La razon es aritmetica y conviene tenerla escrita: un `sleep_for(120ms)` es
 * tiempo de reloj ABSOLUTO. Ralentizar la maquina no achica esa ventana — el
 * worker conserva sus ocho oportunidades de poll, y encima el render tarda mas,
 * lo que le da todavia mas margen. Ahogar la maquina hace todo mas lento; no
 * hace la ventana mas chica.
 *
 * LAS DOS FORMAS
 * --------------
 *   sleepFixed(d)          espera CIEGA. La forma DEBIL. Se escala.
 *   waitUntil(pred, techo) espera POR CONDICION. La forma CORRECTA. NO se escala.
 *
 * Y ahi esta el instrumento entero: con `WMA_TEST_WAIT_SCALE=0` las esperas
 * ciegas colapsan a cero y las esperas por condicion no se enteran. Un test que
 * cambia de veredicto bajo esa escala **depende del reloj**, que es exactamente
 * la clase. No hace falta reconocer una forma sintactica ni cargar la maquina:
 * se le saca el tiempo y se mira quien se cae.
 *
 * El techo de `waitUntil` NO se escala A PROPOSITO, en ninguna direccion. Si se
 * achicara, el instrumento voltearia tambien a los tests correctos y dejaria de
 * discriminar; si se agrandara, estaria escondiendo lentitud real. El dia que
 * los sanitizers necesiten techos mas anchos, eso se agrega con su propia razon
 * y su propia perilla — no colgado de esta.
 */

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

namespace wma_test {

/**
 * Factor por el que se multiplican las esperas CIEGAS. 1.0 salvo que
 * `WMA_TEST_WAIT_SCALE` diga otra cosa; negativo o no numerico se lee como 1.0,
 * porque una variable mal escrita no puede apagar el instrumento en silencio.
 *
 * Se lee una sola vez: que la escala cambie a mitad de una corrida haria que dos
 * tests del mismo binario midieran cosas distintas.
 */
inline double waitScale() {
    static const double kScale = [] {
        const char* raw = std::getenv("WMA_TEST_WAIT_SCALE");
        if (raw == nullptr || *raw == '\0') return 1.0;
        try {
            const double parsed = std::stod(raw);
            return parsed >= 0.0 ? parsed : 1.0;
        } catch (...) {
            return 1.0;
        }
    }();
    return kScale;
}

/**
 * Espera ciega, escalada. **La forma debil, y esta ahi para poder medirla.**
 *
 * Usala solo donde no hay condicion observable que esperar — por ejemplo para
 * dejar correr un rato algo cuyo efecto es justamente que NO pase nada. Si hay
 * algo que se puede consultar, va `waitUntil` y no esto.
 */
inline void sleepFixed(std::chrono::milliseconds d) {
    const auto scaled = std::chrono::duration_cast<std::chrono::nanoseconds>(d * waitScale());
    if (scaled.count() <= 0) return;
    std::this_thread::sleep_for(scaled);
}

/**
 * Espera a que `pred` sea cierto, con techo. Devuelve si llego a serlo.
 *
 * Rapida cuando el mundo va rapido y correcta cuando no: es la forma que hace
 * que el veredicto de un test no dependa de cuanto tarda la maquina. El techo es
 * generoso a proposito — no es un presupuesto de performance, es el punto donde
 * se deja de esperar para poder fallar con un mensaje propio en vez de colgarse.
 */
template <typename Predicate>
bool waitUntil(Predicate pred,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(2000),
               std::chrono::milliseconds poll = std::chrono::milliseconds(1)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (pred()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(poll);
    }
}

}  // namespace wma_test
