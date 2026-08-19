#pragma once

/**
 * @file AnalysisRing.h
 * @brief El ring que lleva la senal de captura al analisis (REQ-001 S1).
 *
 * QUE HACE
 * --------
 * El thread de captura le entrega bloques estereo intercalados; los suma a mono
 * y los deja en un ring de 8192 frames (~170 ms a 48 kHz). Un thread de analisis
 * los drena a su ritmo. El escritor NUNCA se bloquea y NUNCA falla.
 *
 * POR QUE NO ES UNA ENVOLTURA SOBRE `dsp/LockFreeRingBuffer.h`
 * ------------------------------------------------------------
 * La spec de la etapa lo pedia asi, y no se puede: son dos semanticas
 * incompatibles.
 *
 *   - `LockFreeRingBuffer::write()` devuelve `false` y **no escribe** cuando no
 *     entra. Descarta lo NUEVO. Es lo correcto para su uso —el monitoreo, donde
 *     perder el audio mas reciente es preferible a mezclar el orden— y es lo que
 *     esperan los dos rings del `InputNode`.
 *   - La tarea 1.3 de esta etapa pide lo contrario: **descartar lo mas VIEJO**.
 *     Para un afinador es obvio por que — un analisis atrasado no quiere las
 *     muestras de hace 200 ms, quiere las de ahora.
 *
 * Darle sobreescritura a `LockFreeRingBuffer` cambiaria el comportamiento de los
 * dos rings del `InputNode`, que es riesgo por prolijidad. Y emularla desde
 * afuera es imposible sin romper el SPSC: el escritor tendria que avanzar el
 * indice de LECTURA, que es la variable del consumidor, y dos threads
 * escribiendo el mismo indice pierden actualizaciones aunque el tipo sea atomico.
 *
 * COMO ESTA HECHO, Y POR QUE ES SEGURO
 * ------------------------------------
 * Contadores MONOTONOS de frames, no indices que dan la vuelta:
 *
 *   - `mWritten` la escribe SOLO el thread de captura.
 *   - `mRead` la escribe SOLO el thread de analisis.
 *
 * Nadie comparte una variable de escritura, que es lo que hace que esto sea
 * lock-free de verdad y no de nombre. El escritor pisa lo viejo sin preguntar y
 * sin mirar al lector: por eso no puede bloquearse.
 *
 * **El que detecta el atraso es el LECTOR, no el escritor**, y no es un detalle
 * de implementacion: en un ring que sobreescribe, el escritor no tiene forma de
 * saber que alguien se quedo atras — solo escribe. El lector compara `mWritten`
 * contra su propia posicion y, si la distancia paso la capacidad, sabe cuantos
 * frames se perdio EXACTAMENTE.
 *
 * LA CARRERA QUE SI EXISTE, Y COMO SE CIERRA
 * ------------------------------------------
 * Un ring que sobreescribe tiene un peligro real: el escritor puede pasar por
 * encima de la region que el lector esta copiando, y el lector se lleva datos
 * DESGARRADOS — mitad viejos, mitad nuevos, sin que nada avise.
 *
 * Se cierra releyendo `mWritten` DESPUES de copiar. Si en el medio el escritor
 * avanzo lo suficiente como para haber tocado lo que copiamos, la copia se
 * descarta y se cuenta como perdida. Es el mismo patron que un seqlock: no se
 * evita la carrera, se DETECTA y se reintenta.
 *
 * `test_analysis_ring.cpp` la fuerza con una compuerta, no con iteraciones —
 * este repo ya midio que 40 retiros por corrida y 15 corridas no pegan una
 * ventana de microsegundos.
 */

#include "../platform/RtCounter.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace wma::analysis {

class AnalysisRing {
public:
    /// 8192 frames = ~170 ms a 48 kHz. Cubre el jitter de scheduling con margen
    /// y NO sostiene la integracion del strobe — esa vive en el estimador.
    /// Potencia de dos a proposito: el indice se calcula con mascara, no con `%`.
    static constexpr uint32_t kCapacityFrames = 8192;
    static constexpr uint32_t kMask = kCapacityFrames - 1;

    AnalysisRing()
        : mBuffer(std::make_unique<std::atomic<float>[]>(kCapacityFrames)) {
        for (uint32_t i = 0; i < kCapacityFrames; ++i) {
            mBuffer[i].store(0.0f, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Deja un bloque estereo intercalado, sumado a mono. RT-SAFE.
     *
     * No asigna, no loguea, no toma locks y no puede fallar. Si el analisis se
     * atraso, esto pisa lo mas viejo sin enterarse — enterarse es del lector.
     *
     * Un bloque mas grande que la capacidad entera se recorta a los ultimos
     * `kCapacityFrames`: lo anterior ya estaria pisado antes de que nadie lo
     * pudiera leer, asi que copiarlo seria trabajo puro en el thread RT.
     */
    void writeStereo(const float* interleaved, int numFrames) noexcept {
        if (interleaved == nullptr || numFrames <= 0) return;

        uint32_t n = static_cast<uint32_t>(numFrames);
        if (n > kCapacityFrames) {
            interleaved += static_cast<size_t>(n - kCapacityFrames) * 2;
            n = kCapacityFrames;
            // Contarlo es lo que hace al recorte OBSERVABLE. Sin esto, sacar el
            // recorte entero no cambia ni un valor del buffer —las ultimas
            // `kCapacityFrames` escrituras pisan todas las ranuras igual— asi
            // que ningun test de comportamiento puede distinguir las dos
            // versiones: medido, el mutante que lo borra sobrevive. Lo que el
            // recorte ahorra es TRABAJO en el thread RT, y el trabajo no se ve
            // en la salida. El contador lo saca a la superficie.
            mOversizedBlocks.bump();
        }

        const uint64_t w = mWritten.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n; ++i) {
            const float l = interleaved[static_cast<size_t>(i) * 2];
            const float r = interleaved[static_cast<size_t>(i) * 2 + 1];
            mBuffer[static_cast<uint32_t>(w + i) & kMask].store(
                0.5f * (l + r), std::memory_order_relaxed);
        }
        // `release`: publica las muestras ANTES que el contador que las anuncia.
        mWritten.store(w + n, std::memory_order_release);
    }

    /**
     * @brief Drena hasta `maxFrames` al thread de analisis.
     * @return cuantos frames se entregaron (0 si no habia nada nuevo).
     *
     * Si el lector se atraso mas que la capacidad, se salta a lo mas nuevo que
     * sigue intacto y cuenta la perdida. Nunca entrega datos desgarrados: si el
     * escritor paso por encima durante la copia, la descarta.
     */
    int read(float* out, int maxFrames) noexcept;

    /// Frames que el analisis nunca vio porque el escritor los piso.
    uint64_t droppedFrames() const noexcept { return mDropped.get(); }

    /// Bloques que llegaron mas grandes que el ring entero y se recortaron a su
    /// cola. No es un error —es una configuracion rara— pero si dispara, el
    /// thread de captura esta entregando mas de 170 ms de audio por callback y
    /// eso hay que saberlo.
    uint64_t oversizedBlocks() const noexcept { return mOversizedBlocks.get(); }

    /// Veces que una copia se descarto por desgarro. Es un subconjunto del
    /// motivo de `droppedFrames`, contado aparte porque distingue "me atrase"
    /// de "me atrase JUSTO mientras copiaba".
    uint64_t tornReads() const noexcept { return mTorn.get(); }

    /// Frames disponibles ahora mismo, acotado a la capacidad. Informativo.
    uint32_t availableFrames() const noexcept {
        const uint64_t w = mWritten.load(std::memory_order_acquire);
        const uint64_t r = mRead.load(std::memory_order_relaxed);
        const uint64_t avail = w - r;
        return avail > kCapacityFrames ? kCapacityFrames
                                       : static_cast<uint32_t>(avail);
    }

    /// Vacia el ring. NO es RT: se llama desde el thread de control.
    void reset() noexcept {
        mWritten.store(0, std::memory_order_relaxed);
        mRead.store(0, std::memory_order_relaxed);
        for (uint32_t i = 0; i < kCapacityFrames; ++i) {
            mBuffer[i].store(0.0f, std::memory_order_relaxed);
        }
    }

private:
    /**
     * `atomic<float>` RELAJADO, no `float` pelado, y no es ceremonia.
     *
     * Un ring que sobreescribe es un seqlock: el lector copia y RECIEN DESPUES
     * valida. Con `float` pelado eso es una data race formal —escritura y
     * lectura planas del mismo objeto, sin orden entre ellas— y por lo tanto
     * UB, que el compilador puede explotar vectorizando o recargando. Que el
     * algoritmo descarte la copia desgarrada no lo salva: el estandar no dice
     * "lee basura", dice que el programa no tiene significado.
     *
     * TSan lo reporto como carrera de verdad, no como falso positivo, y tenia
     * razon: `writeStereo` escribia y `read` leia el mismo `float`.
     *
     * Relajado alcanza. No se necesita orden entre las muestras entre si — el
     * orden que importa lo da `mWritten` con release/acquire. En ARM64 un
     * load/store relajado de 4 bytes compila al mismo `ldr`/`str` que el acceso
     * plano: cuesta cero instrucciones y compra correctitud formal.
     */
    std::unique_ptr<std::atomic<float>[]> mBuffer;

    /// La escribe SOLO el thread de captura.
    std::atomic<uint64_t> mWritten{0};
    /// La escribe SOLO el thread de analisis.
    std::atomic<uint64_t> mRead{0};

    wma::RtCounter mDropped;
    wma::RtCounter mTorn;
    wma::RtCounter mOversizedBlocks;
};

}  // namespace wma::analysis
