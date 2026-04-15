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
     * @brief Clear all internal DSP state without destroying the effect.
     *
     * Default is a no-op — override in effects that carry state across
     * process() calls (delay lines, comb/allpass buffers, reverb tails,
     * filter memory, envelope followers, etc.).
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
    virtual void reset() {}
};

#endif // EFFECT_H