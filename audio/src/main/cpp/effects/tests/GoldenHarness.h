#ifndef WMA_GOLDEN_HARNESS_H
#define WMA_GOLDEN_HARNESS_H

/**
 * WD-2.2 — el andamio de la suite golden de DSP.
 *
 * QUE PROBLEMA RESUELVE
 * ---------------------
 * Los tests de efectos que habia afirmaban que la salida es finita, que la
 * energia es mayor que cero y que L difiere de R. Ninguno puede detectar que el
 * cutoff de un filtro este una octava corrido, que la Q este invertida, o que un
 * shelf de +9 dB entregue +6. Este header da las tres herramientas que faltaban:
 *
 *   1. CAPTURA de la respuesta al impulso de un `Effect`, por bloques.
 *   2. MEDICION de |H(f)| a partir de esa IR, por DFT de un solo bin en double.
 *   3. GOLDEN: comparar contra un archivo commiteado, con un diff legible.
 *
 * POR QUE LA DFT DE LA IR Y NO UN SWEEP
 * -------------------------------------
 * Para un sistema LTI la IR lo caracteriza por completo: H(e^jw) = suma de
 * h[n]e^(-jwn). Medirlo asi es EXACTO salvo por la cola que se trunca, no tiene
 * ruido de estimacion, y no hay que resolver el problema de la deconvolucion de
 * un sweep. La unica condicion es que la IR haya decaido dentro de la ventana
 * capturada — con kResponseFrames (8192 @ 48 kHz = 170 ms) sobra para cualquier
 * biquad: la constante de tiempo de un Q=10 a 1 kHz son 153 muestras.
 *
 * Se mide en `double` A PROPOSITO. La aritmetica del efecto es float —eso es lo
 * que se esta midiendo— pero el instrumento no tiene por que agregar su propio
 * error al numero.
 *
 * NO SE USA `BiquadFilter::getFrequencyResponse()` COMO REFERENCIA
 * ---------------------------------------------------------------
 * Existe y calcula |H(f)| desde los coeficientes. Compararse contra eso seria
 * auto-referencial: un error en el calculo de coeficientes aparece identico en
 * los dos lados y el test queda verde. Todo lo que hay aca sale del AUDIO que
 * `process()` realmente produjo.
 *
 * LAS DOS REDES GOLDEN, Y POR QUE HACEN FALTA LAS DOS
 * ---------------------------------------------------
 * - `.f32` — la IR muestra por muestra. Es la red fina: cualquier cambio en el
 *   sonido se ve. Es tambien la fragil: un IIR resonante AMPLIFICA una
 *   diferencia de 1 ULP en los coeficientes a lo largo del decay, y `sinf`/`cosf`
 *   no dan bit a bit lo mismo en glibc que en libc++. Por eso los presets
 *   commiteados se quedan en Q baja y la tolerancia es holgada.
 * - `.resp` — |H(f)| en dB en 31 frecuencias log. Es la red robusta: la
 *   magnitud es una funcion SUAVE de los coeficientes, no acumula error, asi
 *   que tolera 0,02 dB entre plataformas y aun asi cacha cualquier cambio real
 *   de DSP (que mueve decimas o unidades de dB). Y es texto: el diff de una
 *   recaptura se lee en el PR.
 *
 * REGENERAR
 * ---------
 * `WMA_GOLDEN_REGEN=1` reescribe los archivos en vez de comparar — y el test se
 * marca SKIPPED, no PASSED. Que una corrida de regeneracion no pueda pasar por
 * una corrida de verificacion no es cosmetico: es la misma regla que
 * `.github/local-gate.json`, donde la atestacion la escribe quien corrio el
 * trabajo y nadie mas.
 *
 * Use `bash scripts/regen-golden.sh`, que ademas deja el diff a la vista.
 */

#include "../Effect.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace wma::golden {

/// Frecuencia de muestreo de referencia de la suite. WD-2.3 barre 44,1/48/96.
inline constexpr int kSampleRate = 48000;

/// Ventana de captura para MEDIR respuesta. 170 ms @ 48 kHz: cualquier biquad
/// de esta libreria ya decayo a ruido numerico mucho antes.
inline constexpr int kResponseFrames = 8192;

/// Ventana de la IR que se COMMITEA. Mas corta a proposito: es la parte con
/// energia audible, y menos muestras es menos superficie de deriva entre libms.
inline constexpr int kGoldenFrames = 2048;

inline constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Señales
// ---------------------------------------------------------------------------

/// Impulso unitario en el frame 0, estereo interleaved.
inline std::vector<float> impulseStereo(int frames) {
    std::vector<float> b(static_cast<size_t>(frames) * 2, 0.0f);
    b[0] = 1.0f;
    b[1] = 1.0f;
    return b;
}

// ---------------------------------------------------------------------------
// Captura
// ---------------------------------------------------------------------------

/**
 * Pasa un impulso por el efecto y devuelve el canal IZQUIERDO de la salida.
 *
 * Procesa en bloques de `blockFrames` para que el troceado sea parte de lo que
 * se mide: un efecto cuyo estado dependa del tamaño de bloque da distinto aca
 * segun como se lo llame, y eso es exactamente lo que hay que poder ver.
 */
inline std::vector<float> captureImpulseResponse(Effect& fx, int frames, int blockFrames) {
    std::vector<float> in = impulseStereo(frames);
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);

    for (int start = 0; start < frames; start += blockFrames) {
        const int n = std::min(blockFrames, frames - start);
        fx.process(in.data() + static_cast<size_t>(start) * 2,
                   out.data() + static_cast<size_t>(start) * 2,
                   n);
    }

    std::vector<float> left(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        left[static_cast<size_t>(i)] = out[static_cast<size_t>(i) * 2];
    }
    return left;
}

/// Atajo con los valores por defecto de la suite: bloques de 512.
inline std::vector<float> captureImpulseResponse(Effect& fx, int frames = kResponseFrames) {
    return captureImpulseResponse(fx, frames, 512);
}

// ---------------------------------------------------------------------------
// Medición
// ---------------------------------------------------------------------------

/// |H(f)| lineal, por DFT de un solo bin sobre la IR. Vale para f = 0 y para
/// f = Nyquist sin ningun caso especial.
inline double magnitudeAt(const std::vector<float>& ir, double freqHz, double sampleRate) {
    const double w = 2.0 * kPi * freqHz / sampleRate;
    double re = 0.0;
    double im = 0.0;
    for (size_t n = 0; n < ir.size(); ++n) {
        const double phase = w * static_cast<double>(n);
        const double h = static_cast<double>(ir[n]);
        re += h * std::cos(phase);
        im -= h * std::sin(phase);
    }
    return std::sqrt(re * re + im * im);
}

/// dB con piso, para que un cero exacto (HPF en DC) no devuelva -inf.
inline double toDb(double linear) {
    constexpr double kFloor = 1e-12;
    return 20.0 * std::log10(linear < kFloor ? kFloor : linear);
}

inline double responseDbAt(const std::vector<float>& ir, double freqHz,
                           double sampleRate = kSampleRate) {
    return toDb(magnitudeAt(ir, freqHz, sampleRate));
}

/// 31 frecuencias log entre 20 Hz y 20 kHz — la reticula de los `.resp`.
inline std::vector<double> responseGrid() {
    constexpr int kPoints = 31;
    std::vector<double> f;
    f.reserve(kPoints);
    const double lo = std::log10(20.0);
    const double hi = std::log10(20000.0);
    for (int i = 0; i < kPoints; ++i) {
        f.push_back(std::pow(10.0, lo + (hi - lo) * i / (kPoints - 1)));
    }
    return f;
}

/// La curva completa en dB sobre esa reticula.
inline std::vector<double> responseCurve(const std::vector<float>& ir,
                                         double sampleRate = kSampleRate) {
    std::vector<double> db;
    for (double f : responseGrid()) {
        db.push_back(responseDbAt(ir, f, sampleRate));
    }
    return db;
}

// ---------------------------------------------------------------------------
// Golden
// ---------------------------------------------------------------------------

inline bool regenRequested() {
    const char* v = std::getenv("WMA_GOLDEN_REGEN");
    return v != nullptr && v[0] != '\0' && std::string(v) != "0";
}

inline std::string goldenPath(const std::string& name, const char* ext) {
    return std::string(WMA_GOLDEN_DIR) + "/" + name + ext;
}

/// Falla el test con un mensaje que dice DONDE regenerar, en vez de dejar al
/// lector adivinando que un archivo no existe.
inline void failMissing(const std::string& path) {
    FAIL() << "No existe el golden:\n  " << path
           << "\n\nSi es un preset nuevo, generalo con:\n"
              "  bash scripts/regen-golden.sh\n"
              "Si NO es nuevo, alguien lo borro — eso es la falla, no el test.";
}

// --- IR muestra por muestra (.f32, binario) --------------------------------

inline bool writeF32(const std::string& path, const std::vector<float>& data) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const size_t n = std::fwrite(data.data(), sizeof(float), data.size(), f);
    std::fclose(f);
    return n == data.size();
}

inline bool readF32(const std::string& path, std::vector<float>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    std::fseek(f, 0, SEEK_END);
    const long bytes = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (bytes < 0 || bytes % static_cast<long>(sizeof(float)) != 0) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(bytes) / sizeof(float));
    const size_t n = std::fread(out.data(), sizeof(float), out.size(), f);
    std::fclose(f);
    return n == out.size();
}

/**
 * Compara una IR contra el golden commiteado.
 *
 * El mensaje de falla lleva el error maximo, el RMS y la PRIMERA muestra que
 * divergio — que es el dato que separa "cambio el sonido entero" de "cambio el
 * transitorio de arranque". Esa distincion fue justamente la que resolvio el
 * defecto que encontro WD-2.1.
 */
inline void checkGoldenSamples(const std::string& name,
                               const std::vector<float>& data,
                               double tolerance) {
    const std::string path = goldenPath(name, ".f32");

    if (regenRequested()) {
        ASSERT_TRUE(writeF32(path, data)) << "No pude escribir " << path;
        GTEST_SKIP() << "REGENERADO (no verificado): " << path;
    }

    std::vector<float> golden;
    if (!readF32(path, golden)) {
        failMissing(path);
        return;
    }

    ASSERT_EQ(golden.size(), data.size())
        << "El golden " << name << " tiene " << golden.size()
        << " muestras y la captura " << data.size()
        << ". Cambio el largo de la ventana, no el DSP — revisa kGoldenFrames.";

    double maxAbs = 0.0;
    double sumSq = 0.0;
    long firstBad = -1;
    for (size_t i = 0; i < data.size(); ++i) {
        const double d = static_cast<double>(data[i]) - static_cast<double>(golden[i]);
        sumSq += d * d;
        if (std::abs(d) > maxAbs) maxAbs = std::abs(d);
        if (firstBad < 0 && std::abs(d) > tolerance) firstBad = static_cast<long>(i);
    }
    const double rms = std::sqrt(sumSq / static_cast<double>(data.size()));

    EXPECT_LE(maxAbs, tolerance)
        << "La IR de " << name << " ya no coincide con su golden.\n"
        << "  error maximo : " << maxAbs << "  (tolerancia " << tolerance << ")\n"
        << "  error RMS    : " << rms << "\n"
        << "  primera muestra fuera de tolerancia: " << firstBad << "\n"
        << "\nSi el cambio es INTENCIONAL, recapturar es una tarea explicita:\n"
           "  bash scripts/regen-golden.sh\n"
           "y el diff va revisado en el PR.";
}

// --- Respuesta en frecuencia (.resp, texto) --------------------------------

inline bool writeResp(const std::string& path, const std::string& name,
                      const std::vector<double>& db) {
    const std::vector<double> freqs = responseGrid();
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    std::fprintf(f, "# watermelon-audio golden response (WD-2.2) — %s\n", name.c_str());
    std::fprintf(f, "# sampleRate=%d  points=%zu\n", kSampleRate, db.size());
    std::fprintf(f, "# freqHz\tmagnitudeDb\n");
    for (size_t i = 0; i < db.size(); ++i) {
        std::fprintf(f, "%.4f\t%.4f\n", freqs[i], db[i]);
    }
    std::fclose(f);
    return true;
}

inline bool readResp(const std::string& path, std::vector<double>& freqs,
                     std::vector<double>& db) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    char line[256];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (line[0] == '#' || line[0] == '\n') continue;
        double hz = 0.0;
        double mag = 0.0;
        if (std::sscanf(line, "%lf %lf", &hz, &mag) == 2) {
            freqs.push_back(hz);
            db.push_back(mag);
        }
    }
    std::fclose(f);
    return true;
}

/**
 * Compara la curva de magnitud contra el golden de texto.
 *
 * Esta es la red que sobrevive al cambio de plataforma: |H(f)| no acumula el
 * error que un IIR si acumula muestra a muestra.
 */
inline void checkGoldenResponse(const std::string& name,
                                const std::vector<double>& db,
                                double toleranceDb) {
    const std::string path = goldenPath(name, ".resp");

    if (regenRequested()) {
        ASSERT_TRUE(writeResp(path, name, db)) << "No pude escribir " << path;
        GTEST_SKIP() << "REGENERADO (no verificado): " << path;
    }

    std::vector<double> gFreq;
    std::vector<double> gDb;
    if (!readResp(path, gFreq, gDb)) {
        failMissing(path);
        return;
    }

    ASSERT_EQ(gDb.size(), db.size())
        << "El golden de respuesta " << name << " tiene otra cantidad de puntos.";

    const std::vector<double> freqs = responseGrid();
    double worst = 0.0;
    size_t worstIdx = 0;
    for (size_t i = 0; i < db.size(); ++i) {
        const double d = std::abs(db[i] - gDb[i]);
        if (d > worst) {
            worst = d;
            worstIdx = i;
        }
    }

    EXPECT_LE(worst, toleranceDb)
        << "La respuesta en frecuencia de " << name << " se movio.\n"
        << "  peor punto : " << freqs[worstIdx] << " Hz\n"
        << "  golden     : " << gDb[worstIdx] << " dB\n"
        << "  medido     : " << db[worstIdx] << " dB\n"
        << "  delta      : " << worst << " dB (tolerancia " << toleranceDb << ")\n"
        << "\nSi el cambio es INTENCIONAL: bash scripts/regen-golden.sh";
}

}  // namespace wma::golden

#endif  // WMA_GOLDEN_HARNESS_H
