 # Etapa 7 — Android 14 bit-perfect y fast path AAudio MMAP

**Estado:** propuesta — no iniciada.
**Dependencias:** stages 1–5 mergeados. Stage 6 no es estrictamente bloqueante pero se recomienda tenerlo para validar.
**Duración estimada:** 3–4 días.
**Severidad:** oportunidad de mercado + mejora significativa de latencia en devices modernos.

---

## 1. Objetivo

Aprovechar las APIs nuevas de Android 14+ ([AudioMixerAttributes.BIT_PERFECT](https://source.android.com/docs/core/audio/preferred-mixer-attr), [AAudio MMAP exclusivo](https://developer.android.com/ndk/guides/audio/audio-latency)) para ofrecer un **camino más rápido y más estable que libusb** cuando el sistema operativo y el device lo soportan. El backend libusb se mantiene como fallback universal y como única opción para devices que requieren control fino (clock source selection, async feedback UAC1 correcto, devices no soportados por el kernel nativo).

**Resultado esperado.** En Pixel 8+ con kernel moderno y un Scarlett Solo 3rd Gen conectado, la librería debe ofrecer un round-trip ≤ 5 ms, usando AAudio MMAP exclusivo con preferred mixer attributes bit-perfect, sin que el consumer tenga que hacer nada distinto más allá de habilitar una flag.

---

## 2. Diseño de política de selección

### 2.1 Nuevo `BackendType::OBOE_USB`

El `OboeBackend` actual usa AAudio pero sin ningún control específico para USB audio. Se introduce una variante dedicada que:

1. Detecta que el device activo es un USB audio device.
2. Consulta `AudioManager.getSupportedMixerAttributes(deviceInfo)` si SDK ≥ 34.
3. Si alguna attribute incluye `AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT`, la setea via `AudioManager.setPreferredMixerAttributes()`.
4. Abre un `oboe::AudioStream` con:
   - `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY`
   - `AAUDIO_SHARING_MODE_EXCLUSIVE`
   - `setDeviceId(usbDeviceId)` para routear al USB device
5. Valida el stream (`getStreamFormat`, `getSampleRate`, etc.).

```cpp
// audio/src/main/cpp/backends/OboeUsbBackend.h
namespace watermelon_audio {

class OboeUsbBackend : public OboeBackend {
public:
    OboeUsbBackend();
    ~OboeUsbBackend() override;

    BackendResult start() override;
    BackendType getType() const override { return BackendType::OBOE_USB; }

    // Configure the USB device to route to (ID comes from AudioDeviceInfo)
    void setTargetUsbDeviceId(int deviceId);
    void setBitPerfectRequested(bool enabled);

private:
    int  mTargetUsbDeviceId = 0;
    bool mBitPerfectRequested = false;
    bool mBitPerfectApplied = false;
};

}  // namespace
```

### 2.2 `BackendSelectionPolicy`

Centralizar la decisión de qué backend usar:

```kotlin
// commonMain/.../api/BackendSelectionPolicy.kt
sealed class BackendSelectionPolicy {
    /** Always use libusb — full control, pro use case */
    object LibUsbAlways : BackendSelectionPolicy()

    /** Always use Oboe — maximum compatibility, minimal control */
    object OboeAlways : BackendSelectionPolicy()

    /** Auto: prefer Oboe/AAudio bit-perfect on Android 14+ if available,
     *  fall back to libusb for advanced control or unsupported devices. */
    data class Auto(
        val preferBitPerfect: Boolean = true,
        val preferLibUsbFor: Set<LibUsbRequirement> = emptySet(),
    ) : BackendSelectionPolicy()

    enum class LibUsbRequirement {
        CLOCK_SOURCE_SELECTION,  // user needs to pick a clock source
        UAC1_ASYNC_FEEDBACK,      // device is UAC1 async — libusb handles better
        PER_CHANNEL_VOLUME,        // native volume granularity needed
        HOT_PLUG_RESILIENCE,        // we want our own recovery logic
        CUSTOM_CHANNEL_ROUTING,   // stage 4 channel routing required
    }
}
```

### 2.3 Decisión en runtime

```kotlin
// androidMain/.../internal/usb/BackendSelector.kt
internal class BackendSelector(
    private val context: Context,
    private val policy: BackendSelectionPolicy,
) {
    fun choose(snapshot: UsbCapabilitySnapshot, streamConfig: UsbStreamConfig): AudioBackendType {
        return when (policy) {
            BackendSelectionPolicy.LibUsbAlways -> AudioBackendType.LIBUSB
            BackendSelectionPolicy.OboeAlways -> AudioBackendType.OBOE
            is BackendSelectionPolicy.Auto -> decideAuto(snapshot, streamConfig, policy)
        }
    }

    private fun decideAuto(
        snapshot: UsbCapabilitySnapshot,
        config: UsbStreamConfig,
        policy: BackendSelectionPolicy.Auto,
    ): AudioBackendType {
        // Hard requirements for libusb
        val needsLibUsb = policy.preferLibUsbFor.any { requirement ->
            when (requirement) {
                CLOCK_SOURCE_SELECTION -> snapshot.clockSources.size > 1
                UAC1_ASYNC_FEEDBACK -> snapshot.uacVersion == 1 && snapshot.hasAsyncFeedback
                PER_CHANNEL_VOLUME -> snapshot.featureUnits.any { it.channelCount > 1 }
                HOT_PLUG_RESILIENCE -> true  // lib handles this well
                CUSTOM_CHANNEL_ROUTING -> config.channelRouting != ChannelRouting.IdentityStereo
            }
        }
        if (needsLibUsb) return AudioBackendType.LIBUSB

        // Prefer Oboe path if Android 14+ AND bit-perfect available
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE &&
            policy.preferBitPerfect &&
            isBitPerfectAvailable(snapshot, config)) {
            return AudioBackendType.OBOE  // will route through OboeUsbBackend
        }

        // Fallback: libusb for pro control, Oboe for compatibility
        return if (snapshot.uacVersion == 2) AudioBackendType.LIBUSB
        else AudioBackendType.OBOE
    }

    private fun isBitPerfectAvailable(
        snapshot: UsbCapabilitySnapshot,
        config: UsbStreamConfig,
    ): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.UPSIDE_DOWN_CAKE) return false
        val am = context.getSystemService(AudioManager::class.java)
        val devices = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
        val usbDevice = devices.firstOrNull {
            it.type == AudioDeviceInfo.TYPE_USB_DEVICE ||
                    it.type == AudioDeviceInfo.TYPE_USB_HEADSET ||
                    it.type == AudioDeviceInfo.TYPE_USB_ACCESSORY
        } ?: return false

        val supportedAttrs = am.getSupportedMixerAttributes(usbDevice)
        return supportedAttrs.any { attr ->
            attr.mixerBehavior == AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT &&
                    attr.format.sampleRate == config.sampleRate &&
                    attr.format.encoding.matchesBitDepth(config.bitDepth)
        }
    }
}
```

### 2.4 API exposure

```kotlin
// IUsbAudioManager.kt
fun setBackendSelectionPolicy(policy: BackendSelectionPolicy)
fun getActiveBackendType(): AudioBackendType  // during streaming
```

---

## 3. Implementación de `OboeUsbBackend::start()`

```cpp
BackendResult OboeUsbBackend::start() {
    std::lock_guard<std::mutex> lock(mStreamMutex);

    if (mIsRunning.load()) return BackendResult::ERROR_ALREADY_RUNNING;

    // 1. Apply preferred mixer attributes if requested and available (Android 14+)
    //    This is done in Kotlin side before native start is called — the flags
    //    just tell the native code what to expect.

    // 2. Build the stream with exclusive + low-latency + target USB device
    oboe::AudioStreamBuilder builder;
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setSharingMode(oboe::SharingMode::Exclusive);
    builder.setDirection(oboe::Direction::Output);
    builder.setFormat(oboe::AudioFormat::Float);  // internal format, Oboe converts
    builder.setSampleRate(mRequestedSampleRate);
    builder.setChannelCount(2);
    builder.setCallback(this);

    if (mTargetUsbDeviceId != 0) {
        builder.setDeviceId(mTargetUsbDeviceId);
    }

    oboe::Result result = builder.openStream(mOutputStream);
    if (result != oboe::Result::OK) {
        LOGE("OboeUsbBackend: failed to open exclusive stream: %s",
             oboe::convertToText(result));
        // Fallback: try non-exclusive
        builder.setSharingMode(oboe::SharingMode::Shared);
        result = builder.openStream(mOutputStream);
        if (result != oboe::Result::OK) {
            return BackendResult::ERROR_STREAM_FAILED;
        }
        LOGW("OboeUsbBackend: fell back to shared mode");
    }

    // 3. Verify the stream is actually what we wanted
    const auto sharingMode = mOutputStream->getSharingMode();
    const auto perfMode    = mOutputStream->getPerformanceMode();
    const int actualSampleRate = mOutputStream->getSampleRate();
    const int bufferFrames = mOutputStream->getFramesPerBurst();

    LOGI("OboeUsbBackend open: sharing=%d perf=%d rate=%d burst=%d bitPerfect=%d",
         static_cast<int>(sharingMode), static_cast<int>(perfMode),
         actualSampleRate, bufferFrames, mBitPerfectApplied);

    // 4. If full-duplex requested, open input stream too (stages 1-4 unchanged)
    if (mFullDuplexEnabled) { /* ... */ }

    result = mOutputStream->requestStart();
    if (result != oboe::Result::OK) {
        mOutputStream->close();
        return BackendResult::ERROR_STREAM_FAILED;
    }

    mIsRunning.store(true);
    return BackendResult::OK;
}
```

---

## 4. Preferred mixer attributes (Android 14+)

### 4.1 Requesting bit-perfect

En Kotlin, **antes** de llamar `nativeStartOboeUsbStream`:

```kotlin
// internal/usb/BitPerfectConfigurator.kt (androidMain, SDK >= 34)
@RequiresApi(Build.VERSION_CODES.UPSIDE_DOWN_CAKE)
internal class BitPerfectConfigurator(private val context: Context) {
    fun requestBitPerfect(
        usbDevice: AudioDeviceInfo,
        sampleRate: Int,
        bitDepth: Int,
        channels: Int,
    ): Boolean {
        val am = context.getSystemService(AudioManager::class.java)
        val supported = am.getSupportedMixerAttributes(usbDevice)
        val desired = supported.firstOrNull {
            it.mixerBehavior == AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT &&
                    it.format.sampleRate == sampleRate &&
                    it.format.channelMask.channelCountMatches(channels) &&
                    it.format.encoding.matchesBitDepth(bitDepth)
        } ?: return false

        // As of Android 14, this requires MODIFY_AUDIO_SETTINGS permission and,
        // for some builds, SYSTEM_ALERT_WINDOW. The library requests MODIFY_AUDIO_SETTINGS.
        val attrs = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
            .build()

        return am.setPreferredMixerAttributes(attrs, usbDevice, desired)
    }

    fun clearBitPerfect(usbDevice: AudioDeviceInfo) {
        val am = context.getSystemService(AudioManager::class.java)
        val attrs = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
            .build()
        am.clearPreferredMixerAttributes(attrs, usbDevice)
    }
}
```

### 4.2 Manifest permission

Agregar en `audio/src/androidMain/AndroidManifest.xml`:

```xml
<uses-permission android:name="android.permission.MODIFY_AUDIO_SETTINGS" />
```

Documentar en los release notes que el consumer debe tener esta permission declarada en su propio manifest merge.

### 4.3 Validation

Tras arrancar el stream, verificar que el format efectivo coincide con lo pedido. Si el device o el kernel degradaron silenciosamente a no-bit-perfect, loggear warning y emitir evento:

```kotlin
sealed class UsbHealthEvent {
    // ...
    data class BitPerfectUnavailable(val requested: AudioFormatSpec, val actual: AudioFormatSpec) : UsbHealthEvent()
}
```

---

## 5. Fallback robusto

La secuencia al arrancar un stream en modo `Auto` con `preferBitPerfect`:

```
1. Kotlin: BackendSelector.choose() → returns OBOE
2. Kotlin: BitPerfectConfigurator.requestBitPerfect() → true/false
3. Kotlin: nativeBridge.initializeOboeUsbBackend(deviceId, bitPerfect=true)
4. Native: OboeUsbBackend::start()
5. Native: verify actual stream format
6. If 2-5 fail at any point:
     - Log reason
     - Emit HealthEvent
     - Fall through to libusb path (transparent to the user)
7. If libusb also fails, fall through to regular Oboe (shared mode, mic/speaker)
```

La transición debe ser transparente — el consumer ve un stream funcionando, solo el `getActiveBackendType()` y los logs revelan qué camino se tomó.

---

## 6. Consideraciones específicas

### 6.1 Permisos

- `MODIFY_AUDIO_SETTINGS` — requerido para `setPreferredMixerAttributes`. Ya presente en muchas apps.
- Runtime permission request para USB — ya existe (stages previos no tocan esto).
- `RECORD_AUDIO` — solo si se usa full-duplex via OboeUsbBackend con input del USB. Ya existe.

### 6.2 OEM differences

Android 14 es parcialmente implementado por OEMs. Samsung y Xiaomi pueden tener bugs específicos. Testear en al menos:
- Pixel 8 Pro (referencia AOSP)
- Samsung Galaxy S24
- Xiaomi 14

Si hay bugs OEM-específicos, mantener blacklist en `usb_compatibility.json`:

```json
{
  "oemBlacklist": {
    "bitPerfect": [
      { "manufacturer": "samsung", "model": "SM-S928B", "reason": "Returns success but outputs garbage" }
    ]
  }
}
```

### 6.3 Latencia esperada

Según las mediciones típicas de 2025–2026 en devices modernos:

| Backend | Pixel 6 | Pixel 8 Pro | Samsung S24 |
|---|---|---|---|
| OboeUsb (shared) | ~18 ms | ~15 ms | ~22 ms |
| OboeUsb (exclusive) | ~12 ms | ~8 ms | ~16 ms |
| OboeUsb (bit-perfect) | — | ~5 ms | ~7 ms |
| Libusb (stages 1-5 done) | ~10 ms | ~10 ms | ~12 ms |

Los números son órdenes de magnitud; tests empíricos en la etapa pueden variar.

---

## 7. Tests

### 7.1 `BackendSelector_test.kt`

- `LibUsbAlways` → siempre LIBUSB
- `OboeAlways` → siempre OBOE
- `Auto` con `preferBitPerfect=true` en SDK < 34 → LIBUSB o OBOE según topología
- `Auto` con `CLOCK_SOURCE_SELECTION` en `preferLibUsbFor` y snapshot con 2 clock sources → LIBUSB
- `Auto` con UAC1 async → LIBUSB
- `Auto` con UAC2 simple stereo en Pixel 8 → OBOE (si mock confirma bit-perfect disponible)

### 7.2 `BitPerfectConfigurator_test.kt` (requiere SDK 34+)

Tests instrumentados en emulator Android 14:
- `requestBitPerfect` en un device no soportado → false
- En un device mockeado soportado → true, y después `clearBitPerfect` devuelve true

### 7.3 Integration — `OboeUsbStreamTest`

Correr un stream completo en un device Pixel 8+ real con Scarlett Solo:
- Verificar `getActiveBackendType() == OBOE_USB`
- Medir latencia con el `LoopbackLatencyMeter` de stage 6
- Comparar con la latencia de `LIBUSB` en el mismo setup
- Ambas deben ser < 15 ms

---

## 8. Criterios de aceptación

- [ ] `OboeUsbBackend` implementado como subclase/variante de `OboeBackend`, con `setDeviceId`, exclusive mode, low-latency performance mode.
- [ ] `BackendSelectionPolicy` definido en commonMain con los 3 casos.
- [ ] `BackendSelector` implementado en androidMain con la lógica `Auto`.
- [ ] `BitPerfectConfigurator` implementado y gated por SDK 34+.
- [ ] `AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT` solicitado correctamente cuando aplica.
- [ ] Fallback automático libusb→OboeUsb→Oboe sin intervención del consumer.
- [ ] `UsbHealthEvent.BitPerfectUnavailable` emitido cuando se solicita y no se obtiene.
- [ ] Tests de `BackendSelector` cubriendo los escenarios principales.
- [ ] Test manual en Pixel 8+: streaming bit-perfect funcional, latencia medida < 10 ms.
- [ ] Test manual en Pixel 6 (sin bit-perfect): fallback transparente a libusb.
- [ ] `AndroidManifest.xml` de la librería declara `MODIFY_AUDIO_SETTINGS`.
- [ ] Release notes documentan la nueva política y los permisos necesarios.

---

## 9. Riesgos específicos

1. **Bit-perfect APIs no implementadas uniformemente.** Algunos OEMs retornan success pero el stream no es realmente bit-perfect. Mitigación: validación post-start con format check y potencial comparación frame-accurate con el test harness de stage 6.
2. **Permiso `MODIFY_AUDIO_SETTINGS` no-grantable runtime.** Es un install-time permission; el consumer debe declararlo en su manifest. Si no lo hace, `setPreferredMixerAttributes` falla silencioso. Documentar claramente.
3. **Exclusive mode no siempre disponible.** Android puede rechazar exclusive sharing si otro app lo tiene. Fallback a shared documentado.
4. **Política `Auto` no-obvia para el consumer.** Si el consumer no entiende cuándo se usa libusb vs Oboe, debugging es difícil. Mitigación: `getActiveBackendType()` + logging verboso con razón de la decisión.
5. **Regressions en NoisyPad.** La política default debe ser conservadora (`Auto` con `preferBitPerfect=true` pero con `preferLibUsbFor` poblado inteligentemente) para no cambiar el comportamiento actual en el primer release. Stage out gradual.

---

## 10. Checklist de commit

Diff aproximado:

- `audio/src/main/cpp/backends/OboeUsbBackend.{h,cpp}` **nuevo** ~350 líneas
- `audio/src/main/cpp/backends/IAudioBackend.h` +1 línea (`OBOE_USB` enum)
- `audio/src/main/cpp/backends/BackendManager.{h,cpp}` +50 líneas (create OboeUsbBackend)
- `audio/src/main/cpp/jni/jni_audio_bridge.cpp` +80 líneas (new init functions)
- `audio/src/commonMain/kotlin/.../api/BackendSelectionPolicy.kt` **nuevo** ~120 líneas
- `audio/src/commonMain/kotlin/.../domain/usb/UsbAudioTypes.kt` +10 líneas (`OBOE_USB` in enum)
- `audio/src/commonMain/kotlin/.../domain/usb/UsbAudioEvents.kt` +10 líneas (`BitPerfectUnavailable`)
- `audio/src/commonMain/kotlin/.../api/IUsbAudioManager.kt` +20 líneas
- `audio/src/androidMain/kotlin/.../internal/usb/BackendSelector.kt` **nuevo** ~180 líneas
- `audio/src/androidMain/kotlin/.../internal/usb/BitPerfectConfigurator.kt` **nuevo** ~120 líneas
- `audio/src/androidMain/kotlin/.../internal/usb/UsbAudioManagerImpl.kt` +150 líneas (policy integration)
- `audio/src/androidMain/AndroidManifest.xml` +1 línea (permission)
- `audio/src/androidUnitTest/.../BackendSelectorTest.kt` **nuevo** ~200 líneas
- `audio/src/androidUnitTest/.../BitPerfectConfiguratorTest.kt` **nuevo** ~120 líneas (requires SDK 34+)
- `docs/usb-audio/bit-perfect-guide.md` **nuevo** ~200 líneas (developer-facing)

Commit messages sugeridos:
1. `feat(backends): OboeUsbBackend with exclusive/low-latency mode`
2. `feat(api): BackendSelectionPolicy for Oboe vs LibUsb choice`
3. `feat(usb): BackendSelector with Auto mode heuristics`
4. `feat(usb): BitPerfectConfigurator for Android 14+ mixer attributes`
5. `feat(usb): transparent fallback OboeUsb -> LibUsb -> Oboe`
6. `feat(api): BitPerfectUnavailable health event`
7. `docs(usb): bit-perfect guide and permission requirements`

---

## 11. Cierre del roadmap

Con esta etapa mergeada, el roadmap está completo. La librería ofrece:

- **Baseline profesional** (stages 1–3): sample rate negotiation, clock source selection, feedback UAC1/UAC2, descubrimiento completo.
- **Composición y control** (stage 4): SplitBackend, channel routing, per-channel volume, recovery robusto.
- **API pulida** (stage 5): observabilidad reactiva, audio focus, allowlist expandida, diagnostic dump.
- **Validación industrial** (stage 6): test harness con impulse latency, FFT, device matrix.
- **Ventaja competitiva** (stage 7): bit-perfect AAudio MMAP en Android 14+ con fallback automático.

El compromiso de "≤ 10 ms round-trip en Scarlett Solo + Pixel 6 o superior" es alcanzable tras stage 5, y "≤ 5 ms en Pixel 8+" tras stage 7.

**Total estimado del roadmap**: 25–37 días de trabajo concentrado, con entregas intermedias cada 3–7 días, todas mergeables independientemente.

---

## 12. Próximos pasos fuera del roadmap actual

Cuando este roadmap esté cerrado, temas que valdría la pena considerar:

- **DSD support** (DSD64/128/256) en devices compatibles via DoP (DSD over PCM).
- **Multi-device routing** (dos USB devices simultáneos) con clock master selection.
- **SIMD en `AudioFormatConverter`** (ARM NEON) para reducir CPU 2-4×.
- **Cross-platform libusb path** (iOS via IOKit, Linux desktop via libusb nativo) — el diseño actual está preparado pero no validado.
- **Remote config de la allowlist** con actualización server-side sin app update.
- **Power profile modes** — "ultra-low-latency" vs "battery saver" con tradeoffs explícitos.

Estos son items de producto, no de audit; su prioridad depende del roadmap comercial de Watermelon Studios y del feedback de NoisyPad.
