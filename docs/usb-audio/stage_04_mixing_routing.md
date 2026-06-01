# Etapa 4 — Routing, mezcla de backends y control fino

**Estado:** implementacion nativa principal lista; validacion hardware/NoisyPad pendiente. Ver [stage_04_audit_and_setup.md](stage_04_audit_and_setup.md) para la auditoria inicial.
**Dependencias:** stages 1, 2, 3 mergeados. Necesita clock sync estable porque el `SplitBackend` confía en drift controlado.
**Duración estimada:** 5–7 días.
**Severidad de los bugs que resuelve:** 1× Mayor + múltiples menores.

---

## 1. Objetivo

Habilitar escenarios profesionales de composición de streams que hoy no son posibles, y resolver el último riesgo crítico de estabilidad — el resize del ring buffer durante streaming.

Capacidades nuevas que agrega esta etapa:

1. **`SplitBackend`** — permite combinar entrada de un backend con salida de otro. El caso de uso explícito pedido: **input desde Oboe (mic del device) + output por USB** (o viceversa).
2. **Resize de ring buffer atomic** — esquema de double-buffer con swap que elimina la race condition actual (`UsbTransferManager.cpp:951-1000`).
3. **Per-channel volume** — `UsbVolumeControl` solo expone master (canal 0). Se extiende a per-channel para devices multicanal.
4. **Routing matrix para multi-canal** — downmix 4→2, selector de canal de captura, channel map explícito.
5. **Recovery automático en hot errors** — más allá del watchdog actual, auto-retry del stream ante errores no-fatales.

**Orden auditado recomendado:** empezar por resize seguro del ring buffer, luego `ChannelMap`, per-channel volume, `RecoveryPolicy`, contrato input-source, `SplitBackend`, y recien al final API/JNI. `SplitBackend` no debe ser el primer cambio porque el codigo actual de Oboe todavia no entrega input real al callback.

### Progreso de implementacion

- Slices 1-5 completados: resize seguro de ring buffer, `ChannelMap`, caps/control hardware por canal, `RecoveryPolicy`, y contrato explicito input-source/output-sink.
- Slice 6 completado a nivel nativo: `SplitBackend` interno, bridge SPSC preallocado, gate por `BackendEndpointCapabilities`, y `DriftResampler` lineal con tests host.
- Slice 6b completado a nivel nativo: `OboeBackend` ahora puede alimentar input real en full-duplex mediante stream de entrada + ring SPSC.
- Slice 7 expone solo el minimo opt-in: `createSplitBackend(inputBackendId, outputBackendId)` y seleccion posterior de `BackendType::SPLIT`.
- Pendiente externo: smoke manual con hardware USB + mic interno y validacion de consumo desde NoisyPad.

---

## 2. `SplitBackend` — arquitectura

### 2.1 Problema

Hoy `BackendManager` mantiene dos backends (Oboe + Libusb) pero solo uno activo. `IAudioCallback::onAudioReady(output, input, frames)` recibe ambos buffers del mismo backend. Si el usuario quiere "input del mic interno + output USB", debe elegir uno u otro.

### 2.2 Diseño

Nuevo backend que **implementa `IAudioBackend` por composición**:

```cpp
// audio/src/main/cpp/backends/SplitBackend.h
namespace watermelon_audio {

class SplitBackend : public IAudioBackend, private IAudioCallback {
public:
    // Takes ownership of the inner backends.
    // `inputSource` supplies input via its own DSP loop; its output is discarded.
    // `outputSink`  supplies output via its own DSP loop; its input is discarded.
    SplitBackend(std::unique_ptr<IAudioBackend> inputSource,
                 std::unique_ptr<IAudioBackend> outputSink);

    // IAudioBackend
    BackendResult start() override;
    void          stop() override;
    void          pause() override;
    void          resume() override;
    void          setCallback(IAudioCallback* callback) override;
    void          setSampleRate(int sampleRate) override;
    void          setBufferSize(int framesPerBuffer) override;
    void          setFullDuplexEnabled(bool enable) override;
    StreamInfo    getStreamInfo() const override;
    bool          isRunning() const override;
    float         getOutputLatencyMs() const override;
    float         getInputLatencyMs() const override;
    BackendType   getType() const override { return BackendType::SPLIT; }
    bool          supportsFullDuplex() const override { return true; }

private:
    // IAudioCallback — both inputSource and outputSink post into here.
    // We multiplex: store input frames when called from inputSource, pull them
    // out when called from outputSink, forward the merged pair to mUserCallback.
    IAudioCallback::Result onAudioReady(
        float* outputData, const float* inputData, int32_t numFrames) override;
    void onBackendError(BackendError error) override;
    void onStreamConfigChanged(const StreamInfo& newInfo) override;

    std::unique_ptr<IAudioBackend> mInputSource;
    std::unique_ptr<IAudioBackend> mOutputSink;
    IAudioCallback* mUserCallback = nullptr;

    // Lock-free ring buffer for input frames (stereo interleaved float).
    // Size: ~40ms worth of frames at 48kHz stereo ≈ 7680 samples. Minimal latency.
    std::unique_ptr<LockFreeRingBuffer> mInputBridge;

    // Drift compensation: if inputSource clock drifts from outputSink clock,
    // use a small linear resampler to absorb the difference without clicks.
    // Drop/insert sample strategy: when input is "too fast" (> 110% of expected)
    // drop one sample per N; when "too slow" insert a zero-crossing duplicate.
    // Target: ±500 PPM tolerance; above that, log warning.
    std::unique_ptr<DriftResampler> mDriftResampler;

    std::atomic<bool> mRunning{false};

    // Statistics
    std::atomic<uint64_t> mBridgeOverruns{0};
    std::atomic<uint64_t> mBridgeUnderruns{0};
};

}  // namespace
```

### 2.3 Flujo del callback

```
  outputSink.onAudioReady(output, _, N)  (DSP thread of output backend)
  ├─ pull N frames from mInputBridge → tempInput
  ├─ if not enough: drift = true, fill missing with last-valid / silence
  ├─ mUserCallback->onAudioReady(output, tempInput, N)
  └─ return

  inputSource.onAudioReady(_, input, N)  (DSP thread of input backend)
  ├─ mDriftResampler->process(input, N, &tempResampled, &outN)
  ├─ mInputBridge->write(tempResampled, outN)  — update bridge stats
  └─ return
```

Ambos DSP threads corren, pero solo el del `outputSink` llama al callback del usuario. El `inputSource` solo alimenta el bridge.

### 2.4 Clock reconciliation

- Si `inputSource` es Oboe y `outputSink` es Libusb (el caso pedido): el reloj que "manda" es el USB (feedback endpoint + clock source). El Oboe se adapta con el drift resampler.
- Si es al revés (USB input → Oboe output): el clock que manda es Oboe (kernel scheduler). USB se adapta con su propio feedback endpoint ya existente.
- Si ambos son Libusb (dos devices USB distintos — caso avanzado): escoger el de mayor bit depth como master, o dejar que el usuario lo declare.

### 2.5 Latencia esperada

```
Input:  Oboe input   ~8 ms
Bridge: ring buffer  ~20 ms (configurable)
Output: Libusb       ~5 ms (stages 1-3 finalizadas)
-----------------------------------
Total round-trip:    ~33 ms
```

Esto es aceptable para grabación/monitoring pero no competitivo con un full-USB path (~10 ms). El `SplitBackend` es la **concesión** al hardware heterogéneo.

### 2.6 Drift resampler

Implementación mínima viable en esta etapa: **linear interpolation** con fractional position accumulator. Suficiente para ±500 PPM de corrección. Para mejor calidad (cubic, sinc) se deja para una etapa futura.

```cpp
class DriftResampler {
public:
    DriftResampler(float sourceRateHz, float targetRateHz);
    void setDriftCorrection(float ppm);  // called from the "master clock" callback

    // Process `inFrames` input frames, write up to `outCapacity` output frames.
    // Returns how many output frames were produced.
    int process(const float* input, int inFrames, int inChannels,
                float* output, int outCapacity);

private:
    float mSourceRate;
    float mTargetRate;
    float mRatio;              // target/source
    double mFractionalPos = 0.0;
    std::array<float, 8> mHistory{};  // last sample per channel, up to 8 channels
};
```

---

## 3. Resize atómico del ring buffer

### 3.1 Problema

`UsbTransferManager::reconfigureBufferSize()` hace `mOutputRingBuffer->resize(newOutputSize)` mientras hay callbacks de transfer leyéndolo y el DSP thread escribiéndolo. Incluso si `LockFreeRingBuffer::resize()` intenta ser "thread-safe", un resize que cambia índices bases y máscaras tiene una ventana de corrupción.

### 3.2 Diseño: double-buffer con swap

```cpp
class UsbTransferManager {
    // Two ring buffer slots — one active, one staging
    std::array<std::unique_ptr<LockFreeRingBuffer>, 2> mOutputRingSlots;
    std::array<std::unique_ptr<LockFreeRingBuffer>, 2> mInputRingSlots;

    // Current active slot index (0 or 1). Read by writers/readers on every op.
    std::atomic<int> mActiveOutputSlot{0};
    std::atomic<int> mActiveInputSlot{0};

    // Getters used by hot path — tiny atomic load
    LockFreeRingBuffer* outputRing() {
        return mOutputRingSlots[mActiveOutputSlot.load(std::memory_order_acquire)].get();
    }
    LockFreeRingBuffer* inputRing() {
        return mInputRingSlots[mActiveInputSlot.load(std::memory_order_acquire)].get();
    }
};

bool UsbTransferManager::reconfigureBufferSize(int newBufferMs) {
    int oldBufferMs = mConfig.ringBufferMs;
    if (newBufferMs == oldBufferMs) return true;

    int current = mActiveOutputSlot.load(std::memory_order_acquire);
    int next = current ^ 1;

    // Build the new ring buffer in the staging slot
    mConfig.ringBufferMs = newBufferMs;
    size_t newOutputSize = static_cast<size_t>(mConfig.ringBufferSamples());
    mOutputRingSlots[next] = std::make_unique<LockFreeRingBuffer>(newOutputSize);
    MemoryUtils::prepareForRealtime(
        mOutputRingSlots[next]->data(), mOutputRingSlots[next]->sizeBytes());

    // Copy any pending data from old to new (bounded by new capacity)
    auto* oldBuf = mOutputRingSlots[current].get();
    auto* newBuf = mOutputRingSlots[next].get();
    size_t avail = oldBuf->availableToRead();
    size_t toCopy = std::min(avail, newBuf->capacity() / 2);  // don't overfill
    if (toCopy > 0) {
        std::vector<float> tmp(toCopy);
        oldBuf->read(tmp.data(), toCopy);
        newBuf->write(tmp.data(), toCopy);
    }

    // Flip atomically. Any callback that picked up `current` before this line
    // will finish using the old buffer one more time, then see the new one
    // on the next iteration. There's no torn state.
    mActiveOutputSlot.store(next, std::memory_order_release);

    // Drain the old slot safely — wait a few iterations (~1ms) so any in-flight
    // reader finishes before we drop it. Do it from a helper thread to avoid
    // blocking the caller.
    std::thread([this, current]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        mOutputRingSlots[current].reset();
    }).detach();

    return true;
}
```

**Garantías del diseño.**
- Los writers del DSP thread leen `outputRing()` cada iteración — una atomic load. Se adaptan al swap en la siguiente vuelta.
- Los readers de `handleOutputComplete` idem.
- No hay desalineamiento de índices porque cada ring buffer es completamente independiente.
- El costo es el `tmp` de copia de pendientes, acotado a ~capacity/2 para evitar overfill.

**Alternativa más conservadora.** Si el double-buffer es demasiado ambicioso, fallback a: `pause stream → reconfigure → resume stream` coordinado por un flag en el DSP thread. Menos elegante, menos glitches si se hace con fade-in/fade-out en los bordes.

### 3.3 Aplicar también al `AudioFormatConverter` buffers temporales

Los `mFloatBuffer` y `mPcmBuffer` de `UsbTransferManager.cpp:90-96` tienen el mismo problema menor. Si el buffer size cambia, el `resize()` reallocatea. Ya está corregido por MemoryUtils pero el resize en caliente sigue sin ser seguro. El double-buffer los resuelve también.

---

## 4. Per-channel volume

### 4.1 Problema

`UsbVolumeControl::setVolume(float)` solo afecta el canal 0 (master). Un device 4-canal (ej: interfaces DJ, multichannel cards) expone volumen por canal en el Feature Unit, pero no se puede usar.

### 4.2 Diseño

Extender `UsbVolumeControl`:

```cpp
class UsbVolumeControl {
public:
    // Existing:
    bool setVolume(float volume);            // master (channel 0)
    float getVolume() const;

    // New:
    bool setChannelVolume(int channelIndex, float volume);
    float getChannelVolume(int channelIndex) const;
    int  getChannelCount() const;

    // Query which channels actually have volume control
    std::vector<bool> getVolumeControlMask() const;
};
```

El protocolo USB es el mismo SET_CUR al FU_VOLUME_CONTROL, pero con `wValue = (VOLUME << 8) | (channelNumber + 1)`. El channel 0 es master, los channels 1..N son físicos.

### 4.3 Exposición Kotlin

```kotlin
// IUsbAudioManager.kt
suspend fun setChannelVolume(channelIndex: Int, volume: Float): Result<Unit>
suspend fun getChannelVolume(channelIndex: Int): Result<Float>
val perChannelVolumeCaps: StateFlow<List<ChannelVolumeInfo>>

data class ChannelVolumeInfo(
    val channelIndex: Int,
    val name: String?,  // "Master", "Front Left", etc., via UAC string descriptor
    val hasVolumeControl: Boolean,
    val hasMuteControl: Boolean,
    val currentVolume: Float,
    val isMuted: Boolean,
)
```

El nombre se extrae de `bmaControls(ch)` en el Feature Unit descriptor + string descriptor indices (`iChannelNames`), que el parser ya tiene parcial.

---

## 5. Channel mapping / routing matrix

### 5.1 Problema

Un device de 4 canales (2 in, 2 out + separately 2 S/PDIF) no tiene forma de decirle al engine "tomá solo los canales 1-2" o "duplicá L en R". El DSP thread ya hace mono→estéreo con -3 dB, pero eso es un caso particular hardcoded.

### 5.2 Diseño

Nuevo tipo:

```cpp
// audio/src/main/cpp/usb/ChannelMap.h
namespace watermelon_audio::usb {

struct ChannelMap {
    // For each logical channel in the engine's stereo/mono output,
    // which physical channel of the USB device does it route to?
    // -1 = silence, 0..N = physical channel index
    std::array<int, 8> engineToDeviceOutput{-1, -1, -1, -1, -1, -1, -1, -1};

    // Mix matrix: to produce engine input channel `i`,
    // sum physical device channels j with weight inMix[i][j]
    // Simplified: default to identity, allow per-row normalization.
    std::array<std::array<float, 8>, 2> inMix{};  // 2 engine input channels
};

}  // namespace
```

El `UsbTransferManager` aplica el map al hacer la conversión float↔PCM:
- Para output: iterar frames; para cada sample del engine, escribir en `physicalChannel[engineToDeviceOutput[i]]`.
- Para input: iterar frames; para cada engine input channel, sumar los physical channels según `inMix`.

La ganancia de -3 dB del mono→estéreo actual se expresa como una fila del `inMix` con pesos 0.707 y 0.707.

### 5.3 Presets

```cpp
static ChannelMap identityStereo();       // L→0, R→1
static ChannelMap monoToStereoDownmix();  // L/R→0 con -3dB
static ChannelMap swapLR();
static ChannelMap leftOnly();             // L→0/1, R→silent
```

### 5.4 API Kotlin

```kotlin
sealed class ChannelRouting {
    object IdentityStereo : ChannelRouting()
    object MonoDownmix : ChannelRouting()
    object SwapLR : ChannelRouting()
    object LeftOnly : ChannelRouting()
    data class Custom(val map: IntArray, val mixMatrix: Array<FloatArray>) : ChannelRouting()
}

suspend fun setChannelRouting(routing: ChannelRouting): Result<Unit>
```

---

## 6. Recovery automático ante errores no-fatales

### 6.1 Problema

El watchdog actual mata el stream cuando detecta > 500 ms sin transferencias exitosas. Algunos devices tienen glitches transitorios (USB bus error por spike EMI, selector conmutando, Android scheduler jitter) que se recuperan en < 1 segundo. El código actual asume device disconnected y hace fallback a Oboe.

### 6.2 Diseño

Añadir un `RecoveryPolicy`:

```cpp
struct RecoveryPolicy {
    int   maxConsecutiveErrorsBeforeRestart = 5;
    int   maxRestartsPerMinute = 3;
    bool  tryResubmitBeforeRestart = true;
    int   quietPeriodMsBetweenRestarts = 500;
};
```

En `UsbTransferManager::handleOutputComplete`, cuando una transferencia falla de forma no-fatal (no `LIBUSB_TRANSFER_NO_DEVICE`, no `LIBUSB_ERROR_NO_DEVICE`), intentar primero re-submit hasta `maxConsecutiveErrorsBeforeRestart`. Si persiste, despachar un evento `RESTART_REQUESTED` al event loop thread (no al hot path). El event loop thread:
1. Cancela transfers pendientes.
2. `libusb_set_interface_alt_setting(handle, if, 0)` → `libusb_set_interface_alt_setting(handle, if, selectedAlt)`.
3. Re-allocate transfers (ya estaban allocados).
4. Re-submit.
5. Incrementa un counter; si supera `maxRestartsPerMinute`, entonces sí declarar disconnect y hacer fallback.

El DSP thread sigue corriendo durante el restart con silencio en output.

---

## 7. Tests

### 7.1 `SplitBackend_test.cpp`

- Creación con dos backends stub; start/stop ciclo limpio.
- El input stub produce un patrón conocido; el output stub verifica que el callback del usuario lo recibe con latencia acotada.
- Drift injection: mover el clock del input a +200 PPM. Verificar que después de 1 segundo las frecuencias convergen con < 50 PPM residual.
- Overrun del bridge: el input produce más frames que el output consume. Verificar que `mBridgeOverruns` incrementa.
- Underrun del bridge: viceversa. Verificar `mBridgeUnderruns`.

### 7.2 `RingBufferSwap_test.cpp`

- Arrancar dos threads: uno lee, otro escribe, durante 5 segundos con swaps cada 100 ms. Verificar que los bytes leídos son los que se escribieron, sin huecos ni duplicados.
- Fuzz: 1000 swaps con tamaños aleatorios entre 32 y 8192 samples. No debe crashear ni corromperse.

### 7.3 Integration — `SplitBackendSmokeTest.kt`

Preset nuevo en el `UsbAudioTestRunner`:

```kotlin
UsbTestPresets.SPLIT_OBOE_IN_USB_OUT: runs input via Oboe (mic), output via USB.
  Pass if:
    - Callback is invoked for 5 seconds continuously
    - bridge underruns < 3% of total callbacks
    - bridge overruns < 3% of total callbacks
    - userCallback sees non-zero input peak (mic picks up something)
```

### 7.4 Channel routing tests

- `ChannelMap::identityStereo` aplicado a un buffer estereo: output idéntico al input.
- `monoToStereoDownmix`: amplitud L+R input = amplitud output × 2 / 0.707.
- `swapLR`: channel 0 va a 1 y viceversa.

### 7.5 Recovery test

Simular 3 errores consecutivos en el stub de libusb callback, luego éxito. Verificar que el stream no se tumba y `getTransferStats().packetsErrors == 3` mientras que el stream sigue corriendo.

---

## 8. Criterios de aceptación

- [x] `SplitBackend` implementa `IAudioBackend` y multiplexa dos inner backends correctamente en tests host con fakes.
- [x] `DriftResampler` compensa ±500 PPM en tests host deterministicos.
- [x] `BackendManager` expone `createSplitBackend(inputType, outputType)` como opcion interna/JNI opt-in.
- [ ] Preset `SPLIT_OBOE_IN_USB_OUT` pasa en Pixel 6 + Scarlett Solo con microfono interno como fuente.
- [x] `UsbTransferManager::reconfigureBufferSize()` usa double-buffer swap; no hay race condition observable en el stress test host.
- [x] `UsbVolumeControl` expone `setChannelVolume(ch, vol)` a nivel nativo; pendiente verificacion manual en device con volumen por canal.
- [x] `ChannelMap` implementado con presets e integrado con default identity.
- [ ] `IUsbAudioManager.perChannelVolumeCaps: StateFlow` operacional; refleja cambios de hardware.
- [ ] `IUsbAudioManager.setChannelRouting(routing)` funcional.
- [x] `RecoveryPolicy` con auto-restart tiene policy pura testeada y wiring nativo; pendiente smoke hardware de errores transitorios reales.
- [ ] Cero underruns en stress test 30 minutos con `RecoveryPolicy` active y errores inyectados cada 3 segundos.

---

## 9. Riesgos específicos

1. **Latencia del SplitBackend aceptable pero no mínima.** Documentar claramente que es un compromiso. Los usuarios que necesiten < 10 ms deben usar full-USB duplex.
2. **Drift resampler lineal introduce aliasing en frecuencias altas.** Mitigación: documentar la limitación; stage futuro puede upgradear a cubic/sinc.
3. **Double-buffer swap no es universal.** Algunos sistemas sin `std::atomic` con load/store acquire/release adecuado pueden tener problemas. Testeo intensivo en ARM64 (target) y mínimo compile-test en ARMv7.
4. **Per-channel volume en devices con controles incompletos.** Muchos devices solo tienen master. Detectar con GET_CUR antes de exponer; si falla, deshabilitar la UI de per-channel.
5. **Recovery policy en loop de error.** Si el device realmente se murió pero el fail no es `NO_DEVICE`, el loop de restart puede enmascarar el problema. Mitigación: `maxRestartsPerMinute = 3` como hard ceiling.

---

## 10. Checklist de commit

Diff aproximado:

- `audio/src/main/cpp/backends/SplitBackend.{h,cpp}` **nuevo** ~650 líneas
- `audio/src/main/cpp/backends/DriftResampler.{h,cpp}` **nuevo** ~220 líneas
- `audio/src/main/cpp/backends/IAudioBackend.h` +1 línea (`SPLIT` enum value)
- `audio/src/main/cpp/backends/BackendManager.{h,cpp}` +60 líneas (createSplit factory)
- `audio/src/main/cpp/usb/UsbTransferManager.{h,cpp}` +150 líneas (double-buffer swap + recovery policy)
- `audio/src/main/cpp/usb/UsbVolumeControl.{h,cpp}` +90 líneas (per-channel)
- `audio/src/main/cpp/usb/ChannelMap.{h,cpp}` **nuevo** ~280 líneas
- `audio/src/main/cpp/backends/LibusbBackend.cpp` +30 líneas (wire ChannelMap instead of hardcoded monoToStereo)
- `audio/src/commonMain/kotlin/.../api/IUsbAudioManager.kt` +40 líneas
- `audio/src/commonMain/kotlin/.../domain/usb/ChannelRouting.kt` **nuevo** ~60 líneas
- `audio/src/androidMain/kotlin/.../internal/usb/UsbAudioManagerImpl.kt` +80 líneas
- `audio/src/test/cpp/backends/SplitBackend_test.cpp` **nuevo** ~300 líneas
- `audio/src/test/cpp/backends/DriftResampler_test.cpp` **nuevo** ~180 líneas
- `audio/src/test/cpp/usb/RingBufferSwap_test.cpp` **nuevo** ~220 líneas
- `audio/src/test/cpp/usb/ChannelMap_test.cpp` **nuevo** ~150 líneas

Commit messages sugeridos:
1. `feat(backends): SplitBackend for heterogeneous input/output composition`
2. `feat(backends): DriftResampler (linear) for clock reconciliation`
3. `fix(usb): atomic double-buffer swap for safe ring buffer resize`
4. `feat(usb): per-channel volume control in UsbVolumeControl`
5. `feat(usb): ChannelMap for explicit routing and matrix mixing`
6. `feat(usb): RecoveryPolicy with bounded auto-restart on transient errors`
7. `feat(api): expose split backend, per-channel volume, channel routing in Kotlin`

---

## 11. Siguiente etapa

Con routing y control fino en su lugar, [stage_05_kotlin_api.md](stage_05_kotlin_api.md) consolida toda la nueva superficie Kotlin, introduce observabilidad de threshold y cierra los gaps de audio focus + lifecycle.
