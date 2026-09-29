package com.watermellonstudios.audio.internal.engine

import com.watermellonstudios.audio.api.IAudioNativeBridge
import com.watermellonstudios.audio.api.InternalWatermelonApi
import com.watermellonstudios.audio.api.LooperStateListener
import com.watermellonstudios.audio.domain.looper.ExportBitDepth
import com.watermellonstudios.audio.domain.looper.LevelEnvelope
import com.watermellonstudios.audio.domain.looper.PitchSeries
import com.watermellonstudios.audio.api.EffectParameterUpdate
import com.watermellonstudios.audio.api.EffectChainSnapshot
import com.watermellonstudios.audio.domain.effect.EffectType
import com.watermellonstudios.audio.domain.engine.EngineParameterDef
import com.watermellonstudios.audio.domain.usb.UsbLatencyProfile
import kotlinx.coroutines.CompletableDeferred

/**
 * Doble de [IAudioNativeBridge] para los tests de [AudioEngineImpl].
 *
 * **Lo NO modelado tira, no devuelve cero.** Es deliberado y es la lección más cara
 * que dejó este repo: un stub que devolvía diez ceros derrotó los fallbacks elvis de
 * su propio caller y el bug shippeó con los tests en verde. Un doble inerte no es
 * "sin comportamiento": es un punto ciego, y con 95 miembros habría 95.
 *
 * Con [notModeled] el efecto se invierte. Si un camino toca algo que este doble no
 * modela, el test **explota diciendo qué**, en vez de pasar sobre un cero inventado.
 * Eso convierte al doble en un aserto extra: `startDoesNotTouchTheEngineWhenInitFailed`
 * pasa **porque** nada más se llamó — si el early return se rompiera, el próximo
 * miembro tiraría.
 *
 * Modelar un miembro nuevo es una línea; hacerlo sólo cuando un test lo necesite es lo
 * que mantiene honesta la cobertura.
 */
@OptIn(InternalWatermelonApi::class)
internal class FakeAudioNativeBridge(
    private val initializationFailed: Boolean = false,
    private val throwOnStart: Throwable? = null,
    /**
     * Lo que el motor contesta a las cuatro del ciclo de vida (REQ-045). Por operación
     * y no uno global: "arrancar falla y parar no" es un escenario real, y un único
     * resultado lo volvería inexpresable.
     */
    private val lifecycleResults: Map<String, Result<Unit>> = emptyMap(),
    /**
     * Compuerta para forzar el interleaving (AC-045.2).
     *
     * Mientras no se complete, **toda** operación del ciclo de vida se queda suspendida
     * DENTRO del bridge. Eso es lo que permite afirmar el orden del mutex sin
     * iteraciones ni `sleep`: el primer llamador queda adentro con el mutex tomado y
     * los que siguen se encolan. Ver el test del orden.
     */
    private val gate: CompletableDeferred<Unit>? = null,
    /**
     * Lo que `getStreamInfoArray()` devuelve, lectura por lectura (REQ-045, 1.11).
     *
     * Una LISTA y no un valor porque el defecto que se prueba es temporal: en el camino
     * Oboe directo la latencia **no está** justo después de arrancar y aparece unos ms
     * después. La última entrada se repite cuando se agota la lista.
     */
    private val streamInfoReadings: List<FloatArray?> = listOf(null),
) : IAudioNativeBridge {

    /**
     * Miembros llamados, en orden. Es lo que afirman los tests.
     *
     * Las cuatro del ciclo de vida anotan `:in` al ENTRAR y `:out` al salir, y los dos
     * hacen falta: con una sola marca por llamada, dos operaciones que se intercalan
     * producen la misma lista que dos que se serializan. La diferencia entre
     * `[a:in, a:out, b:in, b:out]` y `[a:in, b:in, ...]` ES la aserción de AC-045.2.
     */
    val calls = mutableListOf<String>()

    private suspend fun lifecycle(name: String): Result<Unit> {
        calls += "$name:in"
        // El `throw` va DESPUÉS de anotar la entrada y ANTES de la salida: esa lista
        // ("llegó y ahí explotó") es lo que afirma el test del bridge que lanza.
        if (name == "startEngineWithFade") throwOnStart?.let { throw it }
        gate?.await()
        val result = lifecycleResults[name] ?: Result.success(Unit)
        calls += "$name:out"
        return result
    }

    private fun notModeled(name: String): Nothing =
        throw NotImplementedError(
            "FakeAudioNativeBridge no modela '$name'. Si el camino bajo prueba " +
                "llegó acá, o el test cambió de forma o hay que modelarlo a propósito."
        )

    // ---- lo modelado, que es exactamente lo que los tests ejercitan ----------

    override fun hasInitializationFailed(): Boolean {
        calls += "hasInitializationFailed"
        return initializationFailed
    }

    @Deprecated("El motor no puede transportar su fallo por un Unit (REQ-045).")
    override fun startEngineWithFadeSync(fadeTimeMs: Int) {
        calls += "startEngineWithFadeSync"
        throwOnStart?.let { throw it }
    }

    @Deprecated("El motor no puede transportar su fallo por un Unit (REQ-045).")
    override fun stopEngineWithFadeSync(fadeTimeMs: Int) {
        calls += "stopEngineWithFadeSync"
    }

    override fun setOscillatorType(type: Int) {
        calls += "setOscillatorType"
    }

    override fun addEffectSync(typeId: Int): Boolean {
        calls += "addEffectSync"
        return true
    }

    // ---- todo lo demás: no modelado, y por eso tira -------------------------

    override suspend fun startEngine(): Result<Unit> = notModeled("startEngine")
    override suspend fun stopEngine(): Result<Unit> = notModeled("stopEngine")

    // Las cuatro que AudioEngineImpl usa desde REQ-045, en vez de las `*Sync`.
    override suspend fun startEngineWithFade(fadeTimeMs: Int): Result<Unit> =
        lifecycle("startEngineWithFade")
    override suspend fun stopEngineWithFade(fadeTimeMs: Int): Result<Unit> =
        lifecycle("stopEngineWithFade")
    override suspend fun pauseEngineWithFade(fadeTimeMs: Int): Result<Unit> =
        lifecycle("pauseEngineWithFade")
    override suspend fun resumeEngineWithFade(fadeTimeMs: Int): Result<Unit> =
        lifecycle("resumeEngineWithFade")

    // 🔴 Las `*Sync` siguen SIN MODELAR a propósito: desde REQ-045 la librería no las
    // llama, y que tiren es lo que hace que un test se ponga rojo si alguien las
    // vuelve a cablear en vez de pasar en verde sobre un no-op.
    @Deprecated("El motor no puede transportar su fallo por un Unit (REQ-045).")
    override fun pauseEngineWithFadeSync(fadeTimeMs: Int) { notModeled("pauseEngineWithFadeSync") }
    @Deprecated("El motor no puede transportar su fallo por un Unit (REQ-045).")
    override fun resumeEngineWithFadeSync(fadeTimeMs: Int) { notModeled("resumeEngineWithFadeSync") }
    /**
     * El ÚNICO `*Sync` que la librería sigue llamando: `release()`, que no es `suspend` en
     * la superficie pública y no tiene a quién reportarle. Modelado por eso, y porque un
     * `NotImplementedError` es un `Error` —no una `Exception`— así que el `try/catch` de
     * `release()` no lo atrapa y el test se cae en un lugar que no es el que mide.
     */
    @Deprecated("El motor no puede transportar su fallo por un Unit (REQ-045).")
    override fun stopEngineSync() {
        calls += "stopEngineSync"
    }
    override fun getStateVersion(): Long = notModeled("getStateVersion")
    override fun getLastStreamErrorCode(): Int = notModeled("getLastStreamErrorCode")
    override fun isEngineInitialized(): Boolean = notModeled("isEngineInitialized")
    /**
     * `null` = "no hay stream que describir", que es lo que el nativo contesta sin
     * stream abierto. Modelado y no `notModeled` porque `start()` lo llama en su camino
     * feliz, y un doble que tirara ahí volvería intesteable el camino que AC-045.2 pide.
     */
    override fun getStreamInfoArray(): FloatArray? {
        val i = streamInfoReads
        streamInfoReads++
        calls += "getStreamInfoArray"
        return streamInfoReadings[minOf(i, streamInfoReadings.size - 1)]
    }

    /** Cuántas veces se leyó el stream info. Lo afirma el test de la latencia tardía. */
    var streamInfoReads: Int = 0
        private set

    // ---- lo que el POLLER necesita ------------------------------------------
    //
    // Modelado porque `refreshStateFromNative()` corre ANTES del reintento del stream
    // info en el bucle de polling: si cualquiera de estos tirara, el reintento de la
    // latencia (1.11) no se alcanzaría nunca y el test de esa propiedad sería verde por
    // vacío. Devuelven lo que devuelve un motor corriendo y sin errores.

    override fun hasStreamError(): Boolean = false
    override fun clearStreamError() { notModeled("clearStreamError") }
    override fun getEngineState(): Int = nativeEngineState
    override fun getCurrentFadeVolume(): Float = 1.0f
    override fun getTargetFadeVolume(): Float = 1.0f
    override fun getIsFading(): Boolean = false
    override fun getFadeProgress(): Float = 1.0f
    override fun getIsPaused(): Boolean = false

    /**
     * El estado que el nativo reporta. `2` es RUNNING en `EngineLifecycle.fromNativeCode`.
     *
     * El nombre NO es `engineState`: su getter generado choca en la JVM con
     * `getEngineState()` de la interfaz.
     */
    var nativeEngineState: Int = 2
    override fun isUsingReducedBuffers(): Boolean = notModeled("isUsingReducedBuffers")
    override fun getMasterVolume(): Float = notModeled("getMasterVolume")
    override fun getSynthVolume(): Float = notModeled("getSynthVolume")
    override fun setXY(x: Float,  y: Float,  coalesce: Boolean) { notModeled("setXY") }
    override fun setFrequencyAndAmplitude(frequency: Float,  amplitude: Float) { notModeled("setFrequencyAndAmplitude") }
    override fun setFrequencyRange(minHz: Float,  maxHz: Float) { notModeled("setFrequencyRange") }
    override fun setMasterVolume(volume: Float) { notModeled("setMasterVolume") }
    override fun setSynthVolume(volume: Float) { notModeled("setSynthVolume") }
    override fun setSecondaryOscillatorType(type: Int) { notModeled("setSecondaryOscillatorType") }
    override fun setEngineType(type: Int) { notModeled("setEngineType") }
    override fun setEngineParameter(paramId: Int,  value: Float) { notModeled("setEngineParameter") }
    override fun getEngineType(): Int = notModeled("getEngineType")
    override fun getEngineParameterCount(engineType: Int): Int = notModeled("getEngineParameterCount")
    override fun getEngineParameterDef(engineType: Int, paramIndex: Int): EngineParameterDef? =
        notModeled("getEngineParameterDef")
    override fun setBpm(bpm: Float) { notModeled("setBpm") }
    override fun getBpm(): Float = notModeled("getBpm")
    override fun setModulatorType(type: Int) { notModeled("setModulatorType") }
    override fun setModulatorParameter(paramId: Int,  value: Float) { notModeled("setModulatorParameter") }
    override fun removeEffectSync(index: Int) { notModeled("removeEffectSync") }
    override fun setEffectParameterSync(effectIndex: Int,  paramId: Int,  value: Float) { notModeled("setEffectParameterSync") }
    override fun getEffectParameterSync(effectIndex: Int,  paramId: Int): Float = notModeled("getEffectParameterSync")
    override fun setEffectBypassSync(index: Int,  bypass: Boolean) { notModeled("setEffectBypassSync") }
    override fun setEffectsBypassSync(bypass: Boolean) { notModeled("setEffectsBypassSync") }
    override fun isEffectsBypassedSync(): Boolean = notModeled("isEffectsBypassedSync")
    override fun reorderEffectsSync(fromIndex: Int,  toIndex: Int) { notModeled("reorderEffectsSync") }
    override fun getEffectChainSize(): Int = notModeled("getEffectChainSize")
    override fun setRoutingMode(mode: Int) { notModeled("setRoutingMode") }
    override fun getRoutingMode(): Int = notModeled("getRoutingMode")
    override fun setParallelMix(mix: Float) { notModeled("setParallelMix") }
    override fun setFeedbackAmount(amount: Float) { notModeled("setFeedbackAmount") }
    override fun getWaveformSamples(buffer: FloatArray,  size: Int): Int = notModeled("getWaveformSamples")
    override fun enableVoiceSystem(enabled: Boolean) { notModeled("enableVoiceSystem") }
    override fun isVoiceSystemEnabled(): Boolean = notModeled("isVoiceSystemEnabled")
    override fun updateMultiTouch(count: Int,  touchData: FloatArray?) { notModeled("updateMultiTouch") }
    override fun getActiveVoiceCount(): Int = notModeled("getActiveVoiceCount")
    override fun setMaxVoices(maxVoices: Int) { notModeled("setMaxVoices") }
    override fun setVoiceStealingStrategy(strategyId: Int) { notModeled("setVoiceStealingStrategy") }
    override fun setVoiceFilterEnabled(enabled: Boolean) { notModeled("setVoiceFilterEnabled") }
    override fun setVoiceFilterCutoff(hz: Float) { notModeled("setVoiceFilterCutoff") }
    override fun setVoiceFilterResonance(q: Float) { notModeled("setVoiceFilterResonance") }
    override fun setVoiceFilterMode(mode: Int) { notModeled("setVoiceFilterMode") }
    override fun setDualTouchMode(enabled: Boolean) { notModeled("setDualTouchMode") }
    override fun setDualTouchMixMode(modeId: Int) { notModeled("setDualTouchMixMode") }
    override fun triggerChordNotes(frequencies: FloatArray,  amplitude: Float,  oscillatorType: Int) { notModeled("triggerChordNotes") }
    override fun updateChordNotes(frequencies: FloatArray,  amplitude: Float) { notModeled("updateChordNotes") }
    override fun releaseChordNotes() { notModeled("releaseChordNotes") }
    override fun clearMappingConfig(axis: Int) { notModeled("clearMappingConfig") }
    override fun applyAutomation(axis: Int,  normalizedValue: Float) { notModeled("applyAutomation") }
    override fun setAutomationParameter(effectIndex: Int,  paramId: Int,  xyValue: Float) { notModeled("setAutomationParameter") }
    override suspend fun setAudioMode(mode: Int): Result<Unit> = notModeled("setAudioMode")
    override fun getAudioMode(): Int = notModeled("getAudioMode")
    override fun isInModeTransition(): Boolean = notModeled("isInModeTransition")
    override fun setUseBackendManager(useBackendManager: Boolean) { notModeled("setUseBackendManager") }
    override fun createSplitBackend(inputBackendId: Int,  outputBackendId: Int): Boolean = notModeled("createSplitBackend")
    override fun selectBackend(backendId: Int): Boolean = notModeled("selectBackend")
    override fun getCurrentBackendType(): Int = notModeled("getCurrentBackendType")
    override fun isUsbBackendAvailable(): Boolean = notModeled("isUsbBackendAvailable")
    override fun configureUsbBackend(sampleRate: Int,  channels: Int,  bitDepth: Int) { notModeled("configureUsbBackend") }
    override fun setUsbStreamingMode(modeId: Int) { notModeled("setUsbStreamingMode") }
    override fun transportSetBeatsPerBar(beatsPerBar: Int) { notModeled("transportSetBeatsPerBar") }
    override fun transportGetBeatsPerBar(): Int = notModeled("transportGetBeatsPerBar")
    override fun transportFramesPerBeat(): Int = notModeled("transportFramesPerBeat")
    override fun transportFramesPerBar(bars: Int): Int = notModeled("transportFramesPerBar")
    override fun transportStartMetronomeContinuous(everyBeatPattern: Boolean) { notModeled("transportStartMetronomeContinuous") }
    override fun transportStopMetronome() { notModeled("transportStopMetronome") }
    override fun transportIsMetronomeRunning(): Boolean = notModeled("transportIsMetronomeRunning")
    override fun transportIsMetronomeContinuous(): Boolean = notModeled("transportIsMetronomeContinuous")
    override fun transportGetRemainingBeats(): Int = notModeled("transportGetRemainingBeats")

    override fun transportGetPlayFrame(): Long = notModeled("transportGetPlayFrame")
    override fun transportGetBeatsElapsed(): Int = notModeled("transportGetBeatsElapsed")
    override fun setLogCaptureEnabled(enabled: Boolean) { notModeled("setLogCaptureEnabled") }
    override fun drainCapturedLogs(): Array<String> = notModeled("drainCapturedLogs")
    override fun getLogCaptureDropped(): Int = notModeled("getLogCaptureDropped")

    override fun setDualTouch(x1: Float,  y1: Float,  freq1: Float,  amp1: Float,  pressure1: Float,  x2: Float,  y2: Float,  freq2: Float,  amp2: Float,  pressure2: Float,  distance: Float,  angle: Float) { notModeled("setDualTouch") }
    override fun setMappingConfig(axis: Int,  effectIndex: Int,  paramId: Int,  curve: Int,  polarity: Int,  mapMin: Float,  mapMax: Float,  inverted: Boolean) { notModeled("setMappingConfig") }
    override fun setUsbLatencyProfile(profile: UsbLatencyProfile): Result<Unit> = notModeled("setUsbLatencyProfile")
    override fun transportStartMetronome(beats: Int,  firstIsDownbeat: Boolean,  everyBeatPattern: Boolean) { notModeled("transportStartMetronome") }
    override suspend fun getEffectChainSnapshot(): EffectChainSnapshot = notModeled("getEffectChainSnapshot")
    override suspend fun getEffectParameters(index: Int): Map<Int, Float> = notModeled("getEffectParameters")
    override suspend fun isEffectBypassed(index: Int): Boolean = notModeled("isEffectBypassed")
    override suspend fun getEffectCount(): Int = notModeled("getEffectCount")
    override suspend fun getEffectType(index: Int): EffectType? = notModeled("getEffectType")
    override suspend fun addEffect(type: EffectType): Result<Int> = notModeled("addEffect")
    override suspend fun removeEffect(index: Int): Result<Unit> = notModeled("removeEffect")
    override suspend fun setParameter(effectIndex: Int,  paramId: Int,  value: Float): Result<Unit> = notModeled("setParameter")
    override suspend fun setParametersBatch(effectIndex: Int,  parameters: Map<Int, Float>): Result<Unit> = notModeled("setParametersBatch")
    override suspend fun setMultipleEffectParameters(updates: List<EffectParameterUpdate>): Result<Unit> = notModeled("setMultipleEffectParameters")
    override suspend fun setBypass(effectIndex: Int,  bypassed: Boolean): Result<Unit> = notModeled("setBypass")
    override suspend fun setEffectsBypass(bypassed: Boolean): Result<Unit> = notModeled("setEffectsBypass")
    override suspend fun reorderEffects(fromIndex: Int,  toIndex: Int): Result<Unit> = notModeled("reorderEffects")
    override suspend fun clearAllEffects(): Result<Unit> = notModeled("clearAllEffects")
    override fun startInputStreamSync(): Boolean = notModeled("startInputStreamSync")
    override fun stopInputStreamSync() { notModeled("stopInputStreamSync") }
    override fun isInputStreamRunning(): Boolean = notModeled("isInputStreamRunning")
    override fun isInputStarting(): Boolean = notModeled("isInputStarting")
    override fun setInputSourceSync(source: Int) { notModeled("setInputSourceSync") }
    override fun getInputSource(): Int = notModeled("getInputSource")
    override fun setInputGain(gainDb: Float) { notModeled("setInputGain") }
    override fun getInputGain(): Float = notModeled("getInputGain")
    override fun setNoiseGateEnabled(enabled: Boolean) { notModeled("setNoiseGateEnabled") }
    override fun isNoiseGateEnabled(): Boolean = notModeled("isNoiseGateEnabled")
    override fun setNoiseGateThreshold(thresholdDb: Float) { notModeled("setNoiseGateThreshold") }
    override fun isNoiseGateOpen(): Boolean = notModeled("isNoiseGateOpen")
    override fun getInputLevel(channel: Int): Float = notModeled("getInputLevel")
    override fun getInputLevelLinear(channel: Int): Float = notModeled("getInputLevelLinear")
    override fun isInputClipping(): Boolean = notModeled("isInputClipping")
    override fun getInputLatencyMs(): Float = notModeled("getInputLatencyMs")
    override fun getInputMeteringSnapshot(): FloatArray? = notModeled("getInputMeteringSnapshot")
    override fun startTunerSync(): Boolean = notModeled("startTunerSync")
    override fun stopTunerSync() { notModeled("stopTunerSync") }
    override fun isTunerRunning(): Boolean = notModeled("isTunerRunning")
    override fun setTunerTargetHz(hz: Float): Boolean = notModeled("setTunerTargetHz")
    override fun getTunerTargetHz(): Float = notModeled("getTunerTargetHz")
    override fun getTunerSnapshot(): FloatArray? = notModeled("getTunerSnapshot")
    override fun setTunerCandidates(hz: FloatArray): Boolean = notModeled("setTunerCandidates")
    override fun lockTunerString(index: Int): Boolean = notModeled("lockTunerString")
    override fun analyzeTunerBuffer(
        samples: FloatArray,
        channels: Int,
        sampleRate: Int,
        targetHz: Float,
    ): FloatArray? = notModeled("analyzeTunerBuffer")

    override fun analyzeTunerBufferWithCandidates(
        samples: FloatArray,
        channels: Int,
        sampleRate: Int,
        targetHz: Float,
        candidatesHz: FloatArray,
    ): FloatArray? = notModeled("analyzeTunerBufferWithCandidates")
    override fun captureIntonation(slot: Int): Boolean = notModeled("captureIntonation")
    override fun resetIntonation() { notModeled("resetIntonation") }
    override fun intonationState(): Int = notModeled("intonationState")
    override fun intonationDifferenceCents(): Float = notModeled("intonationDifferenceCents")
    override fun setMonitoringEnabledSync(enabled: Boolean) { notModeled("setMonitoringEnabledSync") }
    override fun isMonitoringEnabled(): Boolean = notModeled("isMonitoringEnabled")
    override fun setMonitoringVolume(volume: Float) { notModeled("setMonitoringVolume") }
    override fun getMonitoringVolume(): Float = notModeled("getMonitoringVolume")
    override fun releaseInputNodeSync() { notModeled("releaseInputNodeSync") }
    override fun setArpEnabled(enabled: Boolean) { notModeled("setArpEnabled") }
    override fun isArpEnabled(): Boolean = notModeled("isArpEnabled")
    override fun regenerateArpPattern() { notModeled("regenerateArpPattern") }
    override fun setArpPattern(patternId: Int) { notModeled("setArpPattern") }
    override fun setArpSubdivision(beatsPerStep: Float) { notModeled("setArpSubdivision") }
    override fun setArpOctaveRange(octaves: Int) { notModeled("setArpOctaveRange") }
    override fun setArpScaleIntervals(intervals: IntArray) { notModeled("setArpScaleIntervals") }
    override fun setArpGateLength(gate: Float) { notModeled("setArpGateLength") }
    override fun setArpSwing(swing: Float) { notModeled("setArpSwing") }
    override fun setArpLatch(latch: Boolean) { notModeled("setArpLatch") }
    override fun setArpVelocity(velocity: Float) { notModeled("setArpVelocity") }
    override fun setArpVelocityVariation(variation: Float) { notModeled("setArpVelocityVariation") }
    override fun setArpProbability(probability: Float) { notModeled("setArpProbability") }
    override fun setArpRatchet(active: Boolean) { notModeled("setArpRatchet") }
    override fun setArpTouchActive(active: Boolean) { notModeled("setArpTouchActive") }
    override fun setArpBaseFrequency(frequency: Float) { notModeled("setArpBaseFrequency") }
    override fun getArpCurrentStep(): Int = notModeled("getArpCurrentStep")
    override fun getArpTotalSteps(): Int = notModeled("getArpTotalSteps")
    override fun isArpGateOpen(): Boolean = notModeled("isArpGateOpen")
    override fun loadSoundFont(data: ByteArray): Boolean = notModeled("loadSoundFont")
    override fun loadSoundFontFromPath(path: String): Boolean = notModeled("loadSoundFontFromPath")
    override fun unloadSoundFont() { notModeled("unloadSoundFont") }
    override fun isSoundFontLoaded(): Boolean = notModeled("isSoundFontLoaded")
    override fun setSoundFontPreset(presetIndex: Int) { notModeled("setSoundFontPreset") }
    override fun getSoundFontPresetCount(): Int = notModeled("getSoundFontPresetCount")
    override fun getSoundFontPresetName(presetIndex: Int): String? = notModeled("getSoundFontPresetName")
    override fun getSoundFontPresetKeyRange(presetIndex: Int): IntArray? = notModeled("getSoundFontPresetKeyRange")
    override fun getSoundFontPresetBankProgram(presetIndex: Int): IntArray? = notModeled("getSoundFontPresetBankProgram")
    override fun sfNoteOn(touchId: Int,  midiNote: Int,  velocity: Float) { notModeled("sfNoteOn") }
    override fun sfNoteOff(touchId: Int) { notModeled("sfNoteOff") }
    override fun sfNoteOffAll() { notModeled("sfNoteOffAll") }
    override fun sfNoteOffAllExcept(keepTouchId: Int) { notModeled("sfNoteOffAllExcept") }
    override fun sfSetTouchExpression(touchId: Int, expression: Float) { notModeled("sfSetTouchExpression") }
    override fun sfSetAmbience(reverb: Float, chorus: Float) { notModeled("sfSetAmbience") }
    override fun sfGetAmbienceReverb(): Float = notModeled("sfGetAmbienceReverb")
    override fun sfGetAmbienceChorus(): Float = notModeled("sfGetAmbienceChorus")
    override fun looperSetEnabled(enabled: Boolean) { notModeled("looperSetEnabled") }
    override fun looperPause() { notModeled("looperPause") }
    override fun looperResume() { notModeled("looperResume") }
    override fun looperStopAll() { notModeled("looperStopAll") }
    override fun looperClearAll() { notModeled("looperClearAll") }
    override fun looperIsRecording(): Boolean = notModeled("looperIsRecording")
    override fun looperIsPlaying(): Boolean = notModeled("looperIsPlaying")
    override fun looperTriggerClick(isDownbeat: Boolean) { notModeled("looperTriggerClick") }
    override fun looperPrepareTrack(trackIndex: Int,  lengthFrames: Int,  sampleRate: Int): Boolean = notModeled("looperPrepareTrack")
    override fun looperPrepareTrackBars(trackIndex: Int,  bars: Int,  sampleRate: Int): Int = notModeled("looperPrepareTrackBars")
    override fun looperSetFreeLength(freeLength: Boolean) { notModeled("looperSetFreeLength") }
    override fun looperFinalizeFreeLoop(trackIndex: Int,  loopStart: Int,  loopEnd: Int,  tailFrames: Int): Boolean = notModeled("looperFinalizeFreeLoop")
    override fun looperSetCapabilities(budgetBytes: Long,  maxTracks: Int,  maxFreeSeconds: Int) { notModeled("looperSetCapabilities") }
    override fun looperArmAtNextBar(trackIndex: Int): Long = notModeled("looperArmAtNextBar")
    override fun looperArmInFrames(trackIndex: Int,  offsetFrames: Long): Long = notModeled("looperArmInFrames")
    override fun looperArmSyncedToLoop(trackIndex: Int,  latencyFrames: Long): Long = notModeled("looperArmSyncedToLoop")
    override fun looperCancelArm() { notModeled("looperCancelArm") }
    override fun looperStartRecording(trackIndex: Int) { notModeled("looperStartRecording") }
    override fun looperStopRecording() { notModeled("looperStopRecording") }
    override fun looperAbortRecording() { notModeled("looperAbortRecording") }
    override fun looperStartOverdub(trackIndex: Int) { notModeled("looperStartOverdub") }
    override fun looperIsTrackActive(trackIndex: Int): Boolean = notModeled("looperIsTrackActive")
    override fun looperIsTrackPlaying(trackIndex: Int): Boolean = notModeled("looperIsTrackPlaying")
    override fun looperClearTrack(trackIndex: Int) { notModeled("looperClearTrack") }
    override fun looperPauseTrack(trackIndex: Int) { notModeled("looperPauseTrack") }
    override fun looperResumeTrack(trackIndex: Int) { notModeled("looperResumeTrack") }
    override fun looperResetTrackPlayHead(trackIndex: Int) { notModeled("looperResetTrackPlayHead") }
    override fun looperTrimTrack(trackIndex: Int): Boolean = notModeled("looperTrimTrack")
    override fun looperSetMasterVolume(volume: Float) { notModeled("looperSetMasterVolume") }
    override fun looperGetMasterVolume(): Float = notModeled("looperGetMasterVolume")
    override fun looperSetTrackVolume(trackIndex: Int,  volume: Float) { notModeled("looperSetTrackVolume") }
    override fun looperSetTrackPan(trackIndex: Int,  pan: Float) { notModeled("looperSetTrackPan") }
    override fun looperSetTrackMuted(trackIndex: Int,  muted: Boolean) { notModeled("looperSetTrackMuted") }
    override fun looperSetTrackSpeed(trackIndex: Int,  speed: Float) { notModeled("looperSetTrackSpeed") }
    override fun looperGetTrackSpeed(trackIndex: Int): Float = notModeled("looperGetTrackSpeed")
    override fun looperSetTrackPlayCount(trackIndex: Int,  plays: Int) { notModeled("looperSetTrackPlayCount") }
    override fun looperSetTrackPercussionMode(trackIndex: Int,  percussion: Boolean) { notModeled("looperSetTrackPercussionMode") }
    override fun looperSetTrackSendToFx(trackIndex: Int,  sendToFx: Boolean) { notModeled("looperSetTrackSendToFx") }
    override fun looperIsTrackSendToFx(trackIndex: Int): Boolean = notModeled("looperIsTrackSendToFx")
    override fun looperSetTrackLoopRegion(trackIndex: Int,  startFrame: Long,  endFrame: Long) { notModeled("looperSetTrackLoopRegion") }
    override fun looperResetTrackLoopRegion(trackIndex: Int) { notModeled("looperResetTrackLoopRegion") }
    override fun looperGetTrackLoopStart(trackIndex: Int): Int = notModeled("looperGetTrackLoopStart")
    override fun looperGetTrackLoopEnd(trackIndex: Int): Int = notModeled("looperGetTrackLoopEnd")
    override fun looperGetProgress(): Float = notModeled("looperGetProgress")
    override fun looperGetRecordProgress(): Float = notModeled("looperGetRecordProgress")
    override fun looperGetMasterLoopFrames(): Int = notModeled("looperGetMasterLoopFrames")
    override fun looperGetTrackLengthFrames(trackIndex: Int): Int = notModeled("looperGetTrackLengthFrames")
    override fun looperGetTrackWaveform(trackIndex: Int,  numBins: Int): FloatArray = notModeled("looperGetTrackWaveform")
    override fun looperSaveUndoSnapshot(trackIndex: Int): Boolean = notModeled("looperSaveUndoSnapshot")
    override fun looperRestoreUndo(trackIndex: Int): Boolean = notModeled("looperRestoreUndo")
    override fun looperHasUndo(trackIndex: Int): Boolean = notModeled("looperHasUndo")
    override fun looperFindContentBounds(trackIndex: Int,  thresholdRatio: Float): Pair<Int, Int> = notModeled("looperFindContentBounds")
    override fun looperDetectOnsets(trackIndex: Int,  maxOnsets: Int,  hopFrames: Int,  sensitivity: Float): IntArray = notModeled("looperDetectOnsets")
    override fun looperAnalyzePitch(trackIndex: Int,  hopMs: Double): PitchSeries = notModeled("looperAnalyzePitch")
    override fun looperGetLevelEnvelope(trackIndex: Int,  binsPerSecond: Double): LevelEnvelope = notModeled("looperGetLevelEnvelope")
    override fun looperImportTrack(trackIndex: Int,  filePath: String,  sampleRate: Int): Boolean = notModeled("looperImportTrack")
    override fun looperCaptureTrack(trackIndex: Int,  filePath: String,  bitDepth: Int): Boolean = notModeled("looperCaptureTrack")
    override fun looperExportMix(filePath: String): Boolean = notModeled("looperExportMix")
    override suspend fun looperExportMixPro(filePath: String,  bitDepth: ExportBitDepth,  repeatLoops: Int,  countInBeats: Int,  applyLimiter: Boolean,  projectName: String?,  artist: String?,  comment: String?,  bpm: Int): Boolean = notModeled("looperExportMixPro")
    override suspend fun looperExportStems(directory: String,  bitDepth: ExportBitDepth,  repeatLoops: Int,  countInBeats: Int,  applyLimiter: Boolean,  bpm: Int): Int = notModeled("looperExportStems")
    override fun looperExportTrack(trackIndex: Int,  filePath: String): Boolean = notModeled("looperExportTrack")
    override fun looperSetExportSampleRate(sampleRate: Int) { notModeled("looperSetExportSampleRate") }
    override fun looperGetExportProgress(): Float = notModeled("looperGetExportProgress")
    override fun looperIsExportInProgress(): Boolean = notModeled("looperIsExportInProgress")
    override fun looperCancelExport() { notModeled("looperCancelExport") }
    override fun setLooperStateListener(listener: LooperStateListener?): Boolean = notModeled("setLooperStateListener")
}
