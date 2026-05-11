package com.watermellonstudios.audio.api

/**
 * Platform-agnostic interface for the native audio bridge.
 *
 * Extends [IEffectStateProvider] and [IEffectStateWriter] for effect chain operations.
 * Covers lifecycle, state queries, real-time params, voice system, mode, and backend.
 *
 * Platform-specific operations (USB device management, looper, arpeggiator, SoundFont,
 * latency benchmark) are NOT included — their consumers remain platform-specific.
 *
 * Android implementation: [com.watermellonstudios.audio.internal.bridge.AudioNativeBridge]
 */
interface IAudioNativeBridge : IEffectStateProvider, IEffectStateWriter {

    // ==================== LIFECYCLE ====================

    suspend fun startEngine(): Result<Unit>
    suspend fun stopEngine(): Result<Unit>
    suspend fun startEngineWithFade(fadeTimeMs: Int): Result<Unit>
    suspend fun stopEngineWithFade(fadeTimeMs: Int): Result<Unit>
    suspend fun pauseEngineWithFade(fadeTimeMs: Int): Result<Unit>
    suspend fun resumeEngineWithFade(fadeTimeMs: Int): Result<Unit>

    /** Synchronous lifecycle — for use from AudioEngineImpl (non-suspend context). */
    fun startEngineWithFadeSync(fadeTimeMs: Int)
    fun stopEngineWithFadeSync(fadeTimeMs: Int)
    fun pauseEngineWithFadeSync(fadeTimeMs: Int)
    fun resumeEngineWithFadeSync(fadeTimeMs: Int)
    fun stopEngineSync()

    // ==================== STATE QUERIES ====================

    fun getEngineState(): Int
    fun getStateVersion(): Long
    fun hasStreamError(): Boolean
    fun getLastStreamErrorCode(): Int
    fun clearStreamError()
    fun getIsPaused(): Boolean
    fun isEngineInitialized(): Boolean
    fun hasInitializationFailed(): Boolean
    fun getStreamInfoArray(): FloatArray?
    fun getMasterVolume(): Float

    // ==================== FADE ====================

    fun getCurrentFadeVolume(): Float
    fun getTargetFadeVolume(): Float
    fun getIsFading(): Boolean
    fun getFadeProgress(): Float

    // ==================== REAL-TIME PARAMS (lock-free) ====================

    fun setXY(x: Float, y: Float, coalesce: Boolean = true)
    fun setFrequencyAndAmplitude(frequency: Float, amplitude: Float)
    fun setFrequencyRange(minHz: Float, maxHz: Float)
    fun setMasterVolume(volume: Float)
    fun setOscillatorType(type: Int)
    fun setSecondaryOscillatorType(type: Int)
    fun setEngineType(type: Int)
    fun setEngineParameter(paramId: Int, value: Float)
    fun getEngineType(): Int
    fun setBpm(bpm: Float)
    fun getBpm(): Float
    fun setModulatorType(type: Int)
    fun setModulatorParameter(paramId: Int, value: Float)

    // ==================== EFFECTS (sync variants for AudioEngineImpl) ====================

    fun addEffectSync(typeId: Int): Boolean
    fun removeEffectSync(index: Int)
    fun setEffectParameterSync(effectIndex: Int, paramId: Int, value: Float)
    fun getEffectParameterSync(effectIndex: Int, paramId: Int): Float
    fun setEffectBypassSync(index: Int, bypass: Boolean)
    fun reorderEffectsSync(fromIndex: Int, toIndex: Int)

    // ==================== EFFECT ROUTING ====================

    fun setRoutingMode(mode: Int)
    fun getRoutingMode(): Int
    fun setParallelMix(mix: Float)
    fun setFeedbackAmount(amount: Float)

    // ==================== WAVEFORM ====================

    fun getWaveformSamples(buffer: FloatArray, size: Int): Int

    // ==================== VOICE SYSTEM ====================

    fun enableVoiceSystem(enabled: Boolean)
    fun isVoiceSystemEnabled(): Boolean
    fun updateMultiTouch(count: Int, touchData: FloatArray?)
    fun getActiveVoiceCount(): Int
    fun setMaxVoices(maxVoices: Int)
    fun setVoiceStealingStrategy(strategyId: Int)

    // ==================== DUAL TOUCH ====================

    fun setDualTouchMode(enabled: Boolean)
    fun setDualTouch(
        x1: Float, y1: Float, freq1: Float, amp1: Float, pressure1: Float,
        x2: Float, y2: Float, freq2: Float, amp2: Float, pressure2: Float,
        distance: Float, angle: Float
    )
    fun setDualTouchMixMode(modeId: Int)

    // ==================== MODE ====================

    suspend fun setAudioMode(mode: Int): Result<Unit>
    fun getAudioMode(): Int
    fun isInModeTransition(): Boolean

    // ==================== BACKEND ====================

    fun setUseBackendManager(useBackendManager: Boolean)
    fun createSplitBackend(inputBackendId: Int, outputBackendId: Int): Boolean
    fun selectBackend(backendId: Int): Boolean
    fun getCurrentBackendType(): Int
    fun isUsbBackendAvailable(): Boolean
}
