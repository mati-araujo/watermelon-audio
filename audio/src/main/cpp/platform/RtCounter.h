#pragma once

/**
 * @file RtCounter.h
 * @brief Contador de diagnostico incrementable desde el thread de audio (WD-1.1).
 *
 * POR QUE EXISTE
 * --------------
 * El motor tenia una decena de logs adentro del callback: unos periodicos
 * ("cada 300 callbacks, contame el peak de entrada") y otros de condicion de
 * error ("NaN detectado", "monitor overflow", "buffer overflow"). Los dos tipos
 * violan la regla de RT por la misma razon —`wma::logMessage` formatea y hace
 * un syscall— y los dos existian por el mismo motivo legitimo: alguien
 * necesitaba saber que estaba pasando adentro del callback.
 *
 * Borrar el log sin reemplazarlo tira esa informacion. Un contador la conserva
 * al costo de un `fetch_add` relajado, que en ARM64 es una instruccion.
 *
 * POR QUE NO ES UN GLOBAL
 * -----------------------
 * Cada contador es MIEMBRO del objeto cuyo evento cuenta. Es mas verboso que un
 * bloque global de estadisticas, y es a proposito: los `static` de funcion que
 * habia en el callback son globales de proceso, asi que dos instancias del
 * motor se pisaban los contadores y el comportamiento periodico dependia de
 * cuantos motores existieran (WD-1.5). Un contador que vive en su objeto no
 * tiene ese problema y ademas dice, por su ubicacion, que mide.
 *
 * WD-5.1 los agrega en un `WmaDiagnostics` y los expone por la C API. Hasta
 * entonces se leen desde un test o desde un debugger, que es exactamente lo que
 * el log periodico daba, sin el syscall.
 *
 * CONTRATO
 * --------
 * `bump()` es RT-safe: un `fetch_add` con `memory_order_relaxed`, sin barrera,
 * sin allocation, sin bloqueo. `get()` y `clear()` son para el thread de
 * control. `relaxed` alcanza porque un contador de diagnostico no ordena nada:
 * no hay dato que publicar detras de el.
 */

#include <atomic>
#include <cstdint>

namespace wma {

class RtCounter {
public:
    RtCounter() = default;

    RtCounter(const RtCounter&) = delete;
    RtCounter& operator=(const RtCounter&) = delete;

    /// Incrementa. Llamable desde el thread de audio.
    void bump() noexcept { mValue.fetch_add(1, std::memory_order_relaxed); }

    /// Incrementa en `n`. Llamable desde el thread de audio.
    void bump(uint64_t n) noexcept { mValue.fetch_add(n, std::memory_order_relaxed); }

    /// Lee. Pensado para el thread de control.
    uint64_t get() const noexcept { return mValue.load(std::memory_order_relaxed); }

    /// Pone en cero. Pensado para el thread de control.
    void clear() noexcept { mValue.store(0, std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> mValue{0};
};

}  // namespace wma
