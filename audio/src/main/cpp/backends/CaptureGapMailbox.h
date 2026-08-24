#pragma once

#include <atomic>
#include <cstdint>

namespace wma::backends {

/**
 * @brief El buzon de una discontinuidad de captura que todavia nadie posiciono.
 *
 * REQ-009 S3, tarea 3.4b. Existe por UN principio, que es el que este REQ pago
 * cuatro veces:
 *
 *   🔑 **La posicion de una costura la estampa SIEMPRE el thread que escribe el
 *   ring del afinador.** Cualquier otro la estampa en un instante que no
 *   corresponde al lugar del hueco — y no falla ruidosamente: publica un numero
 *   plausible.
 *
 * En iOS y en USB el que DETECTA el overrun es el callback de ENTRADA, y el que
 * escribe el `AnalysisRing` es el de SALIDA. Son threads distintos. Este buzon
 * es el unico cruce: el detector deja un NUMERO (cuantos frames de captura
 * seguian encolados por delante del hueco), y el escritor lo levanta y lo
 * convierte en posicion, en su propio bloque, con sus propias coordenadas.
 *
 * POR QUE UN NUMERO Y NO UN BOOL. Porque `framesQueuedAhead` es justamente lo
 * que le falta al escritor para saber DONDE cae el hueco: el detector lo mide
 * cuando lo ve, el escritor no lo puede reconstruir despues. Ver el hallazgo F
 * del doc de la etapa.
 *
 * GANA EL MAS LEJANO. Si entre dos consumos se avisan dos huecos, el que vale
 * es el que esta mas adelante: el otro queda de este lado y el descarte que el
 * lejano provoca ya lo cubre. Perder el lejano en cambio dejaria pasar el salto.
 *
 * POR QUE VIVE EN UN HEADER Y NO ADENTRO DE `CoreAudioBackend.mm`. Porque ese
 * archivo es Objective-C++ y solo se compila para iOS: la logica de aca no la
 * podria manejar ningun test de host. Es la misma separacion que 3.2b hizo con
 * el adaptador de Oboe — la plataforma cablea, la regla vive donde se puede
 * verificar.
 *
 * RT-safe de los dos lados: atomicos, sin asignar, sin locks, sin loguear.
 */
class CaptureGapMailbox {
public:
    /// No hay ningun hueco esperando a que lo posicionen.
    static constexpr uint64_t kEmpty = UINT64_MAX;

    /**
     * @brief Deja un hueco en el buzon. Lo llama EL DETECTOR, en su thread.
     *
     * @param framesQueuedAhead frames de captura que seguian encolados por
     *        DELANTE del hueco cuando se lo detecto. Cero cuando el hueco es
     *        aca mismo — el underrun, que detecta el propio callback de salida.
     */
    void note(uint64_t framesQueuedAhead) noexcept {
        uint64_t previo = mPending.load(std::memory_order_relaxed);
        // CAS y no un store: los dos huecos pueden venir de threads distintos
        // (overrun del de entrada, underrun del de salida) y el maximo tiene que
        // sobrevivir a que se pisen. El lazo termina: cada vuelta o gana, o
        // encuentra un valor que ya es mayor o igual.
        while (previo == kEmpty || previo < framesQueuedAhead) {
            if (mPending.compare_exchange_weak(previo, framesQueuedAhead,
                                               std::memory_order_release,
                                               std::memory_order_relaxed)) {
                return;
            }
        }
    }

    /**
     * @brief Vacia el buzon y devuelve lo que habia. Lo llama EL ESCRITOR.
     *
     * @return los frames encolados por delante del hueco, o `kEmpty` si no habia
     *         ninguno. Vaciar es parte de levantarlo: un hueco se posiciona UNA
     *         vez, y dejarlo puesto lo estamparia de nuevo en cada bloque —
     *         una guarda trabada, que es lo que AC-009.2 prohibe.
     */
    uint64_t take() noexcept {
        return mPending.exchange(kEmpty, std::memory_order_acquire);
    }

    /// Descarta lo pendiente sin posicionarlo. Para el arranque y la parada del
    /// stream: un hueco de la sesion anterior no describe a la nueva.
    void clear() noexcept { mPending.store(kEmpty, std::memory_order_release); }

private:
    std::atomic<uint64_t> mPending{kEmpty};
};

}  // namespace wma::backends
