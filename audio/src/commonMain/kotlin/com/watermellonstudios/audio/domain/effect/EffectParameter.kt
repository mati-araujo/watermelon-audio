package com.watermellonstudios.audio.domain.effect

/**
 * Effect parameters with centralized validation ranges.
 *
 * Each parameter defines its valid range and can validate/clamp values.
 * Parameter IDs are LOCAL to each effect type (0, 1, 2, ...).
 *
 * @property id Parameter ID used in native calls
 * @property minValue Minimum valid value
 * @property maxValue Maximum valid value
 * @property defaultValue Default value for the parameter
 * @property displayName Human-readable name for UI
 */
sealed class EffectParameter(
    val id: Int,
    val minValue: Float,
    val maxValue: Float,
    val defaultValue: Float,
    val displayName: String
) {
    /**
     * Validates if a value is within the valid range.
     */
    fun validate(value: Float): Boolean = value in minValue..maxValue

    /**
     * Clamps a value to the valid range.
     */
    fun clamp(value: Float): Float = value.coerceIn(minValue, maxValue)

    /**
     * Returns a validation result with error details if invalid.
     */
    fun validateWithResult(value: Float): ParameterValidationResult {
        return if (validate(value)) {
            ParameterValidationResult.Valid(value)
        } else {
            ParameterValidationResult.Invalid(id, value, minValue, maxValue)
        }
    }

    // ==================== FILTER PARAMETERS ====================

    /** Filter cutoff frequency in Hz (20-20000) */
    data object FilterFrequency : EffectParameter(0, 20f, 20000f, 1000f, "Frequency")

    /** Filter resonance/Q (0.1-10) */
    data object FilterResonance : EffectParameter(1, 0.1f, 10f, 0.707f, "Resonance")

    /** Filter type: 0=LowPass, 1=HighPass, 2=BandPass */
    data object FilterType : EffectParameter(2, 0f, 2f, 0f, "Type")

    // ==================== REVERB PARAMETERS ====================

    /** Reverb decay time (RT60) in seconds (0.1-10) */
    data object ReverbDecay : EffectParameter(0, 0.1f, 10f, 2f, "Decay")

    /** Reverb room size (0.1-2.0) */
    data object ReverbSize : EffectParameter(1, 0.1f, 2f, 0.8f, "Size")

    /** Reverb wet/dry mix (0-1) */
    data object ReverbMix : EffectParameter(2, 0f, 1f, 0.3f, "Mix")

    /** Reverb pre-delay in ms (0-100) */
    data object ReverbPreDelay : EffectParameter(3, 0f, 100f, 20f, "Pre-Delay")

    /** Reverb high frequency damping (0-1) */
    data object ReverbDamping : EffectParameter(4, 0f, 1f, 0.5f, "Damping")

    /** Reverb diffusion (0-1) */
    data object ReverbDiffusion : EffectParameter(5, 0f, 1f, 0.7f, "Diffusion")

    /** Reverb stereo width (0-1) */
    data object ReverbStereoWidth : EffectParameter(6, 0f, 1f, 1f, "Stereo Width")

    /** Reverb early/late reflections balance (0-1) */
    data object ReverbEarlyLateMix : EffectParameter(7, 0f, 1f, 0.5f, "Early/Late")

    /** Reverb modulation depth for shimmer (0-1) */
    data object ReverbModDepth : EffectParameter(8, 0f, 1f, 0f, "Mod Depth")

    /** Reverb modulation rate in Hz (0.1-5) */
    data object ReverbModRate : EffectParameter(9, 0.1f, 5f, 1f, "Mod Rate")

    /** Reverb low cut frequency in Hz (20-500) */
    data object ReverbLowCut : EffectParameter(10, 20f, 500f, 80f, "Low Cut")

    /** Reverb high cut frequency in Hz (1000-20000) */
    data object ReverbHighCut : EffectParameter(11, 1000f, 20000f, 12000f, "High Cut")

    // ==================== DELAY PARAMETERS ====================

    /** Delay time in ms (1-2000) */
    data object DelayTime : EffectParameter(0, 1f, 2000f, 250f, "Time")

    /** Delay feedback (0-0.95) */
    data object DelayFeedback : EffectParameter(1, 0f, 0.95f, 0.4f, "Feedback")

    /** Delay wet/dry mix (0-1) */
    data object DelayMix : EffectParameter(2, 0f, 1f, 0.3f, "Mix")

    /** Delay BPM for sync mode (60-200) */
    data object DelayBpm : EffectParameter(3, 60f, 200f, 120f, "BPM")

    /** Delay note division for sync (1-32) */
    data object DelayNoteDivision : EffectParameter(4, 1f, 32f, 4f, "Division")

    /** Delay sync on/off (0=off, 1=on) */
    data object DelaySync : EffectParameter(5, 0f, 1f, 0f, "Sync")

    // ==================== VOCODER PARAMETERS ====================

    /** Vocoder number of bands (4-32) */
    data object VocoderBandCount : EffectParameter(0, 4f, 32f, 16f, "Bands")

    /** Vocoder formant shift in semitones (-24 to +24) */
    data object VocoderFormantShift : EffectParameter(1, -24f, 24f, 0f, "Formant")

    /** Vocoder envelope attack in ms (0.1-100) */
    data object VocoderAttack : EffectParameter(2, 0.1f, 100f, 5f, "Attack")

    /** Vocoder envelope release in ms (1-500) */
    data object VocoderRelease : EffectParameter(3, 1f, 500f, 50f, "Release")

    /** Vocoder wet/dry mix (0-1) */
    data object VocoderMix : EffectParameter(4, 0f, 1f, 0.8f, "Mix")

    /** Vocoder internal carrier level (0-1) */
    data object VocoderCarrierLevel : EffectParameter(5, 0f, 1f, 0.5f, "Carrier Level")

    /** Vocoder modulator source (0=internal, 1=external mic) */
    data object VocoderModSource : EffectParameter(6, 0f, 1f, 0f, "Mod Source")

    /** Vocoder carrier source (0=input, 1=internal osc) */
    data object VocoderCarrierSource : EffectParameter(7, 0f, 1f, 1f, "Carrier Source")

    /** Vocoder internal oscillator frequency in Hz (50-500) */
    data object VocoderCarrierFreq : EffectParameter(8, 50f, 500f, 110f, "Carrier Freq")

    // ==================== DISTORTION PARAMETERS ====================
    // IDs match C++ DistortionEffect::Param enum in DistortionEffect.h

    /** Distortion input drive (0-1) */
    data object DistortionDrive : EffectParameter(0, 0f, 1f, 0.5f, "Drive")

    /** Distortion tone control (0-1, dark to bright) */
    data object DistortionTone : EffectParameter(1, 0f, 1f, 0.5f, "Tone")

    /** Distortion output level (0-1) */
    data object DistortionLevel : EffectParameter(2, 0f, 1f, 0.7f, "Level")

    /** Distortion wet/dry mix (0-1) */
    data object DistortionMix : EffectParameter(3, 0f, 1f, 1f, "Mix")

    /** Distortion algorithm/pedal type (see DistortionVariants in C++) */
    data object DistortionAlgorithm : EffectParameter(4, 0f, 104f, 0f, "Algorithm")

    // === Pedal-Specific Parameters (meaning varies by pedal) ===

    /**
     * Pedal-specific parameter A:
     * - Tube Screamer: Mid frequency (520-920 Hz)
     * - Klon: Treble boost (0-1)
     * - RAT: Filter cutoff (dark to bright)
     * - Big Muff: Sustain amount (0-1)
     * - Fuzz Face: Bias adjust (0-1)
     * - Metal Zone: Low EQ (+/- 12dB)
     * - HM-2: Low boost (0-12 dB)
     */
    data object DistortionParamA : EffectParameter(5, 0f, 1f, 0.5f, "Param A")

    /**
     * Pedal-specific parameter B:
     * - Tube Screamer: Mid Q/width (0.5-1.0)
     * - Klon: Clean blend (0-1)
     * - RAT: Turbo mode (0=normal, 1=LED clipping)
     * - Big Muff: Mid scoop depth (0-1)
     * - Fuzz Face: Cleanup response (0-1)
     * - Metal Zone: High EQ (+/- 12dB)
     * - HM-2: High boost (0-12 dB)
     */
    data object DistortionParamB : EffectParameter(6, 0f, 1f, 0.5f, "Param B")

    /**
     * Pedal-specific parameter C:
     * - Metal Zone: Mid frequency (200-3000 Hz)
     * - Other pedals: unused (default 0.5)
     */
    data object DistortionParamC : EffectParameter(7, 0f, 1f, 0.5f, "Param C")

    // === Advanced Parameters ===

    /** Distortion oversampling factor (0=off/1x, 1=2x, 2=4x) */
    data object DistortionOversample : EffectParameter(8, 0f, 2f, 1f, "Oversample")

    /** Pre-distortion high-pass filter cutoff in Hz (20-500) */
    data object DistortionPreLowCut : EffectParameter(9, 20f, 500f, 80f, "Pre Low Cut")

    /** Post-distortion low-pass filter cutoff in Hz (1000-20000) */
    data object DistortionPostHighCut : EffectParameter(10, 1000f, 20000f, 12000f, "Post High Cut")

    /** Voltage sag simulation (0-1, emulates dying battery) */
    data object DistortionSag : EffectParameter(11, 0f, 1f, 0f, "Sag")

    /** Transistor bias for fuzz effects (0-1) */
    data object DistortionBias : EffectParameter(12, 0f, 1f, 0.5f, "Bias")

    /** Noise gate threshold (0-1, 0=off) */
    data object DistortionGateThreshold : EffectParameter(13, 0f, 1f, 0f, "Gate")

    // ==================== COMPRESSOR PARAMETERS ====================
    // IDs match C++ CompressorEffect::Param enum

    /** Compressor threshold in dB (-60 to 0) */
    data object CompressorThreshold : EffectParameter(0, -60f, 0f, -20f, "Threshold")

    /** Compressor ratio (1:1 to 20:1, 20 = infinity) */
    data object CompressorRatio : EffectParameter(1, 1f, 20f, 4f, "Ratio")

    /** Compressor attack time in ms (0.1 to 100) */
    data object CompressorAttack : EffectParameter(2, 0.1f, 100f, 10f, "Attack")

    /** Compressor release time in ms (10 to 1000) */
    data object CompressorRelease : EffectParameter(3, 10f, 1000f, 100f, "Release")

    /** Compressor makeup gain in dB (-6 to +24) */
    data object CompressorMakeupGain : EffectParameter(4, -6f, 24f, 0f, "Makeup")

    /** Compressor knee width in dB (0 = hard, 20 = soft) */
    data object CompressorKnee : EffectParameter(5, 0f, 20f, 6f, "Knee")

    // ==================== CHORUS PARAMETERS ====================
    // IDs match C++ ChorusEffect::Param enum

    /** Chorus LFO rate in Hz (0.1 to 10) */
    data object ChorusRate : EffectParameter(0, 0.1f, 10f, 1f, "Rate")

    /** Chorus modulation depth (0 to 100%) */
    data object ChorusDepth : EffectParameter(1, 0f, 100f, 50f, "Depth")

    /** Chorus base delay time in ms (1 to 30) */
    data object ChorusDelay : EffectParameter(2, 1f, 30f, 7f, "Delay")

    /** Chorus feedback (-50 to +50%) */
    data object ChorusFeedback : EffectParameter(3, -50f, 50f, 0f, "Feedback")

    /** Chorus wet/dry mix (0 to 100%) */
    data object ChorusMix : EffectParameter(4, 0f, 100f, 50f, "Mix")

    /** Chorus number of voices (1 to 4) */
    data object ChorusVoices : EffectParameter(5, 1f, 4f, 2f, "Voices")

    // ==================== PHASER PARAMETERS ====================
    // IDs match C++ PhaserEffect::Param enum

    /** Phaser LFO rate in Hz (0.01 to 10) */
    data object PhaserRate : EffectParameter(0, 0.01f, 10f, 0.5f, "Rate")

    /** Phaser modulation depth (0 to 100%) */
    data object PhaserDepth : EffectParameter(1, 0f, 100f, 70f, "Depth")

    /** Phaser number of stages (2, 4, 6, 8, 12) */
    data object PhaserStages : EffectParameter(2, 2f, 12f, 4f, "Stages")

    /** Phaser feedback (-90 to +90%) */
    data object PhaserFeedback : EffectParameter(3, -90f, 90f, 30f, "Feedback")

    /** Phaser wet/dry mix (0 to 100%) */
    data object PhaserMix : EffectParameter(4, 0f, 100f, 50f, "Mix")

    // ==================== AMP SIMULATOR PARAMETERS ====================
    // IDs match C++ AmpSimulator::Param enum

    /** Amp pre-amp drive (0 to 100) */
    data object AmpSimGain : EffectParameter(0, 0f, 100f, 50f, "Gain")

    /** Amp bass EQ (0 to 100, maps to -12dB to +12dB) */
    data object AmpSimBass : EffectParameter(1, 0f, 100f, 50f, "Bass")

    /** Amp mid EQ (0 to 100, maps to -12dB to +12dB) */
    data object AmpSimMid : EffectParameter(2, 0f, 100f, 50f, "Mid")

    /** Amp treble EQ (0 to 100, maps to -12dB to +12dB) */
    data object AmpSimTreble : EffectParameter(3, 0f, 100f, 50f, "Treble")

    /** Amp presence (power amp high frequency boost, 0 to 100) */
    data object AmpSimPresence : EffectParameter(4, 0f, 100f, 50f, "Presence")

    /** Amp master volume (0 to 100) */
    data object AmpSimMaster : EffectParameter(5, 0f, 100f, 75f, "Master")

    /** Amp sag (power supply compression, 0 to 100) */
    data object AmpSimSag : EffectParameter(6, 0f, 100f, 30f, "Sag")

    /** Amp model: 0=Clean, 1=Crunch, 2=High Gain, 3=Modern */
    data object AmpSimModel : EffectParameter(7, 0f, 3f, 1f, "Model")

    /** Tone stack type: 0=Fender, 1=Marshall, 2=Vox, 3=Mesa */
    data object AmpSimTonestack : EffectParameter(8, 0f, 3f, 1f, "Tonestack")

    // ==================== CABINET SIMULATOR PARAMETERS ====================
    // IDs match C++ CabinetSimulator::Param enum

    /** Cabinet type: 0-5 built-in, 6=custom */
    data object CabinetType : EffectParameter(0, 0f, 6f, 2f, "Cabinet")

    /** Cabinet wet/dry mix (0 to 100%) */
    data object CabinetMix : EffectParameter(1, 0f, 100f, 100f, "Mix")

    /** Cabinet low cut frequency in Hz (20 to 500) */
    data object CabinetLowCut : EffectParameter(2, 20f, 500f, 80f, "Low Cut")

    /** Cabinet high cut frequency in Hz (2000 to 20000) */
    data object CabinetHighCut : EffectParameter(3, 2000f, 20000f, 12000f, "High Cut")

    // ==================== DECIMATOR PARAMETERS ====================
    // IDs match C++ DecimatorEffect param constants

    /** Decimator bit depth (1-24 bits) */
    data object DecimatorBitDepth : EffectParameter(0, 1f, 24f, 16f, "Bit Depth")

    /** Decimator target sample rate in Hz (100-48000) */
    data object DecimatorSampleRate : EffectParameter(1, 100f, 48000f, 48000f, "Sample Rate")

    /** Decimator wet/dry mix (0-1) */
    data object DecimatorMix : EffectParameter(2, 0f, 1f, 1f, "Mix")

    // ==================== DECI-HPF PARAMETERS ====================

    /** Deci-HPF bit depth (1-24 bits) */
    data object DeciHpfBitDepth : EffectParameter(0, 1f, 24f, 12f, "Bit Depth")

    /** Deci-HPF high-pass filter cutoff in Hz (20-8000) */
    data object DeciHpfCutoff : EffectParameter(1, 20f, 8000f, 300f, "HPF Cutoff")

    /** Deci-HPF target sample rate in Hz (100-48000) */
    data object DeciHpfSampleRate : EffectParameter(2, 100f, 48000f, 12000f, "Sample Rate")

    /** Deci-HPF wet/dry mix (0-1) */
    data object DeciHpfMix : EffectParameter(3, 0f, 1f, 1f, "Mix")

    // ==================== AUTO PAN PARAMETERS ====================

    /** AutoPan LFO rate in Hz (0.1-20) */
    data object AutoPanRate : EffectParameter(0, 0.1f, 20f, 2f, "Rate")

    /** AutoPan depth (0-1) */
    data object AutoPanDepth : EffectParameter(1, 0f, 1f, 0.8f, "Depth")

    /** AutoPan waveform (0=Sine, 1=Tri, 2=Square) */
    data object AutoPanWaveform : EffectParameter(2, 0f, 2f, 0f, "Waveform")

    /** AutoPan phase offset in degrees (0-360) */
    data object AutoPanPhaseOffset : EffectParameter(3, 0f, 360f, 0f, "Phase Offset")

    /** AutoPan wet/dry mix (0-1) */
    data object AutoPanMix : EffectParameter(4, 0f, 1f, 1f, "Mix")

    // ==================== COMPLEX TREM PARAMETERS ====================

    /** ComplexTrem LFO 1 rate in Hz (0.1-20) */
    data object ComplexTremRate1 : EffectParameter(0, 0.1f, 20f, 4f, "Rate 1")

    /** ComplexTrem LFO 2 rate in Hz (0.1-20) */
    data object ComplexTremRate2 : EffectParameter(1, 0.1f, 20f, 5.5f, "Rate 2")

    /** ComplexTrem depth (0-1) */
    data object ComplexTremDepth : EffectParameter(2, 0f, 1f, 0.6f, "Depth")

    /** ComplexTrem waveform (0=Sine, 1=Tri, 2=Square, 3=Saw) */
    data object ComplexTremWaveform : EffectParameter(3, 0f, 3f, 0f, "Waveform")

    /** ComplexTrem stereo phase in degrees (0-180) */
    data object ComplexTremStereoPhase : EffectParameter(4, 0f, 180f, 0f, "Stereo Phase")

    /** ComplexTrem wet/dry mix (0-1) */
    data object ComplexTremMix : EffectParameter(5, 0f, 1f, 1f, "Mix")

    // ========== RANDOM RESO (KORG NTS-3 FX-001) ==========

    /** RandomReso center frequency (80-12000 Hz) */
    data object RandomResoCenterFreq : EffectParameter(0, 80f, 12000f, 1000f, "Center Freq")

    /** RandomReso resonance / Q factor (0.5-30) */
    data object RandomResoResonance : EffectParameter(1, 0.5f, 30f, 8f, "Resonance")

    /** RandomReso LFO rate (0.1-20 Hz) */
    data object RandomResoLfoRate : EffectParameter(2, 0.1f, 20f, 2f, "LFO Rate")

    /** RandomReso LFO depth (0-1, mapped to 0-4 octaves) */
    data object RandomResoLfoDepth : EffectParameter(3, 0f, 1f, 0.5f, "LFO Depth")

    /** RandomReso wet/dry mix (0-1) */
    data object RandomResoMix : EffectParameter(4, 0f, 1f, 1f, "Mix")

    // ========== HPF-DELAY (KORG NTS-3 FX-004) ==========

    /** HpfDelay HPF cutoff frequency (20-8000 Hz) */
    data object HpfDelayCutoff : EffectParameter(0, 20f, 8000f, 200f, "HPF Cutoff")

    /** HpfDelay delay time (10-2000 ms) */
    data object HpfDelayTime : EffectParameter(1, 10f, 2000f, 300f, "Delay Time")

    /** HpfDelay feedback (0-0.95) */
    data object HpfDelayFeedback : EffectParameter(2, 0f, 0.95f, 0.4f, "Feedback")

    /** HpfDelay wet/dry mix (0-1) */
    data object HpfDelayMix : EffectParameter(3, 0f, 1f, 0.5f, "Mix")

    data object HpfDelaySync : EffectParameter(4, 0f, 1f, 0f, "Sync")
    data object HpfDelaySubdivision : EffectParameter(5, 0f, 5f, 1f, "Subdivision")
    data object HpfDelayPingPong : EffectParameter(6, 0f, 1f, 0f, "Ping Pong")
    data object HpfDelayDucking : EffectParameter(7, 0f, 1f, 0f, "Ducking")
    data object HpfDelayLpfCutoff : EffectParameter(8, 1000f, 20000f, 12000f, "LPF Cutoff")

    // ========== TAPE ECHO (KORG NTS-3 FX-006) ==========

    /** TapeEcho delay time in ms (50-2000) */
    data object TapeEchoDelayTime : EffectParameter(0, 50f, 2000f, 350f, "Delay Time")

    /** TapeEcho feedback (0-0.95) */
    data object TapeEchoFeedback : EffectParameter(1, 0f, 0.95f, 0.5f, "Feedback")

    /** TapeEcho wow/flutter intensity (0-1) */
    data object TapeEchoWowFlutter : EffectParameter(2, 0f, 1f, 0.3f, "Wow/Flutter")

    /** TapeEcho tape age (0-1, controls LPF + hiss) */
    data object TapeEchoTapeAge : EffectParameter(3, 0f, 1f, 0.4f, "Tape Age")

    /** TapeEcho saturation (0-1) */
    data object TapeEchoSaturation : EffectParameter(4, 0f, 1f, 0.2f, "Saturation")

    /** TapeEcho wet/dry mix (0-1) */
    data object TapeEchoMix : EffectParameter(5, 0f, 1f, 0.5f, "Mix")

    data object TapeEchoSync : EffectParameter(6, 0f, 1f, 0f, "Sync")
    data object TapeEchoSubdivision : EffectParameter(7, 0f, 5f, 2f, "Subdivision")
    data object TapeEchoPingPong : EffectParameter(8, 0f, 1f, 0f, "Ping Pong")
    data object TapeEchoDucking : EffectParameter(9, 0f, 1f, 0f, "Ducking")
    data object TapeEchoNoiseLevel : EffectParameter(10, 0f, 1f, 0f, "Noise")

    // ========== HALL REVERB (KORG NTS-3 FX-010) ==========

    /** HallReverb decay time in seconds (0.5-15) */
    data object HallReverbDecayTime : EffectParameter(0, 0.5f, 15f, 3f, "Decay Time")

    /** HallReverb room size (0.1-1.0) */
    data object HallReverbSize : EffectParameter(1, 0.1f, 1f, 0.7f, "Size")

    /** HallReverb pre-delay in ms (0-150) */
    data object HallReverbPreDelay : EffectParameter(2, 0f, 150f, 30f, "Pre-Delay")

    /** HallReverb diffusion (0-1) */
    data object HallReverbDiffusion : EffectParameter(3, 0f, 1f, 0.8f, "Diffusion")

    /** HallReverb HF damping (0-1) */
    data object HallReverbHfDamping : EffectParameter(4, 0f, 1f, 0.4f, "HF Damping")

    /** HallReverb LF damping (0-1) */
    data object HallReverbLfDamping : EffectParameter(5, 0f, 1f, 0.2f, "LF Damping")

    /** HallReverb modulation depth (0-1) */
    data object HallReverbModulation : EffectParameter(6, 0f, 1f, 0.15f, "Modulation")

    /** HallReverb wet/dry mix (0-1) */
    data object HallReverbMix : EffectParameter(7, 0f, 1f, 0.3f, "Mix")

    // ========== RISER REVERB (KORG NTS-3 FX-009) ==========

    /** RiserReverb attack time in ms (100-3000) */
    data object RiserReverbAttackTime : EffectParameter(0, 100f, 3000f, 800f, "Attack Time")

    /** RiserReverb decay in seconds (0.5-10) */
    data object RiserReverbDecay : EffectParameter(1, 0.5f, 10f, 2f, "Decay")

    /** RiserReverb size (0.1-1.0) */
    data object RiserReverbSize : EffectParameter(2, 0.1f, 1f, 0.6f, "Size")

    /** RiserReverb diffusion (0-1) */
    data object RiserReverbDiffusion : EffectParameter(3, 0f, 1f, 0.7f, "Diffusion")

    /** RiserReverb damping (0-1) */
    data object RiserReverbDamping : EffectParameter(4, 0f, 1f, 0.4f, "Damping")

    /** RiserReverb wet/dry mix (0-1) */
    data object RiserReverbMix : EffectParameter(5, 0f, 1f, 0.5f, "Mix")

    // ========== BEAT GRAIN (KORG NTS-3 FX-003) ==========

    /** BeatGrain grain size in ms (1-200) */
    data object BeatGrainGrainSize : EffectParameter(0, 1f, 200f, 50f, "Grain Size")

    /** BeatGrain density (0=1/4, 1=1/8, 2=1/16, 3=1/32) */
    data object BeatGrainDensity : EffectParameter(1, 0f, 3f, 2f, "Density")

    /** BeatGrain position spread (0-1) */
    data object BeatGrainPositionSpread : EffectParameter(2, 0f, 1f, 0.1f, "Position Spread")

    /** BeatGrain pitch shift in semitones (-12 to +12) */
    data object BeatGrainPitchShift : EffectParameter(3, -12f, 12f, 0f, "Pitch Shift")

    /** BeatGrain buffer length in seconds (0.5-4) */
    data object BeatGrainBufferLength : EffectParameter(4, 0.5f, 4f, 2f, "Buffer Length")

    /** BeatGrain wet/dry mix (0-1) */
    data object BeatGrainMix : EffectParameter(5, 0f, 1f, 0.5f, "Mix")

    // ========== SPRING REVERB (GUITAR) ==========

    data object SpringReverbDecay : EffectParameter(0, 0.4f, 5f, 2.2f, "Decay")
    data object SpringReverbTone : EffectParameter(1, 0f, 1f, 0.55f, "Tone")
    data object SpringReverbDrip : EffectParameter(2, 0f, 1f, 0.35f, "Drip")
    data object SpringReverbTension : EffectParameter(3, 0f, 1f, 0.5f, "Tension")
    data object SpringReverbMix : EffectParameter(4, 0f, 1f, 0.25f, "Mix")

    // ========== PLATE REVERB (GUITAR) ==========

    data object PlateReverbDecay : EffectParameter(0, 0.5f, 8f, 2.4f, "Decay")
    data object PlateReverbPreDelay : EffectParameter(1, 0f, 150f, 18f, "Pre-Delay")
    data object PlateReverbDamping : EffectParameter(2, 0f, 1f, 0.35f, "Damping")
    data object PlateReverbModulation : EffectParameter(3, 0f, 1f, 0.12f, "Modulation")
    data object PlateReverbLowCut : EffectParameter(4, 20f, 500f, 120f, "Low Cut")
    data object PlateReverbHighCut : EffectParameter(5, 1000f, 20000f, 9000f, "High Cut")
    data object PlateReverbMix : EffectParameter(6, 0f, 1f, 0.28f, "Mix")

    // ========== SHIMMER REVERB (GUITAR) ==========

    data object ShimmerReverbDecay : EffectParameter(0, 1f, 15f, 5f, "Decay")
    data object ShimmerReverbSize : EffectParameter(1, 0.1f, 1f, 0.85f, "Size")
    data object ShimmerReverbPitchSemitones : EffectParameter(2, -12f, 24f, 12f, "Pitch")
    data object ShimmerReverbAmount : EffectParameter(3, 0f, 1f, 0.35f, "Shimmer")
    data object ShimmerReverbFeedback : EffectParameter(4, 0f, 0.85f, 0.35f, "Feedback")
    data object ShimmerReverbTone : EffectParameter(5, 0f, 1f, 0.65f, "Tone")
    data object ShimmerReverbMix : EffectParameter(6, 0f, 1f, 0.35f, "Mix")

    companion object {
        /**
         * Gets all parameters for a given effect type.
         */
        fun forEffectType(type: EffectType): List<EffectParameter> {
            return when (type) {
                EffectType.FILTER -> listOf(
                    FilterFrequency,
                    FilterResonance,
                    FilterType
                )
                EffectType.REVERB -> listOf(
                    ReverbDecay,
                    ReverbSize,
                    ReverbMix,
                    ReverbPreDelay,
                    ReverbDamping,
                    ReverbDiffusion,
                    ReverbStereoWidth,
                    ReverbEarlyLateMix,
                    ReverbModDepth,
                    ReverbModRate,
                    ReverbLowCut,
                    ReverbHighCut
                )
                EffectType.DELAY -> listOf(
                    DelayTime,
                    DelayFeedback,
                    DelayMix,
                    DelayBpm,
                    DelayNoteDivision,
                    DelaySync
                )
                EffectType.VOCODER -> listOf(
                    VocoderBandCount,
                    VocoderFormantShift,
                    VocoderAttack,
                    VocoderRelease,
                    VocoderMix,
                    VocoderCarrierLevel,
                    VocoderModSource,
                    VocoderCarrierSource,
                    VocoderCarrierFreq
                )
                EffectType.DISTORTION -> listOf(
                    // Universal parameters
                    DistortionDrive,
                    DistortionTone,
                    DistortionLevel,
                    DistortionMix,
                    DistortionAlgorithm,
                    // Pedal-specific parameters
                    DistortionParamA,
                    DistortionParamB,
                    DistortionParamC,
                    // Advanced parameters
                    DistortionOversample,
                    DistortionPreLowCut,
                    DistortionPostHighCut,
                    DistortionSag,
                    DistortionBias,
                    DistortionGateThreshold
                )

                EffectType.COMPRESSOR -> listOf(
                    CompressorThreshold,
                    CompressorRatio,
                    CompressorAttack,
                    CompressorRelease,
                    CompressorMakeupGain,
                    CompressorKnee
                )
                EffectType.CHORUS -> listOf(
                    ChorusRate,
                    ChorusDepth,
                    ChorusDelay,
                    ChorusFeedback,
                    ChorusMix,
                    ChorusVoices
                )
                EffectType.PHASER -> listOf(
                    PhaserRate,
                    PhaserDepth,
                    PhaserStages,
                    PhaserFeedback,
                    PhaserMix
                )
                EffectType.AMP_SIM -> listOf(
                    AmpSimGain,
                    AmpSimBass,
                    AmpSimMid,
                    AmpSimTreble,
                    AmpSimPresence,
                    AmpSimMaster,
                    AmpSimSag,
                    AmpSimModel,
                    AmpSimTonestack
                )
                EffectType.CABINET -> listOf(
                    CabinetType,
                    CabinetMix,
                    CabinetLowCut,
                    CabinetHighCut
                )
                EffectType.DECIMATOR -> listOf(
                    DecimatorBitDepth,
                    DecimatorSampleRate,
                    DecimatorMix
                )
                EffectType.DECI_HPF -> listOf(
                    DeciHpfBitDepth,
                    DeciHpfCutoff,
                    DeciHpfSampleRate,
                    DeciHpfMix
                )
                EffectType.AUTO_PAN -> listOf(
                    AutoPanRate,
                    AutoPanDepth,
                    AutoPanWaveform,
                    AutoPanPhaseOffset,
                    AutoPanMix
                )
                EffectType.COMPLEX_TREM -> listOf(
                    ComplexTremRate1,
                    ComplexTremRate2,
                    ComplexTremDepth,
                    ComplexTremWaveform,
                    ComplexTremStereoPhase,
                    ComplexTremMix
                )
                EffectType.RANDOM_RESO -> listOf(
                    RandomResoCenterFreq,
                    RandomResoResonance,
                    RandomResoLfoRate,
                    RandomResoLfoDepth,
                    RandomResoMix
                )
                EffectType.HPF_DELAY -> listOf(
                    HpfDelayCutoff,
                    HpfDelayTime,
                    HpfDelayFeedback,
                    HpfDelayMix,
                    HpfDelaySync,
                    HpfDelaySubdivision,
                    HpfDelayPingPong,
                    HpfDelayDucking,
                    HpfDelayLpfCutoff
                )
                EffectType.TAPE_ECHO -> listOf(
                    TapeEchoDelayTime,
                    TapeEchoFeedback,
                    TapeEchoWowFlutter,
                    TapeEchoTapeAge,
                    TapeEchoSaturation,
                    TapeEchoMix,
                    TapeEchoSync,
                    TapeEchoSubdivision,
                    TapeEchoPingPong,
                    TapeEchoDucking,
                    TapeEchoNoiseLevel
                )
                EffectType.HALL_REVERB -> listOf(
                    HallReverbDecayTime,
                    HallReverbSize,
                    HallReverbPreDelay,
                    HallReverbDiffusion,
                    HallReverbHfDamping,
                    HallReverbLfDamping,
                    HallReverbModulation,
                    HallReverbMix
                )
                EffectType.RISER_REVERB -> listOf(
                    RiserReverbAttackTime,
                    RiserReverbDecay,
                    RiserReverbSize,
                    RiserReverbDiffusion,
                    RiserReverbDamping,
                    RiserReverbMix
                )
                EffectType.BEAT_GRAIN -> listOf(
                    BeatGrainGrainSize,
                    BeatGrainDensity,
                    BeatGrainPositionSpread,
                    BeatGrainPitchShift,
                    BeatGrainBufferLength,
                    BeatGrainMix
                )
                EffectType.SPRING_REVERB -> listOf(
                    SpringReverbDecay,
                    SpringReverbTone,
                    SpringReverbDrip,
                    SpringReverbTension,
                    SpringReverbMix
                )
                EffectType.PLATE_REVERB -> listOf(
                    PlateReverbDecay,
                    PlateReverbPreDelay,
                    PlateReverbDamping,
                    PlateReverbModulation,
                    PlateReverbLowCut,
                    PlateReverbHighCut,
                    PlateReverbMix
                )
                EffectType.SHIMMER_REVERB -> listOf(
                    ShimmerReverbDecay,
                    ShimmerReverbSize,
                    ShimmerReverbPitchSemitones,
                    ShimmerReverbAmount,
                    ShimmerReverbFeedback,
                    ShimmerReverbTone,
                    ShimmerReverbMix
                )
            }
        }

        /**
         * Gets a specific parameter by effect type and ID.
         */
        fun getParameter(type: EffectType, paramId: Int): EffectParameter? {
            return forEffectType(type).find { it.id == paramId }
        }
    }
}

/**
 * Result of parameter validation.
 */
sealed class ParameterValidationResult {
    data class Valid(val value: Float) : ParameterValidationResult()
    data class Invalid(
        val paramId: Int,
        val value: Float,
        val minValue: Float,
        val maxValue: Float
    ) : ParameterValidationResult()

    val isValid: Boolean get() = this is Valid
}
