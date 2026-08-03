package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.EffectChainSnapshot
import com.watermellonstudios.audio.api.EffectParameterUpdate
import com.watermellonstudios.audio.api.IAudioNativeBridge
import com.watermellonstudios.audio.api.ILooperBridge
import com.watermellonstudios.audio.api.LooperStateListener
import com.watermellonstudios.audio.api.NativeEffectSnapshot
import com.watermellonstudios.audio.domain.effect.EffectParameter
import com.watermellonstudios.audio.domain.effect.EffectType
import com.watermellonstudios.audio.domain.error.NativeBridgeException
import com.watermellonstudios.audio.domain.input.InputMetering
import com.watermellonstudios.audio.domain.looper.ExportBitDepth
import com.watermellonstudios.audio.domain.usb.UsbLatencyProfile
import cnames.structs.WmaEngine
import com.watermellonstudios.audio.internal.cinterop.*
import kotlinx.cinterop.COpaquePointer
import kotlinx.cinterop.CPointer
import kotlinx.cinterop.StableRef
import kotlinx.cinterop.asStableRef
import kotlinx.cinterop.staticCFunction
import kotlinx.cinterop.MemScope
import kotlinx.cinterop.cstr
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
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

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
/**
 * Lo que el puntero `user_data` del callback de C apunta, y lo que hace segura toda
 * esta sección.
 *
 * El despachador del motor **no** garantiza que no haya un evento en vuelo después de
 * quitar el sink: toma una copia del sink una vez por pasada, así que un evento
 * levantado justo antes del `null` todavía llega. O sea que liberar el `StableRef`
 * apenas se desregistra es un use-after-free, no una carrera que se gane ordenando las
 * llamadas.
 *
 * La salida es no liberarlo nunca. El `StableRef` apunta a este holder —uno por bridge,
 * y el bridge vive lo que vive el proceso— y lo que se pone y se saca es [listener].
 * Un evento en vuelo encuentra el holder vivo y el campo en `null`, y no hace nada.
 * Cuesta un objeto por bridge; la alternativa cuesta un crash intermitente.
 */
private class LooperListenerHolder {
    /**
     * Lo escribe el hilo de UI y lo lee el worker del motor: sin `@Volatile` el lector
     * puede no ver nunca la escritura.
     */
    @kotlin.concurrent.Volatile
    var listener: LooperStateListener? = null
}

/**
 * El puente de C a Kotlin. Es `staticCFunction`, así que **no puede capturar nada** —
 * por eso el holder viaja por `user_data` y esto vive a nivel de archivo y no adentro
 * de la clase.
 *
 * Corre en el worker del motor, no en el thread de audio (ver
 * [ILooperBridge.setLooperStateListener]) y no en el hilo de UI. Kotlin/Native adjunta
 * solo el hilo foráneo, así que llamar a Kotlin desde acá es legal; lo que NO es legal
 * es tocar nada que espere estar en el hilo principal, y por eso el contrato le pide al
 * consumidor que marshalee él.
 *
 * Un `type` desconocido se ignora en vez de romper: el enum de C puede crecer, y una
 * versión vieja de esta fachada tiene que sobrevivir a un motor más nuevo.
 */
/**
 * El reparto de un evento a su método, **afuera** del `staticCFunction` a propósito.
 *
 * Adentro sería intestable: un `staticCFunction` privado no se puede llamar desde un
 * test, y el único camino para llegar a él es el thread de audio del motor. Y este
 * `when` es justo donde vive el bug silencioso de esta sección — cruzar progreso con
 * pico no rompe nada, sólo hace que la UI muestre lo que no es.
 *
 * Un `type` desconocido se ignora en vez de romper: el enum de C puede crecer, y una
 * versión vieja de esta fachada tiene que sobrevivir a un motor más nuevo.
 */
@OptIn(ExperimentalForeignApi::class)
internal fun dispatchLooperEvent(
    listener: LooperStateListener,
    type: Int,
    trackIndex: Int,
    value: Float,
) {
    when (type) {
        WMA_LOOPER_EVENT_PROGRESS.toInt() -> listener.onTrackProgress(trackIndex, value)
        WMA_LOOPER_EVENT_PLAYING_CHANGED.toInt() ->
            listener.onTrackPlayingChanged(trackIndex, value != 0f)
        WMA_LOOPER_EVENT_PEAK_CHANGED.toInt() -> listener.onTrackPeakChanged(trackIndex, value)
        WMA_LOOPER_EVENT_RECORD_PROGRESS.toInt() ->
            listener.onTrackRecordProgress(trackIndex, value)
        WMA_LOOPER_EVENT_TRACK_COMPLETED.toInt() -> listener.onTrackCompleted(trackIndex)
        else -> Unit
    }
}

@OptIn(ExperimentalForeignApi::class)
private val looperEventTrampoline = staticCFunction<Int, Int, Float, COpaquePointer?, Unit> {
        type, trackIndex, value, userData ->
    val listener = userData?.asStableRef<LooperListenerHolder>()?.get()?.listener
    if (listener != null) {
        dispatchLooperEvent(listener, type, trackIndex, value)
    }
}

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
    override fun getSynthVolume(): Float = wma_get_synth_volume(engine)

    /** La variante lock-free del conteo de efectos. Ver el KDoc en la interfaz. */
    override fun getEffectChainSize(): Int = wma_effect_chain_size(engine)

    override fun isUsingReducedBuffers(): Boolean = wma_is_using_reduced_buffers(engine)

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

    override fun setSynthVolume(volume: Float) {
        // `takeIf { it.isFinite() }` y no un `coerceIn` a secas: NaN atraviesa
        // coerceIn sin recortarse, y del otro lado multiplica el buffer. Mismo
        // guard que el lado de Android.
        val safe = volume.takeIf { it.isFinite() } ?: return
        wma_set_synth_volume(engine, safe.coerceIn(0f, 1f))
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

    /**
     * El guard de rango es el mismo que el de Android, y no es redundante con el de la
     * C API: `wma_set_audio_mode` rechaza el valor y **vuelve sin decirlo**, porque
     * retorna `void`. Sin este chequeo iOS devolvía `Result.success` habiendo hecho
     * cero, mientras Android devolvía `IllegalArgumentException` — mismo `commonMain`,
     * dos contratos.
     *
     * Lo destapó el control de modo del harness (WA-1.4, call site 26).
     */
    override suspend fun setAudioMode(mode: Int): Result<Unit> =
        concurrency.guarded(BridgeConcurrency.Category.MODE, "setAudioMode") {
            if (mode !in 0..2) {
                return@guarded Result.failure(IllegalArgumentException("Invalid mode: $mode"))
            }
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
     * **Se llama de verdad, y no es un descuido de D4.**
     *
     * `wma_configure_usb_backend` termina en `BackendManager::setSampleRate()`, el
     * manager compartido — no toca ningún backend USB. En iOS el símbolo linkea (está
     * en el `.a` del simulador, verificado con `nm`) y el sample rate se aplica. Un
     * no-op acá descartaría en silencio lo único que esta llamada hace.
     *
     * Contrasta a propósito con [isUsbBackendAvailable], que reporta una capacidad y por
     * eso sí tiene que decir `false`.
     */
    override fun configureUsbBackend(sampleRate: Int, channels: Int, bitDepth: Int) =
        wma_configure_usb_backend(sampleRate, channels, bitDepth)

    /** Ídem: detrás es `BackendManager::setFullDuplexEnabled`, que en iOS sí aplica. */
    override fun setUsbStreamingMode(modeId: Int) = wma_set_usb_streaming_mode(modeId)

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

    // ==================== FILTRO DE VOZ ====================
    //
    // Los rangos se repiten acá y en Android a propósito: son contrato declarado en
    // [IAudioNativeBridge], y quien lo implemente tiene que cumplirlo. Fuera de rango
    // NO se recorta — ver el porqué en la interfaz.

    override fun setVoiceFilterEnabled(enabled: Boolean) =
        wma_voice_filter_set_enabled(engine, enabled)

    override fun setVoiceFilterCutoff(hz: Float) {
        if (!hz.isFinite() || hz < MIN_FILTER_HZ || hz > MAX_FILTER_HZ) return
        wma_voice_filter_set_cutoff(engine, hz)
    }

    override fun setVoiceFilterResonance(q: Float) {
        if (!q.isFinite() || q < 0f || q > 1f) return
        wma_voice_filter_set_resonance(engine, q)
    }

    override fun setVoiceFilterMode(mode: Int) {
        if (mode < 0 || mode > MAX_FILTER_MODE) return
        wma_voice_filter_set_mode(engine, mode)
    }

    // ==================== AUTOMATIZACIÓN Y MAPEO XY ====================
    //
    // Acá el contrato SÍ manda recortar en vez de descartar: son valores normalizados
    // por definición. Lo que se descarta es lo no finito, que no tiene borde al que
    // recortar — `NaN.coerceIn(0f, 1f)` sigue siendo `NaN` y llegaría al motor.

    override fun setMappingConfig(
        axis: Int,
        effectIndex: Int,
        paramId: Int,
        curve: Int,
        polarity: Int,
        mapMin: Float,
        mapMax: Float,
        inverted: Boolean,
    ) {
        if (!mapMin.isFinite() || !mapMax.isFinite()) return
        wma_set_mapping_config(
            engine, axis, effectIndex, paramId, curve, polarity, mapMin, mapMax, inverted,
        )
    }

    override fun clearMappingConfig(axis: Int) = wma_clear_mapping_config(engine, axis)

    override fun applyAutomation(axis: Int, normalizedValue: Float) {
        if (!normalizedValue.isFinite()) return
        wma_apply_automation(engine, axis, normalizedValue.coerceIn(0f, 1f))
    }

    override fun setAutomationParameter(effectIndex: Int, paramId: Int, xyValue: Float) {
        if (!xyValue.isFinite()) return
        wma_set_automation_param(engine, effectIndex, paramId, xyValue.coerceIn(0f, 1f))
    }

    /**
     * Vive lo que vive el bridge y **su `StableRef` no se libera nunca**, a propósito.
     * Ver [LooperListenerHolder].
     */
    private val looperListenerHolder = LooperListenerHolder()
    private val looperListenerRef: StableRef<LooperListenerHolder> by lazy {
        StableRef.create(looperListenerHolder)
    }

    // ==================== LOOPER ====================
    //
    // Empezó siendo el subconjunto de 11 que necesitaba la tira del harness; ahora es
    // [ILooperBridge] entera — 61 miembros, los que NoisyPad realmente llama. Las que
    // siguen sin subir (telemetría, picos, tail, pre-roll) siguen sin caller.
    //
    // Nada de esto toma lock. El looper serializa adentro y estas llamadas vienen de la
    // UI o de una coroutine, nunca del render: la regla RT (D6) sigue valiendo, y por
    // eso todo lo que la UI necesita saber se lee por polling y no llega por callback.

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


    // ---------- habilitación y transporte ----------

    override fun looperSetEnabled(enabled: Boolean) = wma_looper_set_enabled(engine, enabled)

    override fun looperPause() = wma_looper_pause(engine)
    override fun looperResume() = wma_looper_resume(engine)

    override fun looperTriggerClick(isDownbeat: Boolean) =
        wma_looper_trigger_click(engine, isDownbeat)

    // ---------- preparar pistas ----------

    /**
     * `wma_looper_prepare_track` devuelve `WmaResult`, no un booleano.
     *
     * Android compara `>= 0` contra el `int` que le pasa el JNI; acá el tipo es el enum
     * y la comparación correcta es contra [WMA_OK]. Da lo mismo hoy —todos los errores
     * son negativos— pero si alguna vez se agrega un código de éxito distinto de 0, el
     * `>= 0` sigue andando por casualidad y esto sigue andando por definición.
     */
    override fun looperPrepareTrack(trackIndex: Int, lengthFrames: Int, sampleRate: Int): Boolean =
        wma_looper_prepare_track(engine, trackIndex, lengthFrames, sampleRate) == WMA_OK

    override fun looperSetFreeLength(freeLength: Boolean) =
        wma_looper_set_free_length(engine, freeLength)

    override fun looperFinalizeFreeLoop(
        trackIndex: Int,
        loopStart: Int,
        loopEnd: Int,
        tailFrames: Int,
    ): Boolean = wma_looper_finalize_free_loop(engine, trackIndex, loopStart, loopEnd, tailFrames)

    override fun looperSetCapabilities(budgetBytes: Long, maxTracks: Int, maxFreeSeconds: Int) =
        wma_looper_set_capabilities(engine, budgetBytes, maxTracks, maxFreeSeconds)

    // ---------- armado y grabación ----------

    override fun looperArmInFrames(trackIndex: Int, offsetFrames: Long): Long =
        wma_looper_arm_in_frames(engine, trackIndex, offsetFrames)

    override fun looperArmSyncedToLoop(trackIndex: Int, latencyFrames: Long): Long =
        wma_looper_arm_synced_to_loop(engine, trackIndex, latencyFrames)

    override fun looperCancelArm() = wma_looper_cancel_arm(engine)
    override fun looperAbortRecording() = wma_looper_abort_recording(engine)

    override fun looperStartOverdub(trackIndex: Int) = wma_looper_start_overdub(engine, trackIndex)

    // ---------- pistas ----------

    override fun looperClearTrack(trackIndex: Int) = wma_looper_clear_track(engine, trackIndex)
    override fun looperPauseTrack(trackIndex: Int) = wma_looper_pause_track(engine, trackIndex)
    override fun looperResumeTrack(trackIndex: Int) = wma_looper_resume_track(engine, trackIndex)

    override fun looperResetTrackPlayHead(trackIndex: Int) =
        wma_looper_reset_track_playhead(engine, trackIndex)

    override fun looperTrimTrack(trackIndex: Int): Boolean = wma_looper_trim_track(engine, trackIndex)

    // ---------- mezcla ----------

    override fun looperSetMasterVolume(volume: Float) = wma_looper_set_master_volume(engine, volume)
    override fun looperGetMasterVolume(): Float = wma_looper_get_master_volume(engine)

    override fun looperSetTrackVolume(trackIndex: Int, volume: Float) =
        wma_looper_set_track_volume(engine, trackIndex, volume)

    override fun looperSetTrackPan(trackIndex: Int, pan: Float) =
        wma_looper_set_track_pan(engine, trackIndex, pan)

    override fun looperSetTrackMuted(trackIndex: Int, muted: Boolean) =
        wma_looper_set_track_muted(engine, trackIndex, muted)

    override fun looperSetTrackSpeed(trackIndex: Int, speed: Float) =
        wma_looper_set_track_speed(engine, trackIndex, speed)

    override fun looperGetTrackSpeed(trackIndex: Int): Float =
        wma_looper_get_track_speed(engine, trackIndex)

    override fun looperSetTrackPlayCount(trackIndex: Int, plays: Int) =
        wma_looper_set_track_play_count(engine, trackIndex, plays)

    override fun looperSetTrackPercussionMode(trackIndex: Int, percussion: Boolean) =
        wma_looper_set_track_percussion_mode(engine, trackIndex, percussion)

    // ---------- región de loop ----------

    override fun looperSetTrackLoopRegion(trackIndex: Int, startFrame: Long, endFrame: Long) =
        wma_looper_set_track_loop_region(engine, trackIndex, startFrame, endFrame)

    override fun looperResetTrackLoopRegion(trackIndex: Int) =
        wma_looper_reset_track_loop_region(engine, trackIndex)

    override fun looperGetTrackLoopStart(trackIndex: Int): Int =
        wma_looper_get_track_loop_start(engine, trackIndex)

    override fun looperGetTrackLoopEnd(trackIndex: Int): Int =
        wma_looper_get_track_loop_end(engine, trackIndex)

    // ---------- lectura para la UI ----------

    override fun looperGetProgress(): Float = wma_looper_get_progress(engine)
    override fun looperGetRecordProgress(): Float = wma_looper_get_record_progress(engine)
    override fun looperGetMasterLoopFrames(): Int = wma_looper_get_master_loop_frames(engine)

    override fun looperGetTrackLengthFrames(trackIndex: Int): Int =
        wma_looper_get_track_length_frames(engine, trackIndex)

    /**
     * El array se aloca acá y C lo llena en el lugar, con `usePinned` — sin copia
     * intermedia, igual que [getWaveformSamples].
     *
     * **Se devuelve el array entero aunque C haya escrito menos bins**, que es lo que
     * hace Android: el retorno de `wma_looper_get_track_waveform` se ignora en las dos
     * plataformas y los bins que sobran quedan en 0. No es descuido — un array de largo
     * variable obligaría a cada llamador de UI a reescalar su dibujo.
     */
    override fun looperGetTrackWaveform(trackIndex: Int, numBins: Int): FloatArray {
        val bins = FloatArray(numBins)
        if (numBins <= 0) return bins
        bins.usePinned { pinned ->
            wma_looper_get_track_waveform(engine, trackIndex, pinned.addressOf(0), numBins)
        }
        return bins
    }

    // ---------- undo ----------

    override fun looperSaveUndoSnapshot(trackIndex: Int): Boolean =
        wma_looper_save_undo(engine, trackIndex)

    override fun looperRestoreUndo(trackIndex: Int): Boolean =
        wma_looper_restore_undo(engine, trackIndex)

    override fun looperHasUndo(trackIndex: Int): Boolean = wma_looper_has_undo(engine, trackIndex)

    // ---------- análisis ----------

    /**
     * `(0, 0)` cuando C dice que no hay contenido, que es el mismo par que devuelve
     * Android — allá sale de desempaquetar el `long` en 0 que entrega el JNI.
     */
    override fun looperFindContentBounds(trackIndex: Int, thresholdRatio: Float): Pair<Int, Int> =
        memScoped {
            val first = alloc<IntVar>()
            val last = alloc<IntVar>()

            if (!wma_looper_find_content_bounds(
                    engine, trackIndex, thresholdRatio, first.ptr, last.ptr,
                )
            ) {
                return@memScoped 0 to 0
            }
            first.value to last.value
        }

    /**
     * El buffer se dimensiona con [maxOnsets] y **el array que sale tiene el largo real**,
     * no el reservado: acá sí importa, porque cada elemento es una posición de onset y un
     * cero de relleno sería un transitorio en el frame 0 — o sea un dato inventado. Es la
     * diferencia con [looperGetTrackWaveform], donde el relleno es silencio y no miente.
     */
    override fun looperDetectOnsets(
        trackIndex: Int,
        maxOnsets: Int,
        hopFrames: Int,
        sensitivity: Float,
    ): IntArray {
        if (maxOnsets <= 0) return IntArray(0)
        return memScoped {
            val out = allocArray<IntVar>(maxOnsets)
            val count = wma_looper_detect_onsets(
                engine, trackIndex, out, maxOnsets, hopFrames, sensitivity,
            )
            if (count <= 0) IntArray(0) else IntArray(count) { out[it] }
        }
    }

    // ---------- importar y capturar ----------

    override fun looperImportTrack(trackIndex: Int, filePath: String, sampleRate: Int): Boolean =
        wma_looper_import_track(engine, trackIndex, filePath, sampleRate)

    override fun looperCaptureTrack(trackIndex: Int, filePath: String, bitDepth: Int): Boolean =
        wma_looper_capture_track(engine, trackIndex, filePath, bitDepth)

    // ---------- exportar ----------

    /**
     * **`Dispatchers.Default` y no `Dispatchers.IO`, y no es una preferencia.**
     *
     * Android usa `IO`. En Kotlin/Native ese dispatcher **no se puede nombrar**: en
     * `nativeMain` conviven un miembro `internal val Dispatchers.IO` y una extensión
     * pública `actual`, y en Kotlin el miembro le gana a la extensión, así que desde
     * afuera de kotlinx-coroutines el nombre resuelve al `internal` y no compila.
     *
     * `Default` es un pool dimensionado para CPU, así que un export largo le ocupa un
     * hilo al resto. Se acepta porque un export es una acción del usuario, una por vez y
     * con la UI mostrando su progreso — pero queda anotado: si alguna vez hay más de un
     * trabajo pesado concurrente, la salida es un dispatcher propio, no otra constante.
     */
    override suspend fun looperExportMixPro(
        filePath: String,
        bitDepth: ExportBitDepth,
        repeatLoops: Int,
        countInBeats: Int,
        applyLimiter: Boolean,
        projectName: String?,
        artist: String?,
        comment: String?,
        bpm: Int,
    ): Boolean = withContext(Dispatchers.Default) {
        memScoped {
            val options = allocExportOptions(
                bitDepth, repeatLoops, countInBeats, applyLimiter, bpm,
                projectName, artist, comment,
            )
            wma_looper_export_mix_v2(engine, filePath, options.ptr)
        }
    }

    override suspend fun looperExportStems(
        directory: String,
        bitDepth: ExportBitDepth,
        repeatLoops: Int,
        countInBeats: Int,
        applyLimiter: Boolean,
        bpm: Int,
    ): Int = withContext(Dispatchers.Default) {
        memScoped {
            // Los tres metadatos van en null: la C API documenta que el export de stems
            // los ignora, y pasarlos igual haría creer que se escriben.
            val options = allocExportOptions(
                bitDepth, repeatLoops, countInBeats, applyLimiter, bpm,
                projectName = null, artist = null, comment = null,
            )
            wma_looper_export_stems(engine, directory, options.ptr)
        }
    }

    override fun looperExportTrack(trackIndex: Int, filePath: String): Boolean =
        wma_looper_export_track(engine, trackIndex, filePath)

    override fun looperSetExportSampleRate(sampleRate: Int) =
        wma_looper_set_export_sample_rate(engine, sampleRate)

    override fun looperGetExportProgress(): Float = wma_looper_get_export_progress(engine)
    override fun looperIsExportInProgress(): Boolean = wma_looper_is_export_in_progress(engine)
    override fun looperCancelExport() = wma_looper_cancel_export(engine)

    /**
     * Instalar y quitar es sólo mover [LooperListenerHolder.listener]; el callback de C
     * se instala una sola vez y no se quita.
     *
     * Podría quitarse pasando `null` a `wma_looper_set_event_callback`, y **a propósito
     * no se hace**: dejarlo puesto significa que el despachador drena y descarta contra
     * un holder cuyo `listener` es `null`, que es exactamente lo que hace de todos modos
     * cuando no hay sink. Quitarlo agregaría un segundo estado que puede desincronizarse
     * con el campo, sin ahorrar nada — el worker corre igual, porque lo arranca el
     * constructor del motor.
     *
     * Siempre `true`: no hay forma de que falle. Devuelve `Boolean` porque Android sí
     * puede fallar, ahí el registro busca métodos por JNI.
     */
    override fun setLooperStateListener(listener: LooperStateListener?): Boolean {
        looperListenerHolder.listener = listener
        wma_looper_set_event_callback(engine, looperEventTrampoline, looperListenerRef.asCPointer())
        return true
    }

    /**
     * Arma un [WmaExportOptions] dentro del scope que lo va a liberar.
     *
     * **Los tres `const char*` salen de `getPointer(this)` y no de `.ptr`**, y ésa es la
     * parte que importa: las cadenas quedan alocadas en el `memScoped` del llamador, o
     * sea que viven exactamente lo que dura la llamada a C. Un puntero a un `cstr`
     * temporal sería memoria liberada antes de que el export la lea.
     *
     * `null` en un campo de texto significa "dejalo vacío", que es lo que documenta la
     * C API — no "poné la cadena vacía".
     */
    private fun MemScope.allocExportOptions(
        bitDepth: ExportBitDepth,
        repeatLoops: Int,
        countInBeats: Int,
        applyLimiter: Boolean,
        bpm: Int,
        projectName: String?,
        artist: String?,
        comment: String?,
    ): WmaExportOptions = alloc<WmaExportOptions>().apply {
        bit_depth = bitDepth.raw
        repeat_loops = repeatLoops
        count_in_beats = countInBeats
        apply_limiter = applyLimiter
        this.bpm = bpm
        project_name = projectName?.cstr?.getPointer(this@allocExportOptions)
        this.artist = artist?.cstr?.getPointer(this@allocExportOptions)
        this.comment = comment?.cstr?.getPointer(this@allocExportOptions)
    }

    // ==================== ARPEGIADOR ====================
    //
    // Ninguno toma lock, y es la misma regla que aplica Android: el header marca
    // `RT-safe` a todos salvo `set_scale_intervals`, o sea que del otro lado hay
    // `std::atomic`. Un mutex por gesto acá costaría más que lo que protege.
    //
    // Los tres getters son polling y no callback por la regla RT (D6): el thread de
    // audio no entra a Kotlin, así que el paso actual se pregunta.

    override fun setArpEnabled(enabled: Boolean) = wma_arp_set_enabled(engine, enabled)

    override fun isArpEnabled(): Boolean = wma_arp_is_enabled(engine)

    override fun regenerateArpPattern() = wma_arp_regenerate(engine)

    override fun setArpPattern(patternId: Int) = wma_arp_set_pattern(engine, patternId)

    override fun setArpSubdivision(beatsPerStep: Float) =
        wma_arp_set_subdivision(engine, beatsPerStep)

    override fun setArpOctaveRange(octaves: Int) = wma_arp_set_octave_range(engine, octaves)

    /**
     * El único que copia memoria, y el único que no es RT-safe.
     *
     * `usePinned` fija el array durante la llamada para que C lo lea directo, sin
     * una copia intermedia — el mismo patrón que [getWaveformSamples] y
     * [updateMultiTouch]. La C API declara el puntero `const`, así que no escribe
     * sobre él; el `count` va aparte porque un array de C no lo lleva encima.
     *
     * Un array vacío es válido y significa "sin escala", pero `addressOf(0)` sobre un
     * `IntArray` de largo cero tira `ArrayIndexOutOfBoundsException` (medido en el
     * caso gemelo de `loadSoundFont`), así que se corta antes.
     */
    override fun setArpScaleIntervals(intervals: IntArray) {
        if (intervals.isEmpty()) {
            wma_arp_set_scale_intervals(engine, null, 0)
            return
        }
        intervals.usePinned { pinned ->
            wma_arp_set_scale_intervals(engine, pinned.addressOf(0), intervals.size)
        }
    }

    override fun setArpGateLength(gate: Float) = wma_arp_set_gate_length(engine, gate)

    override fun setArpSwing(swing: Float) = wma_arp_set_swing(engine, swing)

    override fun setArpLatch(latch: Boolean) = wma_arp_set_latch(engine, latch)

    override fun setArpVelocity(velocity: Float) = wma_arp_set_velocity(engine, velocity)

    override fun setArpVelocityVariation(variation: Float) =
        wma_arp_set_velocity_variation(engine, variation)

    override fun setArpProbability(probability: Float) =
        wma_arp_set_probability(engine, probability)

    override fun setArpRatchet(active: Boolean) = wma_arp_set_ratchet(engine, active)

    override fun setArpTouchActive(active: Boolean) = wma_arp_set_touch_active(engine, active)

    override fun setArpBaseFrequency(frequency: Float) = wma_arp_set_base_freq(engine, frequency)

    override fun getArpCurrentStep(): Int = wma_arp_get_current_step(engine)

    override fun getArpTotalSteps(): Int = wma_arp_get_total_steps(engine)

    override fun isArpGateOpen(): Boolean = wma_arp_is_gate_open(engine)

    // ==================== SOUNDFONT ====================
    //
    // Las cuatro de polifonía son `RT-safe` y no toman lock, igual que en Android.
    // Las de carga NO son RT-safe —parsean el archivo— pero tampoco toman el mutex:
    // el motor serializa adentro y quien carga un `.sf2` no está en el camino de un
    // gesto.

    /**
     * Los bytes se fijan con `usePinned` para que C los lea directo, sin una copia
     * intermedia — el mismo patrón que [setArpScaleIntervals].
     *
     * **El guard del array vacío no es defensivo, es necesario**: sin él, `addressOf(0)`
     * sobre el array de largo cero tira `ArrayIndexOutOfBoundsException` — medido,
     * sacándolo. Del lado Android el mismo caso llega hasta C y vuelve `false`; acá
     * tiene que cortarse antes. El contrato es el mismo (ver
     * [ISoundFontBridge.loadSoundFont]), el motivo no.
     */
    override fun loadSoundFont(data: ByteArray): Boolean {
        if (data.isEmpty()) return false
        return data.usePinned { pinned ->
            wma_sf_load_data(engine, pinned.addressOf(0), data.size)
        }
    }

    override fun loadSoundFontFromPath(path: String): Boolean {
        if (path.isBlank()) return false
        return wma_sf_load_path(engine, path)
    }

    override fun unloadSoundFont() = wma_sf_unload(engine)

    override fun isSoundFontLoaded(): Boolean = wma_sf_is_loaded(engine)

    override fun setSoundFontPreset(presetIndex: Int) {
        if (presetIndex < 0) return
        wma_sf_set_preset(engine, presetIndex)
    }

    override fun getSoundFontPresetCount(): Int = wma_sf_get_preset_count(engine)

    /**
     * `toKString()` **copia**, y ahí está el punto: la C API documenta que el puntero
     * vale hasta que se descargue el SoundFont, así que devolver algo que lo envuelva
     * dejaría al llamador con memoria colgando después de [unloadSoundFont]. Es la
     * misma razón por la que [drainCapturedLogs] no deja salir su batch.
     */
    override fun getSoundFontPresetName(presetIndex: Int): String? =
        wma_sf_get_preset_name(engine, presetIndex)?.toKString()

    override fun getSoundFontPresetKeyRange(presetIndex: Int): IntArray? = memScoped {
        val minKey = alloc<IntVar>()
        val maxKey = alloc<IntVar>()

        if (!wma_sf_get_preset_key_range(engine, presetIndex, minKey.ptr, maxKey.ptr)) {
            return@memScoped null
        }
        intArrayOf(minKey.value, maxKey.value)
    }

    override fun getSoundFontPresetBankProgram(presetIndex: Int): IntArray? = memScoped {
        val bank = alloc<IntVar>()
        val program = alloc<IntVar>()

        if (!wma_sf_get_preset_bank_program(engine, presetIndex, bank.ptr, program.ptr)) {
            return@memScoped null
        }
        intArrayOf(bank.value, program.value)
    }

    override fun sfNoteOn(touchId: Int, midiNote: Int, velocity: Float) =
        wma_sf_note_on(engine, touchId, midiNote, velocity)

    override fun sfNoteOff(touchId: Int) = wma_sf_note_off(engine, touchId)

    override fun sfNoteOffAll() = wma_sf_note_off_all(engine)

    override fun sfNoteOffAllExcept(keepTouchId: Int) =
        wma_sf_note_off_all_except(engine, keepTouchId)

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

    private companion object {
        /** Los bordes del filtro de voz que declara [IAudioNativeBridge]. */
        const val MIN_FILTER_HZ = 20f
        const val MAX_FILTER_HZ = 20_000f

        /** 0 = paso bajo, 1 = paso alto, 2 = paso banda. */
        const val MAX_FILTER_MODE = 2
    }
}
