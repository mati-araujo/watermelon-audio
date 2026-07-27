package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.EffectChainSnapshot
import com.watermellonstudios.audio.api.EffectParameterUpdate
import com.watermellonstudios.audio.api.IAudioNativeBridge
import com.watermellonstudios.audio.api.NativeEffectSnapshot
import com.watermellonstudios.audio.domain.effect.EffectParameter
import com.watermellonstudios.audio.domain.effect.EffectType
import com.watermellonstudios.audio.domain.error.NativeBridgeException
import com.watermellonstudios.audio.domain.input.InputMetering
import com.watermellonstudios.audio.domain.usb.UsbLatencyProfile
import cnames.structs.WmaEngine
import com.watermellonstudios.audio.internal.cinterop.*
import kotlinx.cinterop.CPointer
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.IntVar
import kotlinx.cinterop.FloatVar
import kotlinx.cinterop.addressOf
import kotlinx.cinterop.alloc
import kotlinx.cinterop.allocArray
import kotlinx.cinterop.get
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.ptr
import kotlinx.cinterop.toKString
import kotlinx.cinterop.usePinned
import kotlinx.cinterop.value

/**
 * Implementación de [IAudioNativeBridge] para iOS sobre los bindings de cinterop
 * (WA-3.2).
 *
 * ## Por qué no hay JNI acá
 *
 * Android habla con el motor por JNI; iOS lo hace por la **misma C API**
 * (`watermelon_audio.h`) vía cinterop, sin shim de Swift ni Objective-C++ en el
 * medio (decisión D1). Las llamadas son directas, que es lo que importa para
 * [setXY]: corre una vez por frame de gesto.
 *
 * ## Disciplina de concurrencia
 *
 * Idéntica a la de Android, y no por coincidencia: los mutexes por categoría y el
 * envelope de errores viven en [BridgeConcurrency] (commonMain, WA-1.4). Dos
 * implementaciones separadas del mismo contrato divergen en silencio.
 *
 * - Las operaciones que mutan estado observable van por `guarded(...)` y devuelven
 *   `Result<T>`.
 * - Las lecturas van por `serialized(...)`.
 * - Los parámetros de tiempo real y los `*Sync` **no toman locks**: del otro lado
 *   hay `std::atomic` y el costo de un mutex por frame no se justifica. Es la
 *   misma regla que aplica Android.
 *
 * ## Regla RT (D6)
 *
 * El thread de audio **jamás** entra a Kotlin: el GC de Kotlin/Native no es
 * RT-safe. Todo lo de acá se llama desde threads de UI o de coroutine; el estado
 * sale por polling, nunca por callback desde el render.
 *
 * ## Qué no soporta iOS
 *
 * USB queda fuera de alcance por plataforma (D4): [isUsbBackendAvailable] es
 * siempre `false`, y [createSplitBackend] y [setUsbLatencyProfile] fallan de forma
 * explícita en vez de fingir que hicieron algo.
 */
@OptIn(ExperimentalForeignApi::class)
internal class IosAudioBridge : IAudioNativeBridge {

    private val concurrency = BridgeConcurrency()

    /**
     * El motor se crea acá y vive lo que viva el bridge.
     *
     * A diferencia de Android —que lo crea perezosamente en `ensureEngine()`— no
     * hay nada que esperar en iOS: no hay `System.loadLibrary`, el `.a` está
     * linkeado estáticamente en el klib. Si `wma_engine_create()` devuelve null no
     * hay bridge posible, así que falla acá y no en la primera llamada.
     */
    private val engine: CPointer<WmaEngine> = requireNotNull(wma_engine_create()) {
        "wma_engine_create() devolvió null: no se pudo crear el motor nativo"
    }

    // ==================== LIFECYCLE ====================

    /**
     * `WMA_FADE_DEFAULT` — pedirle al motor su fade propio, que NO es lo mismo
     * que pedir 0 ms.
     *
     * `startEngine()` y `startEngineWithFade(0)` son dos operaciones distintas:
     * la primera toma el default del motor (una rampa de 10 ms), la segunda
     * corta de una. Android siempre las distinguió porque son dos funciones JNI
     * distintas; acá las dos pasaban `0` y las dos terminaban en el default, así
     * que la misma llamada hacía cosas distintas según la plataforma. La C API
     * ahora tiene cómo decirlo y esto lo dice (WA-2.6, categoría `lifecycle`).
     *
     * El valor viene del header por cinterop, no copiado a mano: si el `#define`
     * cambiara, esto deja de compilar en vez de divergir en silencio.
     */
    private val fadeDefault: Int = WMA_FADE_DEFAULT

    override suspend fun startEngine(): Result<Unit> =
        concurrency.guarded(BridgeConcurrency.Category.LIFECYCLE, "startEngine") {
            wma_engine_start(engine, fadeDefault).asUnitResult("startEngine")
        }

    override suspend fun stopEngine(): Result<Unit> =
        concurrency.guarded(BridgeConcurrency.Category.LIFECYCLE, "stopEngine") {
            wma_engine_stop(engine, fadeDefault).asUnitResult("stopEngine")
        }

    override suspend fun startEngineWithFade(fadeTimeMs: Int): Result<Unit> =
        concurrency.guarded(BridgeConcurrency.Category.LIFECYCLE, "startEngineWithFade") {
            wma_engine_start(engine, fadeTimeMs.coerceAtLeast(0)).asUnitResult("startEngineWithFade")
        }

    override suspend fun stopEngineWithFade(fadeTimeMs: Int): Result<Unit> =
        concurrency.guarded(BridgeConcurrency.Category.LIFECYCLE, "stopEngineWithFade") {
            wma_engine_stop(engine, fadeTimeMs.coerceAtLeast(0)).asUnitResult("stopEngineWithFade")
        }

    override suspend fun pauseEngineWithFade(fadeTimeMs: Int): Result<Unit> =
        concurrency.guarded(BridgeConcurrency.Category.LIFECYCLE, "pauseEngineWithFade") {
            wma_engine_pause(engine, fadeTimeMs.coerceAtLeast(0)).asUnitResult("pauseEngineWithFade")
        }

    override suspend fun resumeEngineWithFade(fadeTimeMs: Int): Result<Unit> =
        concurrency.guarded(BridgeConcurrency.Category.LIFECYCLE, "resumeEngineWithFade") {
            wma_engine_resume(engine, fadeTimeMs.coerceAtLeast(0)).asUnitResult("resumeEngineWithFade")
        }

    // Variantes sincrónicas: las consume AudioEngineImpl desde contextos no-suspend.
    // No toman el mutex de lifecycle a propósito — igual que en Android, donde
    // tampoco lo hacen. El motor serializa internamente.

    override fun startEngineWithFadeSync(fadeTimeMs: Int) {
        wma_engine_start(engine, fadeTimeMs.coerceAtLeast(0))
    }

    override fun stopEngineWithFadeSync(fadeTimeMs: Int) {
        wma_engine_stop(engine, fadeTimeMs.coerceAtLeast(0))
    }

    override fun pauseEngineWithFadeSync(fadeTimeMs: Int) {
        wma_engine_pause(engine, fadeTimeMs.coerceAtLeast(0))
    }

    override fun resumeEngineWithFadeSync(fadeTimeMs: Int) {
        wma_engine_resume(engine, fadeTimeMs.coerceAtLeast(0))
    }

    override fun stopEngineSync() {
        // La contraparte de nativeStopEngine(): sin rampa, no una rampa de 0 ms.
        wma_engine_stop(engine, fadeDefault)
    }

    // ==================== STATE QUERIES ====================

    override fun getEngineState(): Int = wma_get_engine_state(engine)
    override fun getStateVersion(): Long = wma_get_state_version(engine).toLong()
    override fun hasStreamError(): Boolean = wma_has_error(engine)
    override fun getLastStreamErrorCode(): Int = wma_get_last_error_code(engine)
    override fun clearStreamError() = wma_clear_error(engine)
    override fun getIsPaused(): Boolean = wma_is_paused(engine)
    override fun isEngineInitialized(): Boolean = wma_is_initialized(engine)
    override fun hasInitializationFailed(): Boolean = wma_has_init_failed(engine)
    override fun getMasterVolume(): Float = wma_get_master_volume(engine)

    /**
     * `[sampleRate, bufferSize, latencyMs]`, o `null` si el motor no puede
     * informarlos todavía (típicamente porque no hay stream abierto).
     */
    override fun getStreamInfoArray(): FloatArray? = memScoped {
        val sampleRate = alloc<IntVar>()
        val bufferSize = alloc<IntVar>()
        val latencyMs = alloc<FloatVar>()

        if (!wma_get_stream_info(engine, sampleRate.ptr, bufferSize.ptr, latencyMs.ptr)) {
            return@memScoped null
        }
        floatArrayOf(
            sampleRate.value.toFloat(),
            bufferSize.value.toFloat(),
            latencyMs.value,
        )
    }

    // ==================== FADE ====================

    override fun getCurrentFadeVolume(): Float = wma_get_fade_volume(engine)
    override fun getTargetFadeVolume(): Float = wma_get_target_fade_volume(engine)
    override fun getIsFading(): Boolean = wma_is_fading(engine)
    override fun getFadeProgress(): Float = wma_get_fade_progress(engine)

    // ==================== REAL-TIME PARAMS (lock-free) ====================

    /**
     * @param coalesce se ignora en iOS. En Android existe un coalescer que junta
     *   updates de gesto para amortizar el costo de cruzar JNI; una llamada de
     *   cinterop no tiene ese costo, así que agregar el buffer sólo sumaría
     *   latencia de control. Si una medición de WA-4.3 muestra lo contrario, el
     *   lugar para el coalescer es acá.
     */
    override fun setXY(x: Float, y: Float, coalesce: Boolean) {
        wma_set_xy(engine, x.coerceIn(0f, 1f), y.coerceIn(0f, 1f))
    }

    override fun setFrequencyAndAmplitude(frequency: Float, amplitude: Float) {
        wma_set_frequency_amplitude(engine, frequency, amplitude)
    }

    override fun setFrequencyRange(minHz: Float, maxHz: Float) {
        wma_set_frequency_range(engine, minHz, maxHz)
    }

    override fun setMasterVolume(volume: Float) {
        wma_set_master_volume(engine, volume.coerceIn(0f, 1f))
    }

    override fun setOscillatorType(type: Int) = wma_set_oscillator_type(engine, type)
    override fun setSecondaryOscillatorType(type: Int) = wma_set_secondary_oscillator_type(engine, type)
    override fun setEngineType(type: Int) = wma_set_engine_type(engine, type)
    override fun setEngineParameter(paramId: Int, value: Float) = wma_set_engine_param(engine, paramId, value)
    override fun getEngineType(): Int = wma_get_engine_type(engine)
    override fun setBpm(bpm: Float) = wma_set_bpm(engine, bpm)
    override fun getBpm(): Float = wma_get_bpm(engine)

    override fun setModulatorType(type: Int) {
        wma_set_modulator_type(engine, type)
    }

    override fun setModulatorParameter(paramId: Int, value: Float) {
        wma_set_modulator_param(engine, paramId, value)
    }

    // ==================== EFFECTS (variantes sync para AudioEngineImpl) ====================

    override fun addEffectSync(typeId: Int): Boolean = wma_effect_add(engine, typeId) >= 0
    override fun removeEffectSync(index: Int) { wma_effect_remove(engine, index) }
    override fun setEffectParameterSync(effectIndex: Int, paramId: Int, value: Float) {
        wma_effect_set_param(engine, effectIndex, paramId, value)
    }
    override fun getEffectParameterSync(effectIndex: Int, paramId: Int): Float =
        wma_effect_get_param(engine, effectIndex, paramId)
    override fun setEffectBypassSync(index: Int, bypass: Boolean) {
        wma_effect_set_bypass(engine, index, bypass)
    }
    override fun setEffectsBypassSync(bypass: Boolean) {
        wma_effect_set_global_bypass(engine, bypass)
    }
    override fun isEffectsBypassedSync(): Boolean = wma_effect_is_global_bypassed(engine)
    override fun reorderEffectsSync(fromIndex: Int, toIndex: Int) {
        wma_effect_reorder(engine, fromIndex, toIndex)
    }

    // ==================== EFFECT ROUTING ====================

    override fun setRoutingMode(mode: Int) = wma_set_routing_mode(engine, mode)
    override fun getRoutingMode(): Int = wma_get_routing_mode(engine)
    override fun setParallelMix(mix: Float) = wma_set_parallel_mix(engine, mix)
    override fun setFeedbackAmount(amount: Float) = wma_set_feedback_amount(engine, amount)

    // ==================== WAVEFORM ====================

    /**
     * `usePinned` fija el array durante la llamada para que C escriba directo en
     * él: el GC de Kotlin/Native podría moverlo, y copiar un buffer de waveform en
     * cada frame de UI sería gasto puro.
     */
    override fun getWaveformSamples(buffer: FloatArray, size: Int): Int {
        if (buffer.isEmpty() || size <= 0) return 0
        val requested = size.coerceAtMost(buffer.size)
        return buffer.usePinned { pinned ->
            wma_get_waveform_samples(engine, pinned.addressOf(0), requested)
        }
    }

    // ==================== VOICE SYSTEM ====================

    override fun enableVoiceSystem(enabled: Boolean) = wma_voice_enable(engine, enabled)
    override fun isVoiceSystemEnabled(): Boolean = wma_voice_is_enabled(engine)
    override fun getActiveVoiceCount(): Int = wma_voice_get_active_count(engine)
    override fun setMaxVoices(maxVoices: Int) = wma_voice_set_max(engine, maxVoices)
    override fun setVoiceStealingStrategy(strategyId: Int) =
        wma_voice_set_stealing_strategy(engine, strategyId)

    override fun updateMultiTouch(count: Int, touchData: FloatArray?) {
        if (touchData == null || count <= 0) {
            wma_voice_update_multi_touch(engine, null, 0)
            return
        }
        touchData.usePinned { pinned ->
            wma_voice_update_multi_touch(engine, pinned.addressOf(0), count)
        }
    }

    // ==================== DUAL TOUCH ====================

    override fun setDualTouchMode(enabled: Boolean) = wma_set_dual_touch_mode(engine, enabled)
    override fun setDualTouchMixMode(modeId: Int) = wma_set_dual_touch_mix_mode(engine, modeId)

    override fun setDualTouch(
        x1: Float, y1: Float, freq1: Float, amp1: Float, pressure1: Float,
        x2: Float, y2: Float, freq2: Float, amp2: Float, pressure2: Float,
        distance: Float, angle: Float,
    ) {
        wma_set_dual_touch(
            engine,
            x1, y1, freq1, amp1, pressure1,
            x2, y2, freq2, amp2, pressure2,
            distance, angle,
        )
    }

    // ==================== CHORD VOICES ====================

    override fun triggerChordNotes(frequencies: FloatArray, amplitude: Float, oscillatorType: Int) {
        if (frequencies.isEmpty()) return
        frequencies.usePinned { pinned ->
            wma_voice_trigger_chord(
                engine, pinned.addressOf(0), frequencies.size, amplitude, oscillatorType,
            )
        }
    }

    override fun updateChordNotes(frequencies: FloatArray, amplitude: Float) {
        if (frequencies.isEmpty()) return
        frequencies.usePinned { pinned ->
            wma_voice_update_chord(engine, pinned.addressOf(0), frequencies.size, amplitude)
        }
    }

    override fun releaseChordNotes() = wma_voice_release_chord(engine)

    // ==================== MODE ====================

    override suspend fun setAudioMode(mode: Int): Result<Unit> =
        concurrency.guarded(BridgeConcurrency.Category.MODE, "setAudioMode") {
            wma_set_audio_mode(engine, mode)
            Result.success(Unit)
        }

    override fun getAudioMode(): Int = wma_get_audio_mode(engine)
    override fun isInModeTransition(): Boolean = wma_is_in_mode_transition(engine)

    // ==================== INPUT (sección 12) ====================
    //
    // El primer usuario real de `wma_input_*` desde Kotlin en iOS. Hasta acá el
    // bloque existía en la C API y en el JNI, y este lado estaba vacío: el
    // camino de captura de `CoreAudioBackend` se escribió a ciegas y nada lo
    // había tocado desde arriba.
    //
    // Todas son wrappers de una línea a propósito. La composición —qué
    // significa "el medidor", cuándo re-preguntar, qué hacer si no hay nodo—
    // vive en commonMain, que es donde sirve para las dos plataformas.

    override fun startInputStreamSync(): Boolean = wma_input_start(engine)
    override fun stopInputStreamSync() = wma_input_stop(engine)
    override fun isInputStreamRunning(): Boolean = wma_input_is_running(engine)
    override fun isInputStarting(): Boolean = wma_input_is_starting(engine)

    override fun setInputSourceSync(source: Int) = wma_input_set_source(engine, source)
    override fun getInputSource(): Int = wma_input_get_source(engine)

    override fun setInputGain(gainDb: Float) = wma_input_set_gain(engine, gainDb)
    override fun getInputGain(): Float = wma_input_get_gain(engine)

    override fun setNoiseGateEnabled(enabled: Boolean) =
        wma_input_set_noise_gate(engine, enabled)

    override fun isNoiseGateEnabled(): Boolean = wma_input_is_noise_gate_enabled(engine)

    override fun setNoiseGateThreshold(thresholdDb: Float) =
        wma_input_set_noise_gate_threshold(engine, thresholdDb)

    override fun isNoiseGateOpen(): Boolean = wma_input_is_noise_gate_open(engine)

    override fun getInputLevel(channel: Int): Float = wma_input_get_level(engine, channel)

    override fun getInputLevelLinear(channel: Int): Float =
        wma_input_get_level_linear(engine, channel)

    override fun isInputClipping(): Boolean = wma_input_is_clipping(engine)
    override fun getInputLatencyMs(): Float = wma_input_get_latency_ms(engine)

    /**
     * @return null si no hay nodo de entrada, que es **distinto** de "todo en
     *   cero". `wma_input_get_metering_snapshot` deja el buffer intacto cuando
     *   falla, justamente para que nadie lea ceros como una medición; devolver
     *   null preserva esa distinción hasta arriba de todo.
     */
    override fun getInputMeteringSnapshot(): FloatArray? = memScoped {
        val values = allocArray<FloatVar>(InputMetering.VALUE_COUNT)
        if (!wma_input_get_metering_snapshot(engine, values)) {
            return@memScoped null
        }
        FloatArray(InputMetering.VALUE_COUNT) { values[it] }
    }

    override fun setMonitoringEnabledSync(enabled: Boolean) =
        wma_input_set_monitoring(engine, enabled)

    override fun isMonitoringEnabled(): Boolean = wma_input_is_monitoring_enabled(engine)

    override fun setMonitoringVolume(volume: Float) =
        wma_input_set_monitoring_volume(engine, volume.coerceIn(0f, 1f))

    override fun getMonitoringVolume(): Float = wma_input_get_monitoring_volume(engine)

    override fun releaseInputNodeSync() = wma_input_release(engine)

    // ==================== BACKEND ====================

    override fun setUseBackendManager(useBackendManager: Boolean) =
        wma_set_use_backend_manager(engine, useBackendManager)

    override fun selectBackend(backendId: Int): Boolean = wma_select_backend(backendId)
    override fun getCurrentBackendType(): Int = wma_get_backend_type()

    /**
     * Siempre `false` en iOS. No hay acceso USB genérico sin DriverKit ni
     * entitlements (D4, y WA-5.1 en el backlog si el negocio lo pide).
     */
    override fun isUsbBackendAvailable(): Boolean = false

    /**
     * No soportado en iOS. `SplitBackend` compone un backend de entrada con otro
     * de salida —en Android, USB-in + Oboe-out— y ninguna de las dos mitades
     * existe acá: no hay USB (D4) y el camino de captura de `InputNode` todavía
     * no tiene adapter de CoreAudio.
     *
     * Devuelve `false` en vez de fingir éxito: un consumidor que crea haber
     * armado un split y no lo tiene es peor que uno que sabe que no pudo.
     */
    override fun createSplitBackend(inputBackendId: Int, outputBackendId: Int): Boolean = false

    /** No soportado en iOS por la misma razón que [isUsbBackendAvailable]. */
    override fun setUsbLatencyProfile(profile: UsbLatencyProfile): Result<Unit> =
        Result.failure(
            NativeBridgeException.InvalidOperation(
                "setUsbLatencyProfile: USB no está soportado en iOS (D4)",
            ),
        )

    // ==================== IEffectStateProvider ====================

    override suspend fun getEffectChainSnapshot(): EffectChainSnapshot =
        concurrency.serialized(BridgeConcurrency.Category.EFFECTS) {
            val chainSize = wma_effect_chain_size(engine)
            val effects = (0 until chainSize).map { index ->
                val typeId = wma_effect_get_type(engine, index)
                NativeEffectSnapshot(
                    index = index,
                    typeId = typeId,
                    isBypassed = wma_effect_is_bypassed(engine, index),
                    parameters = readParameters(index, typeId),
                )
            }
            EffectChainSnapshot(
                effects = effects,
                version = wma_get_state_version(engine).toLong(),
                isGloballyBypassed = wma_effect_is_global_bypassed(engine),
            )
        }

    override suspend fun getEffectParameters(index: Int): Map<Int, Float> =
        concurrency.serialized(BridgeConcurrency.Category.EFFECTS) {
            val chainSize = wma_effect_chain_size(engine)
            if (index < 0 || index >= chainSize) {
                throw NativeBridgeException.InvalidEffectIndex(index, chainSize)
            }
            readParameters(index, wma_effect_get_type(engine, index))
        }

    override suspend fun isEffectBypassed(index: Int): Boolean =
        concurrency.serialized(BridgeConcurrency.Category.EFFECTS) {
            val chainSize = wma_effect_chain_size(engine)
            if (index < 0 || index >= chainSize) {
                throw NativeBridgeException.InvalidEffectIndex(index, chainSize)
            }
            wma_effect_is_bypassed(engine, index)
        }

    override suspend fun getEffectCount(): Int =
        concurrency.serialized(BridgeConcurrency.Category.EFFECTS) {
            wma_effect_chain_size(engine)
        }

    override suspend fun getEffectType(index: Int): EffectType? =
        concurrency.serialized(BridgeConcurrency.Category.EFFECTS) {
            val chainSize = wma_effect_chain_size(engine)
            if (index < 0 || index >= chainSize) {
                return@serialized null
            }
            EffectType.fromId(wma_effect_get_type(engine, index))
        }

    // ==================== IEffectStateWriter ====================

    override suspend fun addEffect(type: EffectType): Result<Int> =
        concurrency.guarded(BridgeConcurrency.Category.EFFECTS, "addEffect") {
            if (!wma_is_initialized(engine)) {
                return@guarded Result.failure(NativeBridgeException.EngineNotInitialized())
            }
            val index = wma_effect_add(engine, type.id)
            if (index < 0) {
                Result.failure(NativeBridgeException.fromCode(index, "addEffect"))
            } else {
                Result.success(index)
            }
        }

    override suspend fun removeEffect(index: Int): Result<Unit> =
        concurrency.guarded(BridgeConcurrency.Category.EFFECTS, "removeEffect") {
            wma_effect_remove(engine, index).asUnitResult("removeEffect")
        }

    override suspend fun setParameter(effectIndex: Int, paramId: Int, value: Float): Result<Unit> =
        concurrency.guarded(BridgeConcurrency.Category.EFFECTS, "setParameter") {
            wma_effect_set_param(engine, effectIndex, paramId, value).asUnitResult("setParameter")
        }

    override suspend fun setParametersBatch(
        effectIndex: Int,
        parameters: Map<Int, Float>,
    ): Result<Unit> {
        if (parameters.isEmpty()) return Result.success(Unit)

        return concurrency.guarded(BridgeConcurrency.Category.EFFECTS, "setParametersBatch") {
            val ids = parameters.keys.toIntArray()
            val values = FloatArray(ids.size) { parameters.getValue(ids[it]) }
            ids.usePinned { pinnedIds ->
                values.usePinned { pinnedValues ->
                    wma_effect_set_params_batch(
                        engine,
                        effectIndex,
                        pinnedIds.addressOf(0),
                        pinnedValues.addressOf(0),
                        ids.size,
                    )
                }
            }.asUnitResult("setParametersBatch")
        }
    }

    /**
     * La C API no tiene una operación que abarque varios efectos de una, así que
     * se recorre efecto por efecto — pero **bajo un solo lock**, que es lo que le
     * da a la operación su semántica de lote: la cadena no cambia entre updates.
     *
     * Corta en el primer fallo, igual que Android.
     */
    override suspend fun setMultipleEffectParameters(
        updates: List<EffectParameterUpdate>,
    ): Result<Unit> {
        if (updates.isEmpty()) return Result.success(Unit)

        return concurrency.guarded(BridgeConcurrency.Category.EFFECTS, "setMultipleEffectParameters") {
            val chainSize = wma_effect_chain_size(engine)
            for (update in updates) {
                if (update.effectIndex < 0 || update.effectIndex >= chainSize) {
                    return@guarded Result.failure(
                        NativeBridgeException.InvalidEffectIndex(update.effectIndex, chainSize),
                    )
                }
            }
            for (update in updates) {
                val result = wma_effect_set_param(
                    engine, update.effectIndex, update.paramId, update.value,
                )
                if (result != WMA_OK) {
                    return@guarded result.asUnitResult("setMultipleEffectParameters")
                }
            }
            Result.success(Unit)
        }
    }

    override suspend fun setBypass(effectIndex: Int, bypassed: Boolean): Result<Unit> =
        concurrency.guarded(BridgeConcurrency.Category.EFFECTS, "setBypass") {
            wma_effect_set_bypass(engine, effectIndex, bypassed).asUnitResult("setBypass")
        }

    override suspend fun setEffectsBypass(bypassed: Boolean): Result<Unit> =
        concurrency.guarded(BridgeConcurrency.Category.EFFECTS, "setEffectsBypass") {
            wma_effect_set_global_bypass(engine, bypassed).asUnitResult("setEffectsBypass")
        }

    override suspend fun reorderEffects(fromIndex: Int, toIndex: Int): Result<Unit> =
        concurrency.guarded(BridgeConcurrency.Category.EFFECTS, "reorderEffects") {
            wma_effect_reorder(engine, fromIndex, toIndex).asUnitResult("reorderEffects")
        }

    override suspend fun clearAllEffects(): Result<Unit> =
        concurrency.guarded(BridgeConcurrency.Category.EFFECTS, "clearAllEffects") {
            wma_effect_clear_all(engine).asUnitResult("clearAllEffects")
        }

    // ==================== Helpers ====================

    /**
     * Los parámetros de un efecto no se enumeran desde C: se derivan del tipo con
     * [EffectParameter.forEffectType], que vive en commonMain. Android hace
     * exactamente lo mismo, así que ambas plataformas leen el mismo conjunto.
     */
    private fun readParameters(index: Int, typeId: Int): Map<Int, Float> {
        val type = EffectType.fromId(typeId) ?: return emptyMap()
        return EffectParameter.forEffectType(type).associate { param ->
            param.id to wma_effect_get_param(engine, index, param.id)
        }
    }

    /**
     * Traduce un [WmaResult] al `Result<Unit>` del contrato, reusando el mismo
     * `fromCode` que usa Android — los códigos son los mismos porque salen de la
     * misma C API.
     */
    private fun WmaResult.asUnitResult(operation: String): Result<Unit> =
        if (this == WMA_OK) {
            Result.success(Unit)
        } else {
            Result.failure(NativeBridgeException.fromCode(this, operation))
        }

    // ==================== TRANSPORT (reloj musical + metrónomo) ====================
    //
    // Sin locks, igual que Android: del otro lado el scheduler es RT-safe y el
    // estado que se lee son atomics. Un mutex acá sólo agregaría costo.

    override fun transportSetBeatsPerBar(beatsPerBar: Int) =
        wma_transport_set_beats_per_bar(engine, beatsPerBar)

    override fun transportGetBeatsPerBar(): Int = wma_transport_get_beats_per_bar(engine)
    override fun transportFramesPerBeat(): Int = wma_transport_frames_per_beat(engine)
    override fun transportFramesPerBar(bars: Int): Int = wma_transport_frames_per_bar(engine, bars)

    override fun transportStartMetronome(
        beats: Int,
        firstIsDownbeat: Boolean,
        everyBeatPattern: Boolean,
    ) = wma_transport_start_metronome(engine, beats, firstIsDownbeat, everyBeatPattern)

    override fun transportStartMetronomeContinuous(everyBeatPattern: Boolean) =
        wma_transport_start_metronome_continuous(engine, everyBeatPattern)

    override fun transportStopMetronome() = wma_transport_stop_metronome(engine)
    override fun transportIsMetronomeRunning(): Boolean = wma_transport_is_metronome_running(engine)
    override fun transportIsMetronomeContinuous(): Boolean =
        wma_transport_is_metronome_continuous(engine)

    override fun transportGetRemainingBeats(): Int = wma_transport_get_remaining_beats(engine)

    // ==================== LOOPER (el subconjunto de la tira) ====================
    //
    // Primer código de looper que existe en iOS. Las 79 del JNI no se suben en
    // bloque: estas 11 tienen caller (la tira del harness) y las otras no todavía.

    override fun looperPrepareTrackBars(trackIndex: Int, bars: Int, sampleRate: Int): Int =
        wma_looper_prepare_track_bars(engine, trackIndex, bars, sampleRate)

    override fun looperArmAtNextBar(trackIndex: Int): Long =
        wma_looper_arm_at_next_bar(engine, trackIndex)

    override fun looperStartRecording(trackIndex: Int) =
        wma_looper_start_recording(engine, trackIndex)

    override fun looperStopRecording() = wma_looper_stop_recording(engine)
    override fun looperStopAll() = wma_looper_stop_all(engine)
    override fun looperClearAll() = wma_looper_clear_all(engine)
    override fun looperIsRecording(): Boolean = wma_looper_is_recording(engine)
    override fun looperIsPlaying(): Boolean = wma_looper_is_playing(engine)

    override fun looperIsTrackActive(trackIndex: Int): Boolean =
        wma_looper_is_track_active(engine, trackIndex)

    override fun looperIsTrackPlaying(trackIndex: Int): Boolean =
        wma_looper_is_track_playing(engine, trackIndex)

    override fun looperExportMix(filePath: String): Boolean =
        wma_looper_export_mix(engine, filePath)

    // ==================== LOG CAPTURE ====================

    override fun setLogCaptureEnabled(enabled: Boolean) = wma_log_capture_set_enabled(enabled)

    override fun getLogCaptureDropped(): Int = wma_log_capture_dropped()

    /**
     * El batch no sale de acá. `wma_log_capture_drain()` devuelve un handle propio
     * que **hay que liberar**, y el drain es destructivo: si se pierde el handle se
     * pierden las líneas y se filtra la memoria de una vez.
     *
     * Por eso el `try/finally` envuelve **todo** el uso, incluida la lectura de
     * `count`. Y por eso se copia a `String` acá: `wma_log_batch_line()` documenta
     * que el puntero vale hasta que el batch se libera, así que un `List<CPointer>`
     * quedaría con punteros colgando apenas retorna esta función.
     *
     * Un `null` del drain es "no se pudo alocar", no "no había nada" — la C API es
     * explícita en que vacío se entrega igual como batch. Devolver el array vacío
     * es lo correcto en los dos casos para el llamador, pero no son lo mismo.
     */
    override fun drainCapturedLogs(): Array<String> {
        val batch = wma_log_capture_drain() ?: return emptyArray()
        try {
            val count = wma_log_batch_count(batch)
            if (count <= 0) return emptyArray()
            return Array(count) { i ->
                wma_log_batch_line(batch, i)?.toKString() ?: ""
            }
        } finally {
            wma_log_batch_free(batch)
        }
    }
}
