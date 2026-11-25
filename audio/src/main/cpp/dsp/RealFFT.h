#pragma once

/**
 * RealFFT.h — la FFT real del motor (MINI-058). Hay UNA: un analisis o una convolucion nuevos
 * usan esta y no traen otra. Por dentro es pffft (thirdparty/pffft).
 *
 * Tamaños: potencias de 2 de 256 a 16384. Con cualquier otro, `valid()` es false y las
 * transformadas devuelven false sin escribir.
 *
 * Espectro: n floats, {X[0], X[n/2], Re X[1], Im X[1], ..., Re X[n/2-1], Im X[n/2-1]}. X[0] y
 * X[n/2] son reales y comparten el primer par. X[k] = Σ x[i]·e^(-2πi·k·i/n), sin escalar; la
 * inversa divide por n, asi que inversa(directa(x)) = x.
 *
 * El constructor y el destructor alocan y liberan: thread de control. Las transformadas no
 * alocan, no bloquean y no loguean. El scratch es de la instancia: una instancia, un thread.
 */
class RealFFT {
public:
    static constexpr int kMinSize = 256;
    static constexpr int kMaxSize = 16384;

    explicit RealFFT(int n);
    ~RealFFT();

    RealFFT(const RealFFT&) = delete;
    RealFFT& operator=(const RealFFT&) = delete;

    bool valid() const noexcept { return size_ != 0; }

    /// Lee n muestras y escribe n floats de espectro. Los punteros no necesitan alineacion y
    /// pueden ser el mismo.
    bool transformForward(const float* signal, float* spectrum) noexcept;
    /// Lee n floats de espectro y escribe n muestras. Idem.
    bool transformInverse(const float* spectrum, float* signal) noexcept;

private:
    void releaseBuffers() noexcept;

    int size_ = 0;
    void* plan_ = nullptr;  // PFFFT_Setup; el tipo no sale de RealFFT.cpp
    float* in_ = nullptr;
    float* out_ = nullptr;
    float* work_ = nullptr;
};
