package com.watermellonstudios.audio.internal.engine

import com.watermellonstudios.audio.api.IAudioNativeBridge
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
import com.watermellonstudios.audio.domain.error.NativeBridgeException
import com.watermellonstudios.audio.domain.modulator.ModulatorType
import com.watermellonstudios.audio.domain.oscillator.OscillatorType
import com.watermellonstudios.audio.domain.scale.ScaleMode
import com.watermellonstudios.audio.domain.state.AudioError
import com.watermellonstudios.audio.domain.state.AudioState
import com.watermellonstudios.audio.domain.state.EngineLifecycle
import com.watermellonstudios.audio.domain.state.StreamInfo
import com.watermellonstudios.audio.domain.AudioBackendType
import com.watermellonstudios.audio.internal.bridge.BridgeConcurrency
import com.watermellonstudios.audio.internal.bridge.getAudioBridge
import com.watermellonstudios.audio.internal.util.ScaleQuantizer
import com.watermellonstudios.audio.internal.util.epochMillis
import kotlinx.coroutines.CancellationException
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
internal class AudioEngineImpl @OptIn(InternalWatermelonApi::class) constructor(
    private val config: AudioEngineConfig,
    // El puente entra por parametro con `getAudioBridge()` como default, que es
    // el mismo idiom que ya usa [NativeModeStateWriter]. NO es un cambio de
    // comportamiento: un default se evalua al construir, igual que la asignacion
    // que habia antes, y al ser default no toca un solo call site.
    //
    // Lo que habilita es testear esta clase, que hasta 2026-08-13 tenia CERO
    // cobertura en `commonTest` — y no por olvido: `getAudioBridge()` es
    // `expect`, y su actual de JVM es `AudioNativeBridge.getInstance()`, que
    // necesita la lib nativa. Con el puente cableado adentro la clase no se
    // podia ni construir en un test. La ausencia de tests era una imposibilidad,
    // no una deuda de disciplina.
    private val bridge: IAudioNativeBridge = getAudioBridge(),
    // REQ-045 (AC-045.2): la serializacion del ciclo de vida VIVE ACA, no solo dentro
    // del bridge. `start()` no es una llamada: son cinco (preguntar, arrancar,
    // oscilador, efectos por default, leer el stream) mas tres escrituras de `state`.
    // Sin un mutex propio, dos `start()`/`stop()` concurrentes intercalaban esa
    // secuencia y el `state` final dependia de quien escribia ultimo.
    //
    // Es el mismo `LIFECYCLE` de [BridgeConcurrency] que ya usan los dos bridges, y
    // anidar los dos mutexes no puede trabar: son instancias distintas y el orden de
    // toma es siempre el mismo (motor primero, bridge despues).
    //
    // Entra por parametro con default por la misma razon que el bridge: para que un
    // test le pueda dar un dispatcher controlado y afirmar el ORDEN en vez de esperar.
    private val concurrency: BridgeConcurrency = BridgeConcurrency()
) : AudioEngine {

    companion object {
        private const val TAG = "AudioEngine"
    }

    private val logger: AudioLogger = config.logger
    private val analytics = config.analyticsListener

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private var pollingJob: Job? = null
    private var sessionStartTime: Long = 0

    // `maxEffects` se siembra desde la config, y no es cosmético: [addEffect] decide
    // con `effectChain.canAddEffect`, que compara contra ESTE número. Mientras no se
    // sembró, `EffectChainState` se quedaba con su default de 12 y el recorte a 6 de
    // `AudioEngineConfig.tunedFor()` (WA-1.2) no llegaba a aplicarse nunca: medido en
    // el AVD el 2026-07-28 con `gama baja: true`, la cadena aceptó 7 efectos.
    private val _state = MutableStateFlow(AudioState(
        oscillator = config.defaultOscillator,
        effectChain = EffectChainState(maxEffects = config.maxEffects),
    ))
    override val state: StateFlow<AudioState> = _state.asStateFlow()

    override val isRunning: Boolean get() = _state.value.isRunning
    override val isPaused: Boolean get() = _state.value.isPaused

    private var currentScaleMode: ScaleMode = ScaleMode.FREE

    /**
     * Si alguien ya llamó a [release] (REQ-045, B8).
     *
     * `release()` **no puede tomar el mutex**: es `override fun`, no `suspend`, en la
     * superficie pública. Así que la exclusión se compra con este flag en vez de con el
     * lock: las cuatro del ciclo de vida lo miran ADENTRO del mutex, antes de tocar el
     * bridge, y otra vez ANTES de devolver `success` — porque una que ya estaba en vuelo
     * puede terminar DESPUÉS del release, y ahí `success` afirmaría algo sobre un motor
     * que ya no existe.
     *
     * Un `MutableStateFlow` y no un `@Volatile`: la anotación no está en `commonMain` para
     * Kotlin/Native (vive en `kotlin.concurrent` y es experimental), y este módulo no tiene
     * atomicfu. Un `StateFlow` ya es seguro entre hilos en las dos plataformas y no agrega
     * dependencias. La única transición es false → true y nadie la revierte.
     */
    private val releasedFlow = MutableStateFlow(false)
    private val released: Boolean get() = releasedFlow.value

    // ==================== LIFECYCLE ====================

    /** El rechazo tipado de una operación pedida sobre un motor ya liberado (B8). */
    private fun liberado(operacion: String): Result<Unit> {
        logger.warn(TAG, "$operacion pedido sobre un motor ya liberado")
        return Result.failure(NativeBridgeException.InvalidOperation(operacion, "RELEASED"))
    }

    override suspend fun start(fadeMs: Int?): Result<Unit> =
        concurrency.guarded(BridgeConcurrency.Category.LIFECYCLE, "AudioEngine.start") {
            if (released) return@guarded liberado("start")
            startLocked(fadeMs ?: config.defaultFadeMs)
        }

    /**
     * El arranque, ya serializado.
     *
     * 🔴 **El `RUNNING` se publica DESPUES de que el nativo dijo que si** (AC-045.2).
     * Antes se publicaba siempre: `startEngineWithFadeSync` devolvia `Unit`, el fallo
     * del stream no llegaba, y `state.lifecycle` quedaba en `RUNNING` con el stream
     * cerrado. Un consumidor que decidiera por `isRunning` tomaba la decision sobre una
     * afirmacion que nadie verifico.
     */
    private suspend fun startLocked(fade: Int): Result<Unit> {
        try {
            if (bridge.hasInitializationFailed()) {
                val error = AudioError(-1, "Initialization failed due to insufficient memory", false)
                _state.update { it.copy(error = error) }
                analytics.onError(error)
                // B10: la causa se fabrica acá y no viaja desde la C API, pero NO es una
                // re-derivación: `hasInitializationFailed()` es un HECHO nativo cuyo único
                // motivo es una allocation fallida (`mInitializationFailed`, que sólo se
                // pone en el fallo de memoria del constructor). Es un mapeo 1:1, no una
                // adivinanza sobre qué pudo haber pasado.
                return Result.failure(NativeBridgeException.MemoryAllocationFailed())
            }

            logger.info(TAG, "Starting audio engine", mapOf("fadeMs" to fade))
            _state.update { it.copy(lifecycle = EngineLifecycle.STARTING) }

            // La variante `suspend`, que devuelve el codigo del motor. La `*Sync` esta
            // deprecada justamente porque acá no habia donde ponerlo.
            val arrancado = bridge.startEngineWithFade(fade)
            arrancado.exceptionOrNull()?.let { causa ->
                logger.error(TAG, "El motor no pudo arrancar", causa)
                val error = AudioError(-1, "Failed to start: ${causa.message}", true)
                _state.update { it.copy(lifecycle = EngineLifecycle.STOPPED, error = error) }
                analytics.onError(error)
                return Result.failure(causa)
            }

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
            // B8 — se vuelve a mirar DESPUÉS del fade: `release()` pudo haber corrido
            // mientras esto esperaba, y devolver `success` afirmaría que el motor está
            // andando cuando ya lo destruyeron.
            if (released) return liberado("start")
            return Result.success(Unit)

        } catch (e: CancellationException) {
            // 🔴 LA CANCELACIÓN SE RELANZA, NO SE PUBLICA COMO FALLO (REQ-045, 1.10).
            //
            // `CancellationException` es una `Exception`, así que el `catch (e: Exception)`
            // de abajo se la comía ANTES de que `BridgeConcurrency.guarded` pudiera
            // relanzarla — el bug exacto que el KDoc de `guarded` dice que WA-1.4 vino a
            // arreglar para los 22 `catch` del bridge, reintroducido acá.
            //
            // Lo que costaba: cancelar `start()` durante el `delay(fade)` deja el motor
            // nativo ARRANCADO (ya se llamó al bridge), y este catch publicaba `STOPPED`
            // y disparaba `analytics.onError` sobre un motor sonando. El poller volvía a
            // escribir `RUNNING` un tick después: un parpadeo en el estado del consumidor
            // y una métrica de error que no describe ningún error. Es el ENG-14 de
            // NoisyPad.
            //
            // Cancelar NO es fallar: el scope padre decidió, y tiene que enterarse.
            throw e
        } catch (e: Exception) {
            logger.error(TAG, "Failed to start audio engine", e)
            val error = AudioError(-1, "Failed to start: ${e.message}", true)
            _state.update { it.copy(lifecycle = EngineLifecycle.STOPPED, error = error) }
            analytics.onError(error)
            return Result.failure(e)
        }
    }

    override suspend fun stop(fadeMs: Int?): Result<Unit> =
        concurrency.guarded(BridgeConcurrency.Category.LIFECYCLE, "AudioEngine.stop") {
            if (released) return@guarded liberado("stop")
            stopLocked(fadeMs ?: config.defaultFadeMs)
        }

    private suspend fun stopLocked(fade: Int): Result<Unit> {
        try {
            logger.info(TAG, "Stopping audio engine", mapOf("fadeMs" to fade))
            _state.update { it.copy(lifecycle = EngineLifecycle.STOPPING) }

            val parado = bridge.stopEngineWithFade(fade)
            parado.exceptionOrNull()?.let { causa ->
                logger.error(TAG, "El motor no pudo parar", causa)
                // 🔴 NO se publica `STOPPED`, y el poller sigue vivo A PROPÓSITO (M4).
                //
                // Se vuelve ANTES de `stopStatePolling()`, así que el `lifecycle` lo
                // sigue escribiendo `refreshStateFromNative()` con lo que el motor
                // reporta. Eso es lo honesto: el motor dijo que no paró, así que el
                // estado publicado tiene que seguir al motor, no a nuestra intención.
                // Publicar `STOPPED` sería la misma mentira al revés, y apagar el poller
                // dejaría el estado congelado en lo último que alcanzó a escribir.
                _state.update {
                    it.copy(error = AudioError(-1, "Failed to stop: ${causa.message}", true))
                }
                return Result.failure(causa)
            }
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
            if (released) return liberado("stop")
            return Result.success(Unit)

        } catch (e: CancellationException) {
            throw e  // Ver el porqué largo en `startLocked`.
        } catch (e: Exception) {
            logger.error(TAG, "Failed to stop audio engine", e)
            _state.update { it.copy(error = AudioError(-1, "Failed to stop: ${e.message}", true)) }
            return Result.failure(e)
        }
    }

    override suspend fun pause(fadeMs: Int): Result<Unit> =
        concurrency.guarded(BridgeConcurrency.Category.LIFECYCLE, "AudioEngine.pause") {
            if (released) return@guarded liberado("pause")
            transicion(fadeMs, pausado = true)
        }

    override suspend fun resume(fadeMs: Int): Result<Unit> =
        concurrency.guarded(BridgeConcurrency.Category.LIFECYCLE, "AudioEngine.resume") {
            if (released) return@guarded liberado("resume")
            transicion(fadeMs, pausado = false)
        }

    /**
     * Pausa y reanudacion, que son la misma forma con el bit al revés.
     *
     * 🔴 **`isPaused` se publica sólo si el motor lo hizo.** Antes se publicaba
     * siempre: la `*Sync` devolvia `Unit` y encima el `catch` se tragaba la excepcion
     * SIN tocar el estado, asi que un fallo dejaba `isPaused` diciendo lo contrario de
     * lo que pasaba. Era el mismo defecto que `start`, dos veces.
     */
    private suspend fun transicion(fadeMs: Int, pausado: Boolean): Result<Unit> {
        val que = if (pausado) "Pausing" else "Resuming"
        try {
            logger.debug(TAG, "$que audio", mapOf("fadeMs" to fadeMs))
            val hecho = if (pausado) {
                bridge.pauseEngineWithFade(fadeMs)
            } else {
                bridge.resumeEngineWithFade(fadeMs)
            }
            hecho.exceptionOrNull()?.let { causa ->
                logger.error(TAG, "$que audio falló en el motor", causa)
                return Result.failure(causa)
            }
            delay(fadeMs.toLong())
            _state.update { it.copy(isPaused = pausado) }
            if (released) return liberado(if (pausado) "pause" else "resume")
            return Result.success(Unit)
        } catch (e: CancellationException) {
            throw e  // Ver el porqué largo en `startLocked`.
        } catch (e: Exception) {
            logger.error(TAG, "$que audio lanzó", e)
            return Result.failure(e)
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

    /**
     * 🔴 **El `state` se publicaba ANTES de preguntarle al motor, y el rechazo se
     * descartaba** (REQ-045, D3): un id que el motor no acepta dejaba `state.modulator` en
     * el valor nuevo, `analytics` avisando del cambio, y un log que decía que cambió.
     * Ahora el orden es el único correcto — primero el motor, y el `state` sólo si dijo sí.
     */
    override fun setModulator(type: ModulatorType): Result<Unit> {
        val previous = _state.value.modulator
        val result = bridge.setModulatorType(type.id)
        if (result.isFailure) {
            logger.error(TAG, "setModulator: el motor rechazó el tipo", result.exceptionOrNull())
            return result
        }
        _state.update { it.copy(modulator = type) }
        analytics.onModulatorChanged(type, previous)
        logger.debug(TAG, "Modulator changed", mapOf("type" to type.displayName))
        return result
    }

    override fun setModulatorParameter(paramId: Int, value: Float): Result<Unit> =
        bridge.setModulatorParameter(paramId, value)

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

    override fun removeEffect(index: Int): Result<Unit> {
        val currentChain = _state.value.effectChain
        if (index < 0 || index >= currentChain.effects.size) {
            logger.warn(TAG, "Invalid effect index", mapOf("index" to index))
            return Result.failure(
                NativeBridgeException.InvalidEffectIndex(index, currentChain.effects.size),
            )
        }

        val removedEffect = currentChain.effects[index]
        val result = bridge.removeEffectSync(index)
        if (result.isFailure) {
            logger.error(TAG, "removeEffect: el motor no lo quitó", result.exceptionOrNull())
            return result
        }

        val newEffects = currentChain.effects
            .filterNot { it.index == index }
            .mapIndexed { i, effect -> effect.copy(index = i) }

        _state.update {
            it.copy(effectChain = currentChain.copy(effects = newEffects))
        }

        analytics.onEffectRemoved(removedEffect.type, index)
        logger.info(TAG, "Effect removed", mapOf("type" to removedEffect.type.displayName))
        return result
    }

    override fun setEffectParameter(effectIndex: Int, paramId: Int, value: Float): Result<Unit> {
        val result = bridge.setEffectParameterSync(effectIndex, paramId, value)
        if (result.isFailure) return result

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
        return result
    }

    override fun getEffectParameter(effectIndex: Int, paramId: Int): Float {
        return bridge.getEffectParameterSync(effectIndex, paramId)
    }

    override fun setEffectBypass(index: Int, bypass: Boolean): Result<Unit> {
        val result = bridge.setEffectBypassSync(index, bypass)
        if (result.isFailure) return result

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
        return result
    }

    override fun setEffectsBypass(bypass: Boolean): Result<Unit> {
        val result = bridge.setEffectsBypassSync(bypass)
        if (result.isFailure) return result

        _state.update { state ->
            state.copy(effectChain = state.effectChain.copy(isGloballyBypassed = bypass))
        }
        return result
    }

    /**
     * 🔴 **El `removeAt`/`add` corría SIEMPRE**, incluso cuando el motor no había
     * reordenado nada: con un índice fuera de la cadena, esto tiraba
     * `IndexOutOfBoundsException` desde adentro de un `_state.update` después de un no-op
     * mudo en el motor. Ahora el rechazo llega como `failure` y la lista no se toca.
     */
    override fun reorderEffects(fromIndex: Int, toIndex: Int): Result<Unit> {
        val result = bridge.reorderEffectsSync(fromIndex, toIndex)
        if (result.isFailure) {
            logger.error(TAG, "reorderEffects: el motor no reordenó", result.exceptionOrNull())
            return result
        }

        _state.update { state ->
            val mutableList = state.effectChain.effects.toMutableList()
            val item = mutableList.removeAt(fromIndex)
            mutableList.add(toIndex, item)
            val reordered = mutableList.mapIndexed { i, effect -> effect.copy(index = i) }
            state.copy(effectChain = state.effectChain.copy(effects = reordered))
        }
        return result
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
            // NONE, no OBOE: si la consulta falló no sabemos qué backend hay, y
            // OBOE es una respuesta concreta —además de una que en iOS nombra un
            // backend que no existe—. Es el mismo criterio que ya usa `fromId`
            // para un id desconocido: ausencia, no un valor plausible.
            AudioBackendType.NONE
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
        // B8 — la marca va PRIMERO, antes de destruir nada: cualquier operación que entre
        // al mutex después de esta línea se rechaza tipada, y una que ya esté en vuelo la
        // ve antes de devolver `success`. Sin esto, `start()` después de `release()`
        // devolvía `success` sobre un motor destruido.
        releasedFlow.value = true
        stopStatePolling()
        try {
            // 🔴 EL ÚNICO `*Sync` QUE LA LIBRERÍA SIGUE LLAMANDO, y queda declarado.
            //
            // REQ-045 saca los cinco de `start/stop/pause/resume` porque ahí el fallo
            // del motor TENÍA a dónde ir y se perdía. Acá no: `release()` es
            // `override fun` —no `suspend`— en la superficie pública, y volverla
            // `suspend` rompería a todo consumidor en fuente, que es lo que este REQ
            // se comprometió a no hacer. Un teardown no tiene tampoco a quién
            // reportarle: el motor se está yendo.
            @Suppress("DEPRECATION")
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
                    // 1.11 — la latencia llega TARDE, y una sola lectura la pierde para
                    // siempre. En el camino Oboe directo (el que shippea en Android)
                    // `calculateLatencyMillis()` no tiene dato justo después de
                    // `requestStart()`, así que el `refreshStreamInfo()` de `start()` leía
                    // -1 y nadie volvía a preguntar: medido en el moto g42, con
                    // `latencyMillis` en -1.0 para toda la sesión y `onSessionStarted`
                    // recibiendo ese -1.
                    //
                    // Se pregunta de nuevo SÓLO mientras está ausente, así que el costo es
                    // una llamada JNI por poll hasta que aparece y CERO después. Reintentar
                    // siempre habría sido una llamada por poll para toda la sesión, en el
                    // camino que la UI pollea a 100 ms.
                    if (_state.value.streamInfo?.latencyMillis == null) {
                        refreshStreamInfo()
                    }
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
