clclaucl# Etapa 2 — Descubrimiento completo y selección dirigida

**Estado:** IMPLEMENTADA — core mergeado, pending hardware validation.
**Dependencias:** stage 1 mergeado (necesita `configureSampleRate()` funcional y UAC version explícita).
**Duración estimada:** 4–6 días.
**Severidad de los bugs que resuelve:** 1× Mayor, 1× Mayor, 2× menores.

---

## 1. Objetivo

Convertir la información que el parser de descriptores ya extrae (pero que el resto del código reduce prematuramente) en una representación de **topología USB completa**, navegable y observable desde Kotlin, con una API de selección dirigida por preferencias explícitas en lugar del "primer match" actual.

Tres problemas concretos a resolver:

1. **Un solo formato por altsetting.** `UsbStreamingInterface` guarda `UsbAudioFormat format` — si un altsetting declara múltiples formatos Type I (ej: S24_3LE + S32), el parser pisa el anterior. Debe ser `std::vector<UsbAudioFormat> formats`.
2. **Selección "primer match".** `LibusbBackend::selectBestInterfaces()` itera y toma la primera interfaz que acepte el sample rate. Profesionalmente debe haber un `StreamPreference` con pesos (bit depth, sync type, canales, feedback presente) que calcule el mejor altsetting.
3. **Capabilities opacas en Kotlin.** `UsbAudioManagerImpl.parseBasicCapabilities()` devuelve `[44100, 48000, 96000]` y `[16, 24]` hardcoded. Debe derivarse del snapshot real del device.

Al terminar, un device Scarlett 2i2 (que expone múltiples altsettings con combinaciones de 2ch/4ch × 16/24 bit × 44.1/48/88.2/96 kHz) debe ser enumerable completo desde Kotlin, y una llamada `selectAltsetting(preference = highestBitDepth)` debe elegir la combinación correcta.

---

## 2. Archivos tocados

### Nuevos módulos C++

| Archivo | Rol |
|---|---|
| `audio/src/main/cpp/usb/UsbTopology.h` | Struct `UsbTopology` (reemplazo enriquecido de `UsbAudioDevice`) |
| `audio/src/main/cpp/usb/StreamPreference.h` | Struct de pesos y método `score(const UsbStreamingInterface&)` |
| `audio/src/main/cpp/usb/AltsettingSelector.{h,cpp}` | Algoritmo de selección con fallback |

### Modificaciones

| Archivo | Cambio |
|---|---|
| `audio/src/main/cpp/usb/UsbAudioTypes.h` | `UsbStreamingInterface::formats` (vector), typealias de compat `UsbAudioDevice = UsbTopology` |
| `audio/src/main/cpp/usb/UsbDescriptorParser.cpp` | No pisar format; push_back al vector; handle multiple format descriptors per altsetting |
| `audio/src/main/cpp/backends/LibusbBackend.cpp` | `selectBestInterfaces()` delega en `AltsettingSelector` |
| `audio/src/main/cpp/backends/LibusbBackend.h` | `setStreamPreference(StreamPreference)` |
| `audio/src/main/cpp/jni/jni_audio_bridge.cpp` | Exponer `nativeGetCapabilitySnapshot()` que serializa la topología |
| `audio/src/androidMain/.../internal/bridge/AudioNativeBridge.kt` | `external fun nativeGetCapabilitySnapshot(): ByteArray` |
| `audio/src/androidMain/.../internal/usb/UsbAudioManagerImpl.kt` | Reemplazar `parseBasicCapabilities()` por el nuevo camino |
| `audio/src/commonMain/.../domain/usb/UsbAudioTypes.kt` | Nuevos tipos `UsbCapabilitySnapshot`, `AltsettingInfo`, `StreamPreference` |
| `audio/src/commonMain/.../api/IUsbAudioManager.kt` | Nuevas funciones suspend |

---

## 3. Diseño de datos

### 3.1 `UsbTopology` (C++)

```cpp
// audio/src/main/cpp/usb/UsbTopology.h
namespace watermelon_audio::usb {

struct AltsettingDescriptor {
    int interfaceNumber = 0;
    int alternateSetting = 0;
    std::vector<UsbAudioFormat> formats;   // was scalar, now vector
    UsbEndpointDescriptor dataEndpoint;
    std::optional<UsbFeedbackEndpoint> feedbackEndpoint;
    UsbTerminalLink terminalLink;
    SyncType syncType = SyncType::NONE;    // async/adaptive/sync
    bool hasImplicitFeedback = false;
};

struct UsbTopology {
    UsbDeviceInfo deviceInfo;
    int uacVersion = 0;
    int controlInterface = 0;

    std::vector<AltsettingDescriptor> playbackAltsettings;
    std::vector<AltsettingDescriptor> captureAltsettings;

    // UAC2-specific topology
    std::vector<UsbClockSource>   clockSources;
    std::vector<UsbClockSelector> clockSelectors;
    std::vector<UsbClockMultiplier> clockMultipliers;

    // Feature units for volume/mute (both UAC versions)
    std::vector<UsbFeatureUnit> featureUnits;
    std::vector<UsbInputTerminal> inputTerminals;
    std::vector<UsbOutputTerminal> outputTerminals;

    // Helpers
    bool hasCapture() const { return !captureAltsettings.empty(); }
    bool hasPlayback() const { return !playbackAltsettings.empty(); }
    bool isFullDuplex() const { return hasCapture() && hasPlayback(); }

    // All distinct sample rates supported by any playback altsetting
    std::vector<int> playbackSampleRates() const;
    std::vector<int> playbackBitDepths() const;
    std::vector<int> captureSampleRates() const;
    std::vector<int> captureBitDepths() const;
};

// Backward compatibility — point the old name at the new type
using UsbAudioDevice = UsbTopology;

}  // namespace
```

### 3.2 `StreamPreference`

```cpp
// audio/src/main/cpp/usb/StreamPreference.h
namespace watermelon_audio::usb {

struct StreamPreference {
    // Hard constraints — failed altsettings are not even considered
    int    requiredSampleRate   = 48000;
    int    minChannels          = 2;
    bool   requireFeedback      = false;     // async devices benefit

    // Soft weights — higher score wins (0..1 per field)
    float  bitDepthWeight       = 1.0f;      // prefer 24/32 over 16
    float  channelCountWeight   = 0.5f;      // prefer more channels
    float  syncTypeWeight       = 0.8f;      // prefer async > adaptive > sync
    float  feedbackPresentWeight = 0.3f;
    float  implicitFeedbackPenalty = -0.2f;  // mild penalty (harder to manage)

    // Factory presets
    static StreamPreference defaultPro() {
        StreamPreference p;
        p.bitDepthWeight = 1.0f;
        p.syncTypeWeight = 1.0f;
        return p;
    }
    static StreamPreference lowestLatency() {
        StreamPreference p;
        p.requireFeedback = false;
        p.bitDepthWeight = 0.3f;
        p.syncTypeWeight = 0.5f;
        p.feedbackPresentWeight = 0.0f;
        return p;
    }
    static StreamPreference highestFidelity() {
        StreamPreference p;
        p.bitDepthWeight = 1.5f;
        p.channelCountWeight = 0.8f;
        return p;
    }
};

}  // namespace
```

### 3.3 `AltsettingSelector`

```cpp
// audio/src/main/cpp/usb/AltsettingSelector.h
namespace watermelon_audio::usb {

struct ScoredMatch {
    const AltsettingDescriptor* altsetting;
    const UsbAudioFormat* format;
    float score;
};

class AltsettingSelector {
public:
    // Returns nullopt if no altsetting can satisfy the hard constraints.
    // If multiple altsettings have identical top score, returns the one with
    // lowest alternateSetting number (deterministic).
    static std::optional<ScoredMatch> pickPlayback(
        const UsbTopology& topology,
        const StreamPreference& pref);

    static std::optional<ScoredMatch> pickCapture(
        const UsbTopology& topology,
        const StreamPreference& pref);

private:
    static std::vector<ScoredMatch> scoreAll(
        const std::vector<AltsettingDescriptor>& altsettings,
        const StreamPreference& pref);

    static float scoreFormat(const UsbAudioFormat& fmt,
                              const AltsettingDescriptor& alt,
                              const StreamPreference& pref);
};

}  // namespace
```

**Algoritmo.** Para cada altsetting, iterar sobre sus `formats`, descartar los que no cumplen `requiredSampleRate`/`minChannels`/`requireFeedback`, calcular score como suma ponderada y quedarse con el máximo. Desempates por `alternateSetting` bajo → prioriza stabilidad en el setup.

### 3.4 `UsbCapabilitySnapshot` (Kotlin commonMain)

```kotlin
// commonMain/.../domain/usb/UsbCapabilitySnapshot.kt
package com.watermellonstudios.audio.domain.usb

data class UsbCapabilitySnapshot(
    val vendorId: Int,
    val productId: Int,
    val productName: String?,
    val manufacturer: String?,
    val serialNumber: String?,
    val uacVersion: Int,

    val playbackAltsettings: List<AltsettingInfo>,
    val captureAltsettings: List<AltsettingInfo>,

    val clockSources: List<ClockSourceInfo>,
    val featureUnits: List<FeatureUnitInfo>,

    val effectiveOutputSampleRates: List<Int>,
    val effectiveOutputBitDepths: List<Int>,
    val effectiveInputSampleRates: List<Int>,
    val effectiveInputBitDepths: List<Int>,
) {
    val isFullDuplex: Boolean get() = playbackAltsettings.isNotEmpty() && captureAltsettings.isNotEmpty()
    val hasAsyncFeedback: Boolean get() = playbackAltsettings.any { it.hasFeedbackEndpoint }
}

data class AltsettingInfo(
    val interfaceNumber: Int,
    val alternateSetting: Int,
    val formats: List<AudioFormatInfo>,
    val syncType: UsbSyncMode,
    val hasFeedbackEndpoint: Boolean,
    val hasImplicitFeedback: Boolean,
    val dataEndpointAddress: Int,
    val terminalLinkId: Int,
)

data class AudioFormatInfo(
    val channels: Int,
    val bitResolution: Int,
    val bytesPerSample: Int,
    val sampleRates: List<Int>,
    val hasContinuousRates: Boolean,
    val minSampleRate: Int,
    val maxSampleRate: Int,
)

data class ClockSourceInfo(
    val clockId: Int,
    val type: ClockSourceType,
    val syncedToSof: Boolean,
    val hasFrequencyControl: Boolean,
    val hasValidityControl: Boolean,
)

enum class ClockSourceType { EXTERNAL, INTERNAL_FIXED, INTERNAL_VARIABLE, INTERNAL_PROGRAMMABLE, UNKNOWN }

data class FeatureUnitInfo(
    val unitId: Int,
    val sourceId: Int,
    val channelCount: Int,
    val hasMasterVolume: Boolean,
    val hasMasterMute: Boolean,
    val perChannelVolume: List<Boolean>,
    val perChannelMute: List<Boolean>,
)

data class StreamPreference(
    val preferredBitDepth: Int? = null,      // null = any
    val preferredSyncType: UsbSyncMode? = null,
    val requireFeedback: Boolean = false,
    val minChannels: Int = 2,
    val profile: Profile = Profile.DEFAULT_PRO,
) {
    enum class Profile { DEFAULT_PRO, LOWEST_LATENCY, HIGHEST_FIDELITY, CUSTOM }
}
```

---

## 4. Serialización de la snapshot (C++ ↔ Kotlin)

El JNI debe pasar el `UsbTopology` a Kotlin de forma estable, versionada y sin allocations excesivas en el camino.

**Opción elegida: serialización propia a `ByteArray` con esquema versionado.** Evita añadir protobuf como dependencia. El formato se define en un solo header C++ y en un solo `object UsbSnapshotCodec` Kotlin. Versión en el byte 0 (v1 = 0x01).

```cpp
// audio/src/main/cpp/usb/UsbSnapshotCodec.h
namespace watermelon_audio::usb {
std::vector<uint8_t> encodeSnapshot(const UsbTopology& topology);
}  // writes little-endian, length-prefixed strings, v1 format
```

```kotlin
// commonMain/.../domain/usb/UsbSnapshotCodec.kt
object UsbSnapshotCodec {
    fun decode(bytes: ByteArray): UsbCapabilitySnapshot
}
```

Formato v1:
```
[0]       = version = 0x01
[1..4]    = total length (u32 LE)
[5..6]    = vendorId (u16)
[7..8]    = productId (u16)
[9]       = uacVersion (u8)
[10..]    = length-prefixed UTF-8 product name
...       = length-prefixed UTF-8 manufacturer
...       = length-prefixed UTF-8 serial
[..]      = (u16) numPlaybackAltsettings
  per altsetting:
    (u8) interfaceNumber
    (u8) alternateSetting
    (u8) syncType (enum)
    (u8) flags (bit0 = hasFeedback, bit1 = implicitFeedback)
    (u8) dataEndpointAddress
    (u8) terminalLinkId
    (u8) numFormats
      per format:
        (u8) channels
        (u8) bitResolution
        (u8) bytesPerSample
        (u8) flags (bit0 = hasContinuous)
        (u32) minRate, (u32) maxRate
        (u8) numDiscreteRates
          per rate: (u32) rateHz
[..]      = (u16) numCaptureAltsettings  [mismo layout]
[..]      = (u8) numClockSources
  per clockSource:
    (u8) clockId, (u8) type, (u8) flags, (u8) hasFreqControl, (u8) hasValidityControl
[..]      = (u8) numFeatureUnits
  per featureUnit: (u8) unitId, (u8) sourceId, (u8) channelCount,
                    (u8) masterVolFlags, (numChannels) perChannelFlags
```

**Validación.** Decoder Kotlin verifica versión, total length, y lanza `IllegalStateException` si algo no cuadra. Test round-trip C++ ↔ Kotlin con golden snapshots para cada device del allowlist.

---

## 5. Cambios en el parser de descriptores

### 5.1 No pisar formatos

En `UsbDescriptorParser.cpp`, el handler de Type I Format Descriptor actualmente hace:
```cpp
currentInterface->format = parsedFormat;  // overwrites!
```

Debe ser:
```cpp
currentInterface->formats.push_back(parsedFormat);
```

Y el campo `format` se elimina del struct — forzando al resto del código a migrar.

### 5.2 Propagar sync type al altsetting

Hoy el parser guarda el endpoint completo (con sus `bmAttributes`) pero nadie lo consolida al nivel del altsetting. Añadir:
```cpp
currentAltsetting.syncType = decodeSyncType(endpoint.attributes);
currentAltsetting.hasImplicitFeedback = (endpoint.attributes & 0x20) != 0;
```

### 5.3 Marcar terminales y feature units

El parser ya los carga. Asegurarse que `UsbTopology::featureUnits` y `inputTerminals`/`outputTerminals` estén poblados con TODO lo parseado, no solo los "relevantes para volumen".

---

## 6. Cambios en `LibusbBackend`

### 6.1 Reemplazar `selectBestInterfaces()`

```cpp
bool LibusbBackend::selectBestInterfaces() {
    if (!mUsbDevice) return false;

    mSelectedPlayback.reset();
    mSelectedCapture.reset();

    const bool needsPlayback = /* ... */;
    const bool needsCapture  = /* ... */;

    // Build a preference based on what the user requested
    StreamPreference pref;
    pref.requiredSampleRate = mRequestedSampleRate;
    pref.minChannels = 2;  // overridable via new API
    // Apply any user-provided preference from setStreamPreference()
    if (mUserPreference) pref = *mUserPreference;

    if (needsPlayback) {
        auto match = AltsettingSelector::pickPlayback(*mUsbDevice, pref);
        if (!match) {
            LOGE("No playback altsetting matches preference (rate=%d, ch=%d)",
                 pref.requiredSampleRate, pref.minChannels);
            return false;
        }
        mSelectedPlayback = *match->altsetting;
        mSelectedPlaybackFormat = *match->format;
        LOGI("Selected playback IF%d Alt%d [%dch/%dbit] score=%.3f",
             match->altsetting->interfaceNumber,
             match->altsetting->alternateSetting,
             match->format->channels,
             match->format->bitResolution,
             match->score);
    }

    if (needsCapture) {
        auto match = AltsettingSelector::pickCapture(*mUsbDevice, pref);
        if (!match) { /* same handling */ }
        mSelectedCapture = *match->altsetting;
        mSelectedCaptureFormat = *match->format;
    }

    return (needsPlayback == mSelectedPlayback.has_value())
        && (needsCapture == mSelectedCapture.has_value());
}
```

Nota: los campos `mSelectedPlaybackFormat` / `mSelectedCaptureFormat` son nuevos porque el altsetting ahora tiene un vector de formatos; la elección puntual se preserva.

### 6.2 API para setear preferencia

```cpp
// LibusbBackend.h
void setStreamPreference(const usb::StreamPreference& pref) {
    mUserPreference = pref;
}
// LibusbBackend.cpp
std::optional<usb::StreamPreference> mUserPreference;
```

### 6.3 `getCapabilities()` ahora navega `UsbTopology`

El método existe pero construye `DeviceCapabilities` iterando `playbackInterfaces` con `format` escalar. Reescribir para iterar `playbackAltsettings[i].formats`.

---

## 7. Cambios en JNI y Kotlin

### 7.1 Nuevo binding JNI

```cpp
// audio/src/main/cpp/jni/jni_audio_bridge.cpp
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetUsbCapabilitySnapshot(
    JNIEnv* env, jobject thiz) {
    auto* backend = BackendManager::getInstance().getLibusbBackend();
    if (!backend || !backend->isUsbDeviceReady()) return nullptr;

    const auto* topology = backend->getUsbAudioDevice();
    if (!topology) return nullptr;

    auto encoded = usb::encodeSnapshot(*topology);
    jbyteArray result = env->NewByteArray(static_cast<jsize>(encoded.size()));
    env->SetByteArrayRegion(result, 0, static_cast<jsize>(encoded.size()),
                             reinterpret_cast<const jbyte*>(encoded.data()));
    return result;
}
```

### 7.2 Bridge Kotlin

```kotlin
// androidMain/.../internal/bridge/AudioNativeBridge.kt
external fun nativeGetUsbCapabilitySnapshot(): ByteArray?
```

### 7.3 Nuevo camino en `UsbAudioManagerImpl`

Reemplazar `parseBasicCapabilities()` por:

```kotlin
private fun parseCapabilities(device: UsbDevice): UsbAudioCapabilities {
    val raw = nativeBridge.nativeGetUsbCapabilitySnapshot() ?: return fallbackCaps(device)
    val snapshot = runCatching { UsbSnapshotCodec.decode(raw) }
        .getOrElse { e -> Log.e(TAG, "Snapshot decode failed", e); return fallbackCaps(device) }

    return UsbAudioCapabilities(
        sampleRates = snapshot.effectiveOutputSampleRates,
        bitDepths = snapshot.effectiveOutputBitDepths,
        maxOutputChannels = snapshot.playbackAltsettings.maxOfOrNull { alt ->
            alt.formats.maxOfOrNull { it.channels } ?: 0
        } ?: 0,
        maxInputChannels = snapshot.captureAltsettings.maxOfOrNull { alt ->
            alt.formats.maxOfOrNull { it.channels } ?: 0
        } ?: 0,
        syncMode = snapshot.playbackAltsettings.firstOrNull()?.syncType ?: UsbSyncMode.UNKNOWN,
        uacVersion = snapshot.uacVersion,
        isFullDuplex = snapshot.isFullDuplex,
    )
}
```

Guardar el snapshot completo en `_currentSnapshot: MutableStateFlow<UsbCapabilitySnapshot?>` expuesto por el manager.

### 7.4 Nuevas entradas en `IUsbAudioManager`

```kotlin
// commonMain/.../api/IUsbAudioManager.kt
interface IUsbAudioManager {
    // ... existing surface

    /** Latest capability snapshot for the currently selected device, or null. */
    val currentCapabilitySnapshot: StateFlow<UsbCapabilitySnapshot?>

    /**
     * Enumerate all playback altsettings with their scored match against a preference.
     * Useful for UI: "Let the user pick one".
     */
    fun rankPlaybackAltsettings(preference: StreamPreference): List<ScoredAltsetting>

    /**
     * Programmatically select a specific altsetting before calling startStreaming().
     * Overrides the preference-based selection.
     */
    suspend fun selectAltsetting(
        interfaceNumber: Int,
        alternateSetting: Int,
        formatIndex: Int,
    ): Result<Unit>

    /** Set the preference used by the default selector. */
    fun setStreamPreference(preference: StreamPreference)
}

data class ScoredAltsetting(
    val altsetting: AltsettingInfo,
    val format: AudioFormatInfo,
    val score: Double,
    val recommendation: String,
)
```

El `rankPlaybackAltsettings` corre el scoring en el Kotlin (más conveniente para iterar) y expone un ranking observable para la UI. El `selectAltsetting` persiste la elección y la aplica al siguiente `startStreaming`.

---

## 8. Tests

### 8.1 Unit tests de scoring

```cpp
// audio/src/test/cpp/usb/AltsettingSelector_test.cpp

TEST(AltsettingSelector, PrefersHigherBitDepthWhenWeightSet) {
    UsbTopology topology = /* two altsettings: Alt1 S16 2ch, Alt2 S24 2ch */;
    StreamPreference pref = StreamPreference::defaultPro();
    pref.requiredSampleRate = 48000;
    auto match = AltsettingSelector::pickPlayback(topology, pref);
    ASSERT_TRUE(match);
    EXPECT_EQ(match->format->bitResolution, 24);
    EXPECT_EQ(match->altsetting->alternateSetting, /* whatever corresponds to 24 */);
}

TEST(AltsettingSelector, FailsWhenRequireFeedbackNotMet) {
    UsbTopology topology = /* only adaptive altsettings */;
    StreamPreference pref = StreamPreference::defaultPro();
    pref.requireFeedback = true;
    auto match = AltsettingSelector::pickPlayback(topology, pref);
    EXPECT_FALSE(match);
}

TEST(AltsettingSelector, DeterministicOnTie) {
    UsbTopology topology = /* two identical altsettings */;
    auto m1 = AltsettingSelector::pickPlayback(topology, StreamPreference::defaultPro());
    auto m2 = AltsettingSelector::pickPlayback(topology, StreamPreference::defaultPro());
    EXPECT_EQ(m1->altsetting->alternateSetting, m2->altsetting->alternateSetting);
}
```

### 8.2 Test de serialización

```cpp
TEST(UsbSnapshotCodec, RoundTripScarlettSolo3rdGen) {
    UsbTopology topology = loadGoldenSnapshot("scarlett_solo_3rd_gen");
    auto encoded = usb::encodeSnapshot(topology);

    // Decode in C++ for sanity
    UsbTopology decoded = usb::decodeSnapshot(encoded);
    EXPECT_EQ(decoded.playbackAltsettings.size(), topology.playbackAltsettings.size());
    EXPECT_EQ(decoded.uacVersion, 2);

    // Write encoded bytes to a file for the Kotlin test to pick up
    writeGoldenBytes("scarlett_solo_encoded.bin", encoded);
}
```

```kotlin
// androidUnitTest/.../UsbSnapshotCodecTest.kt
@Test
fun decodesScarlettSoloGoldenFixture() {
    val bytes = javaClass.classLoader!!
        .getResourceAsStream("fixtures/scarlett_solo_encoded.bin")!!
        .readBytes()
    val snapshot = UsbSnapshotCodec.decode(bytes)
    assertEquals(2, snapshot.uacVersion)
    assertTrue(snapshot.isFullDuplex)
    assertTrue(snapshot.effectiveOutputSampleRates.contains(48000))
    assertTrue(snapshot.effectiveOutputSampleRates.contains(96000))
}
```

Los fixtures se generan una vez desde un device real y se chequean al repo en `audio/src/test/resources/fixtures/*.bin`.

### 8.3 Integration test en runner

Añadir preset `DISCOVERY_WALK`: abre el device, captura el snapshot, itera sobre cada altsetting y para cada uno hace un streaming de 500 ms, validando que al menos el 80% de los altsettings completan transferencias sin errores. Reporte final lista qué altsettings son efectivamente utilizables.

---

## 9. Criterios de aceptación

- [ ] `UsbStreamingInterface::format` eliminado. Todo el código usa `formats` (vector). Ninguna compilación falla con TODOs pendientes.
- [ ] `AltsettingSelector` implementado con tests unitarios pasando para: preferencia de bit depth, preferencia de sync type, restricción de canales, fallback cuando no hay match, determinismo en empates.
- [ ] `encodeSnapshot`/`decodeSnapshot` con tests de round-trip en al menos 2 devices (UAC1 + UAC2). Golden fixtures comiteados.
- [ ] `nativeGetUsbCapabilitySnapshot()` expuesto en JNI y consumido desde `UsbAudioManagerImpl`.
- [ ] `UsbAudioManagerImpl.parseBasicCapabilities()` eliminado (o reducido a fallback puro cuando el nativo no devuelve snapshot).
- [ ] `currentCapabilitySnapshot: StateFlow<UsbCapabilitySnapshot?>` expuesto por `IUsbAudioManager`.
- [ ] `rankPlaybackAltsettings(preference)` devuelve una lista ordenada correctamente en Scarlett Solo (UAC2) y C-Media UC02 (UAC1).
- [ ] `selectAltsetting(if, alt, formatIndex)` permite override manual y se aplica al siguiente `startStreaming`.
- [ ] En Scarlett Solo (UAC2, que expone ≥ 4 altsettings), el `DISCOVERY_WALK` completa sin errores.
- [ ] `UsbAudioDevice` sigue compilando como typealias (no se rompe ABI externa).

---

## 10. Riesgos específicos

1. **Descriptor parsing legacy.** El parser actual puede estar asumiendo que un altsetting tiene un solo format en otros lugares. Hay que barrer con grep (`iface.format.` → `iface.formats[0].`) antes de borrar el campo escalar.
2. **Fixtures binarios.** Checkear fixtures binarios al repo puede ser polémico (tamaño, diffs ruidosos). Mitigación: mantenerlos pequeños (~2 KB cada uno), organizados en un subdirectorio `fixtures/` con un README que explica cómo regenerarlos.
3. **Cambio de ABI silencioso.** Consumidores de `IUsbAudioManager` (NoisyPad) pueden romper si llaman métodos eliminados. Mitigación: marcar `parseBasicCapabilities` como `@Deprecated` durante esta etapa y remover en una posterior.
4. **Scoring inestable.** Pesos mal elegidos pueden hacer que dos devices idénticos den rankings distintos. Mitigación: tests de regresión con golden scores.

---

## 11. Checklist de commit

Diff aproximado:

- `audio/src/main/cpp/usb/UsbTopology.h` **nuevo** ~160 líneas
- `audio/src/main/cpp/usb/StreamPreference.h` **nuevo** ~70 líneas
- `audio/src/main/cpp/usb/AltsettingSelector.{h,cpp}` **nuevo** ~220 líneas
- `audio/src/main/cpp/usb/UsbSnapshotCodec.{h,cpp}` **nuevo** ~350 líneas
- `audio/src/main/cpp/usb/UsbAudioTypes.h` modificación ~40 líneas
- `audio/src/main/cpp/usb/UsbDescriptorParser.cpp` modificación ~80 líneas
- `audio/src/main/cpp/backends/LibusbBackend.{h,cpp}` modificación ~100 líneas
- `audio/src/main/cpp/jni/jni_audio_bridge.cpp` +40 líneas
- `audio/src/main/cpp/CMakeLists.txt` +3 líneas
- `audio/src/commonMain/kotlin/.../domain/usb/UsbCapabilitySnapshot.kt` **nuevo** ~140 líneas
- `audio/src/commonMain/kotlin/.../domain/usb/UsbSnapshotCodec.kt` **nuevo** ~180 líneas
- `audio/src/commonMain/kotlin/.../api/IUsbAudioManager.kt` +40 líneas
- `audio/src/androidMain/kotlin/.../internal/usb/UsbAudioManagerImpl.kt` -60 / +80
- `audio/src/androidMain/kotlin/.../internal/bridge/AudioNativeBridge.kt` +5 líneas
- `audio/src/test/cpp/usb/AltsettingSelector_test.cpp` **nuevo** ~200 líneas
- `audio/src/test/cpp/usb/UsbSnapshotCodec_test.cpp` **nuevo** ~150 líneas
- `audio/src/test/resources/fixtures/*.bin` **nuevo** ~2 KB × 3 devices

Commit messages sugeridos:
1. `refactor(usb): introduce UsbTopology with multi-format altsettings`
2. `feat(usb): add StreamPreference and AltsettingSelector`
3. `feat(usb): serialize capability snapshot to versioned ByteArray`
4. `feat(usb): expose capability snapshot and altsetting selection to Kotlin`
5. `refactor(usb): drop parseBasicCapabilities, use native snapshot`
6. `test(usb): add altsetting selector and snapshot codec tests`

---

## 12. Siguiente etapa

Con el descubrimiento robusto y la API de selección en su lugar, [stage_03_clock_sync.md](stage_03_clock_sync.md) completa el clock graph UAC 2.0 (clock sources, selectors, multiplicadores) y el feedback endpoint end-to-end.
