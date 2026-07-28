package com.watermellonstudios.audio.api

/**
 * Platform-agnostic interface for the native audio bridge.
 *
 * Extends [IEffectStateProvider] and [IEffectStateWriter] for effect chain operations.
 * Covers lifecycle, state queries, real-time params, voice system, mode, and backend.
 *
 * El arpegiador vive en [IArpeggiatorBridge], el motor de SoundFont en
 * [ISoundFontBridge] y el looper en [ILooperBridge], partidos por el mismo motivo que
 * [IInputBridge] y las dos de efectos: son dominios con consumidores propios y un fake
 * no debería tener que implementar cien métodos para cubrir uno.
 *
 * El looper **estaba** acá, recortado a 11 de sus 79 funciones y con una nota que decía
 * que subir el resto necesitaba su propia justificación. La justificación llegó — ver
 * el KDoc de [ILooperBridge] — así que las 11 se mudaron allá. Nadie que las llame se
 * entera: siguen expuestas por herencia.
 *
 * Lo que sigue **sin** estar acá, y por qué: la gestión de dispositivos USB (D4 —
 * iOS no la tiene) y el benchmark de latencia detallada, cuyo JNI vive en
 * `jni_benchmark.cpp` y no tiene contraparte en la C API.
 *
 * Android implementation: [com.watermellonstudios.audio.internal.bridge.AudioNativeBridge]
 */
interface IAudioNativeBridge :
    IEffectStateProvider,
    IEffectStateWriter,
    IInputBridge,
    IArpeggiatorBridge,
    ISoundFontBridge,
    ILooperBridge {

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

    /**
     * Si el motor recortó el tamaño de buffer por presión de recursos.
     *
     * Es diagnóstico: no cambia nada, explica por qué la latencia medida no coincide
     * con la pedida.
     */
    fun isUsingReducedBuffers(): Boolean
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
    fun setEffectsBypassSync(bypass: Boolean)
    fun isEffectsBypassedSync(): Boolean
    fun reorderEffectsSync(fromIndex: Int, toIndex: Int)

    /**
     * Cuántos efectos tiene la cadena, sin suspender.
     *
     * No es un duplicado de `IEffectStateProvider.getEffectCount()`: aquélla es
     * `suspend` y toma el mutex de efectos porque va con el resto del snapshot. Ésta es
     * la variante lock-free, para los mismos contextos no-suspend que consumen el resto
     * de los `*Sync`.
     */
    fun getEffectChainSize(): Int

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

    // ==================== FILTRO DE VOZ ====================
    //
    // Los cuatro son `RT-safe`. Sus rangos son CONTRATO y no defensa: hasta ahora los
    // recortes vivían sólo dentro de la implementación de Android, que es la forma
    // conocida de que dos implementaciones del mismo contrato diverjan en silencio.
    // Fuera de rango **no hace nada**, en vez de recortar al borde: recortar convertiría
    // un error de unidades del llamador (mandar 0.5 creyendo que es un porcentaje) en un
    // filtro a 20 Hz que sí suena, y por lo tanto en un bug de audio en vez de un no-op
    // visible.

    fun setVoiceFilterEnabled(enabled: Boolean)

    /** Frecuencia de corte en Hz. **Ignora lo que no sea finito o caiga fuera de 20..20000.** */
    fun setVoiceFilterCutoff(hz: Float)

    /** Resonancia. **Ignora lo que no sea finito o caiga fuera de 0..1.** */
    fun setVoiceFilterResonance(q: Float)

    /** 0 = paso bajo, 1 = paso alto, 2 = paso banda. **Ignora cualquier otro valor.** */
    fun setVoiceFilterMode(mode: Int)

    // ==================== DUAL TOUCH ====================

    fun setDualTouchMode(enabled: Boolean)
    fun setDualTouch(
        x1: Float, y1: Float, freq1: Float, amp1: Float, pressure1: Float,
        x2: Float, y2: Float, freq2: Float, amp2: Float, pressure2: Float,
        distance: Float, angle: Float
    )
    fun setDualTouchMixMode(modeId: Int)

    // ==================== CHORD VOICES ====================

    /**
     * Dispara las voces de un acorde. Las voces se asignan en el VoicePool
     * con un SOURCE_ID dedicado a chords (no interfiere con el oscilador principal).
     *
     * @param frequencies Frecuencias de armonía en Hz (NO incluye la raíz)
     * @param amplitude Amplitud 0.0–1.0
     * @param oscillatorType Tipo de oscilador para las voces del acorde
     */
    fun triggerChordNotes(frequencies: FloatArray, amplitude: Float, oscillatorType: Int)

    /** Actualiza freqs y amplitud de las voces activas del acorde (RT-safe). */
    fun updateChordNotes(frequencies: FloatArray, amplitude: Float)

    /** Libera todas las voces del acorde. */
    fun releaseChordNotes()

    // ==================== AUTOMATIZACIÓN Y MAPEO XY ====================
    //
    // Cómo un eje del pad mueve un parámetro de un efecto. El mapeo se configura una vez
    // y después [applyAutomation] corre por frame de gesto, así que es lock-free.
    //
    // Los guards vuelven a ser contrato: un valor no finito **se descarta** y uno fuera
    // de 0..1 **se recorta**. Y acá el recorte sí corresponde, al revés que en el filtro
    // de voz: estos parámetros son normalizados por definición, así que un 1.2 es ruido
    // numérico de la UI y no una unidad equivocada.

    /**
     * Configura qué parámetro mueve un eje.
     *
     * @param axis      0 = X, 1 = Y, 2 = profundidad.
     * @param curve     0 = lineal, 1 = exponencial, 2 = logarítmica, 3 = curva en S.
     * @param polarity  0 = unipolar, 1 = bipolar.
     */
    fun setMappingConfig(
        axis: Int,
        effectIndex: Int,
        paramId: Int,
        curve: Int,
        polarity: Int,
        mapMin: Float,
        mapMax: Float,
        inverted: Boolean,
    )

    /** Desconecta el eje. */
    fun clearMappingConfig(axis: Int)

    /**
     * Mueve el eje. Se llama por frame de gesto.
     *
     * [normalizedValue] se recorta a 0..1; si no es finito, no hace nada.
     */
    fun applyAutomation(axis: Int, normalizedValue: Float)

    /**
     * Escribe directo sobre un parámetro automatizado, sin pasar por un eje.
     *
     * [xyValue] se recorta a 0..1; si no es finito, no hace nada.
     */
    fun setAutomationParameter(effectIndex: Int, paramId: Int, xyValue: Float)

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

    /**
     * **El nombre dice USB; el efecto no es de USB, y eso importa en iOS.**
     *
     * Detrás, `wma_configure_usb_backend` llama a `BackendManager::setSampleRate()` — el
     * manager compartido, no un backend USB. `channels` y `bitDepth` son informativos:
     * el formato real lo elige el backend. O sea que **en iOS esto configura el sample
     * rate de verdad**, y hacerlo un no-op "porque iOS no tiene USB" (D4) descartaría en
     * silencio la única parte que sí tiene efecto.
     *
     * Es distinto de [isUsbBackendAvailable], que **reporta una capacidad** y por eso sí
     * miente si no dice `false`. Éste sólo transporta un pedido.
     */
    fun configureUsbBackend(sampleRate: Int, channels: Int, bitDepth: Int)

    /**
     * 0 = sólo reproducción, 1 = sólo captura, 2 = full-duplex.
     *
     * Misma historia que [configureUsbBackend]: detrás es
     * `BackendManager::setFullDuplexEnabled(mode == 2)`, y el full-duplex es exactamente
     * lo que hace `CoreAudioBackend` en iOS. El nombre quedó del origen Android.
     */
    fun setUsbStreamingMode(modeId: Int)

    /**
     * Select the USB latency profile (Fase 1). Re-parametrizes the USB
     * transfer pipeline (iso transfer duration, URBs in flight, pacer jitter
     * budget, DSP block size, ring capacity).
     *
     * Latched and applied on the next USB stream start — same semantics as the
     * USB streaming mode. A running stream keeps its current profile until it is
     * restarted. Fails only if there is no USB backend.
     */
    fun setUsbLatencyProfile(
        profile: com.watermellonstudios.audio.domain.usb.UsbLatencyProfile
    ): Result<Unit>

    // ==================== TRANSPORT (reloj musical + metrónomo) ====================
    //
    // El Transport es dueño del reloj musical: BPM, beats por compás y sample rate,
    // más un scheduler de metrónomo RT-safe. La UI arma una vez y el thread de audio
    // emite los clicks desde el render callback — que es lo que saca el "a veces
    // suena y a veces no" de un timer manejado desde la UI.
    //
    // El BPM NO está acá: se pone con [setBpm] (arriba), que además de al Transport
    // le llega a los efectos sincronizados al tempo. Un setter aparte dejaría a los
    // dos derivando.

    /** Beats por compás, recortado a 1..16. */
    fun transportSetBeatsPerBar(beatsPerBar: Int)
    fun transportGetBeatsPerBar(): Int

    /** Frames por beat al BPM y sample rate actuales. 0 sin motor. */
    fun transportFramesPerBeat(): Int

    /** Frames en `bars` compases completos. Sirve para cuantizar largos de loop. */
    fun transportFramesPerBar(bars: Int): Int

    /**
     * Programa `beats` clicks a intervalo de beat, desde el próximo bloque de audio.
     *
     * @param beats             cantidad de clicks (4 = un compás de cuenta regresiva).
     *                          `<= 0` **detiene** el metrónomo en vez de arrancarlo.
     * @param firstIsDownbeat   el primer click es downbeat (más agudo y fuerte).
     * @param everyBeatPattern  son downbeat los que caen en `index % beatsPerBar == 0`.
     *                          Tiene prioridad sobre [firstIsDownbeat].
     */
    fun transportStartMetronome(
        beats: Int,
        firstIsDownbeat: Boolean = true,
        everyBeatPattern: Boolean = true,
    )

    /** Click continuo hasta [transportStopMetronome] — la referencia mientras se graba. */
    fun transportStartMetronomeContinuous(everyBeatPattern: Boolean = true)

    /** Cancela lo programado. Un click ya sonando decae solo. */
    fun transportStopMetronome()

    fun transportIsMetronomeRunning(): Boolean
    fun transportIsMetronomeContinuous(): Boolean

    /**
     * Clicks que faltan en una cuenta. 0 en reposo.
     *
     * **En modo continuo devuelve el centinela del scheduler (1), no una cuenta de
     * nada** — un schedule continuo no tiene beats restantes. Hay que preguntar
     * [transportIsMetronomeContinuous] antes de leer esto.
     */
    fun transportGetRemainingBeats(): Int

    // ==================== LOG CAPTURE ====================

    /**
     * Captura de logs del motor — un anillo de 4000 líneas que se lee tirando, no
     * empujando.
     *
     * Es distinto de `AudioLogger`, que es push y entrega cada línea en el momento.
     * Esto existe para que una UI pueda mostrar las últimas N líneas **cuando el
     * usuario las pide**, sin dejar un callback vivo todo el tiempo. Deshabilitada
     * cuesta un load relajado.
     *
     * Global al proceso, no por motor: la C API detrás (`wma_log_capture_*`) no
     * toma `WmaEngine*`, porque el logger tampoco.
     */
    fun setLogCaptureEnabled(enabled: Boolean)

    /**
     * Vacía lo capturado desde la llamada anterior, en formato `"L/TAG: mensaje"`.
     *
     * **Es destructivo**: las líneas se van del anillo. Devuelve un array propio y
     * no un handle a propósito — la C API entrega un `WmaLogBatch*` que hay que
     * liberar, y dejar ese handle cruzar a Kotlin sería regalar un leak a cada
     * llamador. El batch nace y muere dentro de la implementación.
     *
     * Vacío es una respuesta normal, no un error: quiere decir que no pasó nada
     * desde la última vez.
     */
    fun drainCapturedLogs(): Array<String>

    /**
     * Líneas perdidas por desborde del anillo. **No se resetea al vaciar**, así que
     * es un acumulado: si crece entre dos lecturas, la UI está mirando menos de lo
     * que pasó y conviene que lo diga.
     */
    fun getLogCaptureDropped(): Int
}
