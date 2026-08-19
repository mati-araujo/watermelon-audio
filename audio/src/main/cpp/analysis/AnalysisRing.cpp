#include "AnalysisRing.h"

#if defined(WMA_TEST_HOOKS)
#include <thread>

// ===========================================================================
// COMPUERTA DE TEST (WMA_TEST_HOOKS) — la define solo analysis/tests.
//
// En el binario que shippea `WMA_TEST_HOOKS` no esta definido y esto compila a
// nada: ni una variable, ni una rama.
//
// POR QUE HACE FALTA UNA COMPUERTA Y NO ALCANZAN ITERACIONES
// ----------------------------------------------------------
// La carrera que este ring tiene que sobrevivir es que el escritor pase por
// encima de la region que el lector esta copiando. Esa ventana dura lo que dura
// copiar unos miles de floats: microsegundos. Este repo ya midio que bombear a
// ciegas no la pega — 40 retiros por corrida y 15 corridas, y el codigo
// BUGGEADO paso siempre.
//
// La compuerta detiene al lector JUSTO despues de copiar y ANTES de re-chequear,
// que es exactamente el estado donde el defecto vive. Con eso el test es
// determinista en vez de probabilistico.
// ===========================================================================
std::atomic<bool> gAnalysisRingHoldAfterCopy{false};
std::atomic<bool> gAnalysisRingIsInCopy{false};
#endif

namespace wma::analysis {

int AnalysisRing::read(float* out, int maxFrames) noexcept {
    if (out == nullptr || maxFrames <= 0) return 0;

    uint64_t r = mRead.load(std::memory_order_relaxed);
    const uint64_t w = mWritten.load(std::memory_order_acquire);
    uint64_t avail = w - r;

    // Nos atrasamos mas que la capacidad: lo mas viejo ya no existe. Saltamos a
    // lo mas nuevo que sigue intacto y contamos EXACTAMENTE lo que se perdio.
    if (avail > kCapacityFrames) {
        mDropped.bump(avail - kCapacityFrames);
        r = w - kCapacityFrames;
        avail = kCapacityFrames;
    }
    if (avail == 0) {
        mRead.store(r, std::memory_order_relaxed);
        return 0;
    }

    const uint64_t want = static_cast<uint64_t>(maxFrames);
    const uint32_t n = static_cast<uint32_t>(avail < want ? avail : want);

    for (uint32_t i = 0; i < n; ++i) {
        out[i] = mBuffer[static_cast<uint32_t>(r + i) & kMask].load(
            std::memory_order_relaxed);
    }

#if defined(WMA_TEST_HOOKS)
    if (gAnalysisRingHoldAfterCopy.load(std::memory_order_acquire)) {
        gAnalysisRingIsInCopy.store(true, std::memory_order_release);
        while (gAnalysisRingHoldAfterCopy.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        gAnalysisRingIsInCopy.store(false, std::memory_order_release);
    }
#endif

    // El re-chequeo. Si el escritor avanzo lo suficiente como para haber tocado
    // lo que acabamos de copiar, esa copia esta DESGARRADA y no se entrega: se
    // descarta, se cuenta, y el llamador vuelve a pedir.
    const uint64_t w2 = mWritten.load(std::memory_order_acquire);
    if (w2 - r > kCapacityFrames) {
        mTorn.bump();
        mDropped.bump((w2 - r) - kCapacityFrames);
        mRead.store(w2 - kCapacityFrames, std::memory_order_relaxed);
        return 0;
    }

    mRead.store(r + n, std::memory_order_relaxed);
    return static_cast<int>(n);
}

}  // namespace wma::analysis
