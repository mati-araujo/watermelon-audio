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
};

#endif // EFFECT_H