#pragma once

/**
 * @file MasterStage.h
 * @brief Master de Studio (REQ-055 S12, ADR-0013): ganancia + limitador brickwall true-peak.
 *
 * Cadena: ganancia (dB, rampa lineal por BLOQUE al cambiar) -> limitador -> nada mas (sin soft
 * clip ni dither: el dither es del escritor de 16 bits). NO reemplaza a `LookaheadLimiter`
 * (que el movil sigue usando): aquel mide pico de MUESTRA y no garantiza su techo.
 *
 * Limitador, todo a nivel de muestra y por lo tanto invariante al tamaño de bloque:
 *  - DETECCION true-peak: sobremuestreo 4x con el FIR polifasico de ITU-R BS.1770-4 Anexo 2
 *    (48 taps, 12 por fase), enlazado entre canales. El registro del instante j describe las
 *    posiciones [j-6, j-5): la muestra x[j-6] y sus tres fases intermedias.
 *  - LOOKAHEAD fijo de 5 ms (D frames): la salida es la entrada demorada D frames por la ganancia.
 *  - GANANCIA REQUERIDA r_j = techo / pico_j (o 1), MINIMO sobre una ventana de D+3 registros
 *    (deque monotona de capacidad fija), limitada en subida por una RELEASE exponencial
 *    (env = min(m, env + (1-env)(1-c))) y suavizada con una media movil de N = D-12 muestras.
 *    Garantia: la media de N valores cada uno <= r_j cubre, para toda muestra que el pico j
 *    puede tocar (incluido el rizado del FIR, +-7 muestras), una ganancia <= r_j: la ganancia
 *    BAJA antes de que el pico salga de la linea de retardo, sin saltos.
 *  - Guarda final: clamp al techo. No es el limitador (`clampHits()` cuenta cuando actua).
 * Por debajo del umbral la ganancia es EXACTAMENTE 1: la salida es la entrada demorada, bit a bit
 * (la media movil es entera en Q24: no hay residuo de redondeo).
 *
 * RT: todo el estado se dimensiona en `prepareMaster()` para el sample rate maximo; los demas
 * metodos no alocan, no bloquean ni loguean. `configureMaster()` son stores atomicos (cualquier
 * hilo); `processMaster()` los lee UNA vez por bloque. `resetMaster()` y `prepareMaster()` no son
 * concurrentes con `processMaster()` (el llamador los serializa: RT quiesced o sin RT).
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace wm {

struct MasterParams {
    float gainDb = 0.0f;
    float ceilingDbtp = -1.0f;
    float releaseMs = 150.0f;
};

class MasterStage {
public:
    static constexpr int kMaxSampleRate = 192000;
    static constexpr int kMinSampleRate = 8000;
    static constexpr float kLookaheadSeconds = 0.005f;

    MasterStage() = default;
    MasterStage(const MasterStage&) = delete;
    MasterStage& operator=(const MasterStage&) = delete;

    /// Dimensiona el estado (la PRIMERA vez aloca; despues solo recalcula) y resetea.
    void prepareMaster(int sampleRate) {
        if (mRing.empty()) {
            mRing.assign(static_cast<size_t>(kRingSize) * 2, 0.0f);
            mDqIdx.assign(kDqSize, 0);
            mDqVal.assign(kDqSize, 1.0f);
            mEnvRing.assign(kRingSize, kUnityQ);
        }
        mSampleRate = std::clamp(sampleRate, kMinSampleRate, kMaxSampleRate);
        const int d = static_cast<int>(std::lround(kLookaheadSeconds * static_cast<float>(mSampleRate)));
        mLookahead = std::clamp(d, 32, kMaxLookahead);
        mWindow = mLookahead + 3;
        mBox = mLookahead - 12;
        mBoxFullQ = static_cast<int64_t>(mBox) * kUnityQ;
        mBoxInv = 1.0 / static_cast<double>(mBoxFullQ);
        mCachedRelease = -1.0f;
        mCachedCeiling = 1.0e9f;
        resetMaster();
    }

    void configureMaster(const MasterParams& p) {
        mGainDb.store(p.gainDb, std::memory_order_relaxed);
        mCeilingDb.store(p.ceilingDbtp, std::memory_order_relaxed);
        mReleaseMs.store(p.releaseMs, std::memory_order_relaxed);
    }

    MasterParams currentMasterParams() const {
        return {mGainDb.load(std::memory_order_relaxed), mCeilingDb.load(std::memory_order_relaxed),
                mReleaseMs.load(std::memory_order_relaxed)};
    }

    /// Estado de una instancia fresca. La ganancia vigente SALTA al objetivo (sin rampa).
    void resetMaster() {
        if (mRing.empty()) return;
        std::fill(mRing.begin(), mRing.end(), 0.0f);
        std::fill(mEnvRing.begin(), mEnvRing.end(), kUnityQ);
        mWrite = 0;
        mDqHead = 0;
        mDqTail = 0;
        mTime = 0;
        mEnv = 1.0;
        mBoxSumQ = mBoxFullQ;
        mGainCur = static_cast<float>(std::pow(10.0, static_cast<double>(
                       mGainDb.load(std::memory_order_relaxed)) / 20.0));
        mClampHits = 0;
    }

    /// Latencia declarada: frames que la salida llega despues de la entrada.
    int latencyFrames() const { return mRing.empty() ? 0 : mLookahead; }

    /// Veces que la guarda final recorto una muestra (en operacion normal: 0).
    uint64_t clampHits() const { return mClampHits; }

    /// Procesa `frames` frames estereo intercalados, en el lugar.
    void processMaster(float* inout, int frames) {
        if (mRing.empty() || inout == nullptr || frames <= 0) return;
        refreshBlockParams();
        const float g0 = mGainCur;
        const float g1 = mGainTarget;
        const float inv = 1.0f / static_cast<float>(frames);
        const float ceil = mCeilLin;
        const float safeCeil = ceil * kCeilMargin;
        const float rel = mRelCoef;
        const unsigned mask = kRingSize - 1;
        const unsigned dqMask = kDqSize - 1;

        for (int i = 0; i < frames; ++i) {
            // ---- ganancia (rampa lineal por bloque; constante = exacta) ----
            const float gain = g0 == g1 ? g0 : g0 + (g1 - g0) * (static_cast<float>(i + 1) * inv);
            float l = inout[i * 2];
            float r = inout[i * 2 + 1];
            if (gain != 1.0f) {
                l *= gain;
                r *= gain;
            }
            if (!std::isfinite(l)) l = 0.0f;
            if (!std::isfinite(r)) r = 0.0f;

            // ---- linea de retardo ----
            const unsigned w = mWrite;
            mRing[static_cast<size_t>(w & mask) * 2] = l;
            mRing[static_cast<size_t>(w & mask) * 2 + 1] = r;

            // ---- deteccion true-peak del registro j = mTime ----
            const float peak = detectTruePeak(w, mask) * kDetectHeadroom;
            const float req = peak > ceil ? safeCeil / peak : 1.0f;

            // ---- minimo sobre la ventana (deque monotona de capacidad fija) ----
            while (mDqTail != mDqHead && mDqVal[(mDqTail - 1) & dqMask] >= req) --mDqTail;
            mDqIdx[mDqTail & dqMask] = mTime;
            mDqVal[mDqTail & dqMask] = req;
            ++mDqTail;
            while (mDqIdx[mDqHead & dqMask] + static_cast<int64_t>(mWindow) <= mTime) ++mDqHead;
            const float m = mDqVal[mDqHead & dqMask];

            // ---- release exponencial y media movil entera (Q24) ----
            double env = mEnv + (1.0 - mEnv) * (1.0 - static_cast<double>(rel));
            if (1.0 - env < kSnapUnity) env = 1.0;
            if (static_cast<double>(m) < env) env = static_cast<double>(m);
            mEnv = env;
            const int32_t q = static_cast<int32_t>(std::floor(env * static_cast<double>(kUnityQ)));
            const unsigned slot = w & mask;
            const int32_t leaving = mEnvRing[(slot - static_cast<unsigned>(mBox)) & mask];
            mEnvRing[slot] = q;
            mBoxSumQ += static_cast<int64_t>(q) - static_cast<int64_t>(leaving);
            const float g = mBoxSumQ == mBoxFullQ
                                ? 1.0f
                                : static_cast<float>(static_cast<double>(mBoxSumQ) * mBoxInv);

            // ---- salida: entrada demorada x ganancia, con la guarda al techo ----
            const unsigned dIdx = (w - static_cast<unsigned>(mLookahead)) & mask;
            float outL = mRing[static_cast<size_t>(dIdx) * 2];
            float outR = mRing[static_cast<size_t>(dIdx) * 2 + 1];
            if (g != 1.0f) {
                outL *= g;
                outR *= g;
            }
            if (outL > ceil) { outL = ceil; ++mClampHits; }
            else if (outL < -ceil) { outL = -ceil; ++mClampHits; }
            if (outR > ceil) { outR = ceil; ++mClampHits; }
            else if (outR < -ceil) { outR = -ceil; ++mClampHits; }
            inout[i * 2] = outL;
            inout[i * 2 + 1] = outR;

            mWrite = w + 1;
            ++mTime;
        }
        mGainCur = g1;
    }

private:
    static constexpr unsigned kRingSize = 2048;   // potencia de 2 >= lookahead maximo + FIR
    static constexpr unsigned kDqSize = 2048;
    static constexpr int kMaxLookahead = 960;     // 5 ms a 192 kHz
    static constexpr int32_t kUnityQ = 1 << 24;
    static constexpr float kCeilMargin = 1.0f - 1.0e-6f;
    static constexpr double kSnapUnity = 1.0e-7;
    // +0,1 dB sobre lo medido: el FIR de 4x subestima el pico de la reconstruccion ideal en
    // contenido agudo (medido: hasta ~0,07 dB con ruido limitado a 0,2 fs, 0,4 dB a 0,45 fs).
    static constexpr float kDetectHeadroom = 1.01158f;

    // BS.1770-4 Anexo 2, tabla 1: 4 fases x 12 taps. Fase 2 = fase 1 invertida; fase 3 = fase 0 invertida.
    static constexpr float kPhase0[12] = {
        0.0017089843750f, 0.0109863281250f, -0.0196533203125f, 0.0332031250000f,
        -0.0594482421875f, 0.1373291015625f, 0.9721679687500f, -0.1022949218750f,
        0.0476074218750f, -0.0266113281250f, 0.0148925781250f, -0.0083007812500f};
    static constexpr float kPhase1[12] = {
        -0.0291748046875f, 0.0292968750000f, -0.0517578125000f, 0.0891113281250f,
        -0.1665039062500f, 0.4650878906250f, 0.7797851562500f, -0.2003173828125f,
        0.1015625000000f, -0.0582275390625f, 0.0330810546875f, -0.0189208984375f};

public:
    /// Fase `phase` (0..3), tap `i` (0..11) del FIR de sobremuestreo. Publico para el test de la tabla.
    static constexpr float firTap(int phase, int i) {
        switch (phase) {
            case 0: return kPhase0[i];
            case 1: return kPhase1[i];
            case 2: return kPhase1[11 - i];
            default: return kPhase0[11 - i];
        }
    }

private:
    /// Pico (enlazado L/R) del registro del instante mTime: x[j-6] y las fases 0..3 del FIR.
    float detectTruePeak(unsigned w, unsigned mask) const {
        float best = 0.0f;
        for (int ch = 0; ch < 2; ++ch) {
            float x[12];   // x[i] = muestra j-i
            for (int i = 0; i < 12; ++i) {
                x[i] = mRing[static_cast<size_t>((w - static_cast<unsigned>(i)) & mask) * 2 +
                             static_cast<size_t>(ch)];
            }
            best = std::max(best, std::fabs(x[6]));
            for (int p = 0; p < 4; ++p) {
                float acc = 0.0f;
                for (int i = 0; i < 12; ++i) acc += firTap(p, i) * x[i];
                best = std::max(best, std::fabs(acc));
            }
        }
        return best;
    }

    void refreshBlockParams() {
        mGainTarget = static_cast<float>(std::pow(10.0, static_cast<double>(
                          mGainDb.load(std::memory_order_relaxed)) / 20.0));
        const float c = mCeilingDb.load(std::memory_order_relaxed);
        if (c != mCachedCeiling) {
            mCachedCeiling = c;
            mCeilLin = static_cast<float>(std::pow(10.0, static_cast<double>(c) / 20.0));
        }
        const float r = mReleaseMs.load(std::memory_order_relaxed);
        if (r != mCachedRelease) {
            mCachedRelease = r;
            mRelCoef = static_cast<float>(std::exp(
                -1.0 / (static_cast<double>(std::max(r, 0.1f)) * 0.001 * static_cast<double>(mSampleRate))));
        }
    }

    std::atomic<float> mGainDb{0.0f};
    std::atomic<float> mCeilingDb{-1.0f};
    std::atomic<float> mReleaseMs{150.0f};

    int mSampleRate = 48000;
    int mLookahead = 240;
    int mWindow = 243;
    int mBox = 228;
    int64_t mBoxFullQ = 0;
    double mBoxInv = 0.0;

    std::vector<float> mRing;
    std::vector<int64_t> mDqIdx;
    std::vector<float> mDqVal;
    std::vector<int32_t> mEnvRing;
    unsigned mWrite = 0;
    unsigned mDqHead = 0;
    unsigned mDqTail = 0;
    int64_t mTime = 0;
    double mEnv = 1.0;
    int64_t mBoxSumQ = 0;
    float mGainCur = 1.0f;
    float mGainTarget = 1.0f;
    float mCeilLin = 0.891f;
    float mRelCoef = 0.0f;
    float mCachedCeiling = 1.0e9f;
    float mCachedRelease = -1.0f;
    uint64_t mClampHits = 0;
};

}  // namespace wm
