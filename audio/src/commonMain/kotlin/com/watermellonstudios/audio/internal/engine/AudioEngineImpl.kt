package com.watermellonstudios.audio.internal.engine

import com.watermellonstudios.audio.api.InternalWatermelonApi
import com.watermellonstudios.audio.api.AudioEngine
import com.watermellonstudios.audio.api.DualTouchParams
import com.watermellonstudios.audio.api.MultiTouchPoint
import com.watermellonstudios.audio.api.VoiceStealingStrategy
import com.watermellonstudios.audio.api.config.AudioEngineConfig
import com.watermellonstudios.audio.callback.AudioLogger
import com.watermellonstudios.audio.domain.effect.EffectChainState
import com.watermellonstudios.audio.domain.effect.EffectState
import com.watermellonstudios.audio.domain.effect.EffectType
import com.watermellonstudios.audio.domain.modulator.ModulatorType
import com.watermellonstudios.audio.domain.oscillator.OscillatorType
import com.watermellonstudios.audio.domain.scale.ScaleMode
import com.watermellonstudios.audio.domain.state.AudioError
import com.watermellonstudios.audio.domain.state.AudioState
import com.watermellonstudios.audio.domain.state.EngineLifecycle
import com.watermellonstudios.audio.domain.state.StreamInfo
import com.watermellonstudios.audio.domain.usb.AudioBackendType
import com.watermellonstudios.audio.internal.bridge.getAudioBridge
import com.watermellonstudios.audio.internal.util.ScaleQuantizer
import com.watermellonstudios.audio.internal.util.epochMillis
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * Implementation of [AudioEngine].
 *
 * This class is internal. Use [com.watermellonstudios.audio.api.AudioEngineFactory] to create instances.
 */
internal class AudioEngineImpl(
    private val config: AudioEngineConfig
) : AudioEngine {

    companion object {
        private const val TAG = "AudioEngine"
    }

    // El motor es el implementador del puente, no un consumidor: las factories
    // publicas se construyen encima de el. Ver [InternalWatermelonApi].
    @OptIn(InternalWatermelonApi::class)
    private val bridge = getAudioBridge()

    private val logger: AudioLogger = config.logger
    private val analytics = config.analyticsListener

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private var pollingJob: Job? = null
    private var sessionStartTime: Long = 0

    private val _state = MutableStateFlow(AudioState(
        oscillator = config.defaultOscillator
    ))
    override val state: StateFlow<AudioState> = _state.asStateFlow()

    override val isRunning: Boolean get() = _state.value.isRunning
    override val isPaused: Boolean get() = _state.value.isPaused

    private var currentScaleMode: ScaleMode = ScaleMode.FREE

    // ==================== LIFECYCLE ====================

    override suspend fun start(fadeMs: Int?) {
        val fade = fadeMs ?: config.defaultFadeMs

        try {
            if (bridge.hasInitializationFailed()) {
                val error = AudioError(-1, "Initialization failed due to insufficient memory", false)
                _state.update { it.copy(error = error) }
                analytics.onError(error)
                return
            }

            logger.info(TAG, "Starting audio engine", mapOf("fadeMs" to fade))
            _state.update { it.copy(lifecycle = EngineLifecycle.STARTING) }

            bridge.startEngineWithFadeSync(fade)
            bridge.setOscillatorType(config.defaultOscillator.id)

            // Add default effects
            config.defaultEffects.forEach { effectType ->
                bridge.addEffectSync(effectType.id)
            }

            _state.update { it.copy(lifecycle = EngineLifecycle.RUNNING) }

            // Get stream info
            refreshStreamInfo()

            // Analytics
            sessionStartTime = epochMillis()
            _state.value.streamInfo?.let { analytics.onSessionStarted(it) }

            // Start state polling
            startStatePolling()

            delay(fade.toLong())
            logger.info(TAG, "Audio engine started")

        } catch (e: Exception) {
            logger.error(TAG, "Failed to start audio engine", e)
            val error = AudioError(-1, "Failed to start: ${e.message}", true)
            _state.update { it.copy(lifecycle = EngineLifecycle.STOPPED, error = error) }
            analytics.onError(error)
        }
    }

    override suspend fun stop(fadeMs: Int?) {
        val fade = fadeMs ?: config.defaultFadeMs

        try {
            logger.info(TAG, "Stopping audio engine", mapOf("fadeMs" to fade))
            _state.update { it.copy(lifecycle = EngineLifecycle.STOPPING) }

            bridge.stopEngineWithFadeSync(fade)
            stopStatePolling()

            delay(fade.toLong())

            _state.update { it.copy(lifecycle = EngineLifecycle.STOPPED) }

            // Analytics
            if (sessionStartTime > 0) {
                val duration = epochMillis() - sessionStartTime
                analytics.onSessionEnded(duration)
                sessionStartTime = 0
            }

            logger.info(TAG, "Audio engine stopped")

        } catch (e: Exception) {
            logger.error(TAG, "Failed to stop audio engine", e)
            _state.update { it.copy(error = AudioError(-1, "Failed to stop: ${e.message}", true)) }
        }
    }

    override suspend fun pause(fadeMs: Int) {
        try {
            logger.debug(TAG, "Pausing audio", mapOf("fadeMs" to fadeMs))
            bridge.pauseEngineWithFadeSync(fadeMs)
            delay(fadeMs.toLong())
            _state.update { it.copy(isPaused = true) }
        } catch (e: Exception) {
            logger.error(TAG, "Failed to pause audio", e)
        }
    }

    override suspend fun resume(fadeMs: Int) {
        try {
            logger.debug(TAG, "Resuming audio", mapOf("fadeMs" to fadeMs))
            bridge.resumeEngineWithFadeSync(fadeMs)
            delay(fadeMs.toLong())
            _state.update { it.copy(isPaused = false) }
        } catch (e: Exception) {
            logger.error(TAG, "Failed to resume audio", e)
        }
    }

    // ==================== OSCILLATOR ====================

    override fun setOscillator(type: OscillatorType) {
        val previous = _state.value.oscillator
        _state.update { it.copy(oscillator = type) }
        bridge.setOscillatorType(type.id)
        analytics.onOscillatorChanged(type, previous)
        logger.debug(TAG, "Oscillator changed", mapOf("type" to type.displayName))
    }

    override fun setXY(x: Float, y: Float) {
        val clampedX = x.coerceIn(0f, 1f)
        val clampedY = y.coerceIn(0f, 1f)

        val frequency = ScaleQuantizer.quantizeFrequency(clampedX, currentScaleMode)
        val amplitude = clampedY

        bridge.setFrequencyAndAmplitude(frequency, amplitude)

        _state.update {
            it.copy(
                xPosition = clampedX,
                yPosition = clampedY,
                frequency = frequency,
                amplitude = amplitude
            )
        }
    }

    override fun setFrequencyAndAmplitude(frequency: Float, amplitude: Float) {
        bridge.setFrequencyAndAmplitude(frequency, amplitude.coerceIn(0f, 1f))
        _state.update {
            it.copy(frequency = frequency, amplitude = amplitude.coerceIn(0f, 1f))
        }
    }

    // ==================== MODULATOR ====================

    override fun setModulator(type: ModulatorType) {
        val previous = _state.value.modulator
        _state.update { it.copy(modulator = type) }
        bridge.setModulatorType(type.id)
        analytics.onModulatorChanged(type, previous)
        logger.debug(TAG, "Modulator changed", mapOf("type" to type.displayName))
    }

    override fun setModulatorParameter(paramId: Int, value: Float) {
        bridge.setModulatorParameter(paramId, value)
    }

    // ==================== EFFECTS ====================

    override fun addEffect(type: EffectType): Boolean {
        val currentChain = _state.value.effectChain
        if (!currentChain.canAddEffect) {
            logger.warn(TAG, "Cannot add effect - chain is full")
            return false
        }

        val success = bridge.addEffectSync(type.id)
        if (success) {
            val newEffect = EffectState(
                index = currentChain.effects.size,
                type = type
            )
            _state.update {
                it.copy(
                    effectChain = currentChain.copy(
                        effects = currentChain.effects + newEffect
                    )
                )
            }
            analytics.onEffectAdded(type, newEffect.index)
            logger.info(TAG, "Effect added", mapOf("type" to type.displayName))
        }
        return success
    }

    override fun removeEffect(index: Int) {
        val currentChain = _state.value.effectChain
        if (index < 0 || index >= currentChain.effects.size) {
            logger.warn(TAG, "Invalid effect index", mapOf("index" to index))
            return
        }

        val removedEffect = currentChain.effects[index]
        bridge.removeEffectSync(index)

        val newEffects = currentChain.effects
            .filterNot { it.index == index }
            .mapIndexed { i, effect -> effect.copy(index = i) }

        _state.update {
            it.copy(effectChain = currentChain.copy(effects = newEffects))
        }

        analytics.onEffectRemoved(removedEffect.type, index)
        logger.info(TAG, "Effect removed", mapOf("type" to removedEffect.type.displayName))
    }

    override fun setEffectParameter(effectIndex: Int, paramId: Int, value: Float) {
        bridge.setEffectParameterSync(effectIndex, paramId, value)

        _state.update { state ->
            val newEffects = state.effectChain.effects.map { effect ->
                if (effect.index == effectIndex) {
                    effect.copy(parameters = effect.parameters + (paramId to value))
                } else {
                    effect
                }
            }
            state.copy(effectChain = state.effectChain.copy(effects = newEffects))
        }
    }

    override fun getEffectParameter(effectIndex: Int, paramId: Int): Float {
        return bridge.getEffectParameterSync(effectIndex, paramId)
    }

    override fun setEffectBypass(index: Int, bypass: Boolean) {
        bridge.setEffectBypassSync(index, bypass)

        _state.update { state ->
            val newEffects = state.effectChain.effects.map { effect ->
                if (effect.index == index) {
                    effect.copy(isBypassed = bypass)
                } else {
                    effect
                }
            }
            state.copy(effectChain = state.effectChain.copy(effects = newEffects))
        }
    }

    override fun setEffectsBypass(bypass: Boolean) {
        bridge.setEffectsBypassSync(bypass)

        _state.update { state ->
            state.copy(effectChain = state.effectChain.copy(isGloballyBypassed = bypass))
        }
    }

    override fun reorderEffects(fromIndex: Int, toIndex: Int) {
        bridge.reorderEffectsSync(fromIndex, toIndex)

        _state.update { state ->
            val mutableList = state.effectChain.effects.toMutableList()
            val item = mutableList.removeAt(fromIndex)
            mutableList.add(toIndex, item)
            val reordered = mutableList.mapIndexed { i, effect -> effect.copy(index = i) }
            state.copy(effectChain = state.effectChain.copy(effects = reordered))
        }
    }

    // ==================== SCALE ====================

    override fun setScaleMode(mode: ScaleMode) {
        val previous = currentScaleMode
        currentScaleMode = mode

        // Reset hysteresis when switching scales to immediately quantize to the new scale
        ScaleQuantizer.resetHysteresis()

        // Re-quantize current frequency
        val newFrequency = ScaleQuantizer.quantizeFrequency(_state.value.xPosition, mode)
        bridge.setFrequencyAndAmplitude(newFrequency, _state.value.amplitude)
        _state.update { it.copy(frequency = newFrequency) }

        analytics.onScaleModeChanged(mode, previous)
        logger.debug(TAG, "Scale mode changed", mapOf("mode" to mode.label))
    }

    // ==================== CHORD VOICES ====================

    override fun triggerChord(frequencies: FloatArray, amplitude: Float, oscillatorType: Int) {
        bridge.triggerChordNotes(frequencies, amplitude.coerceIn(0f, 1f), oscillatorType)
    }

    override fun updateChord(frequencies: FloatArray, amplitude: Float) {
        bridge.updateChordNotes(frequencies, amplitude.coerceIn(0f, 1f))
    }

    override fun releaseChord() {
        bridge.releaseChordNotes()
    }

    // ==================== VOLUME ====================

    override fun setMasterVolume(volume: Float) {
        val clamped = volume.coerceIn(0f, 1f)
        bridge.setMasterVolume(clamped)
        _state.update { it.copy(masterVolume = clamped) }
    }

    // ==================== VISUALIZATION ====================

    override fun getWaveformSamples(buffer: FloatArray, size: Int): Int {
        return bridge.getWaveformSamples(buffer, size)
    }

    // ==================== DUAL TOUCH ====================

    override fun setDualTouchEnabled(enabled: Boolean) {
        bridge.setDualTouchMode(enabled)
        logger.debug(TAG, "Dual touch mode", mapOf("enabled" to enabled))
    }

    override fun updateDualTouch(params: DualTouchParams) {
        bridge.setDualTouch(
            params.x1, params.y1, params.freq1, params.amp1, params.pressure1,
            params.x2, params.y2, params.freq2, params.amp2, params.pressure2,
            params.distance, params.angle
        )
    }

    override fun setDualTouchMixMode(mode: Int) {
        bridge.setDualTouchMixMode(mode)
    }

    override fun setSecondaryOscillator(type: OscillatorType) {
        bridge.setSecondaryOscillatorType(type.id)
    }

    // ==================== VOICE SYSTEM (Phase 2) ====================

    override fun enableVoiceSystem(enabled: Boolean) {
        bridge.enableVoiceSystem(enabled)
        logger.info(TAG, "Voice system", mapOf("enabled" to enabled))
    }

    override fun isVoiceSystemEnabled(): Boolean {
        return bridge.isVoiceSystemEnabled()
    }

    override fun updateMultiTouch(touches: List<MultiTouchPoint>) {
        if (touches.isEmpty()) {
            // Release all touches
            bridge.updateMultiTouch(0, null)
            return
        }

        // Convert to flattened array: [x, y, freq, amp, pressure] * N
        val touchData = FloatArray(touches.size * 5)
        touches.forEachIndexed { index, touch ->
            val offset = index * 5
            touchData[offset + 0] = touch.x
            touchData[offset + 1] = touch.y
            touchData[offset + 2] = touch.frequency
            touchData[offset + 3] = touch.amplitude
            touchData[offset + 4] = touch.pressure
        }

        bridge.updateMultiTouch(touches.size, touchData)
    }

    override fun getActiveVoiceCount(): Int {
        return bridge.getActiveVoiceCount()
    }

    override fun setMaxVoices(maxVoices: Int) {
        bridge.setMaxVoices(maxVoices.coerceIn(1, 16))
        logger.debug(TAG, "Max voices set", mapOf("maxVoices" to maxVoices))
    }

    override fun setVoiceStealingStrategy(strategy: VoiceStealingStrategy) {
        bridge.setVoiceStealingStrategy(strategy.id)
        logger.debug(TAG, "Voice stealing strategy", mapOf("strategy" to strategy.name))
    }

    // ==================== AUDIO BACKEND ====================

    override fun setAudioBackend(type: AudioBackendType): Boolean {
        if (isRunning) {
            logger.warn(TAG, "Cannot change backend while engine is running")
            return false
        }

        return try {
            // Enable BackendManager if switching to USB
            if (type == AudioBackendType.LIBUSB) {
                bridge.setUseBackendManager(true)
            }

            val success = bridge.selectBackend(type.id)
            if (success) {
                logger.info(TAG, "Audio backend switched to ${type.displayName}")
            } else {
                logger.error(TAG, "Failed to switch to backend ${type.displayName}")
            }
            success
        } catch (e: Exception) {
            logger.error(TAG, "Exception switching backend", e)
            false
        }
    }

    override fun getAudioBackend(): AudioBackendType {
        return try {
            AudioBackendType.fromId(bridge.getCurrentBackendType())
        } catch (e: Exception) {
            logger.error(TAG, "Exception getting backend type", e)
            AudioBackendType.OBOE
        }
    }

    override fun isUsbBackendAvailable(): Boolean {
        return try {
            bridge.isUsbBackendAvailable()
        } catch (e: Exception) {
            logger.error(TAG, "Exception checking USB backend availability", e)
            false
        }
    }

    // ==================== CLEANUP ====================

    override fun release() {
        logger.info(TAG, "Releasing audio engine")
        stopStatePolling()
        try {
            bridge.stopEngineSync()
        } catch (e: Exception) {
            logger.error(TAG, "Error during release", e)
        }
        scope.coroutineContext[Job]?.cancel()
    }

    // ==================== PRIVATE ====================

    private fun startStatePolling() {
        stopStatePolling()
        pollingJob = scope.launch {
            while (isActive) {
                try {
                    refreshStateFromNative()
                    val interval = when {
                        _state.value.isFading -> 16L
                        _state.value.isRunning -> 100L
                        else -> 500L
                    }
                    delay(interval)
                } catch (e: Exception) {
                    logger.error(TAG, "Error in state polling", e)
                    delay(200L)
                }
            }
        }
    }

    private fun stopStatePolling() {
        pollingJob?.cancel()
        pollingJob = null
    }

    private fun refreshStateFromNative() {
        // Check for errors
        if (bridge.hasStreamError()) {
            val errorCode = bridge.getLastStreamErrorCode()
            val error = AudioError.fromStreamError(errorCode)
            _state.update { it.copy(error = error) }
            analytics.onError(error)
            bridge.clearStreamError()
        }

        // Update lifecycle
        val lifecycle = EngineLifecycle.fromNativeCode(bridge.getEngineState())

        // Update volume/fade
        val currentFade = bridge.getCurrentFadeVolume()
        val targetFade = bridge.getTargetFadeVolume()
        val isFading = bridge.getIsFading()
        val fadeProgress = bridge.getFadeProgress()

        // Update paused state
        val isPaused = bridge.getIsPaused()

        _state.update {
            it.copy(
                lifecycle = lifecycle,
                isPaused = isPaused,
                currentFadeVolume = currentFade,
                targetFadeVolume = targetFade,
                isFading = isFading,
                fadeProgress = fadeProgress
            )
        }
    }

    private fun refreshStreamInfo() {
        val info = StreamInfo.fromNativeArray(bridge.getStreamInfoArray())
        if (info != null) {
            _state.update { it.copy(streamInfo = info) }
        }
    }
}
