#pragma once

#include <atomic>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../dsp/SIMDUtils.h"
#include "../platform/Logger.h"
#include "../platform/RtCounter.h"

// Logging macros local to this header (header-only, no LOG_TAG from .cpp)
#define DTM_LOGI(...) wma::logMessage(wma::LogLevel::INFO, "DualTouchMgr", __VA_ARGS__)
#define DTM_LOGE(...) wma::logMessage(wma::LogLevel::ERROR, "DualTouchMgr", __VA_ARGS__)

/**
 * @brief Modos de mezcla para dual touch.
 *
 * Cada slot (dedo) es UNA voz con su engine y su envolvente (REQ-052): ataque
 * de ~5 ms al apoyar, release exponencial de ~80 ms a −60 dB al levantar —con
 * la ultima frecuencia y amplitud del dedo— y, si el dedo vuelve durante el
 * release, la ganancia sube desde donde estaba. Con UN solo dedo la salida es
 * esa voz a ganancia 1 en todos los modos; con dos, la ley de cada modo; y las
 * transiciones entre esos estados siguen a las envolventes, sin escalon.
 *
 * Desde REQ-052 (2.22.0) **SUM y AVERAGE son la misma ley: suma a ganancia 1
 * por voz**, igual que un dedo solo. Antes los dos multiplicaban la suma por
 * 0,5, asi que una voz bajaba ~6 dB al entrar la otra y subia ~6 dB al salir.
 * El pico de la suma lo cuida la proteccion de salida (LookaheadLimiter,
 * −0,5 dBFS), no una ganancia que dependa de cuantos dedos haya. AVERAGE
 * conserva su nombre (y su valor) por compatibilidad.
 */
enum class DualTouchMixMode {
    SUM = 0,        ///< Suma a ganancia 1 por voz (misma ley que AVERAGE desde REQ-052)
    AVERAGE = 1,    ///< DEFAULT. Suma a ganancia 1 por voz; el nombre es historico
    MAX = 2,        ///< max(|voz1|, |voz2|) con el signo de la suma
    CROSSFADE = 3,  ///< (1 − d)·voz1 + d·voz2, con d = distancia entre dedos en [0, 1]
    RING = 4,       ///< voz1·voz2·0,5 (ring modulation)
    AMPLITUDE_BALANCED = 5  ///< pesos amp_i / (amp1 + amp2), con la amplitud de cada dedo
};

/**
 * @brief Snapshot of all dual-touch state for use in the audio callback.
 *
 * Avoids multiple atomic loads by batch-reading everything in one call.
 */
struct TouchState {
    float x1, y1, freq1, amp1, pressure1;
    float x2, y2, freq2, amp2, pressure2;
    float distance, angle;
    bool active;
    int secondaryOscIndex;
    DualTouchMixMode mixMode;
};

/**
 * @class DualTouchManager
 * @brief Owns all dual-touch atomic parameters, buffers, and mixing logic.
 *
 * Extracted from AudioEngine (Phase 1E) to reduce its member count.
 * Header-only, RT-safe mixing path (no allocations after construction).
 */
class DualTouchManager {
public:
    DualTouchManager() {
        try {
            mTouch1Buffer.resize(8192);  // 4096 frames * 2 channels
            mTouch2Buffer.resize(8192);
            DTM_LOGI("DualTouchManager: buffers allocated successfully");
        } catch (const std::bad_alloc& e) {
            DTM_LOGE("DualTouchManager: failed to allocate buffers: %s", e.what());
        }
    }

    // ========== ENABLED STATE ==========

    void setEnabled(bool enabled) {
        bool wasEnabled = mDualTouchMode.load(std::memory_order_acquire);

        if (enabled && !wasEnabled) {
            // ANTES de publicar `true` (REQ-052): con el modo dual apagado el
            // thread de audio no toca estos buffers ni las envolventes, asi que
            // limpiarlos y pedir envolventes en cero ahora no compite con el
            // primer bloque dual, que ya ve la epoca nueva.
            clearBuffers();
            mDualTouchMode.store(true, std::memory_order_release);
            DTM_LOGI("Dual touch mode ENABLED (UNIFIED: using same oscillator for both touches)");
            return;
        }
        mDualTouchMode.store(enabled, std::memory_order_release);

        if (!enabled && wasEnabled) {
            DTM_LOGI("Dual touch mode DISABLED");
            // Clear stale dual-touch state to prevent glitches when
            // returning to single-touch mode.
            mTouch1Amp.store(0.0f, std::memory_order_release);
            mTouch2Amp.store(0.0f, std::memory_order_release);
            mTouch1Freq.store(0.0f, std::memory_order_release);
            mTouch2Freq.store(0.0f, std::memory_order_release);
        }
    }

    bool isEnabled() const {
        return mDualTouchMode.load(std::memory_order_acquire);
    }

    // ========== PARAMETER UPDATE ==========

    void update(float x1, float y1, float freq1, float amp1, float pressure1,
                float x2, float y2, float freq2, float amp2, float pressure2,
                float distance, float angle) {
        mTouch1X.store(x1, std::memory_order_release);
        mTouch1Y.store(y1, std::memory_order_release);
        mTouch1Freq.store(freq1, std::memory_order_release);
        mTouch1Amp.store(amp1, std::memory_order_release);
        mTouch1Pressure.store(pressure1, std::memory_order_release);

        mTouch2X.store(x2, std::memory_order_release);
        mTouch2Y.store(y2, std::memory_order_release);
        mTouch2Freq.store(freq2, std::memory_order_release);
        mTouch2Amp.store(amp2, std::memory_order_release);
        mTouch2Pressure.store(pressure2, std::memory_order_release);

        mTouchDistance.store(distance, std::memory_order_release);
        mTouchAngle.store(angle, std::memory_order_release);
    }

    // ========== MIX MODE ==========

    void setMixMode(DualTouchMixMode mode) {
        mDualTouchMixMode.store(mode, std::memory_order_release);
        DTM_LOGI("Dual touch mix mode set to: %d", static_cast<int>(mode));
    }

    DualTouchMixMode getMixMode() const {
        return mDualTouchMixMode.load(std::memory_order_acquire);
    }

    // ========== SECONDARY OSCILLATOR ==========

    void setSecondaryOscillatorType(int typeId) {
        mSecondaryOscillatorIndex.store(typeId, std::memory_order_release);
        DTM_LOGI("Secondary oscillator type set to: %d", typeId);
    }

    int getSecondaryOscillatorType() const {
        return mSecondaryOscillatorIndex.load(std::memory_order_acquire);
    }

    // ========== SNAPSHOT (batch atomic read) ==========

    /**
     * @brief Read all touch parameters in one call for use in the audio callback.
     * Reduces the number of scattered atomic loads in the hot path.
     */
    TouchState snapshot() const {
        TouchState s;
        s.active = mDualTouchMode.load(std::memory_order_acquire);
        s.x1 = mTouch1X.load(std::memory_order_acquire);
        s.y1 = mTouch1Y.load(std::memory_order_acquire);
        s.freq1 = mTouch1Freq.load(std::memory_order_acquire);
        s.amp1 = mTouch1Amp.load(std::memory_order_acquire);
        s.pressure1 = mTouch1Pressure.load(std::memory_order_acquire);
        s.x2 = mTouch2X.load(std::memory_order_acquire);
        s.y2 = mTouch2Y.load(std::memory_order_acquire);
        s.freq2 = mTouch2Freq.load(std::memory_order_acquire);
        s.amp2 = mTouch2Amp.load(std::memory_order_acquire);
        s.pressure2 = mTouch2Pressure.load(std::memory_order_acquire);
        s.distance = mTouchDistance.load(std::memory_order_acquire);
        s.angle = mTouchAngle.load(std::memory_order_acquire);
        s.secondaryOscIndex = mSecondaryOscillatorIndex.load(std::memory_order_acquire);
        s.mixMode = mDualTouchMixMode.load(std::memory_order_acquire);
        return s;
    }

    // ========== ENVOLVENTE POR SLOT (REQ-052) ==========
    //
    // Cada slot (0 = principal, 1 = el otro) es UNA voz: su engine, su ultima
    // frecuencia y amplitud, y una envolvente de ganancia propia. El estado de
    // abajo (`mSlotVoice`, coeficientes, epoca y rate cacheados) lo leen y lo
    // escriben SOLO el thread de audio; desde el thread de control entran dos
    // atomics (los coeficientes publicados y `mEnvelopeEpoch`) y el thread de audio los
    // consulta una vez por bloque.

    /// Ataque lineal desde la ganancia actual hasta 1 (D5).
    static constexpr float kSlotAttackMs = 5.0f;
    /// Release exponencial: llega a `kSlotOffGain` (−60 dB) en este tiempo (D4).
    static constexpr float kSlotReleaseMs = 80.0f;
    /// −60 dB. Debajo de esto el release termina y el engine del slot deja de procesarse.
    static constexpr float kSlotOffGain = 1.0e-3f;
    /// Umbral de "el dedo esta apoyado" — el mismo que usaba renderDualTouch.
    static constexpr float kSlotHeldAmp = 0.001f;

    /// Lo que el thread de audio tiene que renderizar en este bloque.
    struct SlotBlockPlan {
        bool render[2];   ///< el engine del slot se procesa (apoyado o en release)
        float freq[2];    ///< frecuencia a usar (la retenida si esta en release)
        float amp[2];     ///< amplitud a usar (la retenida si esta en release)
    };

    /**
     * @brief Calcula los coeficientes de la envolvente para @p sampleRate y los
     * publica. Thread de control (camino de prepare); el thread de audio los
     * toma al planificar el bloque siguiente.
     */
    void setEnvelopeSampleRate(int sampleRate) noexcept {
        if (sampleRate <= 0) return;
        const float attackSamples = kSlotAttackMs * 0.001f * static_cast<float>(sampleRate);
        const float releaseSamples = kSlotReleaseMs * 0.001f * static_cast<float>(sampleRate);
        mSharedAttackStep.store(1.0f / std::max(1.0f, attackSamples), std::memory_order_relaxed);
        // g·r^N = kSlotOffGain con g = 1 y N = releaseSamples.
        mSharedReleaseCoeff.store(std::exp(std::log(kSlotOffGain) / std::max(1.0f, releaseSamples)),
                                  std::memory_order_relaxed);
    }

    /**
     * @brief Pide que las dos envolventes vuelvan a cero (sin release pendiente).
     * Thread de control: al prender el modo dual y al parar el motor.
     */
    void requestEnvelopeRestart() noexcept {
        mEnvelopeEpoch.fetch_add(1, std::memory_order_acq_rel);
    }

    /**
     * @brief Decide, al principio del bloque, que slots se procesan y con que
     * frecuencia y amplitud. Retiene freq/amp mientras el dedo esta apoyado y
     * los usa durante el release. Thread de audio. RT-safe.
     */
    SlotBlockPlan planSlotBlock(const TouchState& ts) noexcept {
        mAttackStep = mSharedAttackStep.load(std::memory_order_relaxed);
        mReleaseCoeff = mSharedReleaseCoeff.load(std::memory_order_relaxed);

        const uint32_t epoch = mEnvelopeEpoch.load(std::memory_order_acquire);
        if (epoch != mSeenEnvelopeEpoch) {
            mSeenEnvelopeEpoch = epoch;
            for (auto& v : mSlotVoice) {
                v.gain = 0.0f;
                v.held = false;
            }
        }

        const float amps[2] = {ts.amp1, ts.amp2};
        const float freqs[2] = {ts.freq1, ts.freq2};

        SlotBlockPlan plan{};
        for (int k = 0; k < 2; ++k) {
            SlotVoice& v = mSlotVoice[k];
            v.held = amps[k] > kSlotHeldAmp;
            if (v.held) {
                // El control escribe freq antes que amp, y el soltar manda
                // freq = 0: un bloque puede leer freq nueva (0) con amp vieja.
                // Una frecuencia no positiva no se retiene, o todo el release
                // sonaria a 0 Hz.
                if (freqs[k] > 0.0f) v.heldFreq = freqs[k];
                v.heldAmp = amps[k];
            }
            plan.render[k] = v.held || v.gain > 0.0f;
            plan.freq[k] = v.heldFreq;
            plan.amp[k] = v.heldAmp;
        }
        return plan;
    }

    /**
     * @brief El bloque NO paso por el camino dual: las envolventes vuelven a cero.
     *
     * Lo llama el thread de audio en cada bloque que se renderiza por otro
     * camino (un dedo, SoundFont, voice system, oscilador apagado...). Sin
     * esto, un dedo apoyado al salir del camino dual dejaria su envolvente
     * congelada en "apoyado, ganancia 1", y al volver sin dedos sonaria un
     * release fantasma de la nota vieja. Al volver con el dedo apoyado, la voz
     * entra con su ataque. RT-safe; dos comparaciones cuando no hay nada.
     */
    void abandonSlotVoices() noexcept {
        for (auto& v : mSlotVoice) {
            if (v.held || v.gain > 0.0f) {
                v.held = false;
                v.gain = 0.0f;
            }
        }
    }

    /**
     * @brief Un solo slot sonando: aplica su envolvente EN EL LUGAR sobre @p buffer.
     * Con el dedo apoyado y la ganancia ya en 1 no toca nada (mismo costo que
     * el camino de un dedo de antes). Thread de audio. RT-safe.
     */
    void shapeSoloSlot(int slot, float* buffer, int32_t numFrames) noexcept {
        SlotVoice& v = mSlotVoice[slot == 1 ? 1 : 0];
        if (v.held && v.gain >= 1.0f) return;  // regimen: ganancia 1
        for (int32_t i = 0; i < numFrames; ++i) {
            const float g = advanceSlotGain(v);
            buffer[i * 2] *= g;
            buffer[i * 2 + 1] *= g;
        }
    }

    /**
     * @brief Los dos slots sonando: aplica las envolventes y mezcla.
     *
     * Con x1, x2 la salida de cada engine (sin envolvente) y g1, g2 sus
     * ganancias, la mezcla es la interpolacion bilineal entre los cuatro
     * estados de presencia:
     *
     *     out = g1·(1−g2)·x1 + g2·(1−g1)·x2 + g1·g2·L(x1, x2)
     *
     * donde L es la ley del modo con los dos dedos (MAX es la excepcion: ver
     * su caso). Con un solo dedo (g del
     * otro = 0) da ese dedo a ganancia 1, que es lo que el modo dual hacia con
     * un dedo en TODOS los modos; con los dos en 1 da L, la ley de siempre; y
     * es continua entre medio, que es lo que pide AC-052.8. Para SUM/AVERAGE
     * (L = x1 + x2) se reduce a g1·x1 + g2·x2: la suma a unidad de D2/D3.
     *
     * Thread de audio. RT-safe.
     */
    void blendSlotVoices(const float* buffer1, const float* buffer2,
                         float* output, int32_t numFrames,
                         const TouchState& ts) noexcept {
        SlotVoice& v1 = mSlotVoice[0];
        SlotVoice& v2 = mSlotVoice[1];
        // Regimen con los dos dedos: las dos ganancias quedan en 1 todo el
        // bloque, asi que la mezcla es la ley sola, al mismo costo que antes.
        const bool settled = v1.held && v2.held && v1.gain >= 1.0f && v2.gain >= 1.0f;

        switch (ts.mixMode) {
            case DualTouchMixMode::MAX:
                if (settled) {
                    const int32_t totalSamples = numFrames * 2;
                    for (int32_t i = 0; i < totalSamples; ++i) {
                        const float absMax = std::max(std::abs(buffer1[i]), std::abs(buffer2[i]));
                        output[i] = (buffer1[i] + buffer2[i] >= 0.0f) ? absMax : -absMax;
                    }
                    return;
                }
                // MAX se aplica sobre las voces YA envueltas: max(|g1·x1|, |g2·x2|)
                // con el signo de la suma. Con un dedo da ese dedo (max con 0) y
                // con los dos en 1 da la ley de siempre, igual que la forma
                // bilineal; pero ademas |salida| es continua durante la
                // transicion. La bilineal mezcla x1 con una ley cuyo signo salta
                // (el de la suma), y ese salto, escalado por g1·g2, aparece como
                // escalon en |salida| — medido: 0,24 contra 0,036 de regimen.
                for (int32_t i = 0; i < numFrames; ++i) {
                    const float g1 = advanceSlotGain(v1);
                    const float g2 = advanceSlotGain(v2);
                    for (int c = 0; c < 2; ++c) {
                        const float a = g1 * buffer1[i * 2 + c];
                        const float b = g2 * buffer2[i * 2 + c];
                        const float absMax = std::max(std::abs(a), std::abs(b));
                        output[i * 2 + c] = (a + b >= 0.0f) ? absMax : -absMax;
                    }
                }
                return;

            case DualTouchMixMode::CROSSFADE: {
                const float d = std::clamp(ts.distance, 0.0f, 1.0f);
                if (settled) {
                    simd::mixStereoBuffers(output, buffer1, buffer2, 1.0f - d, d, numFrames);
                    return;
                }
                blendWithLaw(buffer1, buffer2, output, numFrames,
                             [d](float a, float b) { return (1.0f - d) * a + d * b; });
                return;
            }

            case DualTouchMixMode::RING:
                if (settled) {
                    const int32_t totalSamples = numFrames * 2;
                    for (int32_t i = 0; i < totalSamples; ++i) {
                        output[i] = buffer1[i] * buffer2[i] * 0.5f;
                    }
                    return;
                }
                blendWithLaw(buffer1, buffer2, output, numFrames,
                             [](float a, float b) { return a * b * 0.5f; });
                return;

            case DualTouchMixMode::AMPLITUDE_BALANCED: {
                // Pesos con la amplitud RETENIDA: el dedo en release conserva
                // la suya, asi que la ley no salta cuando su amp cruda pasa a 0.
                const float a1 = v1.heldAmp;
                const float a2 = v2.heldAmp;
                const float total = a1 + a2;
                const float w1 = total > kSlotHeldAmp ? a1 / total : 0.5f;
                const float w2 = total > kSlotHeldAmp ? a2 / total : 0.5f;
                if (settled) {
                    simd::mixStereoBuffers(output, buffer1, buffer2, w1, w2, numFrames);
                    return;
                }
                blendWithLaw(buffer1, buffer2, output, numFrames,
                             [w1, w2](float a, float b) { return w1 * a + w2 * b; });
                return;
            }

            case DualTouchMixMode::SUM:
            case DualTouchMixMode::AVERAGE:
            default:
                if (settled) {
                    // Regimen con los dos dedos: suma a unidad, sin costo por muestra extra.
                    simd::addStereoBuffers(output, buffer1, buffer2, numFrames,
                                           /*applyHeadroom=*/false);
                    return;
                }
                for (int32_t i = 0; i < numFrames; ++i) {
                    const float g1 = advanceSlotGain(v1);
                    const float g2 = advanceSlotGain(v2);
                    output[i * 2] = g1 * buffer1[i * 2] + g2 * buffer2[i * 2];
                    output[i * 2 + 1] = g1 * buffer1[i * 2 + 1] + g2 * buffer2[i * 2 + 1];
                }
                return;
        }
    }

    // ========== SONDA: BLOQUES PROCESADOS POR SLOT (REQ-052) ==========

    /**
     * @brief Cuenta un bloque en el que el engine (u oscilador) del slot se proceso.
     *
     * Lo llama SOLO el thread de audio, desde `AudioEngine::renderDualTouch`.
     * RT-safe: un `fetch_add` relajado (`wma::RtCounter`).
     */
    void countSlotBlock(int slot) noexcept {
        if (slot == 0) mSlotBlocks[0].bump();
        else if (slot == 1) mSlotBlocks[1].bump();
    }

    /**
     * @brief Cuantos bloques proceso el engine del slot (0 = principal, 1 = el otro).
     *
     * Sonda de tests (REQ-052, AC-052.3/.6): es el unico observable de "el engine
     * de ese slot dejo de procesarse" — la salida pasa por DC block y limitador,
     * asi que "exactamente cero" no lo distingue de un engine que sigue corriendo
     * a ganancia minima. Para el thread de control.
     */
    uint64_t slotBlocksRendered(int slot) const noexcept {
        if (slot == 0) return mSlotBlocks[0].get();
        if (slot == 1) return mSlotBlocks[1].get();
        return 0;
    }

    // ========== BUFFER ACCESS ==========

    float* getTouch1Buffer() { return mTouch1Buffer.data(); }
    float* getTouch2Buffer() { return mTouch2Buffer.data(); }

    bool hasBuffers() const {
        return !mTouch1Buffer.empty() && !mTouch2Buffer.empty();
    }

    void clearBuffers() {
        // Un buffer limpio sin envolvente limpia dejaria un release a medias
        // sonando sobre ceros: las dos vuelven a cero juntas.
        requestEnvelopeRestart();
        if (!mTouch1Buffer.empty()) std::fill(mTouch1Buffer.begin(), mTouch1Buffer.end(), 0.0f);
        if (!mTouch2Buffer.empty()) std::fill(mTouch2Buffer.begin(), mTouch2Buffer.end(), 0.0f);
        DTM_LOGI("Dual touch buffers cleared (size: %zu)", mTouch1Buffer.size());
    }

private:
    // Modo dual touch activo
    std::atomic<bool> mDualTouchMode{false};

    // Parámetros de touch 1
    std::atomic<float> mTouch1X{0.0f};
    std::atomic<float> mTouch1Y{0.0f};
    std::atomic<float> mTouch1Freq{440.0f};
    std::atomic<float> mTouch1Amp{0.0f};
    std::atomic<float> mTouch1Pressure{0.0f};

    // Parámetros de touch 2
    std::atomic<float> mTouch2X{0.0f};
    std::atomic<float> mTouch2Y{0.0f};
    std::atomic<float> mTouch2Freq{440.0f};
    std::atomic<float> mTouch2Amp{0.0f};
    std::atomic<float> mTouch2Pressure{0.0f};

    // Parámetros de interacción
    std::atomic<float> mTouchDistance{0.0f};
    std::atomic<float> mTouchAngle{0.0f};

    // Oscilador secundario (para touch 2)
    std::atomic<int> mSecondaryOscillatorIndex{1};

    // Modo de mezcla
    std::atomic<DualTouchMixMode> mDualTouchMixMode{DualTouchMixMode::AVERAGE};

    // Sonda REQ-052: bloques procesados por slot (los escribe el thread de audio).
    wma::RtCounter mSlotBlocks[2];

    // ========== ENVOLVENTE POR SLOT: estado del thread de audio (REQ-052) ==========

    struct SlotVoice {
        float gain = 0.0f;       ///< ganancia actual de la envolvente [0, 1]
        bool held = false;       ///< el dedo esta apoyado (amp > kSlotHeldAmp) en este bloque
        float heldFreq = 440.0f; ///< ultima frecuencia con el dedo apoyado
        float heldAmp = 0.0f;    ///< ultima amplitud con el dedo apoyado
    };

    /// Avanza UNA muestra la envolvente del slot y devuelve la ganancia a aplicar.
    /// Apoyado: rampa lineal hacia 1 desde donde este (ataque y re-toque).
    /// Levantado: decae exponencial; debajo de −60 dB queda en 0 y el slot se apaga.
    float advanceSlotGain(SlotVoice& v) const noexcept {
        if (v.held) {
            v.gain = std::min(1.0f, v.gain + mAttackStep);
        } else if (v.gain > 0.0f) {
            v.gain *= mReleaseCoeff;
            if (v.gain < kSlotOffGain) v.gain = 0.0f;
        }
        return v.gain;
    }

    /// Mezcla bilineal por presencia (ver blendSlotVoices) con la ley @p law.
    template <typename Law>
    void blendWithLaw(const float* buffer1, const float* buffer2, float* output,
                      int32_t numFrames, Law law) noexcept {
        SlotVoice& v1 = mSlotVoice[0];
        SlotVoice& v2 = mSlotVoice[1];
        for (int32_t i = 0; i < numFrames; ++i) {
            const float g1 = advanceSlotGain(v1);
            const float g2 = advanceSlotGain(v2);
            const float only1 = g1 * (1.0f - g2);
            const float only2 = g2 * (1.0f - g1);
            const float both = g1 * g2;
            for (int c = 0; c < 2; ++c) {
                const float x1 = buffer1[i * 2 + c];
                const float x2 = buffer2[i * 2 + c];
                output[i * 2 + c] = only1 * x1 + only2 * x2 + both * law(x1, x2);
            }
        }
    }

    SlotVoice mSlotVoice[2];
    // Copia del bloque de los coeficientes publicados (thread de audio).
    float mAttackStep = 1.0f / 240.0f;   // 5 ms a 48 kHz
    float mReleaseCoeff = 0.99820f;      // 80 ms a −60 dB a 48 kHz
    uint32_t mSeenEnvelopeEpoch = 0;

    // Entradas desde el thread de control. El default es 48 kHz hasta que el
    // camino de prepare publique el rate real.
    std::atomic<float> mSharedAttackStep{1.0f / 240.0f};
    std::atomic<float> mSharedReleaseCoeff{0.99820f};
    std::atomic<uint32_t> mEnvelopeEpoch{0};

    // Buffers pre-alocados para dual touch (RT-safe)
    std::vector<float> mTouch1Buffer;
    std::vector<float> mTouch2Buffer;
};
