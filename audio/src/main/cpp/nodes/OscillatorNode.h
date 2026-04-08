#pragma once

#include "../core/graph/AudioNode.h"
#include "../oscillators/Oscillators.h"
#include "../engines/SynthEngine.h"
#include "../modulators/SignalModulator.h"
#include "../modulators/BurstModulator.h"
#include "../modulators/AMModulator.h"
#include "../modulators/FMModulator.h"
#include "../modulators/PWMModulator.h"
#include "../modulators/EnvelopeModulator.h"
#include "../modulators/RingModulator.h"
#include "../modulators/GateModulator.h"
#include <memory>
#include <vector>

/**
 * @class OscillatorNode
 * @brief Nodo de oscilador para el Audio Graph
 *
 * Wrapper del sistema de osciladores existente que lo integra
 * en la arquitectura del Audio Graph. Soporta:
 * - Múltiples tipos de oscilador (Sine, Square, Saw, Triangle, Noise, BandNoise)
 * - Moduladores de señal
 * - Modo dual touch
 */
class OscillatorNode : public AudioNode {
public:
    OscillatorNode();
    ~OscillatorNode() override = default;

    NodeType getType() const override { return NodeType::OSCILLATOR; }
    const char* getName() const override { return "Oscillator"; }

    void prepare(int sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(AudioBuffer& inputBuffer, int numFrames) override;

    // ========== Interfaz pública (thread-safe) ==========

    /**
     * @brief Actualiza posición XY (mapea a frecuencia/amplitud)
     */
    void updateXY(float x, float y);

    /**
     * @brief Establece frecuencia y amplitud directamente
     */
    void setFrequencyAndAmplitude(float freq, float amp);

    /**
     * @brief Sets the dynamic frequency range for XY mapping (Phase 10A).
     * Lock-free, safe to call from any thread.
     */
    void setFrequencyRange(float minHz, float maxHz);

    /**
     * @brief Cambia el tipo de oscilador
     * @param type 0=Sine, 1=Square, 2=Saw, 3=Triangle, 4=Noise, 5=BandNoise
     */
    void setOscillatorType(int type);
    int getOscillatorType() const;

    // ========== Modulador ==========

    /**
     * @brief Cambia el tipo de modulador
     * @param type 0=NONE, 1=BURST, 2=AM, 3=FM, 4=PWM, 5=ENV, 6=RING, 7=GATE
     */
    void setModulatorType(int type);
    int getModulatorType() const;

    /**
     * @brief Establece parámetro del modulador activo
     */
    void setModulatorParameter(int paramId, float value);

    // ========== Dual touch ==========

    void setDualTouchMode(bool enabled);
    bool getDualTouchMode() const;

    void updateDualTouch(float x1, float y1, float freq1, float amp1,
                         float x2, float y2, float freq2, float amp2);

    void setSecondaryOscillatorType(int type);
    int getSecondaryOscillatorType() const;

    // ========== Synth Engine (Phase 6) ==========

    /**
     * @brief Set the active engine type
     * @param type 0=CLASSIC (legacy oscillators), 1+=SynthEngine
     */
    void setEngineType(int type);
    int getEngineType() const;

    /**
     * @brief Set a parameter on the current synth engine
     */
    void setEngineParameter(int paramId, float value);

    /**
     * @brief Register a SynthEngine instance for a given type
     * @param type Engine type ID (must be > 0)
     * @param engine Pointer to engine (OscillatorNode does NOT own it)
     */
    void registerEngine(int type, SynthEngine* engine);

private:
    void createOscillators();
    void createModulators();

    // Mapeo de XY a frecuencia/amplitud
    float mapXToFrequency(float x) const;
    float mapYToAmplitude(float y) const;

private:
    // Osciladores (del código existente)
    std::vector<std::unique_ptr<AudioSource>> mOscillators;
    std::atomic<int> mCurrentOscillatorIndex{0};

    // Oscilador secundario para dual touch
    std::atomic<int> mSecondaryOscillatorIndex{1};

    // Moduladores
    std::vector<std::unique_ptr<SignalModulator>> mModulators;
    std::atomic<int> mCurrentModulatorIndex{0};

    // Parámetros
    std::atomic<float> mFrequency{440.0f};
    std::atomic<float> mAmplitude{0.0f};

    // Dual touch
    std::atomic<bool> mDualTouchMode{false};
    std::atomic<float> mFrequency2{440.0f};
    std::atomic<float> mAmplitude2{0.0f};

    // ========== Synth Engine (Phase 6) ==========
    std::atomic<int> mCurrentEngineType{0};  // 0 = CLASSIC
    static constexpr int MAX_ENGINES = 7;  // 0=CLASSIC..6=SOUNDFONT
    std::array<SynthEngine*, MAX_ENGINES> mEngines{};  // Non-owning ptrs, index = EngineTypeId

    // Buffers temporales (pre-alocados, RT-safe)
    std::vector<float> mTempBuffer;
    std::vector<float> mTouch1Buffer;
    std::vector<float> mTouch2Buffer;

    // Dynamic frequency range (Phase 10A) — lock-free, set from UI thread
    std::atomic<float> mMinFreq{20.0f};
    std::atomic<float> mMaxFreq{2000.0f};
};
