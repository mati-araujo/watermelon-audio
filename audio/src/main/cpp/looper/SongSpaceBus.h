#pragma once

/**
 * @file SongSpaceBus.h
 * @brief Contrato del bus de envio «Espacio» de la cancion (REQ-055 S12, ADR-0013 §1).
 *
 * El AudioLooper (header-only, sin dependencia de effects/) lo invoca a traves de esta interfaz;
 * la implementacion concreta (reverb 100 % wet en paralelo con delay 100 % wet, cada uno con su
 * retorno) vive en core/SongSpaceBus.h, que es quien enlaza watermelon-effects. El bus se crea en
 * el hilo de control, se publica por un puntero atomico y NUNCA se libera mientras viva el motor.
 */

namespace wm {

/// Parametros del espacio. Los valida la frontera C (rangos en watermelon_audio.h).
struct SongSpaceParams {
    float reverbReturn = 0.0f;
    float reverbSize = 0.8f;
    float reverbDecayS = 2.0f;
    float reverbDamping = 0.5f;
    float reverbPreDelayMs = 20.0f;
    float reverbLowCutHz = 80.0f;
    float reverbHighCutHz = 12000.0f;
    float delayReturn = 0.0f;
    float delayTimeMs = 250.0f;
    float delayFeedback = 0.35f;
};

class ISongSpaceBus {
public:
    virtual ~ISongSpaceBus() = default;

    /// Control, con el RT quiesced: sample rate y bloque maximo. Puede alocar.
    virtual void prepareSpaceBus(int sampleRate, int maxBlockFrames) = 0;

    /// RT: aplica los parametros (los setters de los efectos no son atomicos entre si, por eso los
    /// corre el hilo que procesa).
    virtual void applySpaceParams(const SongSpaceParams& params) = 0;

    /// RT: `fxOut` (estereo intercalado) recibe el retorno del bus para el envio `send`.
    virtual void runSpaceBus(const float* send, float* fxOut, int frames) = 0;

    /// Estado de una instancia fresca (colas, LFOs, suavizado del delay al target). Sin alocar.
    virtual void resetSpaceBus() = 0;
};

}  // namespace wm
