# Etapa 5 — API Kotlin, observabilidad y lifecycle

**Estado:** propuesta — no iniciada.
**Dependencias:** stages 1, 2, 3, 4 mergeados. Consolida todo lo expuesto en etapas anteriores en una superficie Kotlin pulida y coherente.
**Duración estimada:** 3–5 días.
**Severidad de los bugs que resuelve:** 1× Mayor + pulido general de lifecycle, audio focus, allowlist.

---

## 1. Objetivo

Cerrar el **MVP profesional** de la librería desde el punto de vista del consumidor. Al terminar esta etapa, la API Kotlin debe permitir a NoisyPad (u otro consumer):

1. Enumerar devices USB con snapshot completo de capacidades (altsettings, formatos, clock sources, volumen per-channel).
2. Elegir proactivamente altsetting, clock source, channel routing y volume per-channel.
3. Observar reactivamente eventos de salud (drift, underrun rate, clock switch, jitter p99) sin polling.
4. Manejar correctamente audio focus loss, app background, screen-off, process death.
5. Expandir la allowlist a ≥ 25 devices con una estructura de datos que soporte remote config futura.

Esta etapa **no introduce nuevas capacidades de bajo nivel**; concentra, documenta y valida la superficie pública.

---

## 2. Consolidación del `IUsbAudioManager`

### 2.1 Surface final esperado

```kotlin
// commonMain/.../api/IUsbAudioManager.kt
interface IUsbAudioManager {
    // --- Discovery & state ---
    val connectedDevices: StateFlow<List<UsbAudioDevice>>
    val selectedDevice: StateFlow<UsbAudioDevice?>
    val connectionState: StateFlow<UsbConnectionState>
    val deviceEvents: SharedFlow<UsbDeviceEvent>
    val healthEvents: Flow<UsbHealthEvent>                    // stage 3
    val currentCapabilitySnapshot: StateFlow<UsbCapabilitySnapshot?>  // stage 2
    val currentTransferStats: StateFlow<UsbTransferStats?>    // was pull-only, now reactive

    // --- Lifecycle ---
    fun startMonitoring()
    fun stopMonitoring()
    suspend fun release()

    // --- Device selection & permission ---
    suspend fun refreshDevices()
    suspend fun connectDevice(device: UsbAudioDevice): Result<UsbAudioDevice>
    suspend fun disconnectDevice(): Result<Unit>
    suspend fun requestPermission(device: UsbAudioDevice): Result<Boolean>
    fun hasPermission(device: UsbAudioDevice): Boolean

    // --- Streaming configuration ---
    fun setStreamPreference(preference: StreamPreference)    // stage 2
    fun rankPlaybackAltsettings(pref: StreamPreference): List<ScoredAltsetting>  // stage 2
    suspend fun selectAltsetting(interfaceNumber: Int, alternateSetting: Int, formatIndex: Int): Result<Unit>  // stage 2
    suspend fun selectClockSource(clockSourceId: Int): Result<Unit>  // stage 3
    suspend fun setChannelRouting(routing: ChannelRouting): Result<Unit>  // stage 4

    // --- Streaming ---
    suspend fun startStreaming(config: UsbStreamConfig = UsbStreamConfig.DEFAULT): Result<StreamHandle>
    suspend fun startSplitStream(
        inputSource: BackendChoice,
        outputSink: BackendChoice,
        config: UsbStreamConfig = UsbStreamConfig.DEFAULT,
    ): Result<StreamHandle>                                   // stage 4
    suspend fun stopStreaming(): Result<Unit>

    // --- Volume ---
    val volumeState: StateFlow<UsbVolumeState>
    val perChannelVolumeCaps: StateFlow<List<ChannelVolumeInfo>>  // stage 4
    suspend fun setOutputVolume(volume: Float): Result<Unit>
    suspend fun setInputVolume(volume: Float): Result<Unit>
    suspend fun setChannelVolume(channelIndex: Int, volume: Float): Result<Unit>  // stage 4
    suspend fun toggleOutputMute(): Result<Unit>
    suspend fun adjustOutputVolume(delta: Float): Result<Unit>

    // --- Capabilities query ---
    suspend fun getDeviceCapabilities(): Result<UsbAudioCapabilities>  // now uses snapshot
    fun shouldInterceptVolumeButtons(): Boolean
    fun supportsFullDuplex(): Boolean
    fun hasCapture(): Boolean

    // --- Compat & test ---
    fun getCompatibilityStatus(device: UsbAudioDevice): UsbCompatibilityResult
    suspend fun exportDeviceReport(): String  // new: diagnostic dump for support tickets
}
```

### 2.2 Deprecaciones

Marcar como `@Deprecated`:
- `getTransferStats()` — reemplazado por `currentTransferStats: StateFlow`.
- `parseBasicCapabilities` interno (ya removido en stage 2).
- Las dos sobrecargas de `startStreaming(sampleRate, channels, bitDepth, mode)` — reemplazadas por `startStreaming(config)` con `UsbStreamConfig`.

Mantener las versiones legacy durante un ciclo de release, con `ReplaceWith` anotaciones:

```kotlin
@Deprecated(
    "Use startStreaming(config) with UsbStreamConfig",
    ReplaceWith("startStreaming(UsbStreamConfig(sampleRate, channels, bitDepth, mode))"),
    level = DeprecationLevel.WARNING,
)
suspend fun startStreaming(sampleRate: Int, channels: Int, bitDepth: Int,
                            mode: UsbStreamingMode = UsbStreamingMode.PLAYBACK_ONLY): Result<Unit>
```

### 2.3 `StreamHandle`

Nuevo tipo: un handle opaco que representa un stream activo, permitiendo observar métricas específicas del stream y pararlo sin ambigüedad si hay múltiples.

```kotlin
data class StreamHandle(
    val id: String,
    val deviceId: String,
    val backendType: AudioBackendType,
    val startedAtMs: Long,
    val configSnapshot: UsbStreamConfig,
) {
    fun isActive(): Boolean  // check against manager
}
```

Simple por ahora; se puede enriquecer en etapas futuras con streams concurrentes.

---

## 3. Observabilidad reactiva

### 3.1 Transfer stats como StateFlow

Hoy se accede a `getTransferStats()` mediante polling. Reemplazar por:

```kotlin
// UsbAudioManagerImpl.kt
private val _currentTransferStats = MutableStateFlow<UsbTransferStats?>(null)
override val currentTransferStats: StateFlow<UsbTransferStats?> = _currentTransferStats.asStateFlow()

private fun startStatsPoller() {
    scope.launch(Dispatchers.Default) {
        while (isActive && _connectionState.value == UsbConnectionState.STREAMING) {
            val stats = nativeBridge.getUsbTransferStats()?.let { UsbTransferStats.fromNativeArray(it) }
            _currentTransferStats.value = stats
            delay(250)  // 4Hz update — enough for UI meters, cheap enough not to impact CPU
        }
        _currentTransferStats.value = null
    }
}
```

El polling interno en 250 ms es aceptable para métricas de UI; el hot path nativo no se toca.

### 3.2 Integración con `UsbLatencyProfiler`

El `UsbTransferStats` Kotlin debe recibir los campos nuevos del profiler expuestos vía `nativeGetUsbProfilingStats` (ya existen en `jni_usb.cpp:77-94`), ampliados con los de clock sync (stage 3):

```kotlin
data class UsbTransferStats(
    // ... existing

    // Clock sync (stage 3)
    val currentSampleRateHz: Float,
    val driftPpm: Float,
    val feedbackPacketsReceived: Long,
    val feedbackPacketsInvalid: Long,
    val activeClockSourceId: Int,

    // Jitter (already in profiler)
    val dspCallbackJitterP95Us: Float,
    val dspCallbackJitterP99Us: Float,
    val outputTransferP99LatencyUs: Float,
    val inputTransferP99LatencyUs: Float,

    // Bridge stats (stage 4, only populated in SplitBackend streams)
    val bridgeOverruns: Long = 0L,
    val bridgeUnderruns: Long = 0L,
)
```

### 3.3 Diagnostic dump

```kotlin
suspend fun exportDeviceReport(): String = withContext(Dispatchers.IO) {
    val snapshot = currentCapabilitySnapshot.value ?: return@withContext "No device connected"
    val stats = currentTransferStats.value
    val volumeState = volumeState.value
    val caps = getDeviceCapabilities().getOrNull()

    buildString {
        appendLine("═══ USB Audio Diagnostic Report ═══")
        appendLine("Generated: ${formatIso8601(System.currentTimeMillis())}")
        appendLine("Library: watermelon-audio ${BuildConfig.VERSION_NAME}")
        appendLine()
        appendLine("── Device ──")
        appendLine("  Manufacturer:  ${snapshot.manufacturer ?: "<unknown>"}")
        appendLine("  Product:       ${snapshot.productName ?: "<unknown>"}")
        appendLine("  VID:PID:       ${snapshot.vendorId.toHex4()}:${snapshot.productId.toHex4()}")
        appendLine("  Serial:        ${snapshot.serialNumber ?: "<none>"}")
        appendLine("  UAC version:   ${snapshot.uacVersion}")
        appendLine()
        appendLine("── Altsettings (playback) ──")
        snapshot.playbackAltsettings.forEach { alt ->
            appendLine("  IF${alt.interfaceNumber} Alt${alt.alternateSetting}  " +
                       "sync=${alt.syncType}  feedback=${alt.hasFeedbackEndpoint}")
            alt.formats.forEach { fmt ->
                appendLine("    ${fmt.channels}ch/${fmt.bitResolution}bit  " +
                           "rates=${fmt.sampleRates.joinToString()}")
            }
        }
        appendLine()
        appendLine("── Clock sources ──")
        snapshot.clockSources.forEach {
            appendLine("  ${it.clockId}  type=${it.type}  syncedToSof=${it.syncedToSof}")
        }
        appendLine()
        appendLine("── Current stats ──")
        stats?.let {
            appendLine("  Sample rate:    ${"%.2f".format(it.currentSampleRateHz)} Hz (drift ${"%.1f".format(it.driftPpm)} PPM)")
            appendLine("  Latency (out):  avg=${"%.2f".format(it.avgLatencyMs)} ms  p99=${"%.2f".format(it.outputTransferP99LatencyUs / 1000f)} ms")
            appendLine("  DSP jitter:     p95=${"%.0f".format(it.dspCallbackJitterP95Us)} µs  p99=${"%.0f".format(it.dspCallbackJitterP99Us)} µs")
            appendLine("  Underruns:      ${it.underruns}")
            appendLine("  Overruns:       ${it.overruns}")
            appendLine("  Feedback pkts:  ok=${it.feedbackPacketsReceived} invalid=${it.feedbackPacketsInvalid}")
        } ?: appendLine("  (no stats available — not streaming)")
        appendLine()
        appendLine("── Volume ──")
        appendLine("  Output mode:    ${volumeState.capabilities.outputMode}")
        appendLine("  Output level:   ${"%.2f".format(volumeState.outputVolume)}")
        appendLine("  Output muted:   ${volumeState.outputMuted}")
        // ... input, per-channel
    }
}
```

Ese reporte es la herramienta de soporte primaria cuando un device falla en un end user.

---

## 4. Audio focus y lifecycle

### 4.1 Problema actual

`UsbAudioManagerImpl` maneja wake lock y detección de disconnect, pero **no responde a audio focus changes**. Si otra app toma el foco, el stream USB sigue corriendo — esto es deseable para algunos casos pero problemático en otros (Android duck request, phone call, etc.).

### 4.2 Diseño

Añadir un `AudioFocusHandler` (androidMain) que opcionalmente se registra:

```kotlin
// androidMain/.../internal/usb/AudioFocusHandler.kt
internal class AudioFocusHandler(
    private val context: Context,
    private val onFocusLost: () -> Unit,
    private val onFocusGained: () -> Unit,
    private val onFocusLostTransient: (canDuck: Boolean) -> Unit,
) {
    private val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private var focusRequest: AudioFocusRequest? = null
    private val changeListener = AudioManager.OnAudioFocusChangeListener { change ->
        when (change) {
            AudioManager.AUDIOFOCUS_GAIN -> onFocusGained()
            AudioManager.AUDIOFOCUS_LOSS -> onFocusLost()
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT -> onFocusLostTransient(false)
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> onFocusLostTransient(true)
        }
    }

    fun request(): Boolean {
        val attrs = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
            .build()
        focusRequest = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
            .setAudioAttributes(attrs)
            .setAcceptsDelayedFocusGain(true)
            .setOnAudioFocusChangeListener(changeListener)
            .build()
        return audioManager.requestAudioFocus(focusRequest!!) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
    }

    fun release() {
        focusRequest?.let { audioManager.abandonAudioFocusRequest(it) }
        focusRequest = null
    }
}
```

Políticas configurables:

```kotlin
enum class AudioFocusPolicy {
    NONE,             // don't request focus at all (library stays passive)
    TRANSIENT_PAUSE,  // pause on transient loss, resume on gain
    TRANSIENT_DUCK,   // keep streaming at -6 dB during duckable transient
    PERSISTENT,       // never stop; just post an event
}

fun setAudioFocusPolicy(policy: AudioFocusPolicy)
```

`TRANSIENT_PAUSE` llama `pause()` en el backend, que ya existe en `IAudioBackend`. Al recuperar foco, `resume()`. `TRANSIENT_DUCK` aplica -6 dB al volumen digital (via `setDigitalOutputVolume(0.5)`) durante la duración.

### 4.3 Eventos publicados

Añadir al `UsbDeviceEvent`:

```kotlin
object AudioFocusLost : UsbDeviceEvent()
object AudioFocusGained : UsbDeviceEvent()
data class AudioFocusTransient(val canDuck: Boolean) : UsbDeviceEvent()
```

El consumer puede decidir qué hacer.

### 4.4 Background & process death

- **Background con wake lock**: ya funciona. Mantener.
- **Screen off**: en Android 12+ con foreground service, el stream sigue. Documentar que el consumer debe tener un `ForegroundService` si quiere streaming con pantalla apagada.
- **Process death**: Android mata el proceso, todo el nativo se libera automáticamente. El device USB queda disponible para la siguiente instancia. No hay recovery state; documentarlo.
- **Deep sleep con doze**: el wake lock `PARTIAL_WAKE_LOCK` previene doze. Mantener.

### 4.5 Lifecycle hooks para el consumer

```kotlin
interface UsbAudioLifecycleCallback {
    fun onStreamingWillStart(device: UsbAudioDevice, config: UsbStreamConfig) {}
    fun onStreamingDidStart(handle: StreamHandle) {}
    fun onStreamingWillStop(handle: StreamHandle) {}
    fun onStreamingDidStop(handle: StreamHandle) {}
    fun onDeviceDidConnect(device: UsbAudioDevice) {}
    fun onDeviceDidDisconnect(deviceId: String) {}
    fun onFatalError(error: UsbDeviceEvent.Error) {}
}

fun registerLifecycleCallback(callback: UsbAudioLifecycleCallback)
fun unregisterLifecycleCallback(callback: UsbAudioLifecycleCallback)
```

Útil para integrar con Lifecycle-aware components sin atar al ViewModel.

---

## 5. Compatibility allowlist 2.0

### 5.1 Problema

`UsbDeviceCompatibility.kt` tiene 10 devices hardcoded. Para una librería profesional, apuntamos a ≥ 25 initial + mecanismo para crecer.

### 5.2 Diseño

Convertir la allowlist en un **resource bundle** JSON bajo `audio/src/androidMain/resources/usb_compatibility.json`, cargable en runtime. El struct sigue siendo `CompatibleDevice` (kotlin) pero la fuente es parseable:

```json
{
  "version": 1,
  "updatedAt": "2026-04-10",
  "devices": [
    {
      "vendorId": "0x1235",
      "productId": "0x8211",
      "name": "Focusrite Scarlett Solo 3rd Gen",
      "manufacturer": "Focusrite",
      "uacVersion": 2,
      "testedSampleRates": [44100, 48000, 88200, 96000, 176400, 192000],
      "testedBitDepths": [24],
      "testedChannels": [2],
      "syncMode": "ASYNCHRONOUS",
      "knownIssues": [],
      "notes": "Pro USB interface with external clock source option",
      "compatibility": "VERIFIED",
      "firstSupportedVersion": "1.0.0"
    }
    // ...
  ]
}
```

El loader lo parsea al arrancar `UsbDeviceCompatibility` y mantiene el lookup O(1). Si el JSON está ausente o corrupto, fallback al array hardcoded como safety net.

### 5.3 Expansion inicial a 25+

Incluir, como mínimo:

- **Budget DACs**: C-Media UC02, C-Media CM6206, Sound Blaster Play! 3 & 4, FiiO BTR5 & BTR7, iFi Zen Air, Apple USB-C Adapter, Google USB-C Adapter.
- **Portable DAC/AMPs**: Hiby FC3 & FC4, FiiO KA1 & KA3 & KA5, Topping NX3s, THX Onyx, Astell&Kern AK HC2.
- **Mid-range interfaces**: Focusrite Scarlett Solo 3rd Gen, Scarlett 2i2 3rd Gen, Behringer UMC22 & UMC202HD & UMC204HD, M-Audio M-Track Solo, PreSonus AudioBox USB 96.
- **USB mics**: Blue Yeti, Shure MV7, Audio-Technica AT2020USB+, Rode NT-USB Mini.

Cada entry con los campos validados (ideal: testeados con el device real; en su defecto, derivados del descriptor parseado).

### 5.4 Remote config hook (opcional, no implementado en esta etapa)

Dejar el diseño hecho: un `RemoteCompatibilityProvider` interface que puede inyectarse para fetch-at-startup desde un endpoint HTTPS de Watermelon Studios. Fuera del scope de esta etapa; infra para habilitarlo sin refactor.

---

## 6. Integración de descriptor parsing nativo completa

Eliminar el TODO en `UsbAudioManagerImpl.parseBasicCapabilities()`. Ya resuelto en stage 2 conceptualmente, aquí se cierra cualquier código muerto y se valida.

**Checklist.**

- [ ] Eliminar la función `parseBasicCapabilities` si quedó como fallback.
- [ ] Verificar que `getDeviceCapabilities()` siempre devuelve datos derivados del snapshot o un `Result.failure` legible.
- [ ] Los test runners usan el snapshot para validar que los parámetros de streaming no excedan lo soportado.

---

## 7. Documentación y ejemplos

Un criterio "listo" incluye documentación ampliada:

- `audio/src/commonMain/kotlin/.../api/IUsbAudioManager.kt` — KDoc completo con ejemplos en el class-level.
- Un archivo `docs/usage/usb-audio-quickstart.md` (nuevo): 5 ejemplos:
  1. "Conectar y reproducir tono"
  2. "Streaming full-duplex con selección de altsetting"
  3. "Split backend Oboe input + USB output"
  4. "Observar drift y responder a threshold events"
  5. "Controlar volumen per-channel"
- Release notes con la lista de nuevas APIs y migraciones obligatorias.

---

## 8. Tests

### 8.1 `IUsbAudioManager` — contrato

Tests Kotlin que validen, con un `FakeUsbAudioManager` mock:

- Todas las StateFlow emiten valor inicial sin bloquear.
- Las operaciones suspend retornan `Result.success` en happy path y `Result.failure` con el error esperado en cada falla.
- Los deprecados siguen funcionando.

### 8.2 Audio focus behavior

Tests con un mock de `AudioFocusHandler`:
- `TRANSIENT_PAUSE`: simula pérdida, verifica que el backend entra en pause; simula recuperación, verifica resume.
- `TRANSIENT_DUCK`: simula pérdida, verifica que `setOutputVolume` baja a 0.5; recuperación restaura volumen previo.
- `PERSISTENT`: simula pérdida, verifica que solo emite evento, sin cambios de estado.
- `NONE`: no se registra focus request.

### 8.3 Allowlist loading

- JSON válido parsea a 25+ devices.
- JSON corrupto hace fallback al hardcoded.
- `checkCompatibility()` devuelve resultados consistentes con los de antes para los 10 devices originales.

### 8.4 Exporter diagnóstico

- `exportDeviceReport()` sin device conectado retorna un mensaje legible, no crashea.
- Con device conectado streaming, el reporte incluye todos los campos esperados.
- Golden test: comparar output en un device stub con un string esperado.

---

## 9. Criterios de aceptación

- [ ] `IUsbAudioManager` final incluye todas las funciones listadas en §2.1 y compila sin warnings en commonMain y androidMain.
- [ ] Legacy `startStreaming(Int, Int, Int, ...)` sigue funcionando con `@Deprecated(WARNING)` y `ReplaceWith`.
- [ ] `currentTransferStats: StateFlow<UsbTransferStats?>` emite updates cada 250 ms durante streaming, null cuando no.
- [ ] `AudioFocusHandler` implementado, con `AudioFocusPolicy` configurable por el consumer.
- [ ] `UsbDeviceEvent.AudioFocusLost`, `.AudioFocusGained`, `.AudioFocusTransient(canDuck)` se emiten correctamente.
- [ ] `usb_compatibility.json` con ≥ 25 devices checkeados al repo; loader con fallback funcional.
- [ ] `UsbDeviceCompatibility.checkCompatibility()` usa el loader y mantiene compatibilidad exacta con los 10 devices originales (tests de regresión).
- [ ] `exportDeviceReport()` implementado y testeado en un device real.
- [ ] `docs/usage/usb-audio-quickstart.md` con 5 ejemplos funcionales, verificados compilando en el proyecto de ejemplo.
- [ ] `StreamHandle` retornado por `startStreaming()` y `startSplitStream()`, único por stream activo.
- [ ] Test runner existente pasa todas las regresiones con la nueva surface.
- [ ] NoisyPad (el consumer principal) compila sin cambios obligatorios tras consumir la nueva versión (backward compat via typealiases y deprecations).

---

## 10. Riesgos específicos

1. **ABI breakage con NoisyPad.** El mayor riesgo es que las typealiases no cubran todo. Mitigación: hacer una build de NoisyPad contra la branch al inicio de la etapa y durante, no al final.
2. **JSON compatibility loader tamaño.** Parsear JSON en cold start puede añadir 50–200 ms. Lazy loading bajo `lazy {}`.
3. **Audio focus policy incorrecta por default.** Default `NONE` para no cambiar comportamiento actual; documentar claramente que profesionales deben elegir `TRANSIENT_PAUSE` o `PERSISTENT`.
4. **Flow collection en consumer olvidada.** Los StateFlow sin consumer activo no gastan CPU (coldish), pero hay que documentar claramente qué collect es responsabilidad del consumer.
5. **`exportDeviceReport()` con info sensible.** El serial number es PII en algunos contextos. Ofrecer un parámetro `redactSerial: Boolean = true` por default.

---

## 11. Checklist de commit

Diff aproximado:

- `audio/src/commonMain/kotlin/.../api/IUsbAudioManager.kt` +180 / -40
- `audio/src/commonMain/kotlin/.../domain/usb/StreamHandle.kt` **nuevo** ~40 líneas
- `audio/src/commonMain/kotlin/.../domain/usb/UsbAudioEvents.kt` +30 líneas
- `audio/src/commonMain/kotlin/.../domain/usb/UsbTransferStats.kt` +40 líneas (clock + jitter fields)
- `audio/src/androidMain/kotlin/.../internal/usb/UsbAudioManagerImpl.kt` +300 / -80 (consolidation)
- `audio/src/androidMain/kotlin/.../internal/usb/AudioFocusHandler.kt` **nuevo** ~120 líneas
- `audio/src/androidMain/kotlin/.../internal/usb/UsbCompatibilityLoader.kt` **nuevo** ~200 líneas
- `audio/src/androidMain/resources/usb_compatibility.json` **nuevo** ~800 líneas (25+ devices)
- `audio/src/androidMain/kotlin/.../internal/usb/UsbDeviceCompatibility.kt` -180 / +80 (delegates to loader)
- `audio/src/commonMain/kotlin/.../api/UsbAudioLifecycleCallback.kt` **nuevo** ~30 líneas
- `docs/usage/usb-audio-quickstart.md` **nuevo** ~500 líneas con ejemplos
- `audio/src/androidUnitTest/.../UsbAudioManagerContractTest.kt` **nuevo** ~250 líneas
- `audio/src/androidUnitTest/.../AudioFocusHandlerTest.kt` **nuevo** ~180 líneas
- `audio/src/androidUnitTest/.../UsbCompatibilityLoaderTest.kt` **nuevo** ~120 líneas

Commit messages sugeridos:
1. `feat(api): consolidate IUsbAudioManager surface (stages 1-4 integration)`
2. `feat(api): StreamHandle for active stream tracking`
3. `feat(api): currentTransferStats as StateFlow for reactive meters`
4. `feat(lifecycle): AudioFocusHandler with configurable policy`
5. `feat(compat): JSON-based compatibility allowlist with 25+ devices`
6. `feat(api): exportDeviceReport for diagnostic dumps`
7. `docs(usb): add usb-audio-quickstart guide with 5 examples`
8. `test(api): contract tests for IUsbAudioManager`

---

## 12. Siguiente etapa

Con la API consolidada, [stage_06_test_harness.md](stage_06_test_harness.md) introduce el test harness profesional — impulse-based loopback latency, tone sweep, FFT y device matrix automation.
