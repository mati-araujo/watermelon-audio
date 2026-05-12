package com.watermellonstudios.audio.internal.bridge

import android.util.Log
import com.watermellonstudios.audio.api.EffectChainSnapshot
import com.watermellonstudios.audio.api.IAudioNativeBridge
import com.watermellonstudios.audio.api.IEffectStateProvider
import com.watermellonstudios.audio.api.IEffectStateWriter
import com.watermellonstudios.audio.api.NativeEffectSnapshot
import com.watermellonstudios.audio.domain.effect.EffectParameter
import com.watermellonstudios.audio.domain.effect.EffectType
import com.watermellonstudios.audio.domain.error.NativeBridgeException
import com.watermellonstudios.audio.domain.usb.StreamPreference
import com.watermellonstudios.audio.internal.native.NativeLibraryLoader
import com.watermellonstudios.audio.internal.optimization.EffectParameterUpdate
import com.watermellonstudios.audio.internal.optimization.JniMetrics
import com.watermellonstudios.audio.internal.optimization.SnapshotCacheConfig
import com.watermellonstudios.audio.internal.optimization.XYCoalescerConfig
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

/**
 * Unified native bridge for all audio engine JNI operations.
 *
 * ## Replaces
 *
 * This bridge consolidates and replaces:
 * - [NativeAudioBridge] (effects only, robust)
 * - [com.watermellonstudios.audio.internal.bridge.AudioNativeBridge] (raw JNI, deprecated)
 *
 * ## Architecture
 *
 * Operations are categorized into three types:
 *
 * ### 1. Lifecycle Operations (suspend, mutex-protected)
 * - Engine start/stop/pause/resume
 * - Protected by [lifecycleMutex]
 * - Return [Result] for error handling
 *
 * ### 2. State-Modifying Operations (suspend, mutex-protected)
 * - Effect chain modifications: add, remove, reorder, set parameters
 * - Mode changes: setAudioMode, vocoder configuration
 * - Input operations: start/stop input stream, monitoring
 * - Protected by category-specific mutexes
 * - Return [Result] for error handling
 *
 * ### 3. Real-Time Operations (non-suspend, lock-free)
 * - XY position updates
 * - Frequency/amplitude changes
 * - Oscillator type selection
 * - No mutex, uses atomic operations in C++
 * - Designed for high-frequency calls from touch handlers
 *
 * ## Thread Safety
 *
 * - Lifecycle and state operations are coroutine-safe via category mutexes
 * - Real-time operations are designed for UI thread, lock-free
 * - C++ side uses std::atomic for cross-thread communication
 *
 * ## Error Handling
 *
 * All mutable operations return [Result<T>]:
 * - Success: Contains the result value
 * - Failure: Contains [NativeBridgeException] with error code
 *
 * Error codes are synchronized with C++ JniError namespace.
 *
 * ## Usage Example
 *
 * ```kotlin
 * val bridge = AudioNativeBridge.getInstance()
 *
 * // Lifecycle (suspend)
 * bridge.startEngine().onFailure { handleError(it) }
 *
 * // Effects (suspend)
 * bridge.addEffect(EffectType.REVERB)
 *     .onSuccess { index -> println("Added at $index") }
 *
 * // Real-time (no suspend, call from UI thread)
 * bridge.setXY(0.5f, 0.5f)
 * ```
 *
 * @see IEffectStateProvider For read operations interface
 * @see IEffectStateWriter For write operations interface
 */
class AudioNativeBridge private constructor() : IAudioNativeBridge {

    companion object {
        private const val TAG = "AudioNativeBridge"
        const val MAX_EFFECTS = 12

        @Volatile
        private var instance: AudioNativeBridge? = null

        fun getInstance(): AudioNativeBridge {
            return instance ?: synchronized(this) {
                instance ?: AudioNativeBridge().also { instance = it }
            }
        }

        init {
            NativeLibraryLoader.ensureLoaded()
        }
    }

    // ==================== Category Mutexes ====================

    /** For engine lifecycle: start, stop, pause, resume */
    private val lifecycleMutex = Mutex()

    /** For effect chain modifications: add, remove, reorder, set parameters */
    private val effectsMutex = Mutex()

    /** For mode transitions */
    private val modeMutex = Mutex()

    /** For input node operations */
    private val inputMutex = Mutex()

    // Note: Real-time params (setXY, setFrequency) are lock-free

    // Track last known state version for change detection
    private var lastKnownVersion: Long = 0

    // ==================== Phase 4 Optimizations ====================

    // 4.3 Snapshot Cache
    private var cachedSnapshot: EffectChainSnapshot? = null
    private var cachedSnapshotVersion: Long = -1
    private var snapshotCacheConfig = SnapshotCacheConfig()

    /**
     * Configure snapshot caching behavior.
     */
    fun configureSnapshotCache(config: SnapshotCacheConfig) {
        snapshotCacheConfig = config
        if (!config.enabled) {
            invalidateSnapshotCache()
        }
    }

    /**
     * Invalidate the snapshot cache manually.
     */
    fun invalidateSnapshotCache() {
        cachedSnapshot = null
        cachedSnapshotVersion = -1
    }

    // 4.2 XY Coalescer
    private var xyCoalescerConfig = XYCoalescerConfig()
    private var pendingXYUpdate = AtomicBoolean(false)
    @Volatile
    private var coalescedX: Float = 0f
    @Volatile
    private var coalescedY: Float = 0f
    private val xyCoalescerScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    /**
     * Configure XY coalescing behavior.
     */
    fun configureXYCoalescer(config: XYCoalescerConfig) {
        xyCoalescerConfig = config
    }

    /**
     * Enable or disable JNI metrics collection (Phase 4.4).
     *
     * When enabled, tracks call counts and timing for JNI operations.
     * Use [getJniMetricsReport] to retrieve the metrics.
     */
    fun setJniMetricsEnabled(enabled: Boolean) {
        JniMetrics.enabled = enabled
    }

    /**
     * Get a formatted report of JNI operation metrics (Phase 4.4).
     *
     * @return Human-readable metrics report, or empty string if metrics disabled
     */
    fun getJniMetricsReport(): String {
        return if (JniMetrics.enabled) {
            JniMetrics.getReport()
        } else {
            "JNI metrics collection is disabled"
        }
    }

    /**
     * Reset JNI metrics to start a new measurement period (Phase 4.4).
     */
    fun resetJniMetrics() {
        JniMetrics.reset()
    }

    // ==================== Phase 4.1: Batch Operations ====================

    /**
     * Update parameters across multiple effects in a single JNI call (Phase 4.1).
     *
     * More efficient than calling setParameter() for each effect when updating
     * multiple parameters across different effects simultaneously.
     *
     * @param updates List of effect parameter updates
     * @return Result.success if all updates applied, Result.failure otherwise
     */
    suspend fun setMultipleEffectParameters(
        updates: List<EffectParameterUpdate>
    ): Result<Unit> = withContext(Dispatchers.Default) {
        if (updates.isEmpty()) return@withContext Result.success(Unit)

        effectsMutex.withLock {
            try {
                val chainSize = nativeGetEffectChainSize()

                // Validate all indices first
                for (update in updates) {
                    if (update.effectIndex < 0 || update.effectIndex >= chainSize) {
                        return@withContext Result.failure(
                            NativeBridgeException.InvalidEffectIndex(update.effectIndex, chainSize)
                        )
                    }
                }

                // Prepare arrays for JNI
                val effectIndices = IntArray(updates.size)
                val paramIds = IntArray(updates.size)
                val values = FloatArray(updates.size)

                updates.forEachIndexed { i, update ->
                    effectIndices[i] = update.effectIndex
                    paramIds[i] = update.paramId
                    values[i] = update.value
                }

                val result = JniMetrics.measured("setMultipleEffectParameters") {
                    nativeSetMultipleEffectParameters(effectIndices, paramIds, values)
                }

                if (result != 0) {
                    Log.e(TAG, "setMultipleEffectParameters: native returned error $result")
                    return@withContext Result.failure(
                        NativeBridgeException.fromCode(result, "setMultipleEffectParameters")
                    )
                }

                Log.d(TAG, "setMultipleEffectParameters: ${updates.size} updates applied")
                Result.success(Unit)

            } catch (e: Exception) {
                Log.e(TAG, "setMultipleEffectParameters: exception", e)
                Result.failure(e)
            }
        }
    }

    // ==================== Lifecycle Operations ====================

    /**
     * Start the audio engine.
     *
     * @return Result.success(Unit) if started, Result.failure with error otherwise
     */
    override suspend fun startEngine(): Result<Unit> = withContext(Dispatchers.Default) {
        lifecycleMutex.withLock {
            try {
                JniMetrics.measured("startEngine") {
                    nativeStartEngine()
                }
                Log.d(TAG, "startEngine: success")
                Result.success(Unit)
            } catch (e: Exception) {
                Log.e(TAG, "startEngine failed", e)
                Result.failure(e)
            }
        }
    }

    /**
     * Stop the audio engine.
     */
    override suspend fun stopEngine(): Result<Unit> = withContext(Dispatchers.Default) {
        lifecycleMutex.withLock {
            try {
                JniMetrics.measured("stopEngine") {
                    nativeStopEngine()
                }
                Log.d(TAG, "stopEngine: success")
                Result.success(Unit)
            } catch (e: Exception) {
                Log.e(TAG, "stopEngine failed", e)
                Result.failure(e)
            }
        }
    }

    /**
     * Start engine with fade-in.
     *
     * @param fadeTimeMs Fade duration in milliseconds
     */
    override suspend fun startEngineWithFade(fadeTimeMs: Int): Result<Unit> = withContext(Dispatchers.Default) {
        lifecycleMutex.withLock {
            try {
                nativeStartEngineWithFade(fadeTimeMs.coerceAtLeast(0))
                Log.d(TAG, "startEngineWithFade: fadeTimeMs=$fadeTimeMs")
                Result.success(Unit)
            } catch (e: Exception) {
                Log.e(TAG, "startEngineWithFade failed", e)
                Result.failure(e)
            }
        }
    }

    /**
     * Stop engine with fade-out.
     *
     * @param fadeTimeMs Fade duration in milliseconds
     */
    override suspend fun stopEngineWithFade(fadeTimeMs: Int): Result<Unit> = withContext(Dispatchers.Default) {
        lifecycleMutex.withLock {
            try {
                nativeStopEngineWithFade(fadeTimeMs.coerceAtLeast(0))
                Log.d(TAG, "stopEngineWithFade: fadeTimeMs=$fadeTimeMs")
                Result.success(Unit)
            } catch (e: Exception) {
                Log.e(TAG, "stopEngineWithFade failed", e)
                Result.failure(e)
            }
        }
    }

    /**
     * Pause engine with fade-out.
     *
     * @param fadeTimeMs Fade duration in milliseconds
     */
    override suspend fun pauseEngineWithFade(fadeTimeMs: Int): Result<Unit> = withContext(Dispatchers.Default) {
        lifecycleMutex.withLock {
            try {
                nativePauseEngineWithFade(fadeTimeMs.coerceAtLeast(0))
                Log.d(TAG, "pauseEngineWithFade: fadeTimeMs=$fadeTimeMs")
                Result.success(Unit)
            } catch (e: Exception) {
                Log.e(TAG, "pauseEngineWithFade failed", e)
                Result.failure(e)
            }
        }
    }

    /**
     * Resume engine with fade-in.
     *
     * @param fadeTimeMs Fade duration in milliseconds
     */
    override suspend fun resumeEngineWithFade(fadeTimeMs: Int): Result<Unit> = withContext(Dispatchers.Default) {
        lifecycleMutex.withLock {
            try {
                nativeResumeEngineWithFade(fadeTimeMs.coerceAtLeast(0))
                Log.d(TAG, "resumeEngineWithFade: fadeTimeMs=$fadeTimeMs")
                Result.success(Unit)
            } catch (e: Exception) {
                Log.e(TAG, "resumeEngineWithFade failed", e)
                Result.failure(e)
            }
        }
    }

    // ==================== Lifecycle Operations (Synchronous - Legacy Compatibility) ====================

    /**
     * Start engine with fade-in synchronously (for legacy callers).
     */
    override fun startEngineWithFadeSync(fadeTimeMs: Int) = nativeStartEngineWithFade(fadeTimeMs.coerceAtLeast(0))

    /**
     * Stop engine with fade-out synchronously (for legacy callers).
     */
    override fun stopEngineWithFadeSync(fadeTimeMs: Int) = nativeStopEngineWithFade(fadeTimeMs.coerceAtLeast(0))

    /**
     * Pause engine with fade-out synchronously (for legacy callers).
     */
    override fun pauseEngineWithFadeSync(fadeTimeMs: Int) = nativePauseEngineWithFade(fadeTimeMs.coerceAtLeast(0))

    /**
     * Resume engine with fade-in synchronously (for legacy callers).
     */
    override fun resumeEngineWithFadeSync(fadeTimeMs: Int) = nativeResumeEngineWithFade(fadeTimeMs.coerceAtLeast(0))

    /**
     * Stop engine synchronously (for legacy callers).
     */
    override fun stopEngineSync() = nativeStopEngine()

    // ==================== State Queries (No mutex needed) ====================

    /**
     * Get current engine state.
     *
     * @return 0=Stopped, 1=Starting, 2=Running, 3=Stopping
     */
    override fun getEngineState(): Int = nativeGetEngineState()

    /**
     * Check if engine is paused.
     */
    fun isPaused(): Boolean = nativeGetIsPaused()

    /**
     * Check if engine is paused (legacy alias).
     */
    override fun getIsPaused(): Boolean = nativeGetIsPaused()

    /**
     * Get state version for sync detection.
     */
    override fun getStateVersion(): Long = nativeGetStateVersion()

    /**
     * Check if stream has error.
     */
    override fun hasStreamError(): Boolean = nativeHasStreamError()

    /**
     * Get last stream error code.
     */
    override fun getLastStreamErrorCode(): Int = nativeGetLastStreamErrorCode()

    /**
     * Clear stream error flag.
     */
    override fun clearStreamError() = nativeClearStreamError()

    /**
     * Check if initialization has failed.
     */
    override fun hasInitializationFailed(): Boolean = nativeHasInitializationFailed()

    /**
     * Check if engine is initialized.
     */
    override fun isEngineInitialized(): Boolean {
        return try {
            nativeIsEngineInitialized()
        } catch (e: Exception) {
            Log.e(TAG, "isEngineInitialized: exception", e)
            false
        }
    }

    /**
     * Get stream info if available.
     *
     * @return Triple of (sampleRate, bufferSize, latencyMs) or null
     */
    fun getStreamInfo(): Triple<Int, Int, Float>? {
        val info = nativeGetStreamInfo() ?: return null
        if (info.size < 3) return null
        return Triple(info[0].toInt(), info[1].toInt(), info[2])
    }

    /**
     * Get stream info as FloatArray (legacy compatibility).
     * Used by StreamInfo.fromNativeArray().
     *
     * @return FloatArray [sampleRate, bufferSize, latencyMs] or null
     */
    override fun getStreamInfoArray(): FloatArray? = nativeGetStreamInfo()

    // ==================== Volume Operations ====================

    /**
     * Set master volume.
     *
     * @param volume Volume level (0.0 to 1.0)
     */
    override fun setMasterVolume(volume: Float) {
        if (!volume.isFinite()) return
        nativeSetMasterVolume(volume.coerceIn(0f, 1f))
    }

    /**
     * Get current fade volume.
     */
    override fun getCurrentFadeVolume(): Float = nativeGetCurrentFadeVolume()

    /**
     * Get target fade volume.
     */
    override fun getTargetFadeVolume(): Float = nativeGetTargetFadeVolume()

    /**
     * Check if currently fading.
     */
    fun isFading(): Boolean = nativeGetIsFading()

    /**
     * Check if currently fading (legacy alias).
     */
    override fun getIsFading(): Boolean = nativeGetIsFading()

    /**
     * Get fade progress (0.0 to 1.0).
     */
    override fun getFadeProgress(): Float = nativeGetFadeProgress()

    // ==================== Output Level Metering (Phase 1 - Gain Staging) ====================

    /**
     * Get current master volume.
     *
     * @return Volume level (0.0 to 1.0)
     */
    override fun getMasterVolume(): Float = nativeGetMasterVolume()

    /**
     * Get output peak level for a channel (linear).
     *
     * @param channel 0 = left, 1 = right
     * @return Peak level (0.0 to 1.0+)
     */
    fun getOutputPeakLevel(channel: Int): Float = nativeGetOutputPeakLevel(channel)

    /**
     * Get output RMS level for a channel (linear).
     *
     * @param channel 0 = left, 1 = right
     * @return RMS level (0.0 to 1.0+)
     */
    fun getOutputRmsLevel(channel: Int): Float = nativeGetOutputRmsLevel(channel)

    /**
     * Get output peak level for a channel (in dB).
     *
     * @param channel 0 = left, 1 = right
     * @return Peak level in dB (-100.0 to 0.0+)
     */
    fun getOutputPeakLevelDb(channel: Int): Float = nativeGetOutputPeakLevelDb(channel)

    /**
     * Get output RMS level for a channel (in dB).
     *
     * @param channel 0 = left, 1 = right
     * @return RMS level in dB (-100.0 to 0.0+)
     */
    fun getOutputRmsLevelDb(channel: Int): Float = nativeGetOutputRmsLevelDb(channel)

    /**
     * Get all output levels in a single call (more efficient for metering UI).
     *
     * @return FloatArray [peakL, peakR, rmsL, rmsR] or null if engine not running
     */
    fun getOutputLevels(): FloatArray? = nativeGetOutputLevels()

    /**
     * Data class for stereo output levels.
     */
    data class StereoLevels(
        val peakLeft: Float,
        val peakRight: Float,
        val rmsLeft: Float,
        val rmsRight: Float
    ) {
        val peakMax: Float get() = maxOf(peakLeft, peakRight)
        val rmsAverage: Float get() = (rmsLeft + rmsRight) / 2f
    }

    /**
     * Get stereo output levels as a data class.
     *
     * @return StereoLevels or null if engine not running
     */
    fun getStereoOutputLevels(): StereoLevels? {
        val levels = nativeGetOutputLevels() ?: return null
        if (levels.size < 4) return null
        return StereoLevels(
            peakLeft = levels[0],
            peakRight = levels[1],
            rmsLeft = levels[2],
            rmsRight = levels[3]
        )
    }

    // ==================== Real-Time Operations (Lock-free) ====================

    /**
     * Set XY position for oscillator control.
     *
     * This is a real-time operation, designed to be called frequently
     * from touch handlers. It uses lock-free atomics in C++.
     *
     * When coalescing is enabled via [configureXYCoalescer], rapid updates
     * are combined into a single JNI call per window period.
     *
     * @param x X position (0.0 to 1.0)
     * @param y Y position (0.0 to 1.0)
     * @param coalesce If true, uses coalescing when enabled. Default follows config.
     */
    override fun setXY(x: Float, y: Float, coalesce: Boolean) {
        if (!x.isFinite() || !y.isFinite()) {
            Log.w(TAG, "setXY: invalid values")
            return
        }

        val clampedX = x.coerceIn(0f, 1f)
        val clampedY = y.coerceIn(0f, 1f)

        // 4.2: XY Coalescing for high-frequency updates
        if (coalesce && xyCoalescerConfig.enabled) {
            coalescedX = clampedX
            coalescedY = clampedY

            if (pendingXYUpdate.compareAndSet(false, true)) {
                xyCoalescerScope.launch {
                    delay(xyCoalescerConfig.windowMs)
                    pendingXYUpdate.set(false)
                    // Send final coalesced value
                    JniMetrics.measured("setXY") {
                        nativeSetXY(coalescedX, coalescedY)
                    }
                }
            }
        } else {
            // Direct call without coalescing
            JniMetrics.measured("setXY") {
                nativeSetXY(clampedX, clampedY)
            }
        }
    }

    /**
     * Set frequency and amplitude directly.
     *
     * @param frequency Frequency in Hz (20-20000)
     * @param amplitude Amplitude (0.0 to 1.0)
     */
    override fun setFrequencyAndAmplitude(frequency: Float, amplitude: Float) {
        if (!frequency.isFinite() || !amplitude.isFinite()) {
            Log.w(TAG, "setFrequencyAndAmplitude: invalid values")
            return
        }
        nativeSetFrequencyAndAmplitude(
            frequency.coerceIn(20f, 20000f),
            amplitude.coerceIn(0f, 1f)
        )
    }

    /**
     * Set the dynamic frequency range for XY mapping (Phase 10A).
     * Lock-free, safe to call at any time.
     *
     * @param minHz Minimum frequency in Hz (8-20000)
     * @param maxHz Maximum frequency in Hz (8-20000)
     */
    override fun setFrequencyRange(minHz: Float, maxHz: Float) {
        if (!minHz.isFinite() || !maxHz.isFinite() || maxHz <= minHz) {
            Log.w(TAG, "setFrequencyRange: invalid range ($minHz, $maxHz)")
            return
        }
        val clampedMin = minHz.coerceIn(8f, 20000f)
        val clampedMax = maxHz.coerceIn(8f, 20000f)
        if (clampedMax <= clampedMin) return
        nativeSetFrequencyRange(clampedMin, clampedMax)
    }

    /**
     * Set oscillator type.
     *
     * @param type 0=Sine, 1=Square, 2=Saw, 3=Triangle, 4=Noise
     */
    override fun setOscillatorType(type: Int) {
        if (type !in 0..4) {
            Log.w(TAG, "setOscillatorType: invalid type $type")
            return
        }
        nativeSetOscillatorType(type)
    }

    // ==================== Synth Engine Operations (Phase 6) ====================

    /**
     * Set the active synthesis engine type.
     *
     * @param type 0=Classic, 1=KarplusStrong, 2=FM, 3=Wavetable, 4=Granular, 5=Supersaw
     */
    override fun setEngineType(type: Int) {
        if (type < 0 || type > 6) {
            Log.w(TAG, "setEngineType: invalid type $type")
            return
        }
        nativeSetEngineType(type)
    }

    /**
     * Set a parameter on the current synth engine.
     *
     * @param paramId Parameter index (0 to 5)
     * @param value Parameter value (typically 0.0-1.0)
     */
    override fun setEngineParameter(paramId: Int, value: Float) {
        if (paramId < 0 || paramId > 5) {
            Log.w(TAG, "setEngineParameter: invalid paramId $paramId")
            return
        }
        nativeSetEngineParameter(paramId, value)
    }

    /**
     * Get the current engine type.
     *
     * @return Engine type ID
     */
    override fun getEngineType(): Int {
        return nativeGetEngineType()
    }

    // ========== SOUNDFONT ENGINE (Phase 8) ==========

    /**
     * Load a SoundFont from raw .sf2 file bytes.
     *
     * @param data Raw SF2 file content
     * @return true if loading succeeded
     */
    fun loadSoundFont(data: ByteArray): Boolean {
        if (data.isEmpty()) {
            Log.w(TAG, "loadSoundFont: empty data")
            return false
        }
        return nativeLoadSoundFont(data)
    }

    /**
     * Load a SoundFont from a file path using mmap (zero-copy).
     * Preferred over loadSoundFont(ByteArray) for large files — avoids
     * JVM heap allocation and JNI byte array copy.
     *
     * @param path Absolute path to .sf2 file
     * @return true if loading succeeded
     */
    fun loadSoundFontFromPath(path: String): Boolean {
        if (path.isBlank()) {
            Log.w(TAG, "loadSoundFontFromPath: empty path")
            return false
        }
        return nativeLoadSoundFontFromPath(path)
    }

    /**
     * Unload the current SoundFont.
     */
    fun unloadSoundFont() {
        nativeUnloadSoundFont()
    }

    /**
     * Set the active preset for SoundFont engine.
     *
     * @param presetIndex Preset index (0 to presetCount-1)
     */
    fun setSoundFontPreset(presetIndex: Int) {
        if (presetIndex < 0) {
            Log.w(TAG, "setSoundFontPreset: invalid index $presetIndex")
            return
        }
        nativeSetSoundFontPreset(presetIndex)
    }

    /**
     * Get number of presets in loaded SoundFont.
     */
    fun getSoundFontPresetCount(): Int {
        return nativeGetSoundFontPresetCount()
    }

    /**
     * Get preset name by index.
     *
     * @return Preset name, or null if invalid
     */
    fun getSoundFontPresetName(presetIndex: Int): String? {
        return nativeGetSoundFontPresetName(presetIndex)
    }

    /**
     * Check if a SoundFont is loaded.
     */
    fun isSoundFontLoaded(): Boolean {
        return nativeIsSoundFontLoaded()
    }

    /**
     * Get the MIDI key range for a SoundFont preset (Phase 10B).
     * @param presetIndex Preset index (0-based)
     * @return IntArray [minKey, maxKey] or null if preset has no regions
     */
    fun getSoundFontPresetKeyRange(presetIndex: Int): IntArray? {
        return nativeGetSoundFontPresetKeyRange(presetIndex)
    }

    // ========== SOUNDFONT POLYPHONY (Phase 8E) ==========

    /**
     * Start/update a SoundFont note for a touch point.
     * Lock-free — safe to call at touch rate.
     */
    fun sfNoteOn(touchId: Int, midiNote: Int, velocity: Float) {
        nativeSfNoteOn(touchId, midiNote, velocity)
    }

    /**
     * Release a SoundFont note for a touch point.
     */
    fun sfNoteOff(touchId: Int) {
        nativeSfNoteOff(touchId)
    }

    /**
     * Release all SoundFont notes.
     */
    fun sfNoteOffAll() {
        nativeSfNoteOffAll()
    }

    /**
     * Get waveform samples for visualization.
     *
     * @param buffer Buffer to fill with samples
     * @param size Number of samples to retrieve
     * @return Number of samples written
     */
    override fun getWaveformSamples(buffer: FloatArray, size: Int): Int {
        return nativeGetWaveformSamples(buffer, size.coerceIn(0, buffer.size))
    }

    // ==================== Modulator Operations ====================

    /**
     * Set modulator type.
     *
     * @param type Modulator type ID
     */
    override fun setModulatorType(type: Int) {
        nativeSetModulatorType(type)
    }

    /**
     * Set modulator parameter.
     *
     * @param paramId Parameter ID
     * @param value Parameter value
     */
    override fun setModulatorParameter(paramId: Int, value: Float) {
        if (!value.isFinite()) return
        nativeSetModulatorParameter(paramId, value)
    }

    // ==================== Voice Filter Operations (Phase 6) ====================

    fun setVoiceFilterEnabled(enabled: Boolean) {
        nativeSetVoiceFilterEnabled(enabled)
    }

    fun setVoiceFilterCutoff(hz: Float) {
        if (!hz.isFinite() || hz < 20f || hz > 20000f) return
        nativeSetVoiceFilterCutoff(hz)
    }

    fun setVoiceFilterResonance(q: Float) {
        if (!q.isFinite() || q < 0f || q > 1f) return
        nativeSetVoiceFilterResonance(q)
    }

    fun setVoiceFilterMode(mode: Int) {
        if (mode < 0 || mode > 2) return
        nativeSetVoiceFilterMode(mode)
    }

    // ==================== Effect Operations (Synchronous - Legacy Compatibility) ====================

    /**
     * Add effect synchronously (for legacy callers).
     *
     * @param typeId Effect type ID
     * @return true if added successfully
     */
    override fun addEffectSync(typeId: Int): Boolean {
        if (nativeGetEffectChainSize() >= MAX_EFFECTS) return false
        return nativeAddEffect(typeId) >= 0
    }

    /**
     * Remove effect synchronously (for legacy callers).
     */
    override fun removeEffectSync(index: Int) {
        if (index >= 0 && index < nativeGetEffectChainSize()) {
            nativeRemoveEffect(index)
        }
    }

    /**
     * Set effect parameter synchronously (for legacy callers).
     */
    override fun setEffectParameterSync(effectIndex: Int, paramId: Int, value: Float) {
        if (effectIndex >= 0 && effectIndex < nativeGetEffectChainSize() && value.isFinite()) {
            nativeSetEffectParameter(effectIndex, paramId, value)
        }
    }

    /**
     * Get effect parameter synchronously (for legacy callers).
     */
    override fun getEffectParameterSync(effectIndex: Int, paramId: Int): Float {
        if (effectIndex < 0 || effectIndex >= nativeGetEffectChainSize()) return 0f
        return nativeGetEffectParameter(effectIndex, paramId)
    }

    /**
     * Set effect bypass synchronously (for legacy callers).
     */
    override fun setEffectBypassSync(index: Int, bypass: Boolean) {
        if (index >= 0 && index < nativeGetEffectChainSize()) {
            nativeSetEffectBypass(index, bypass)
        }
    }

    // ==================== Global BPM ====================

    /**
     * Set global BPM for tempo-synced effects.
     * Lock-free: safe to call from any thread.
     */
    override fun setBpm(bpm: Float) {
        nativeSetBpm(bpm)
    }

    /**
     * Get current global BPM.
     */
    override fun getBpm(): Float = nativeGetBpm()

    // ==================== Effect Routing Mode ====================

    /**
     * Set routing mode for the effect chain.
     * Lock-free: safe to call from any thread.
     * @param mode RoutingMode ordinal (0=Serial, 1=Parallel, 2=Split2x2, 3=SerialParallel, 4=ParallelSerial, 5=Feedback)
     */
    override fun setRoutingMode(mode: Int) {
        nativeSetRoutingMode(mode)
    }

    override fun getRoutingMode(): Int = nativeGetRoutingMode()

    /**
     * Set parallel mix balance (0.0=branchA, 1.0=branchB).
     * Lock-free.
     */
    override fun setParallelMix(mix: Float) {
        nativeSetParallelMix(mix)
    }

    /**
     * Set feedback amount (clamped to 0.0-0.95 in C++).
     * Lock-free.
     */
    override fun setFeedbackAmount(amount: Float) {
        nativeSetFeedbackAmount(amount)
    }

    /**
     * Reorder effects synchronously (for legacy callers).
     */
    override fun reorderEffectsSync(fromIndex: Int, toIndex: Int) {
        val chainSize = nativeGetEffectChainSize()
        if (fromIndex in 0 until chainSize && toIndex in 0 until chainSize) {
            nativeReorderEffects(fromIndex, toIndex)
        }
    }

    // ==================== IEffectStateWriter Implementation ====================

    override suspend fun addEffect(type: EffectType): Result<Int> = withContext(Dispatchers.Default) {
        effectsMutex.withLock {
            try {
                val engineInit = isEngineInitialized()
                Log.d(TAG, "addEffect: engineInitialized=$engineInit")

                if (!engineInit) {
                    Log.e(TAG, "addEffect: engine NOT initialized!")
                    return@withContext Result.failure(
                        NativeBridgeException.EngineNotInitialized()
                    )
                }

                val currentSize = nativeGetEffectChainSize()
                Log.d(TAG, "addEffect: currentSize=$currentSize, MAX_EFFECTS=$MAX_EFFECTS")

                if (currentSize >= MAX_EFFECTS) {
                    Log.w(TAG, "addEffect: chain full")
                    return@withContext Result.failure(
                        NativeBridgeException.EffectChainFull(MAX_EFFECTS)
                    )
                }

                val result = JniMetrics.measured("addEffect") {
                    nativeAddEffect(type.id)
                }

                if (result < 0) {
                    Log.e(TAG, "addEffect: native returned error $result")
                    return@withContext Result.failure(
                        NativeBridgeException.fromCode(result, "addEffect")
                    )
                }

                // Invalidate snapshot cache after adding effect
                invalidateSnapshotCache()

                Log.d(TAG, "addEffect: type=${type.displayName}, index=$result")
                Result.success(result)

            } catch (e: Exception) {
                Log.e(TAG, "addEffect: exception", e)
                Result.failure(e)
            }
        }
    }

    override suspend fun removeEffect(index: Int): Result<Unit> = withContext(Dispatchers.Default) {
        effectsMutex.withLock {
            try {
                val chainSize = nativeGetEffectChainSize()
                if (index < 0 || index >= chainSize) {
                    return@withContext Result.failure(
                        NativeBridgeException.InvalidEffectIndex(index, chainSize)
                    )
                }

                val result = JniMetrics.measured("removeEffect") {
                    nativeRemoveEffect(index)
                }

                if (result != 0) {
                    Log.e(TAG, "removeEffect: native returned error $result")
                    return@withContext Result.failure(
                        NativeBridgeException.fromCode(result, "removeEffect")
                    )
                }

                // Invalidate snapshot cache after removing effect
                invalidateSnapshotCache()

                Log.d(TAG, "removeEffect: index=$index")
                Result.success(Unit)

            } catch (e: Exception) {
                Log.e(TAG, "removeEffect: exception", e)
                Result.failure(e)
            }
        }
    }

    override suspend fun setParameter(
        effectIndex: Int,
        paramId: Int,
        value: Float
    ): Result<Unit> = withContext(Dispatchers.Default) {
        effectsMutex.withLock {
            try {
                val chainSize = nativeGetEffectChainSize()
                if (effectIndex < 0 || effectIndex >= chainSize) {
                    return@withContext Result.failure(
                        NativeBridgeException.InvalidEffectIndex(effectIndex, chainSize)
                    )
                }

                val effectType = getEffectTypeSync(effectIndex)
                if (effectType == null) {
                    return@withContext Result.failure(
                        NativeBridgeException.InvalidEffectIndex(effectIndex)
                    )
                }

                val validationResult = validateParameter(effectType, paramId, value)
                if (validationResult.isFailure) {
                    return@withContext Result.failure(validationResult.exceptionOrNull()!!)
                }

                val validatedValue = validationResult.getOrThrow()

                val result = JniMetrics.measured("setParameter") {
                    nativeSetEffectParameter(effectIndex, paramId, validatedValue)
                }

                if (result != 0) {
                    Log.e(TAG, "setParameter: native returned error $result")
                    return@withContext Result.failure(
                        NativeBridgeException.fromCode(result, "setParameter")
                    )
                }

                Result.success(Unit)

            } catch (e: Exception) {
                Log.e(TAG, "setParameter: exception", e)
                Result.failure(e)
            }
        }
    }

    override suspend fun setParametersBatch(
        effectIndex: Int,
        parameters: Map<Int, Float>
    ): Result<Unit> = withContext(Dispatchers.Default) {
        if (parameters.isEmpty()) return@withContext Result.success(Unit)

        effectsMutex.withLock {
            try {
                val chainSize = nativeGetEffectChainSize()
                if (effectIndex < 0 || effectIndex >= chainSize) {
                    return@withContext Result.failure(
                        NativeBridgeException.InvalidEffectIndex(effectIndex, chainSize)
                    )
                }

                val effectType = getEffectTypeSync(effectIndex)
                if (effectType == null) {
                    return@withContext Result.failure(
                        NativeBridgeException.InvalidEffectIndex(effectIndex)
                    )
                }

                val validatedParams = mutableMapOf<Int, Float>()
                for ((paramId, value) in parameters) {
                    val validationResult = validateParameter(effectType, paramId, value)
                    if (validationResult.isFailure) {
                        return@withContext Result.failure(validationResult.exceptionOrNull()!!)
                    }
                    validatedParams[paramId] = validationResult.getOrThrow()
                }

                val paramIds = validatedParams.keys.toIntArray()
                val values = validatedParams.values.toFloatArray()

                val result = JniMetrics.measured("setParametersBatch") {
                    nativeSetEffectParametersBatch(effectIndex, paramIds, values)
                }

                if (result != 0) {
                    Log.e(TAG, "setParametersBatch: native returned error $result")
                    return@withContext Result.failure(
                        NativeBridgeException.fromCode(result, "setParametersBatch")
                    )
                }

                Log.d(TAG, "setParametersBatch: ${parameters.size} params for effect $effectIndex")
                Result.success(Unit)

            } catch (e: Exception) {
                Log.e(TAG, "setParametersBatch: exception", e)
                Result.failure(e)
            }
        }
    }

    override suspend fun setBypass(effectIndex: Int, bypassed: Boolean): Result<Unit> =
        withContext(Dispatchers.Default) {
            effectsMutex.withLock {
                try {
                    val chainSize = nativeGetEffectChainSize()
                    if (effectIndex < 0 || effectIndex >= chainSize) {
                        return@withContext Result.failure(
                            NativeBridgeException.InvalidEffectIndex(effectIndex, chainSize)
                        )
                    }

                    val result = nativeSetEffectBypass(effectIndex, bypassed)

                    if (result != 0) {
                        Log.e(TAG, "setBypass: native returned error $result")
                        return@withContext Result.failure(
                            NativeBridgeException.fromCode(result, "setBypass")
                        )
                    }

                    Log.d(TAG, "setBypass: index=$effectIndex, bypassed=$bypassed")
                    Result.success(Unit)

                } catch (e: Exception) {
                    Log.e(TAG, "setBypass: exception", e)
                    Result.failure(e)
                }
            }
        }

    override suspend fun reorderEffects(fromIndex: Int, toIndex: Int): Result<Unit> =
        withContext(Dispatchers.Default) {
            effectsMutex.withLock {
                try {
                    val chainSize = nativeGetEffectChainSize()

                    if (fromIndex < 0 || fromIndex >= chainSize) {
                        return@withContext Result.failure(
                            NativeBridgeException.InvalidEffectIndex(fromIndex, chainSize)
                        )
                    }
                    if (toIndex < 0 || toIndex >= chainSize) {
                        return@withContext Result.failure(
                            NativeBridgeException.InvalidEffectIndex(toIndex, chainSize)
                        )
                    }

                    if (fromIndex == toIndex) {
                        return@withContext Result.success(Unit)
                    }

                    val result = nativeReorderEffects(fromIndex, toIndex)

                    if (result != 0) {
                        Log.e(TAG, "reorderEffects: native returned error $result")
                        return@withContext Result.failure(
                            NativeBridgeException.fromCode(result, "reorderEffects")
                        )
                    }

                    Log.d(TAG, "reorderEffects: $fromIndex -> $toIndex")
                    Result.success(Unit)

                } catch (e: Exception) {
                    Log.e(TAG, "reorderEffects: exception", e)
                    Result.failure(e)
                }
            }
        }

    override suspend fun clearAllEffects(): Result<Unit> = withContext(Dispatchers.Default) {
        effectsMutex.withLock {
            try {
                var chainSize = nativeGetEffectChainSize()

                while (chainSize > 0) {
                    val result = nativeRemoveEffect(chainSize - 1)
                    if (result != 0) {
                        Log.e(TAG, "clearAllEffects: failed to remove effect at ${chainSize - 1}")
                        return@withContext Result.failure(
                            NativeBridgeException.fromCode(result, "clearAllEffects")
                        )
                    }
                    chainSize = nativeGetEffectChainSize()
                }

                Log.d(TAG, "clearAllEffects: all effects removed")
                Result.success(Unit)

            } catch (e: Exception) {
                Log.e(TAG, "clearAllEffects: exception", e)
                Result.failure(e)
            }
        }
    }

    // ==================== IEffectStateProvider Implementation ====================

    override suspend fun getEffectChainSnapshot(): EffectChainSnapshot =
        withContext(Dispatchers.Default) {
            // 4.3: Check cache first if enabled
            if (snapshotCacheConfig.enabled) {
                val currentVersion = nativeGetStateVersion()
                cachedSnapshot?.let { cached ->
                    if (cachedSnapshotVersion == currentVersion) {
                        return@withContext cached
                    }
                }
            }

            effectsMutex.withLock {
                JniMetrics.measured("getEffectChainSnapshot") {
                    val effects = mutableListOf<NativeEffectSnapshot>()
                    val chainSize = nativeGetEffectChainSize()
                    val engineInitialized = isEngineInitialized()

                    if (chainSize > 0 || !engineInitialized) {
                        Log.d(TAG, "getEffectChainSnapshot: chainSize=$chainSize, engineInitialized=$engineInitialized")
                    }

                    for (i in 0 until chainSize) {
                        val typeId = nativeGetEffectType(i)
                        val isBypassed = nativeIsEffectBypassed(i)
                        val params = getEffectParametersSync(i, typeId)

                        effects.add(
                            NativeEffectSnapshot(
                                index = i,
                                typeId = typeId,
                                isBypassed = isBypassed,
                                parameters = params
                            )
                        )
                    }

                    val version = nativeGetStateVersion()
                    lastKnownVersion = version

                    val snapshot = EffectChainSnapshot(
                        effects = effects,
                        version = version
                    )

                    // 4.3: Update cache
                    if (snapshotCacheConfig.enabled) {
                        cachedSnapshot = snapshot
                        cachedSnapshotVersion = version
                    }

                    snapshot
                }
            }
        }

    /**
     * Get effect chain snapshot with explicit caching (Phase 4.3).
     *
     * Returns cached snapshot if state version hasn't changed.
     */
    suspend fun getEffectChainSnapshotCached(): EffectChainSnapshot {
        val currentVersion = nativeGetStateVersion()

        // Return cached if version matches
        cachedSnapshot?.let { cached ->
            if (cachedSnapshotVersion == currentVersion) {
                return cached
            }
        }

        // Fetch fresh snapshot (this will also update cache)
        return getEffectChainSnapshot()
    }

    override suspend fun getEffectParameters(index: Int): Map<Int, Float> =
        withContext(Dispatchers.Default) {
            effectsMutex.withLock {
                val chainSize = nativeGetEffectChainSize()
                if (index < 0 || index >= chainSize) {
                    throw NativeBridgeException.InvalidEffectIndex(index, chainSize)
                }

                val typeId = nativeGetEffectType(index)
                getEffectParametersSync(index, typeId)
            }
        }

    override suspend fun isEffectBypassed(index: Int): Boolean = withContext(Dispatchers.Default) {
        effectsMutex.withLock {
            val chainSize = nativeGetEffectChainSize()
            if (index < 0 || index >= chainSize) {
                throw NativeBridgeException.InvalidEffectIndex(index, chainSize)
            }
            nativeIsEffectBypassed(index)
        }
    }

    override suspend fun getEffectCount(): Int = withContext(Dispatchers.Default) {
        effectsMutex.withLock {
            nativeGetEffectChainSize()
        }
    }

    override suspend fun getEffectType(index: Int): EffectType? = withContext(Dispatchers.Default) {
        effectsMutex.withLock {
            val chainSize = nativeGetEffectChainSize()
            if (index < 0 || index >= chainSize) {
                return@withContext null
            }
            val typeId = nativeGetEffectType(index)
            EffectType.fromId(typeId)
        }
    }

    /**
     * Checks if state has changed since last snapshot.
     */
    suspend fun hasStateChanged(): Boolean = withContext(Dispatchers.Default) {
        val currentVersion = nativeGetStateVersion()
        currentVersion != lastKnownVersion
    }

    // ==================== Mode System Operations ====================

    /**
     * Set audio mode.
     *
     * @param mode 0=CHAOS_PAD, 1=INPUT_FX, 2=MIX
     */
    override suspend fun setAudioMode(mode: Int): Result<Unit> = withContext(Dispatchers.Default) {
        modeMutex.withLock {
            try {
                if (mode !in 0..2) {
                    return@withContext Result.failure(
                        IllegalArgumentException("Invalid mode: $mode")
                    )
                }
                nativeSetAudioMode(mode)
                Log.d(TAG, "setAudioMode: mode=$mode")
                Result.success(Unit)
            } catch (e: Exception) {
                Log.e(TAG, "setAudioMode failed", e)
                Result.failure(e)
            }
        }
    }

    /**
     * Get current audio mode.
     */
    override fun getAudioMode(): Int = nativeGetAudioMode()

    /**
     * Check if mode transition is in progress.
     */
    override fun isInModeTransition(): Boolean = nativeIsInModeTransition()

    /**
     * Get mode transition progress (0.0 to 1.0).
     */
    fun getModeTransitionProgress(): Float = nativeGetModeTransitionProgress()

    /**
     * Get mode name for display.
     */
    fun getModeName(mode: Int): String = nativeGetModeName(mode)

    /**
     * Check if mode requires input.
     */
    fun modeRequiresInput(mode: Int): Boolean = nativeModeRequiresInput(mode)

    // ==================== Input Operations ====================

    /**
     * Start input stream for mic/line input.
     */
    suspend fun startInputStream(): Result<Boolean> = withContext(Dispatchers.Default) {
        inputMutex.withLock {
            try {
                val success = nativeStartInputStream()
                Log.d(TAG, "startInputStream: success=$success")
                Result.success(success)
            } catch (e: Exception) {
                Log.e(TAG, "startInputStream failed", e)
                Result.failure(e)
            }
        }
    }

    /**
     * Start input stream synchronously (for legacy callers).
     * @return true if started successfully
     */
    fun startInputStreamSync(): Boolean = nativeStartInputStream()

    /**
     * Stop input stream.
     */
    suspend fun stopInputStream(): Result<Unit> = withContext(Dispatchers.Default) {
        inputMutex.withLock {
            try {
                nativeStopInputStream()
                Log.d(TAG, "stopInputStream: success")
                Result.success(Unit)
            } catch (e: Exception) {
                Log.e(TAG, "stopInputStream failed", e)
                Result.failure(e)
            }
        }
    }

    /**
     * Stop input stream synchronously (for legacy callers).
     */
    fun stopInputStreamSync() = nativeStopInputStream()

    /**
     * Check if input stream is running.
     */
    fun isInputStreamRunning(): Boolean = nativeIsInputStreamRunning()

    /**
     * Set input source.
     */
    suspend fun setInputSource(source: Int): Result<Unit> = withContext(Dispatchers.Default) {
        inputMutex.withLock {
            try {
                nativeSetInputSource(source)
                Result.success(Unit)
            } catch (e: Exception) {
                Log.e(TAG, "setInputSource failed", e)
                Result.failure(e)
            }
        }
    }

    /**
     * Set input source synchronously (for legacy callers).
     */
    fun setInputSourceSync(source: Int) = nativeSetInputSource(source)

    /**
     * Get current input source.
     */
    fun getInputSource(): Int = nativeGetInputSource()

    /**
     * Set input gain in dB.
     */
    fun setInputGain(gainDb: Float) {
        if (!gainDb.isFinite()) return
        nativeSetInputGain(gainDb)
    }

    /**
     * Get current input gain.
     */
    fun getInputGain(): Float = nativeGetInputGain()

    /**
     * Set noise gate enabled.
     */
    fun setNoiseGateEnabled(enabled: Boolean) = nativeSetNoiseGateEnabled(enabled)

    /**
     * Check if noise gate is enabled.
     */
    fun isNoiseGateEnabled(): Boolean = nativeIsNoiseGateEnabled()

    /**
     * Set noise gate threshold in dB.
     */
    fun setNoiseGateThreshold(thresholdDb: Float) {
        if (!thresholdDb.isFinite()) return
        nativeSetNoiseGateThreshold(thresholdDb)
    }

    /**
     * Get input level in dB for a channel.
     */
    fun getInputLevel(channel: Int): Float = nativeGetInputLevel(channel)

    /**
     * Get input level (linear) for a channel.
     */
    fun getInputLevelLinear(channel: Int): Float = nativeGetInputLevelLinear(channel)

    /**
     * Check if input is clipping.
     */
    fun isInputClipping(): Boolean = nativeIsInputClipping()

    /**
     * Check if noise gate is open.
     */
    fun isNoiseGateOpen(): Boolean = nativeIsNoiseGateOpen()

    /**
     * Get input latency in milliseconds.
     */
    fun getInputLatencyMs(): Float = nativeGetInputLatencyMs()

    /**
     * Release input node resources.
     */
    suspend fun releaseInputNode(): Result<Unit> = withContext(Dispatchers.Default) {
        inputMutex.withLock {
            try {
                nativeReleaseInputNode()
                Result.success(Unit)
            } catch (e: Exception) {
                Log.e(TAG, "releaseInputNode failed", e)
                Result.failure(e)
            }
        }
    }

    /**
     * Release input node resources synchronously (for legacy callers).
     */
    fun releaseInputNodeSync() = nativeReleaseInputNode()

    // ==================== Monitoring Operations ====================

    /**
     * Enable/disable monitoring.
     */
    suspend fun setMonitoringEnabled(enabled: Boolean): Result<Unit> = withContext(Dispatchers.Default) {
        inputMutex.withLock {
            try {
                nativeSetMonitoringEnabled(enabled)
                Log.d(TAG, "setMonitoringEnabled: enabled=$enabled")
                Result.success(Unit)
            } catch (e: Exception) {
                Log.e(TAG, "setMonitoringEnabled failed", e)
                Result.failure(e)
            }
        }
    }

    /**
     * Enable/disable monitoring synchronously (for legacy callers).
     */
    fun setMonitoringEnabledSync(enabled: Boolean) = nativeSetMonitoringEnabled(enabled)

    /**
     * Check if monitoring is enabled.
     */
    fun isMonitoringEnabled(): Boolean = nativeIsMonitoringEnabled()

    /**
     * Set monitoring volume.
     */
    fun setMonitoringVolume(volume: Float) {
        if (!volume.isFinite()) return
        nativeSetMonitoringVolume(volume.coerceIn(0f, 1f))
    }

    /**
     * Get monitoring volume.
     */
    fun getMonitoringVolume(): Float = nativeGetMonitoringVolume()

    // ==================== Dual Touch Operations ====================

    /**
     * Set dual touch mode enabled.
     */
    override fun setDualTouchMode(enabled: Boolean) = nativeSetDualTouchMode(enabled)

    /**
     * Update dual touch state.
     */
    override fun setDualTouch(
        x1: Float, y1: Float, freq1: Float, amp1: Float, pressure1: Float,
        x2: Float, y2: Float, freq2: Float, amp2: Float, pressure2: Float,
        distance: Float, angle: Float
    ) {
        nativeSetDualTouch(
            x1, y1, freq1, amp1, pressure1,
            x2, y2, freq2, amp2, pressure2,
            distance, angle
        )
    }

    /**
     * Set dual touch mix mode.
     */
    override fun setDualTouchMixMode(modeId: Int) = nativeSetDualTouchMixMode(modeId)

    /**
     * Set secondary oscillator type.
     */
    override fun setSecondaryOscillatorType(type: Int) = nativeSetSecondaryOscillatorType(type)

    /**
     * Check if dual touch mode is enabled.
     */
    fun getDualTouchMode(): Boolean = nativeGetDualTouchMode()

    // ==================== Voice System Operations ====================

    /**
     * Enable or disable the polyphonic voice system.
     */
    override fun enableVoiceSystem(enabled: Boolean) = nativeEnableVoiceSystem(enabled)

    /**
     * Check if voice system is enabled.
     */
    override fun isVoiceSystemEnabled(): Boolean = nativeIsVoiceSystemEnabled()

    /**
     * Update multi-touch state for voice system.
     */
    override fun updateMultiTouch(count: Int, touchData: FloatArray?) = nativeUpdateMultiTouch(count, touchData)

    /**
     * Get the number of currently active voices.
     */
    override fun getActiveVoiceCount(): Int = nativeGetActiveVoiceCount()

    /**
     * Set maximum number of simultaneous voices.
     */
    override fun setMaxVoices(maxVoices: Int) = nativeSetMaxVoices(maxVoices)

    /**
     * Set voice stealing strategy.
     */
    override fun setVoiceStealingStrategy(strategyId: Int) = nativeSetVoiceStealingStrategy(strategyId)

    // ==================== Chord Operations (Phase 9C) ====================

    /**
     * Trigger chord voices in VoicePool.
     * @param frequencies Harmony frequencies (not including root)
     * @param amplitude Voice amplitude 0.0-1.0
     * @param oscillatorType Oscillator type for chord voices
     */
    fun triggerChordNotes(frequencies: FloatArray, amplitude: Float, oscillatorType: Int) =
        nativeTriggerChordNotes(frequencies, amplitude, oscillatorType)

    /**
     * Update frequencies/amplitude of active chord voices.
     */
    fun updateChordNotes(frequencies: FloatArray, amplitude: Float) =
        nativeUpdateChordNotes(frequencies, amplitude)

    /**
     * Release all chord voices.
     */
    fun releaseChordNotes() = nativeReleaseChordNotes()

    // ==================== Vocoder Operations ====================

    /**
     * Set vocoder carrier source.
     */
    fun setVocoderCarrierSource(useInternalCarrier: Boolean) = nativeSetVocoderCarrierSource(useInternalCarrier)

    /**
     * Set vocoder carrier frequency.
     */
    fun setVocoderCarrierFrequency(frequency: Float) {
        if (!frequency.isFinite()) return
        nativeSetVocoderCarrierFrequency(frequency.coerceIn(20f, 2000f))
    }

    /**
     * Check if a vocoder effect is present.
     */
    fun hasVocoderEffect(): Boolean = nativeHasVocoderEffect()

    /**
     * Set vocoder modulator source.
     */
    fun setVocoderModulatorSource(useExternalMod: Boolean) = nativeSetVocoderModulatorSource(useExternalMod)

    // ==================== Private Helpers ====================

    private fun validateParameter(
        effectType: EffectType,
        paramId: Int,
        value: Float
    ): Result<Float> {
        if (!value.isFinite()) {
            return Result.failure(
                NativeBridgeException.ParameterOutOfRange(paramId, value, 0f, 0f)
            )
        }

        val param = EffectParameter.getParameter(effectType, paramId)

        return if (param != null) {
            if (param.validate(value)) {
                Result.success(value)
            } else {
                Log.w(TAG, "Parameter $paramId value $value out of range, clamping")
                Result.success(param.clamp(value))
            }
        } else {
            Log.w(TAG, "Unknown parameter $paramId for ${effectType.displayName}")
            Result.success(value)
        }
    }

    private fun getEffectTypeSync(index: Int): EffectType? {
        val typeId = nativeGetEffectType(index)
        return if (typeId >= 0) EffectType.fromId(typeId) else null
    }

    private fun getEffectParametersSync(index: Int, typeId: Int): Map<Int, Float> {
        val effectType = EffectType.fromId(typeId) ?: return emptyMap()
        val params = mutableMapOf<Int, Float>()

        for (param in EffectParameter.forEffectType(effectType)) {
            val value = nativeGetEffectParameter(index, param.id)
            params[param.id] = value
        }

        return params
    }

    // ==================== Native Methods: Lifecycle ====================

    private external fun nativeStartEngine()
    private external fun nativeStopEngine()
    private external fun nativeStartEngineWithFade(fadeTimeMs: Int)
    private external fun nativeStopEngineWithFade(fadeTimeMs: Int)
    private external fun nativePauseEngineWithFade(fadeTimeMs: Int)
    private external fun nativeResumeEngineWithFade(fadeTimeMs: Int)

    // ==================== Native Methods: State ====================

    private external fun nativeGetEngineState(): Int
    private external fun nativeGetIsPaused(): Boolean
    private external fun nativeGetStateVersion(): Long
    private external fun nativeHasStreamError(): Boolean
    private external fun nativeGetLastStreamErrorCode(): Int
    private external fun nativeClearStreamError()
    private external fun nativeHasInitializationFailed(): Boolean
    private external fun nativeIsEngineInitialized(): Boolean
    private external fun nativeGetStreamInfo(): FloatArray?

    // ==================== Native Methods: Volume ====================

    private external fun nativeSetMasterVolume(volume: Float)
    private external fun nativeGetCurrentFadeVolume(): Float
    private external fun nativeGetTargetFadeVolume(): Float
    private external fun nativeGetIsFading(): Boolean
    private external fun nativeGetFadeProgress(): Float

    // ==================== Native Methods: Output Level Metering ====================

    private external fun nativeGetMasterVolume(): Float
    private external fun nativeGetOutputPeakLevel(channel: Int): Float
    private external fun nativeGetOutputRmsLevel(channel: Int): Float
    private external fun nativeGetOutputPeakLevelDb(channel: Int): Float
    private external fun nativeGetOutputRmsLevelDb(channel: Int): Float
    private external fun nativeGetOutputLevels(): FloatArray?

    // ==================== Native Methods: Real-time ====================

    private external fun nativeSetXY(x: Float, y: Float)
    private external fun nativeSetFrequencyAndAmplitude(frequency: Float, amplitude: Float)
    private external fun nativeSetFrequencyRange(minHz: Float, maxHz: Float)
    private external fun nativeSetOscillatorType(type: Int)
    private external fun nativeSetEngineType(type: Int)
    private external fun nativeSetEngineParameter(paramId: Int, value: Float)
    private external fun nativeGetEngineType(): Int
    // SoundFont (Phase 8)
    private external fun nativeLoadSoundFont(data: ByteArray): Boolean
    private external fun nativeLoadSoundFontFromPath(path: String): Boolean
    private external fun nativeUnloadSoundFont()
    private external fun nativeSetSoundFontPreset(presetIndex: Int)
    private external fun nativeGetSoundFontPresetCount(): Int
    private external fun nativeGetSoundFontPresetName(presetIndex: Int): String?
    private external fun nativeIsSoundFontLoaded(): Boolean
    private external fun nativeGetSoundFontPresetKeyRange(presetIndex: Int): IntArray?
    // SoundFont polyphony (Phase 8E)
    private external fun nativeSfNoteOn(touchId: Int, midiNote: Int, velocity: Float)
    private external fun nativeSfNoteOff(touchId: Int)
    private external fun nativeSfNoteOffAll()
    private external fun nativeSetVoiceFilterEnabled(enabled: Boolean)
    private external fun nativeSetVoiceFilterCutoff(hz: Float)
    private external fun nativeSetVoiceFilterResonance(q: Float)
    private external fun nativeSetVoiceFilterMode(mode: Int)
    private external fun nativeGetWaveformSamples(buffer: FloatArray, size: Int): Int

    // ==================== Native Methods: Modulator ====================

    private external fun nativeSetModulatorType(type: Int): Int
    private external fun nativeSetModulatorParameter(paramId: Int, value: Float): Int

    // ==================== Native Methods: Effects ====================

    private external fun nativeAddEffect(typeId: Int): Int
    private external fun nativeRemoveEffect(index: Int): Int
    private external fun nativeSetEffectParameter(index: Int, paramId: Int, value: Float): Int
    private external fun nativeSetEffectParametersBatch(index: Int, paramIds: IntArray, values: FloatArray): Int
    private external fun nativeSetMultipleEffectParameters(effectIndices: IntArray, paramIds: IntArray, values: FloatArray): Int
    private external fun nativeSetEffectBypass(index: Int, bypass: Boolean): Int
    private external fun nativeReorderEffects(fromIndex: Int, toIndex: Int): Int
    private external fun nativeGetEffectChainSize(): Int
    private external fun nativeGetEffectType(index: Int): Int
    private external fun nativeGetEffectParameter(index: Int, paramId: Int): Float
    private external fun nativeIsEffectBypassed(index: Int): Boolean

    // ==================== Native Methods: Global BPM ====================

    private external fun nativeSetBpm(bpm: Float)
    private external fun nativeGetBpm(): Float

    // ==================== Native Methods: Effect Routing ====================

    private external fun nativeSetRoutingMode(mode: Int)
    private external fun nativeGetRoutingMode(): Int
    private external fun nativeSetParallelMix(mix: Float)
    private external fun nativeSetFeedbackAmount(amount: Float)

    // ==================== Native Methods: Mode ====================

    private external fun nativeSetAudioMode(mode: Int)
    private external fun nativeGetAudioMode(): Int
    private external fun nativeIsInModeTransition(): Boolean
    private external fun nativeGetModeTransitionProgress(): Float
    private external fun nativeGetModeName(mode: Int): String
    private external fun nativeModeRequiresInput(mode: Int): Boolean

    // ==================== Native Methods: Input ====================

    private external fun nativeStartInputStream(): Boolean
    private external fun nativeStopInputStream()
    private external fun nativeIsInputStreamRunning(): Boolean
    private external fun nativeSetInputSource(source: Int)
    private external fun nativeGetInputSource(): Int
    private external fun nativeSetInputGain(gainDb: Float)
    private external fun nativeGetInputGain(): Float
    private external fun nativeSetNoiseGateEnabled(enabled: Boolean)
    private external fun nativeIsNoiseGateEnabled(): Boolean
    private external fun nativeSetNoiseGateThreshold(thresholdDb: Float)
    private external fun nativeGetInputLevel(channel: Int): Float
    private external fun nativeGetInputLevelLinear(channel: Int): Float
    private external fun nativeIsInputClipping(): Boolean
    private external fun nativeIsNoiseGateOpen(): Boolean
    private external fun nativeGetInputLatencyMs(): Float
    private external fun nativeReleaseInputNode()

    // ==================== Native Methods: Monitoring ====================

    private external fun nativeSetMonitoringEnabled(enabled: Boolean)
    private external fun nativeIsMonitoringEnabled(): Boolean
    private external fun nativeSetMonitoringVolume(volume: Float)
    private external fun nativeGetMonitoringVolume(): Float

    // ==================== Native Methods: Dual Touch ====================

    private external fun nativeSetDualTouchMode(enabled: Boolean)
    private external fun nativeSetDualTouch(
        x1: Float, y1: Float, freq1: Float, amp1: Float, pressure1: Float,
        x2: Float, y2: Float, freq2: Float, amp2: Float, pressure2: Float,
        distance: Float, angle: Float
    )
    private external fun nativeSetDualTouchMixMode(modeId: Int)
    private external fun nativeSetSecondaryOscillatorType(typeId: Int)
    private external fun nativeGetDualTouchMode(): Boolean

    // ==================== Native Methods: Voice System ====================

    private external fun nativeEnableVoiceSystem(enable: Boolean)
    private external fun nativeIsVoiceSystemEnabled(): Boolean
    private external fun nativeUpdateMultiTouch(count: Int, touchData: FloatArray?)
    private external fun nativeGetActiveVoiceCount(): Int
    private external fun nativeSetMaxVoices(maxVoices: Int)
    private external fun nativeSetVoiceStealingStrategy(strategy: Int)

    // ==================== Native Methods: Chord (Phase 9C) ====================

    private external fun nativeTriggerChordNotes(frequencies: FloatArray, amplitude: Float, oscillatorType: Int)
    private external fun nativeUpdateChordNotes(frequencies: FloatArray, amplitude: Float)
    private external fun nativeReleaseChordNotes()

    // ==================== USB Backend Operations ====================

    /**
     * Check if USB backend is available.
     */
    override fun isUsbBackendAvailable(): Boolean = nativeIsUsbBackendAvailable()

    /**
     * Set whether to use BackendManager for audio output.
     */
    override fun setUseBackendManager(useBackendManager: Boolean) = nativeSetUseBackendManager(useBackendManager)

    /**
     * Select audio backend.
     *
     * @param backendId Backend ID (1=OBOE, 2=LIBUSB, 3=SPLIT)
     * @return true if backend was selected successfully
     */
    override fun selectBackend(backendId: Int): Boolean = nativeSelectBackend(backendId)

    /**
     * Create an opt-in split backend from two existing native backends.
     *
     * Use backend IDs 1=OBOE and 2=LIBUSB. After this returns true, call
     * [selectBackend] with backend ID 3 to activate the split.
     */
    override fun createSplitBackend(inputBackendId: Int, outputBackendId: Int): Boolean =
        nativeCreateSplitBackend(inputBackendId, outputBackendId)

    /**
     * Get current backend type.
     */
    override fun getCurrentBackendType(): Int = nativeGetCurrentBackendType()

    /**
     * Set USB streaming mode.
     *
     * @param modeId Mode ID (0=PLAYBACK_ONLY, 1=CAPTURE_ONLY, 2=FULL_DUPLEX)
     */
    fun setUsbStreamingMode(modeId: Int) = nativeSetUsbStreamingMode(modeId)

    /**
     * Configure USB backend parameters.
     */
    fun configureUsbBackend(sampleRate: Int, channels: Int, bitDepth: Int) =
        nativeConfigureUsbBackend(sampleRate, channels, bitDepth)

    // ==================== Memory / Resource Operations ====================

    /**
     * Check if using reduced buffers due to low memory.
     */
    fun isUsingReducedBuffers(): Boolean = nativeIsUsingReducedBuffers()

    // ==================== Automation Operations ====================

    /**
     * Set automation parameter from XY pad.
     * This is used when mapping XY values to effect parameters.
     */
    fun setAutomationParameter(effectIndex: Int, paramId: Int, xyValue: Float) {
        if (!xyValue.isFinite()) return
        nativeSetAutomationParameter(effectIndex, paramId, xyValue.coerceIn(0f, 1f))
    }

    // ==================== XY Mapping Config (Phase 4) ====================

    /**
     * Configure mapping for an axis.
     * @param axis 0=X, 1=Y, 2=DEPTH
     */
    fun setMappingConfig(
        axis: Int, effectIndex: Int, paramId: Int,
        curve: Int, polarity: Int,
        mapMin: Float, mapMax: Float, inverted: Boolean
    ) {
        if (!mapMin.isFinite() || !mapMax.isFinite()) return
        nativeSetMappingConfig(axis, effectIndex, paramId, curve, polarity, mapMin, mapMax, inverted)
    }

    /**
     * Clear mapping for an axis (disables automation for that axis).
     */
    fun clearMappingConfig(axis: Int) {
        nativeClearMappingConfig(axis)
    }

    /**
     * Set depth axis value. Lock-free real-time path.
     */
    fun setDepthValue(value: Float) {
        if (!value.isFinite()) return
        nativeSetDepthValue(value.coerceIn(0f, 1f))
    }

    /**
     * Apply automation for an axis using stored mapping config.
     * Lock-free real-time path — called at ~60Hz from XY updates.
     */
    fun applyAutomation(axis: Int, normalizedValue: Float) {
        if (!normalizedValue.isFinite()) return
        nativeApplyAutomation(axis, normalizedValue.coerceIn(0f, 1f))
    }

    // ==================== Effect Chain Queries (Public) ====================

    /**
     * Get current effect chain size.
     * This is a public version for callers outside the effects system.
     */
    fun getEffectChainSize(): Int = nativeGetEffectChainSize()

    // ==================== Native Methods: Vocoder ====================

    private external fun nativeSetVocoderCarrierSource(useInternalCarrier: Boolean)
    private external fun nativeSetVocoderCarrierFrequency(frequency: Float)
    private external fun nativeHasVocoderEffect(): Boolean
    private external fun nativeSetVocoderModulatorSource(useExternalMod: Boolean)

    // ==================== Native Methods: USB Backend ====================

    private external fun nativeIsUsbBackendAvailable(): Boolean
    private external fun nativeSetUseBackendManager(use: Boolean)
    private external fun nativeCreateSplitBackend(inputBackendId: Int, outputBackendId: Int): Boolean
    private external fun nativeSelectBackend(backendId: Int): Boolean
    private external fun nativeGetCurrentBackendType(): Int
    private external fun nativeSetUsbStreamingMode(modeId: Int)
    private external fun nativeConfigureUsbBackend(sampleRate: Int, channels: Int, bitDepth: Int)

    // ==================== Native Methods: Memory ====================

    private external fun nativeIsUsingReducedBuffers(): Boolean

    // ==================== Native Methods: Automation ====================

    private external fun nativeSetAutomationParameter(effectIndex: Int, paramId: Int, xyValue: Float)

    // ==================== Native Methods: XY Mapping Config (Phase 4) ====================

    private external fun nativeSetMappingConfig(
        axis: Int, effectIndex: Int, paramId: Int,
        curve: Int, polarity: Int,
        mapMin: Float, mapMax: Float, inverted: Boolean
    )
    private external fun nativeClearMappingConfig(axis: Int)
    private external fun nativeSetDepthValue(value: Float)
    private external fun nativeApplyAutomation(axis: Int, normalizedValue: Float)

    // ==================== USB Device Operations ====================

    /**
     * Initialize USB device with file descriptor from Android.
     *
     * @param fileDescriptor File descriptor from UsbDeviceConnection.getFileDescriptor()
     * @param usbfsPath Path like "/dev/bus/usb/001/002" from UsbDevice.getDeviceName()
     * @return true if initialization succeeded
     */
    fun initializeUsbDevice(fileDescriptor: Int, usbfsPath: String): Boolean =
        nativeInitializeUsbDevice(fileDescriptor, usbfsPath)

    /**
     * Close USB device connection and release resources.
     */
    fun closeUsbDevice() = nativeCloseUsbDevice()

    /**
     * Check if USB device is initialized and ready.
     */
    fun isUsbDeviceInitialized(): Boolean = nativeIsUsbDeviceInitialized()

    /**
     * Parse USB Audio descriptors and get capabilities.
     *
     * @return FloatArray with parsed capabilities or null on error
     */
    fun parseUsbDescriptors(): FloatArray? = nativeParseUsbDescriptors()

    /**
     * Start USB streaming with configuration.
     *
     * @param sampleRate Sample rate in Hz
     * @param channels Number of channels
     * @param bitDepth Bit depth
     * @return true if streaming started
     */
    fun startUsbStreaming(sampleRate: Int, channels: Int, bitDepth: Int): Boolean =
        nativeStartUsbStreaming(sampleRate, channels, bitDepth)

    /**
     * Stop USB streaming.
     */
    fun stopUsbStreaming() = nativeStopUsbStreaming()

    /**
     * Get USB transfer statistics.
     *
     * @return FloatArray with stats or null if not streaming
     */
    fun getUsbTransferStats(): FloatArray? = nativeGetUsbTransferStats()

    /**
     * Start USB streaming with mode.
     *
     * @param sampleRate Sample rate in Hz
     * @param channels Number of channels
     * @param bitDepth Bit depth
     * @param streamingMode 0=playback only, 1=capture only, 2=full duplex
     * @return true if streaming started
     */
    fun startUsbStreamingWithMode(
        sampleRate: Int,
        channels: Int,
        bitDepth: Int,
        streamingMode: Int
    ): Boolean = nativeStartUsbStreamingWithMode(sampleRate, channels, bitDepth, streamingMode)

    /**
     * Check if USB device supports full-duplex.
     */
    fun usbDeviceSupportsFullDuplex(): Boolean = nativeUsbDeviceSupportsFullDuplex()

    /**
     * Check if USB device has capture capability.
     */
    fun usbDeviceHasCapture(): Boolean = nativeUsbDeviceHasCapture()

    /**
     * Get UAC version of connected device.
     *
     * @return 1 for UAC 1.0, 2 for UAC 2.0, 0 if not connected
     */
    fun getUsbDeviceUacVersion(): Int = nativeGetUsbDeviceUacVersion()

    /**
     * Get the full capability snapshot of the connected USB device.
     * Returns null if no device is initialized natively.
     */
    fun getUsbCapabilitySnapshot(): ByteArray? = nativeGetUsbCapabilitySnapshot()

    /**
     * Set the USB stream preference used by native altsetting selection.
     * Takes effect on the next USB start.
     */
    fun setUsbStreamPreference(preference: StreamPreference): Boolean =
        nativeSetUsbStreamPreference(
            preference.preferredSampleRate,
            preference.minChannels,
            preference.requireFeedback,
            preference.profile.ordinal
        )

    /**
     * Select a playback altsetting+format to apply on the next USB start.
     */
    fun selectUsbAltsetting(interfaceNumber: Int, alternateSetting: Int, formatIndex: Int): Boolean =
        nativeSelectUsbAltsetting(interfaceNumber, alternateSetting, formatIndex)

    /**
     * Select a UAC2 clock source to apply on the next USB start.
     */
    fun selectUsbClockSource(clockSourceId: Int): Boolean =
        nativeSelectUsbClockSource(clockSourceId)

    /**
     * Check if USB device was disconnected.
     */
    fun isUsbDeviceDisconnected(): Boolean = nativeIsUsbDeviceDisconnected()

    /**
     * Get USB health status.
     *
     * @return IntArray with [isDisconnected, errors, lastSuccessAge] or null
     */
    fun getUsbHealthStatus(): IntArray? = nativeGetUsbHealthStatus()

    /**
     * Fallback to Oboe backend when USB fails.
     */
    fun fallbackToOboeBackend() = nativeFallbackToOboeBackend()

    /**
     * Get adaptive buffer statistics.
     */
    fun getAdaptiveBufferStats(): FloatArray? = nativeGetAdaptiveBufferStats()

    /**
     * Get current USB buffer size in milliseconds.
     */
    fun getCurrentUsbBufferMs(): Int = nativeGetCurrentUsbBufferMs()

    // ==================== USB Volume Operations ====================

    /**
     * Get USB volume capabilities.
     *
     * @return FloatArray with volume capability flags and ranges
     */
    fun getUsbVolumeCapabilities(): FloatArray? = nativeGetUsbVolumeCapabilities()

    /**
     * Set USB output volume.
     *
     * @param volume Linear volume 0.0 to 1.0
     */
    fun setUsbOutputVolume(volume: Float) = nativeSetUsbOutputVolume(volume)

    /**
     * Get current USB output volume.
     */
    fun getUsbOutputVolume(): Float = nativeGetUsbOutputVolume()

    /**
     * Set USB input volume.
     *
     * @param volume Linear volume 0.0 to 1.0
     */
    fun setUsbInputVolume(volume: Float) = nativeSetUsbInputVolume(volume)

    /**
     * Get current USB input volume.
     */
    fun getUsbInputVolume(): Float = nativeGetUsbInputVolume()

    /**
     * Set USB output mute state.
     */
    fun setUsbOutputMute(muted: Boolean) = nativeSetUsbOutputMute(muted)

    /**
     * Check if USB output is muted.
     */
    fun isUsbOutputMuted(): Boolean = nativeIsUsbOutputMuted()

    /**
     * Set USB input mute state.
     */
    fun setUsbInputMute(muted: Boolean) = nativeSetUsbInputMute(muted)

    /**
     * Check if USB input is muted.
     */
    fun isUsbInputMuted(): Boolean = nativeIsUsbInputMuted()

    // ==================== Native Methods: USB Device ====================

    private external fun nativeInitializeUsbDevice(fileDescriptor: Int, usbfsPath: String): Boolean
    private external fun nativeCloseUsbDevice()
    private external fun nativeIsUsbDeviceInitialized(): Boolean
    private external fun nativeParseUsbDescriptors(): FloatArray?
    private external fun nativeStartUsbStreaming(sampleRate: Int, channels: Int, bitDepth: Int): Boolean
    private external fun nativeStopUsbStreaming()
    private external fun nativeGetUsbTransferStats(): FloatArray?
    private external fun nativeStartUsbStreamingWithMode(sampleRate: Int, channels: Int, bitDepth: Int, streamingMode: Int): Boolean
    private external fun nativeUsbDeviceSupportsFullDuplex(): Boolean
    private external fun nativeUsbDeviceHasCapture(): Boolean
    private external fun nativeGetUsbDeviceUacVersion(): Int
    private external fun nativeGetUsbCapabilitySnapshot(): ByteArray?
    private external fun nativeSetUsbStreamPreference(
        preferredSampleRate: Int,
        minChannels: Int,
        requireFeedback: Boolean,
        profile: Int
    ): Boolean
    private external fun nativeSelectUsbAltsetting(interfaceNumber: Int, alternateSetting: Int, formatIndex: Int): Boolean
    private external fun nativeSelectUsbClockSource(clockSourceId: Int): Boolean
    private external fun nativeIsUsbDeviceDisconnected(): Boolean
    private external fun nativeGetUsbHealthStatus(): IntArray?
    private external fun nativeFallbackToOboeBackend()
    private external fun nativeGetAdaptiveBufferStats(): FloatArray?
    private external fun nativeGetCurrentUsbBufferMs(): Int

    // ==================== Native Methods: USB Volume ====================

    private external fun nativeGetUsbVolumeCapabilities(): FloatArray?
    private external fun nativeSetUsbOutputVolume(volume: Float)
    private external fun nativeGetUsbOutputVolume(): Float
    private external fun nativeSetUsbInputVolume(volume: Float)
    private external fun nativeGetUsbInputVolume(): Float
    private external fun nativeSetUsbOutputMute(muted: Boolean)
    private external fun nativeIsUsbOutputMuted(): Boolean
    private external fun nativeSetUsbInputMute(muted: Boolean)
    private external fun nativeIsUsbInputMuted(): Boolean

    // ==================== Latency Benchmark Operations ====================

    /**
     * Gets detailed latency information for the current audio stream.
     *
     * @return FloatArray with:
     *   [0] outputLatencyMs - Output latency in milliseconds
     *   [1] inputLatencyMs - Input latency in milliseconds (-1 if not available)
     *   [2] sampleRate - Current sample rate in Hz
     *   [3] framesPerBurst - Frames per callback burst
     *   [4] bufferCapacity - Total buffer capacity in frames
     *   [5] isAAudio - 1.0 if using AAudio, 0.0 if OpenSL ES
     *   [6] isExclusive - 1.0 if using Exclusive mode, 0.0 if Shared
     *   [7] isLowLatency - 1.0 if using LowLatency mode, 0.0 if Normal
     */
    fun getDetailedLatencyInfo(): FloatArray? = nativeGetDetailedLatencyInfo()

    /**
     * Runs a quick latency optimization test.
     * Tests different configurations and returns potential improvements.
     *
     * @return FloatArray with:
     *   [0] bestLatencyMs - Best achievable latency
     *   [1] currentLatencyMs - Current latency
     *   [2] canUseExclusive - 1.0 if Exclusive mode is available
     *   [3] improvementPercent - Potential improvement percentage
     */
    fun runLatencyOptimizationTest(): FloatArray? = nativeRunLatencyOptimizationTest()

    /**
     * Starts a round-trip latency measurement.
     * Requires physical loopback (output connected to input).
     * Call getRoundTripResult() to get the result after measurement completes.
     *
     * @return true if test started successfully
     */
    fun startRoundTripTest(): Boolean = nativeStartRoundTripTest()

    /**
     * Gets the result of a round-trip latency test.
     *
     * @return FloatArray with:
     *   [0] latencyMs - Measured round-trip latency (-1 if not completed)
     *   [1] state - Test state: 0=idle, 1=running, 2=complete, 3=timeout
     */
    fun getRoundTripResult(): FloatArray? = nativeGetRoundTripResult()

    /**
     * Cancels an ongoing round-trip test.
     */
    fun cancelRoundTripTest() = nativeCancelRoundTripTest()

    /**
     * Gets recommended buffer size for a target latency.
     *
     * @param targetLatencyMs Target latency in milliseconds
     * @return Recommended buffer size in frames, or -1 if cannot achieve
     */
    fun getRecommendedBufferSize(targetLatencyMs: Float): Int =
        nativeGetRecommendedBufferSize(targetLatencyMs)

    /**
     * Checks if AAudio is available on this device.
     * AAudio provides lower latency than OpenSL ES on supported devices.
     *
     * @return true if AAudio is available
     */
    fun isAAudioAvailable(): Boolean = nativeIsAAudioAvailable()

    /**
     * Gets a formatted latency report string for display.
     * Includes all relevant latency metrics and configuration info.
     *
     * @return Formatted report string
     */
    fun getLatencyReport(): String = nativeGetLatencyReport()

    // ==================== USB Profiling Operations ====================

    /**
     * Get detailed USB latency profiling statistics.
     *
     * @return FloatArray with 18 elements containing profiling stats, or null if USB not initialized
     */
    fun getUsbProfilingStats(): FloatArray? = nativeGetUsbProfilingStats()

    /**
     * Enable or disable USB latency profiling.
     *
     * @param enabled true to enable, false to disable
     */
    fun setUsbProfilingEnabled(enabled: Boolean) = nativeSetUsbProfilingEnabled(enabled)

    /**
     * Reset USB latency profiling statistics.
     */
    fun resetUsbProfilingStats() = nativeResetUsbProfilingStats()

    // ==================== Native Methods: Latency Benchmark ====================

    private external fun nativeGetDetailedLatencyInfo(): FloatArray?
    private external fun nativeRunLatencyOptimizationTest(): FloatArray?
    private external fun nativeStartRoundTripTest(): Boolean
    private external fun nativeGetRoundTripResult(): FloatArray?
    private external fun nativeCancelRoundTripTest()
    private external fun nativeGetRecommendedBufferSize(targetLatencyMs: Float): Int
    private external fun nativeIsAAudioAvailable(): Boolean
    private external fun nativeGetLatencyReport(): String

    // ==================== Native Methods: USB Profiling ====================

    private external fun nativeGetUsbProfilingStats(): FloatArray?
    private external fun nativeSetUsbProfilingEnabled(enabled: Boolean)
    private external fun nativeResetUsbProfilingStats()

    // ==================== Arpeggiator (Phase 7) ====================

    /**
     * Enable/disable the arpeggiator.
     * When enabled, the arp overrides oscillator frequency/amplitude with
     * rhythmic note patterns synced to BPM.
     */
    fun setArpEnabled(enabled: Boolean) {
        nativeSetArpEnabled(enabled)
        Log.d(TAG, "Arp enabled: $enabled")
    }

    /** Check if arpeggiator is currently enabled */
    fun isArpEnabled(): Boolean = nativeIsArpEnabled()

    /** Set arp pattern (matches ArpPattern.id from core-domain) */
    fun setArpPattern(patternId: Int) = nativeSetArpPattern(patternId)

    /** Set arp rate as beats per step (e.g., 0.5 = 1/8 note, 0.25 = 1/16 note) */
    fun setArpSubdivision(beatsPerStep: Float) = nativeSetArpSubdivision(beatsPerStep)

    /** Set arp octave range (1-4) */
    fun setArpOctaveRange(octaves: Int) = nativeSetArpOctaveRange(octaves)

    /** Set gate length (0.05 staccato - 1.0 legato) */
    fun setArpGateLength(gate: Float) = nativeSetArpGateLength(gate)

    /** Set swing amount (0.5 straight - 0.75 hard swing) */
    fun setArpSwing(swing: Float) = nativeSetArpSwing(swing)

    /** Enable/disable latch mode (arp continues after releasing touch) */
    fun setArpLatch(latch: Boolean) = nativeSetArpLatch(latch)

    /** Set base velocity (0.0-1.0) */
    fun setArpVelocity(velocity: Float) = nativeSetArpVelocity(velocity)

    /** Set velocity random variation amount (0.0-0.5) */
    fun setArpVelocityVariation(variation: Float) = nativeSetArpVelocityVariation(variation)

    /** Set per-step trigger probability (0.0-1.0, 1.0 = all steps play) */
    fun setArpProbability(probability: Float) = nativeSetArpProbability(probability)

    /**
     * Set the scale intervals for arp note generation.
     * @param intervals Semitone offsets (e.g., [0,2,4,5,7,9,11] for major scale)
     */
    fun setArpScaleIntervals(intervals: IntArray) = nativeSetArpScaleIntervals(intervals)

    /** Notify arp that touch is active/inactive (drives gate) */
    fun setArpTouchActive(active: Boolean) = nativeSetArpTouchActive(active)

    /** Set the base frequency from XY pad (arp patterns are relative to this) */
    fun setArpBaseFrequency(frequency: Float) = nativeSetArpBaseFrequency(frequency)

    /** Get current step index (for UI step visualizer) */
    fun getArpCurrentStep(): Int = nativeGetArpCurrentStep()

    /** Get total steps in current pattern (for UI step visualizer) */
    fun getArpTotalSteps(): Int = nativeGetArpTotalSteps()

    /** Ratchet: momentary double-time while held */
    fun setArpRatchet(active: Boolean) = nativeSetArpRatchet(active)

    /** Regenerate pattern for Random/Stochastic/Walk */
    fun regenerateArpPattern() = nativeRegenerateArpPattern()

    /** Check if arp gate is currently open (for visual pulse indicator) */
    fun isArpGateOpen(): Boolean = nativeIsArpGateOpen()

    // ==================== Native Methods: Arpeggiator ====================

    private external fun nativeSetArpEnabled(enabled: Boolean)
    private external fun nativeIsArpEnabled(): Boolean
    private external fun nativeSetArpPattern(patternId: Int)
    private external fun nativeSetArpSubdivision(beatsPerStep: Float)
    private external fun nativeSetArpOctaveRange(octaves: Int)
    private external fun nativeSetArpGateLength(gate: Float)
    private external fun nativeSetArpSwing(swing: Float)
    private external fun nativeSetArpLatch(latch: Boolean)
    private external fun nativeSetArpVelocity(velocity: Float)
    private external fun nativeSetArpVelocityVariation(variation: Float)
    private external fun nativeSetArpProbability(probability: Float)
    private external fun nativeSetArpScaleIntervals(intervals: IntArray)
    private external fun nativeSetArpTouchActive(active: Boolean)
    private external fun nativeSetArpBaseFrequency(frequency: Float)
    private external fun nativeSetArpRatchet(active: Boolean)
    private external fun nativeRegenerateArpPattern()
    private external fun nativeGetArpCurrentStep(): Int
    private external fun nativeGetArpTotalSteps(): Int
    private external fun nativeIsArpGateOpen(): Boolean

    // ========== AUDIO LOOPER (Phase 11) ==========

    // State mutations (call from coroutine/UI thread)
    private external fun nativeLooperPrepareTrack(trackIndex: Int, lengthFrames: Int, sampleRate: Int): Int
    private external fun nativeLooperPrepareTrackBars(trackIndex: Int, bars: Int, sampleRate: Int): Int
    private external fun nativeLooperArmAtNextBar(trackIndex: Int): Long
    private external fun nativeLooperCancelArm()
    private external fun nativeLooperGetArmedTrack(): Int
    private external fun nativeLooperSetTailMs(ms: Int)
    private external fun nativeLooperGetTailMs(): Int
    private external fun nativeLooperStartRecordingWithPreRoll(trackIndex: Int, preRollMs: Int)
    private external fun nativeLooperStartRecording(trackIndex: Int)
    private external fun nativeLooperStopRecording()
    private external fun nativeLooperStartOverdub(trackIndex: Int)
    private external fun nativeLooperStopAll()
    private external fun nativeLooperPause()
    private external fun nativeLooperResume()
    private external fun nativeLooperSetFreeLength(freeLength: Boolean)
    private external fun nativeLooperSetTrackMuted(trackIndex: Int, muted: Boolean)
    private external fun nativeLooperSetTrackVolume(trackIndex: Int, volume: Float)
    private external fun nativeLooperSetTrackPan(trackIndex: Int, pan: Float)
    private external fun nativeLooperClearTrack(trackIndex: Int)
    private external fun nativeLooperClearAll()
    private external fun nativeLooperSetEnabled(enabled: Boolean)

    // Lock-free queries (safe from any thread)
    private external fun nativeLooperGetProgress(): Float
    private external fun nativeLooperGetTrackPeakLevel(trackIndex: Int): Float
    private external fun nativeLooperIsTrackActive(trackIndex: Int): Boolean
    private external fun nativeLooperIsPlaying(): Boolean
    private external fun nativeLooperIsRecording(): Boolean
    private external fun nativeLooperGetMasterLoopFrames(): Int
    private external fun nativeLooperGetRecordProgress(): Float

    // Per-track playback control
    private external fun nativeLooperPauseTrack(trackIndex: Int)
    private external fun nativeLooperResumeTrack(trackIndex: Int)
    private external fun nativeLooperIsTrackPlaying(trackIndex: Int): Boolean
    private external fun nativeLooperGetTrackProgress(trackIndex: Int): Float
    private external fun nativeLooperGetTrackLengthFrames(trackIndex: Int): Int
    private external fun nativeLooperResetTrackPlayHead(trackIndex: Int)
    private external fun nativeLooperSaveUndoSnapshot(trackIndex: Int): Boolean
    private external fun nativeLooperRestoreUndo(trackIndex: Int): Boolean
    private external fun nativeLooperHasUndo(trackIndex: Int): Boolean
    private external fun nativeLooperGetTrackWaveform(trackIndex: Int, outBins: FloatArray, numBins: Int): Int
    private external fun nativeLooperSetTrackSpeed(trackIndex: Int, speed: Float)
    private external fun nativeLooperGetTrackSpeed(trackIndex: Int): Float
    private external fun nativeLooperSetMasterVolume(volume: Float)
    private external fun nativeLooperGetMasterVolume(): Float
    private external fun nativeLooperSetTrackLoopRegion(trackIndex: Int, startFrame: Int, endFrame: Int)
    private external fun nativeLooperResetTrackLoopRegion(trackIndex: Int)
    private external fun nativeLooperGetTrackLoopStart(trackIndex: Int): Int
    private external fun nativeLooperGetTrackLoopEnd(trackIndex: Int): Int
    private external fun nativeLooperTriggerClick(isDownbeat: Boolean)
    private external fun nativeLooperGetInputPeak(): Float

    // Musical transport (BPM-driven scheduler, RT-safe metronome)
    private external fun nativeTransportSetBeatsPerBar(beatsPerBar: Int)
    private external fun nativeTransportGetBeatsPerBar(): Int
    private external fun nativeTransportFramesPerBeat(): Int
    private external fun nativeTransportFramesPerBar(bars: Int): Int
    private external fun nativeTransportStartMetronome(
        beats: Int, firstIsDownbeat: Boolean, everyBeatPattern: Boolean
    )
    private external fun nativeTransportStartMetronomeContinuous(everyBeatPattern: Boolean)
    private external fun nativeTransportStopMetronome()
    private external fun nativeTransportIsMetronomeRunning(): Boolean
    private external fun nativeTransportIsMetronomeContinuous(): Boolean
    private external fun nativeTransportGetRemainingBeats(): Int

    private external fun nativeLooperExportMix(filePath: String): Boolean
    private external fun nativeLooperExportTrack(trackIndex: Int, filePath: String): Boolean
    private external fun nativeLooperImportTrack(trackIndex: Int, filePath: String, sampleRate: Int): Boolean

    // Export V2 (with options + metadata + limiter)
    private external fun nativeLooperExportMixV2(
        filePath: String, bitDepth: Int, repeatLoops: Int,
        countInBeats: Int, applyLimiter: Boolean,
        projectName: String?, artist: String?, comment: String?, bpm: Int
    ): Boolean
    private external fun nativeLooperExportStems(
        directory: String, bitDepth: Int, repeatLoops: Int,
        countInBeats: Int, applyLimiter: Boolean, bpm: Int
    ): Int
    private external fun nativeLooperGetExportProgress(): Float
    private external fun nativeLooperCancelExport()
    private external fun nativeLooperIsExportInProgress(): Boolean

    // Telemetry
    private external fun nativeLooperGetFramesDropped(): Long
    private external fun nativeLooperGetExportsCompleted(): Long
    private external fun nativeLooperGetExportsFailed(): Long
    private external fun nativeLooperGetStemsWritten(): Long
    private external fun nativeLooperGetArmedTriggered(): Long
    private external fun nativeLooperResetTelemetry()

    // Public Looper API

    fun looperPrepareTrack(trackIndex: Int, lengthFrames: Int, sampleRate: Int): Boolean {
        val result = nativeLooperPrepareTrack(trackIndex, lengthFrames, sampleRate)
        return result >= 0
    }

    /**
     * Prepare a track quantized to N musical bars at the current Transport
     * BPM/beats-per-bar/sample rate. Returns the actual loop length in frames,
     * or -1 if preparation failed (memory budget, invalid bars, transport not ready).
     */
    fun looperPrepareTrackBars(trackIndex: Int, bars: Int, sampleRate: Int): Int =
        nativeLooperPrepareTrackBars(trackIndex, bars, sampleRate)

    /**
     * Arm a track to start recording at the next bar boundary on the Transport.
     * Used to keep multi-track recordings phase-aligned. Returns the absolute
     * trigger frame (Transport playFrame), or -1 on failure.
     */
    fun looperArmAtNextBar(trackIndex: Int): Long = nativeLooperArmAtNextBar(trackIndex)

    /** Cancel a pending armed recording (does not affect a recording in progress). */
    fun looperCancelArm() = nativeLooperCancelArm()

    /** Returns the armed track index, or -1 if no track is armed. */
    fun looperGetArmedTrack(): Int = nativeLooperGetArmedTrack()

    /**
     * Tail capture length in milliseconds. The looper allocates `loopFrames + tailFrames`
     * per track and continues recording past the loop boundary into the tail region.
     * On playback, the tail is mixed into the start of each iteration with linear
     * fade-out, preserving sustain of pads/delays/reverbs at the loop seam.
     * Default 250 ms. Affects tracks prepared AFTER this call. Set to 0 to disable.
     */
    fun looperSetTailMs(ms: Int) = nativeLooperSetTailMs(ms)
    fun looperGetTailMs(): Int = nativeLooperGetTailMs()

    /**
     * Start recording with a pre-roll seed: the first `preRollMs` of the track
     * is filled with the most recent post-FX audio captured by the engine,
     * eliminating the reaction-time gap between hearing a sound and pressing REC.
     * preRollMs is clamped to [0, 1000]. preRollMs=0 falls back to looperStartRecording.
     */
    fun looperStartRecordingWithPreRoll(trackIndex: Int, preRollMs: Int) =
        nativeLooperStartRecordingWithPreRoll(trackIndex, preRollMs)

    fun looperStartRecording(trackIndex: Int) = nativeLooperStartRecording(trackIndex)
    fun looperStopRecording() = nativeLooperStopRecording()
    fun looperStartOverdub(trackIndex: Int) = nativeLooperStartOverdub(trackIndex)
    fun looperStopAll() = nativeLooperStopAll()
    fun looperPause() = nativeLooperPause()
    fun looperResume() = nativeLooperResume()
    fun looperSetFreeLength(freeLength: Boolean) = nativeLooperSetFreeLength(freeLength)
    fun looperClearTrack(trackIndex: Int) = nativeLooperClearTrack(trackIndex)
    fun looperClearAll() = nativeLooperClearAll()
    fun looperSetEnabled(enabled: Boolean) = nativeLooperSetEnabled(enabled)

    // Real-time params (lock-free, no suspend)
    fun looperSetTrackMuted(trackIndex: Int, muted: Boolean) = nativeLooperSetTrackMuted(trackIndex, muted)
    fun looperSetTrackVolume(trackIndex: Int, volume: Float) = nativeLooperSetTrackVolume(trackIndex, volume)
    fun looperSetTrackPan(trackIndex: Int, pan: Float) = nativeLooperSetTrackPan(trackIndex, pan)

    // Metering queries (lock-free)
    fun looperGetProgress(): Float = nativeLooperGetProgress()
    fun looperGetTrackPeakLevel(trackIndex: Int): Float = nativeLooperGetTrackPeakLevel(trackIndex)
    fun looperIsTrackActive(trackIndex: Int): Boolean = nativeLooperIsTrackActive(trackIndex)
    fun looperIsPlaying(): Boolean = nativeLooperIsPlaying()
    fun looperIsRecording(): Boolean = nativeLooperIsRecording()
    fun looperGetMasterLoopFrames(): Int = nativeLooperGetMasterLoopFrames()
    fun looperGetRecordProgress(): Float = nativeLooperGetRecordProgress()

    // Per-track playback control
    fun looperPauseTrack(trackIndex: Int) = nativeLooperPauseTrack(trackIndex)
    fun looperResumeTrack(trackIndex: Int) = nativeLooperResumeTrack(trackIndex)
    fun looperIsTrackPlaying(trackIndex: Int): Boolean = nativeLooperIsTrackPlaying(trackIndex)
    fun looperGetTrackProgress(trackIndex: Int): Float = nativeLooperGetTrackProgress(trackIndex)
    fun looperGetTrackLengthFrames(trackIndex: Int): Int = nativeLooperGetTrackLengthFrames(trackIndex)
    fun looperResetTrackPlayHead(trackIndex: Int) = nativeLooperResetTrackPlayHead(trackIndex)
    fun looperSaveUndoSnapshot(trackIndex: Int): Boolean = nativeLooperSaveUndoSnapshot(trackIndex)
    fun looperRestoreUndo(trackIndex: Int): Boolean = nativeLooperRestoreUndo(trackIndex)
    fun looperHasUndo(trackIndex: Int): Boolean = nativeLooperHasUndo(trackIndex)

    // Track waveform
    fun looperGetTrackWaveform(trackIndex: Int, numBins: Int = 24): FloatArray {
        val bins = FloatArray(numBins)
        nativeLooperGetTrackWaveform(trackIndex, bins, numBins)
        return bins
    }

    // Track speed
    fun looperSetTrackSpeed(trackIndex: Int, speed: Float) = nativeLooperSetTrackSpeed(trackIndex, speed)
    fun looperGetTrackSpeed(trackIndex: Int): Float = nativeLooperGetTrackSpeed(trackIndex)

    // Master volume (lock-free)
    fun looperSetMasterVolume(volume: Float) = nativeLooperSetMasterVolume(volume)
    fun looperGetMasterVolume(): Float = nativeLooperGetMasterVolume()

    // Loop region (lock-free)
    fun looperSetTrackLoopRegion(trackIndex: Int, startFrame: Int, endFrame: Int) =
        nativeLooperSetTrackLoopRegion(trackIndex, startFrame, endFrame)
    fun looperResetTrackLoopRegion(trackIndex: Int) = nativeLooperResetTrackLoopRegion(trackIndex)
    fun looperGetTrackLoopStart(trackIndex: Int): Int = nativeLooperGetTrackLoopStart(trackIndex)
    fun looperGetTrackLoopEnd(trackIndex: Int): Int = nativeLooperGetTrackLoopEnd(trackIndex)

    // Metronome click (lock-free)
    fun looperTriggerClick(isDownbeat: Boolean) = nativeLooperTriggerClick(isDownbeat)

    /**
     * Linear input peak [0..1], max of L/R channels. Useful for pre-record level
     * indicator UI when the user is about to arm a track from input_fx mode.
     * Returns 0 when no input source is active.
     */
    fun looperGetInputPeak(): Float = nativeLooperGetInputPeak()

    // ========== TRANSPORT (BPM, beats, RT-safe metronome scheduler) ==========
    //
    // Use this instead of looperTriggerClick() for sample-accurate metronome.
    // The scheduler runs in C++ on the audio thread, so clicks fire on time
    // regardless of UI/Compose jank.
    //
    // Typical pre-count usage:
    //   setBpm(120f)
    //   transportSetBeatsPerBar(4)
    //   transportStartMetronome(beats = 4, firstIsDownbeat = true,
    //                           everyBeatPattern = true)
    //
    // Loop-length quantization (frames for N bars at current BPM/SR):
    //   val frames = transportFramesPerBar(2)  // 2-bar loop
    //   looperPrepareTrack(idx, frames, sr)

    fun transportSetBeatsPerBar(beatsPerBar: Int) =
        nativeTransportSetBeatsPerBar(beatsPerBar)
    fun transportGetBeatsPerBar(): Int = nativeTransportGetBeatsPerBar()
    fun transportFramesPerBeat(): Int = nativeTransportFramesPerBeat()
    fun transportFramesPerBar(bars: Int): Int = nativeTransportFramesPerBar(bars)
    fun transportStartMetronome(
        beats: Int,
        firstIsDownbeat: Boolean = true,
        everyBeatPattern: Boolean = true
    ) = nativeTransportStartMetronome(beats, firstIsDownbeat, everyBeatPattern)
    /**
     * Run the metronome continuously (as an in-take reference click) until
     * [transportStopMetronome] is called. NoisyPad wires this to a user-facing
     * "metronome during recording" toggle.
     */
    fun transportStartMetronomeContinuous(everyBeatPattern: Boolean = true) =
        nativeTransportStartMetronomeContinuous(everyBeatPattern)
    fun transportStopMetronome() = nativeTransportStopMetronome()
    fun transportIsMetronomeRunning(): Boolean = nativeTransportIsMetronomeRunning()
    fun transportIsMetronomeContinuous(): Boolean = nativeTransportIsMetronomeContinuous()
    fun transportGetRemainingBeats(): Int = nativeTransportGetRemainingBeats()

    // Export / Import (call from IO thread)
    fun looperExportMix(filePath: String): Boolean = nativeLooperExportMix(filePath)
    fun looperExportTrack(trackIndex: Int, filePath: String): Boolean = nativeLooperExportTrack(trackIndex, filePath)
    fun looperImportTrack(trackIndex: Int, filePath: String, sampleRate: Int): Boolean = nativeLooperImportTrack(trackIndex, filePath, sampleRate)

    // ========== EXPORT V2 (suspend wrappers, professional) ==========
    //
    // These run on Dispatchers.IO so the caller (UI / coroutine scope) is not
    // blocked during file write. Cancellation: call looperCancelExport() from
    // any thread; the C++ side bails at the next track/iteration.
    //
    // bitDepth: 16 (PCM), 24 (PCM), or 32 (IEEE float).
    // repeatLoops: number of full-loop iterations (>=1).
    // countInBeats: leading silence in beats (uses Transport's framesPerBeat).
    // applyLimiter: true-peak limiter (-1 dBFS, 5 ms lookahead).
    // bpm=0 → engine uses the current Transport BPM in metadata.

    enum class ExportBitDepth(val raw: Int) { PCM_16(16), PCM_24(24), FLOAT_32(32) }

    suspend fun looperExportMixPro(
        filePath: String,
        bitDepth: ExportBitDepth = ExportBitDepth.PCM_16,
        repeatLoops: Int = 1,
        countInBeats: Int = 0,
        applyLimiter: Boolean = true,
        projectName: String? = null,
        artist: String? = null,
        comment: String? = null,
        bpm: Int = 0
    ): Boolean = withContext(Dispatchers.IO) {
        nativeLooperExportMixV2(
            filePath, bitDepth.raw, repeatLoops, countInBeats,
            applyLimiter, projectName, artist, comment, bpm
        )
    }

    /**
     * Export every active track as a separate WAV file in `directory`. Each stem
     * shares the same length and bit depth so they can be loaded into a DAW.
     * @return number of stems written, or -1 on failure.
     */
    suspend fun looperExportStems(
        directory: String,
        bitDepth: ExportBitDepth = ExportBitDepth.PCM_16,
        repeatLoops: Int = 1,
        countInBeats: Int = 0,
        applyLimiter: Boolean = true,
        bpm: Int = 0
    ): Int = withContext(Dispatchers.IO) {
        nativeLooperExportStems(
            directory, bitDepth.raw, repeatLoops, countInBeats, applyLimiter, bpm
        )
    }

    /** Polled by UI to render a progress bar. [0..1]. */
    fun looperGetExportProgress(): Float = nativeLooperGetExportProgress()

    /** Request cancellation of the current export. The export bails at the next iteration. */
    fun looperCancelExport() = nativeLooperCancelExport()

    fun looperIsExportInProgress(): Boolean = nativeLooperIsExportInProgress()

    // ========== TELEMETRY ==========
    //
    // Lock-free monotonic counters for runtime observability. Useful for
    // debug overlays, crash reports, or in-app diagnostics. Values are not
    // synchronized — read with `looperGetTelemetry()` for an atomic snapshot.

    data class LooperTelemetry(
        val framesDropped: Long,
        val exportsCompleted: Long,
        val exportsFailed: Long,
        val stemsWritten: Long,
        val armedTriggered: Long,
    )

    fun looperGetTelemetry(): LooperTelemetry = LooperTelemetry(
        framesDropped    = nativeLooperGetFramesDropped(),
        exportsCompleted = nativeLooperGetExportsCompleted(),
        exportsFailed    = nativeLooperGetExportsFailed(),
        stemsWritten     = nativeLooperGetStemsWritten(),
        armedTriggered   = nativeLooperGetArmedTriggered(),
    )

    fun looperResetTelemetry() = nativeLooperResetTelemetry()
}
