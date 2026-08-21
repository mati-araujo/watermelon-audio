package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.looper.ExportBitDepth

/**
 * El looper de la sección 19 de `watermelon_audio.h`, como contrato propio.
 *
 * ## Esto reemplaza al subconjunto de 11 que vivía en [IAudioNativeBridge]
 *
 * Aquel bloque decía, textual: *"El JNI tiene 79 funciones; acá hay 11, que son
 * exactamente las que necesita la tira del harness… Si NoisyPad pide el looper completo
 * desde `commonMain`, eso es un ticket con su propia justificación."*
 *
 * Es este ticket. NoisyPad llama **51** de las 74 del bridge de Android, y su
 * `LooperControllerAdapter` no puede salir de `:app` —o sea que iOS no puede tener
 * looper— hasta que existan en Kotlin común. Las 11 originales no se duplican: se
 * mudaron acá, así que [IAudioNativeBridge] las sigue exponiendo por herencia y ningún
 * llamador se entera.
 *
 * ## Sigue siendo un subconjunto, y sigue siendo a propósito
 *
 * Quedan afuera 23 miembros del bridge de Android **que no tienen un solo llamador**:
 * la telemetría (`looperGetTelemetry`, `looperResetTelemetry`,
 * `looperGetDroppedEvents`), los picos de nivel (`looperGetInputPeak`,
 * `looperGetTrackPeakLevel`, `looperGetTrackProgress`), el tail configurable
 * (`looperGetTailMs`/`looperSetTailMs`), `looperArmSyncedToLoopQuantized`,
 * `looperStartRecordingWithPreRoll`, `looperGetArmedTrack` y
 * `looperIsTrackPercussionMode`. La regla de opt-in no cambió: entran cuando aparezca
 * quien los use.
 *
 * ## El único que NO PUEDE estar acá
 *
 * **`looperExportMixCompressed`** renderiza un WAV y lo transcodifica a AAC con
 * `MediaCodec`. No es que falte declararlo: en iOS el equivalente es `AVAssetWriter`, o
 * sea una implementación nueva, no una firma. Se queda en `androidMain` hasta que
 * alguien escriba la mitad de Apple.
 *
 * `setLooperStateListener` **también** estaba afuera y ya no lo está: necesitaba una
 * superficie nueva en la C API (`wma_looper_set_event_callback`), que ahora existe. Ver
 * [setLooperStateListener].
 *
 * ## Sobre los dos `suspend`
 *
 * [looperExportMixPro] y [looperExportStems] son `suspend` porque bloquean mientras
 * escriben. **La interfaz no dice en qué dispatcher corren, y es deliberado**: Android
 * usa `Dispatchers.IO` y en Kotlin/Native ese dispatcher es inalcanzable —hay un miembro
 * `internal` que eclipsa la extensión pública, así que ni siquiera se puede nombrar—.
 * Cada implementación elige el suyo; lo que el contrato promete es que no hay que
 * llamarlas desde el hilo de UI.
 */
interface ILooperBridge {

    // ==================== HABILITACIÓN Y TRANSPORTE ====================

    fun looperSetEnabled(enabled: Boolean)

    fun looperPause()
    fun looperResume()
    fun looperStopAll()
    fun looperClearAll()

    fun looperIsRecording(): Boolean
    fun looperIsPlaying(): Boolean

    /** Un click puntual, para que la UI marque el pulso sin montar un metrónomo. */
    fun looperTriggerClick(isDownbeat: Boolean)

    // ==================== PREPARAR PISTAS ====================

    /**
     * Reserva la pista con un largo explícito en frames.
     *
     * @return `false` si no se pudo reservar (presupuesto de memoria, índice inválido).
     */
    fun looperPrepareTrack(trackIndex: Int, lengthFrames: Int, sampleRate: Int): Boolean

    /**
     * Prepara la pista con un largo de `bars` compases al reloj actual.
     *
     * @return frames reservados, o **-1** si `bars` desborda int32 al pasarlo a
     *         frames. Ese -1 es de la tanda 3 de WA-2.6: antes alocaba una pista con
     *         el largo envuelto.
     */
    fun looperPrepareTrackBars(trackIndex: Int, bars: Int, sampleRate: Int): Int

    /** Modo de largo libre: la pista se define al parar de grabar, no antes. */
    fun looperSetFreeLength(freeLength: Boolean)

    /**
     * Cierra una toma de largo libre fijando su región y su cola.
     *
     * @return `false` si la región es inválida para esa pista.
     */
    fun looperFinalizeFreeLoop(
        trackIndex: Int,
        loopStart: Int,
        loopEnd: Int,
        tailFrames: Int,
    ): Boolean

    /**
     * El techo de recursos del looper según el tier del dispositivo.
     *
     * Pasar 0 en cualquiera deja ese valor como estaba. `maxTracks` se recorta al techo
     * del motor y **nunca baja por debajo de una pista ya activa**.
     */
    fun looperSetCapabilities(budgetBytes: Long, maxTracks: Int, maxFreeSeconds: Int)

    // ==================== ARMADO Y GRABACIÓN ====================

    /**
     * Arma la pista para empezar a grabar en el próximo límite de compás.
     *
     * @return el frame absoluto del disparo (`>= 0`), o **-1 si no se armó nada**.
     *
     * **El -1 hay que mostrarlo.** El bug de la tanda 2 de WA-2.6 era exactamente esto:
     * devolvía un trigger frame para una grabación que nunca arrancaba. Un botón que
     * sólo diga "armado" no lo vuelve a ver.
     */
    fun looperArmAtNextBar(trackIndex: Int): Long

    /** Arma con un desplazamiento explícito. Mismo contrato de -1 que [looperArmAtNextBar]. */
    fun looperArmInFrames(trackIndex: Int, offsetFrames: Long): Long

    /**
     * Arma sincronizado al loop maestro, compensando la latencia de entrada.
     * Mismo contrato de -1 que [looperArmAtNextBar].
     */
    fun looperArmSyncedToLoop(trackIndex: Int, latencyFrames: Long): Long

    fun looperCancelArm()

    fun looperStartRecording(trackIndex: Int)
    fun looperStopRecording()

    /** Descarta lo que se está grabando en vez de cerrarlo. */
    fun looperAbortRecording()

    /** Sigue grabando sobre lo que la pista ya tiene, sin borrarlo. */
    fun looperStartOverdub(trackIndex: Int)

    // ==================== PISTAS ====================

    fun looperIsTrackActive(trackIndex: Int): Boolean
    fun looperIsTrackPlaying(trackIndex: Int): Boolean

    fun looperClearTrack(trackIndex: Int)
    fun looperPauseTrack(trackIndex: Int)
    fun looperResumeTrack(trackIndex: Int)
    fun looperResetTrackPlayHead(trackIndex: Int)

    /** Recorta el silencio de los extremos. `false` si no había nada que recortar. */
    fun looperTrimTrack(trackIndex: Int): Boolean

    // ==================== MEZCLA ====================

    fun looperSetMasterVolume(volume: Float)
    fun looperGetMasterVolume(): Float

    fun looperSetTrackVolume(trackIndex: Int, volume: Float)
    fun looperSetTrackPan(trackIndex: Int, pan: Float)
    fun looperSetTrackMuted(trackIndex: Int, muted: Boolean)

    fun looperSetTrackSpeed(trackIndex: Int, speed: Float)
    fun looperGetTrackSpeed(trackIndex: Int): Float

    /** Cuántas vueltas suena la pista antes de callarse. 0 = indefinido. */
    fun looperSetTrackPlayCount(trackIndex: Int, plays: Int)

    /** Modo percusión: la pista dispara one-shots en vez de sostener el loop. */
    fun looperSetTrackPercussionMode(trackIndex: Int, percussion: Boolean)

    /**
     * Rutear la pista **por la cadena de efectos** (REQ-007). Default: `false`.
     *
     * Con `false` la pista se mezcla donde siempre —downstream de la cadena— y ni el fade
     * de pausa ni los efectos la tocan. Con `true` se suma a la **entrada** de la cadena, o
     * sea entra al bus del instrumento, y eso tiene dos contrapartidas que no son efectos
     * colaterales sino el precio del ruteo:
     *
     * - **recibe el fade** de pausa y de cambio de escena: deja de valer para ella el
     *   invariante "los loops sobreviven a la transición";
     * - **entra al tap de grabación**: grabar mientras suena la mete en la toma. No es
     *   evitable — el tap lee la salida aguas abajo de la cadena, donde la pista ya es
     *   inseparable del synth.
     *
     * El **exportador no ve este flag**: lee los buffers de pista directo, así que una pista
     * marcada se exporta **seca**. Es deliberado (la toma es lo que se grabó), pero conviene
     * saberlo porque es una asimetría con lo que se oye.
     *
     * Índice fuera de rango: sin efecto, igual que el resto de la familia `looperSetTrack*`.
     */
    fun looperSetTrackSendToFx(trackIndex: Int, sendToFx: Boolean)

    /** Si la pista está ruteada por la cadena. Ver [looperSetTrackSendToFx]. */
    fun looperIsTrackSendToFx(trackIndex: Int): Boolean

    // ==================== REGIÓN DE LOOP ====================

    fun looperSetTrackLoopRegion(trackIndex: Int, startFrame: Long, endFrame: Long)
    fun looperResetTrackLoopRegion(trackIndex: Int)
    fun looperGetTrackLoopStart(trackIndex: Int): Int
    fun looperGetTrackLoopEnd(trackIndex: Int): Int

    // ==================== LECTURA PARA LA UI ====================
    //
    // Todo esto es polling. El único camino de push del looper es
    // [setLooperStateListener], y llega por un worker del motor, no por el thread de
    // audio — ver su KDoc para por qué eso no contradice a D6.

    /** Avance del loop maestro, 0..1. */
    fun looperGetProgress(): Float

    /** Avance de la grabación en curso, 0..1. */
    fun looperGetRecordProgress(): Float

    fun looperGetMasterLoopFrames(): Int
    fun looperGetTrackLengthFrames(trackIndex: Int): Int

    /**
     * La forma de onda de la pista, resumida en [numBins] valores.
     *
     * Devuelve un array propio de largo [numBins]: el buffer se aloca y se llena del
     * lado de la implementación, así que el llamador no maneja memoria prestada.
     */
    fun looperGetTrackWaveform(trackIndex: Int, numBins: Int = 24): FloatArray

    // ==================== UNDO ====================

    fun looperSaveUndoSnapshot(trackIndex: Int): Boolean
    fun looperRestoreUndo(trackIndex: Int): Boolean
    fun looperHasUndo(trackIndex: Int): Boolean

    // ==================== ANÁLISIS ====================

    /**
     * Primer y último frame audible de la pista, para recortar el silencio de una toma
     * libre. [thresholdRatio] es la fracción del pico que cuenta como contenido.
     *
     * Devuelve `(first, lastExclusive)`, o `(0, 0)` si la pista está en silencio o el
     * índice no es válido.
     */
    fun looperFindContentBounds(trackIndex: Int, thresholdRatio: Float): Pair<Int, Int>

    /**
     * Posiciones de los transitorios, ascendentes — para sacarle el tempo a una toma
     * libre a partir de su ritmo.
     *
     * Sólo desde hilo de UI o de coroutine, y después de parar de grabar.
     *
     * @param hopFrames  ventana de análisis (256 ≈ 5,3 ms a 48 kHz).
     * @param sensitivity `> 1` detecta más onsets.
     */
    fun looperDetectOnsets(
        trackIndex: Int,
        maxOnsets: Int = 512,
        hopFrames: Int = 256,
        sensitivity: Float = 1.0f,
    ): IntArray

    // ==================== IMPORTAR Y CAPTURAR ====================

    /** Carga un WAV en la pista, resampleando si hace falta. */
    fun looperImportTrack(trackIndex: Int, filePath: String, sampleRate: Int): Boolean

    /**
     * Escribe el buffer **completo** de la pista, ignorando la región de loop.
     *
     * Es lo que usa el guardado de sesión, y por eso toma la profundidad en crudo: con
     * [ExportBitDepth.FLOAT_32] el round-trip es sin pérdida.
     */
    fun looperCaptureTrack(trackIndex: Int, filePath: String, bitDepth: Int): Boolean

    // ==================== EXPORTAR ====================

    /**
     * Exporta la mezcla a un archivo. **Sincrónico — llamar fuera del main thread.**
     *
     * Devuelve `false` en vez de dejar escapar una excepción (tanda 4 de WA-2.6):
     * antes, un export imposible abortaba el proceso.
     */
    fun looperExportMix(filePath: String): Boolean

    /** La mezcla, con opciones y metadatos. Ver la nota sobre dispatchers en la interfaz. */
    suspend fun looperExportMixPro(
        filePath: String,
        bitDepth: ExportBitDepth = ExportBitDepth.PCM_16,
        repeatLoops: Int = 1,
        countInBeats: Int = 0,
        applyLimiter: Boolean = true,
        projectName: String? = null,
        artist: String? = null,
        comment: String? = null,
        bpm: Int = 0,
    ): Boolean

    /**
     * Cada pista activa como un WAV propio en [directory], todos del mismo largo y
     * profundidad para que entren derecho en un DAW.
     *
     * @return cuántos stems se escribieron, o **-1** si falló.
     */
    suspend fun looperExportStems(
        directory: String,
        bitDepth: ExportBitDepth = ExportBitDepth.PCM_16,
        repeatLoops: Int = 1,
        countInBeats: Int = 0,
        applyLimiter: Boolean = true,
        bpm: Int = 0,
    ): Int

    fun looperExportTrack(trackIndex: Int, filePath: String): Boolean

    fun looperSetExportSampleRate(sampleRate: Int)

    /** Avance del export en vuelo, 0..1. */
    fun looperGetExportProgress(): Float
    fun looperIsExportInProgress(): Boolean

    /** Le pide al export en vuelo que pare. No es instantáneo. */
    fun looperCancelExport()

    // ==================== EVENTOS DE ESTADO (push) ====================

    /**
     * Instala (o quita, con `null`) el receptor de eventos de estado del looper.
     *
     * ## Esto es push, y NO contradice la regla RT
     *
     * El thread de audio **no** llama a [listener]: empuja a una cola lock-free y sigue.
     * Un worker propio del motor la drena cada ~15 ms y desde ahí llama acá. O sea que
     * D6 —"el thread de audio jamás entra a Kotlin"— se sigue cumpliendo, y por eso este
     * es el único lugar de toda la fachada donde el motor llama hacia arriba en vez de
     * ser preguntado.
     *
     * Existe porque lo contrario se midió y era caro: el consumidor sondeaba progreso,
     * estado y pico de 8 pistas cada 33 ms —unas 800 llamadas por segundo— y la UI se
     * notaba. El motor además coalesce (progreso cada ≥ 2048 frames, pico cada ≥ 0,5 dB,
     * `playing` sólo en transiciones), que es lo que un sondeo no puede hacer.
     *
     * ## Los callbacks NO llegan en el hilo de UI
     *
     * Llegan en ese worker. Quien implemente [LooperStateListener] tiene que saltar al
     * hilo de UI por su cuenta y ser thread-safe con lo que toque.
     *
     * ## Quitar el listener no corta los eventos en vuelo
     *
     * El worker toma una copia del sink una vez por pasada, así que un evento levantado
     * justo antes de un `setLooperStateListener(null)` **todavía llega**. No es una
     * carrera que se gane ordenando las llamadas: es el contrato del despachador. Una
     * implementación no puede liberar nada que el callback necesite apenas lo quita.
     *
     * Sólo hay un listener a la vez: registrar reemplaza al anterior.
     *
     * @return `false` si no se pudo instalar. Quitarlo siempre devuelve `true`.
     */
    fun setLooperStateListener(listener: LooperStateListener?): Boolean
}
