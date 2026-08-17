#ifndef EFFECT_H
#define EFFECT_H

/**
 * @class Effect
 * @brief Base class for audio effects
 *
 * All effects must implement process(), setParam(), getParam(), and setSampleRate().
 */
class Effect {
public:
    virtual ~Effect() = default;

    /**
     * @brief Process audio through the effect
     * @param input Input buffer (stereo interleaved)
     * @param output Output buffer (stereo interleaved)
     * @param numFrames Number of frames to process
     */
    virtual void process(float* input, float* output, int numFrames) = 0;

    /**
     * @brief Set an effect parameter
     * @param paramId Parameter ID
     * @param value Parameter value
     */
    virtual void setParam(int paramId, float value) = 0;

    /**
     * @brief Get an effect parameter
     * @param paramId Parameter ID
     * @return Parameter value
     */
    virtual float getParam(int paramId) = 0;

    /**
     * @brief Set the sample rate for the effect
     * @param sampleRate Sample rate in Hz (e.g., 48000)
     *
     * IMPROVED: Effects can now adapt to dynamic sample rate changes.
     * This method should be called when the effect is created or when
     * the audio stream's sample rate changes.
     */
    virtual void setSampleRate(int sampleRate) = 0;

    /**
     * @brief Set global BPM for tempo-synced effects
     * @param bpm Beats per minute (default no-op, override in effects that need it)
     */
    virtual void setBpm(float bpm) { (void)bpm; }

    /**
     * @brief Retardo que el efecto le agrega a la senal DIRECTA, en samples (WD-3.1).
     *
     * QUE ES Y QUE NO ES
     * ------------------
     * Es el corrimiento en el tiempo del camino SECO: cuantos samples tarda en
     * salir lo que entro. NO es el largo del eco de un delay, ni el pre-delay de
     * un reverb, ni la cola de una convolucion — todo eso es el SONIDO del
     * efecto y sale ademas de la senal directa, no en lugar de ella.
     *
     * La prueba practica: meta un impulso y busque donde aparece la PRIMERA
     * energia a la salida. Un delay con mix 50% la pone en el sample 0 (el
     * directo) y el eco despues: latencia 0. Un lookahead limiter la pone recien
     * en el sample 240: latencia 240.
     *
     * POR QUE HAY QUE DECLARARLO SI HOY TODOS DAN CERO
     * -----------------------------------------------
     * Porque el dia que alguien agregue un limiter con lookahead, un EQ de fase
     * lineal o una convolucion particionada, los modos de routing PARALLEL,
     * SPLIT_2X2, SERIAL_PARALLEL, PARALLEL_SERIAL y FEEDBACK van a sumar ramas
     * desalineadas — y eso es un filtro peine, audible y reproducible, que
     * ningun test de los que hay detectaria.
     *
     * Retrofitear el contrato despues cuesta tocar los 23 efectos. Declararlo
     * ahora, con todos en cero, cuesta una linea y deja el mecanismo puesto.
     * Y no queda como promesa: `test_effect_latency.cpp` MIDE la respuesta al
     * impulso de cada efecto y falla si lo declarado no coincide.
     *
     * Un efecto cuyo retardo dependa de sus parametros debe devolver el valor
     * vigente, y `EffectChain` re-consulta en cada cambio estructural.
     */
    virtual int getLatencySamples() const { return 0; }

    /**
     * @brief Clear all internal DSP state without destroying the effect.
     *
     * ES VIRTUAL PURA A PROPOSITO (WD-3.2). Antes tenia default `{}`, y ese
     * default era el equivocado: un efecto con estado que se olvidara de
     * overridearla compilaba perfecto y filtraba su cola vieja al contexto
     * nuevo, sin que nada avisara. El barrido property-based de WD-2.2 midio el
     * resultado de ese default: **16 de los 23 efectos registrados** no cumplian
     * el contrato.
     *
     * Ahora el compilador obliga a decidir. Un efecto SIN estado escribe un
     * `{}` explicito y un comentario que diga por que — que es una afirmacion
     * revisable, a diferencia de un silencio.
     *
     * COMO SE ESCRIBE UNA QUE ANDE
     * ----------------------------
     * Limpiar los buffers grandes NO alcanza, y ese fue el error mas comun de
     * los que ya existian: cinco de seis `reset()` incompletos limpiaban delay
     * lines, combs y FDN, y se olvidaban del estado escalar chico. Hay que
     * enumerar TODO lo que `process()` escribe:
     *
     *   - buffers (delay, comb, allpass, granos) y sus indices de escritura
     *   - memoria de filtros: `z1/z2`, `x1/x2/y1/y2`, estados de one-pole
     *   - envolventes y seguidores de nivel
     *   - fase de los LFO
     *   - **los `ParameterSmoother`** — este es el que se olvida siempre
     *
     * Lo que NO se toca son los PARAMETROS (lo que escribe `setParam`). Un
     * `reset()` que vuelva el cutoff a su default no limpia estado: cambia la
     * configuracion del usuario.
     *
     * Un smoother se resetea al VALOR VIGENTE de su parametro
     * (`mSmoother.reset(mParam.load())`), no a cero: dejarlo en cero hace que el
     * parametro suba desde cero durante el tiempo de smoothing, que es un
     * fade-in audible. El constructor tiene la misma obligacion — `DISTORTION`
     * hacia bien lo primero y mal lo segundo.
     *
     * Called when the audio context changes in a way that would otherwise
     * let stale state bleed through — e.g. transitioning from OSCILLATOR
     * mode (where effects may have been fed loud synth audio for seconds)
     * into INPUT_FX mode (where they'll process quiet mic input). Without
     * reset, a reverb tail cooked by chaos_pad leaks into the first
     * blocks of input_fx as a loud residual burst.
     *
     * Must be RT-safe: NO allocations, NO locks. Zero-fill existing
     * buffers only; do not resize them.
     *
     * Called from the audio thread (via EffectChain::reset() which is
     * dispatched from AudioEngine::onAudioReady when a reset is
     * pending). Effect state is owned by the audio thread so this is
     * race-free with respect to process().
     */
    virtual void reset() = 0;
};

#endif // EFFECT_H