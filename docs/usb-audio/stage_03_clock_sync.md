# Etapa 3 — Clock sync profesional

**Estado:** CERRADA funcionalmente para entrada a Stage 4 — RANGE de clock sources, `UsbClockGraph`, selectors/multipliers, Clock Selector `CUR`, bInterval en snapshot/timing y observabilidad básica de clock health ya existen. La validación manual de hardware fue reportada como OK; quedan como pendientes explícitos la validación prolongada de drift/jitter, más devices con `bInterval > 1` y refinamiento de thresholds/diagnóstico en más hardware.
**Dependencias:** stages 1 y 2 mergeados. Necesita `UsbTopology` para navegar el clock graph y `configureSampleRate()` para aplicar la selección.
**Duración estimada:** 3–4 días.
**Severidad de los bugs que resuelve:** 1× Crítico + pulido del feedback end-to-end iniciado en stage 1.

**Relevamiento 2026-04-30:** `ClockSourceRangeParser.h` existe y está testeado; `LibusbBackend::populateClockSourceRates()` consulta UAC2 `RANGE`; `configureSampleRate()` ya intenta resolver clocks desde los terminales seleccionados y evita SET_CUR redundante con GET_CUR previo.

**Cierre Stage 3 / entrada Stage 4 (2026-05-04):** `UsbClockGraph` navega terminales, selectors y multipliers hasta la fuente final; `LibusbBackend` aplica Clock Selector `CUR` cuando corresponde; `UsbTransferStats` expone sample rate medido, drift PPM, feedback counters y clock source activo; `UsbCapabilitySnapshot` serializa `bInterval`; el pacing nativo deriva `framesPerPacket` desde velocidad USB + `bInterval`. La validación manual de hardware fue reportada como OK antes de preparar la entrada a Stage 4.

---

## 1. Objetivo

Cerrar el capítulo de clock sync profesional: resolver el grafo completo UAC 2.0 (Clock Sources, Clock Selectors, Clock Multipliers), implementar la selección explícita antes de `start()`, leer el rango de frecuencias soportadas por cada clock source vía `RANGE` request, y cerrar el loop end-to-end del feedback endpoint (stage 1 lo dejó funcional pero sin diagnóstico ni exposición de métricas).

Problemas concretos resueltos:

1. **Clock source selection UAC 2.0.** `UsbDescriptorParser` guarda los clock sources en `UsbTopology::clockSources` pero nadie los usa. Devices con múltiples fuentes (word clock externo, internal VCO, SPDIF in) no son controlables.
2. **`queryClockSourceSampleRates` TODO stub** (`UsbDescriptorParser.cpp:894`). Los rates soportados por cada clock no se consultan vía `RANGE` request.
3. **Clock selector nunca configurado.** Resuelto en el path de `LibusbBackend::configureSampleRate()`: el graph elige fuente final, consulta selector `GET_CUR` y aplica selector `SET_CUR` si es escribible.
4. **Métricas de drift no observables desde Kotlin.** Parcialmente resuelto: `UsbTransferStats` expone drift/sample-rate/feedback counters/clock source activo y `IUsbAudioManager.healthEvents` emite drift/underrun/clock-source changes.

Estado real de esos puntos al 2026-04-30:

- El punto 2 está parcialmente resuelto fuera del parser: `LibusbBackend::populateClockSourceRates()` consulta `RANGE` y usa `ClockSourceRangeParser.h`.
- El punto 1 está parcialmente resuelto para topologías simples: `configureSampleRate()` usa `resolveClockSourceId(terminalLink)` para elegir clock IDs. No navega todavía selectors/multipliers.
- El punto 3 está implementado para la fuente default o seleccionada manualmente, con fallback no fatal si el selector es read-only o rechaza `SET_CUR`.
- El punto 4 está implementado como observabilidad básica; quedan validación prolongada y refinamiento de thresholds/eventos.

---

## 2. Diseño del clock graph

En UAC 2.0 el grafo de clock es:

```
 [Clock Source A] ─┐
 [Clock Source B] ─┼─▶ [Clock Selector] ─▶ [Clock Multiplier?] ─▶ [Input/Output Terminal]
 [Clock Source C] ─┘                                                  │
                                                                       └─▶ [Feature Unit]
```

Cada nodo tiene un `bUnitId/bClockID`. Los terminales referencian su clock vía `bCSourceID`. Un Clock Selector puede conmutar entre múltiples clock sources y tiene su propio control (`CS_X_CLOCK_SELECTOR_CONTROL`, valor = id de la fuente activa).

### 2.1 `UsbClockGraph` (nuevo)

```cpp
// audio/src/main/cpp/usb/UsbClockGraph.h
namespace watermelon_audio::usb {

class UsbClockGraph {
public:
    explicit UsbClockGraph(const UsbTopology& topology);

    // All clock sources reachable from a given terminal (following selectors)
    std::vector<const UsbClockSource*> reachableSourcesFor(int terminalId) const;

    // The currently selected path from a terminal to a clock source.
    // Returns nullopt if no path or ambiguous without querying device.
    struct ActivePath {
        int terminalId;
        const UsbClockSelector* selector = nullptr;   // may be null
        const UsbClockMultiplier* multiplier = nullptr;
        const UsbClockSource* source = nullptr;
    };
    std::optional<ActivePath> resolvePath(int terminalId) const;

    // Does this device require a selector CUR to pick the source, or is
    // there only a single source (in which case no selector action is needed)?
    bool requiresSelectorConfiguration(int terminalId) const;

    // Find the "best" default source: internal programmable > internal variable > internal fixed > external
    const UsbClockSource* pickDefaultSource(int terminalId) const;

private:
    const UsbTopology& mTopology;
    std::unordered_map<int, const UsbClockSource*> mSourceById;
    std::unordered_map<int, const UsbClockSelector*> mSelectorById;
    std::unordered_map<int, const UsbClockMultiplier*> mMultiplierById;
    // Adjacency: nodeId -> list of input ids it reads from
    std::unordered_map<int, std::vector<int>> mEdges;

    void buildGraph();
    const void* findNodeById(int id) const;
};

}  // namespace
```

### 2.2 `UsbControlRequests` (helper centralizado)

Mover todos los `libusb_control_transfer` class-specific a un helper único para no repetir bitfields en cada llamada:

```cpp
// audio/src/main/cpp/usb/UsbControlRequests.h
namespace watermelon_audio::usb {

class UsbControlRequests {
public:
    UsbControlRequests(libusb_device_handle* handle, int controlInterface, int timeoutMs = 1000)
        : mHandle(handle), mControlInterface(controlInterface), mTimeoutMs(timeoutMs) {}

    // UAC2 Clock Source sample frequency
    Result<uint32_t> getClockSourceCurSampleRate(int clockSourceId);
    Result<void>      setClockSourceCurSampleRate(int clockSourceId, uint32_t rate);
    Result<std::vector<SampleRateRange>> getClockSourceRangeSampleRates(int clockSourceId);

    // UAC2 Clock Selector
    Result<uint8_t>  getClockSelectorCur(int selectorId);
    Result<void>     setClockSelectorCur(int selectorId, uint8_t sourceIdIndex);

    // UAC2 Clock Source validity
    Result<bool>     getClockSourceValid(int clockSourceId);

    // UAC1 endpoint sampling frequency
    Result<uint32_t> getUac1EndpointSampleRate(uint8_t endpointAddress);
    Result<void>     setUac1EndpointSampleRate(uint8_t endpointAddress, uint32_t rate);

private:
    libusb_device_handle* mHandle;
    int mControlInterface;
    int mTimeoutMs;
};

}  // namespace
```

Los métodos de `UsbVolumeControl` eventualmente pueden migrar aquí también (stage 4), manteniendo la separación actual durante esta etapa para no ampliar el scope.

---

## 3. Tareas

### 3.0 Ya implementado antes de retomar la etapa

- `ClockSourceRangeParser.h` parsea respuestas UAC2 `RANGE` y las aplica a `UsbClockSource`.
- `test_clock_range_parser.cpp` cubre rates discretos, rangos continuos, payloads truncados y orden estable.
- `LibusbBackend::populateClockSourceRates()` limpia rates stale y consulta `RANGE` por clock source con frequency control.
- `UsbSnapshotCodec` ya serializa sample rates/min/max/continuous de clock sources.
- `configureSampleRate()` ya hace GET_CUR previo, evita SET_CUR redundante y puede aplicar SET_CUR a más de un clock source resuelto desde playback/capture.

Estas piezas reducen el scope restante, pero no reemplazan `UsbClockGraph`: si `bCSourceID` apunta a un selector o multiplier, el código actual todavía puede tratar ese nodo como si fuera un clock source final.

### 3.1 `UsbClockGraph::buildGraph()` y `reachableSourcesFor()`

- Iterar `topology.clockSources`, `clockSelectors`, `clockMultipliers`, construir los mapas y las aristas.
- Un `UsbClockSelector` tiene `bNrInPins` y un array de `baCSourceID(i)`; las aristas van selector → cada fuente.
- Un `UsbClockMultiplier` tiene `bCSourceID`; arista multiplier → source.
- Los terminales tienen `bCSourceID` que apunta a algún nodo del grafo.

Test unitario: construir una topología artificial con 2 sources + 1 selector + 1 multiplier + 1 terminal y verificar que `reachableSourcesFor(terminalId)` devuelve las 2 sources.

### 3.2 Consultar rangos de sample rate

```cpp
// UsbControlRequests.cpp
Result<std::vector<SampleRateRange>>
UsbControlRequests::getClockSourceRangeSampleRates(int clockSourceId) {
    // UAC2 spec 5.2.1 RANGE layout for 4-byte values:
    //   (u16) wNumSubRanges
    //   per sub-range: (u32) dMIN, (u32) dMAX, (u32) dRES
    //
    // bmRequestType = 0xA1 (D2H | Class | Interface)
    // bRequest      = RANGE (0x02)
    // wValue        = CS_SAM_FREQ_CONTROL << 8
    // wIndex        = (clockSourceId << 8) | controlInterface

    uint8_t firstBuf[2];
    int r = libusb_control_transfer(
        mHandle,
        0xA1,
        UAC2_REQUEST_RANGE,
        static_cast<uint16_t>(UAC2_CS_SAM_FREQ_CONTROL << 8),
        static_cast<uint16_t>((clockSourceId << 8) | mControlInterface),
        firstBuf, 2, mTimeoutMs);
    if (r != 2) return Error("Failed to read RANGE header");

    uint16_t numSubRanges = firstBuf[0] | (firstBuf[1] << 8);
    const size_t totalLen = 2 + static_cast<size_t>(numSubRanges) * 12;
    std::vector<uint8_t> buf(totalLen);
    r = libusb_control_transfer(
        mHandle, 0xA1, UAC2_REQUEST_RANGE,
        static_cast<uint16_t>(UAC2_CS_SAM_FREQ_CONTROL << 8),
        static_cast<uint16_t>((clockSourceId << 8) | mControlInterface),
        buf.data(), static_cast<uint16_t>(totalLen), mTimeoutMs);
    if (r != static_cast<int>(totalLen)) return Error("RANGE payload short");

    std::vector<SampleRateRange> ranges;
    ranges.reserve(numSubRanges);
    const uint8_t* p = buf.data() + 2;
    for (int i = 0; i < numSubRanges; ++i) {
        SampleRateRange range;
        range.minHz = readU32LE(p);      p += 4;
        range.maxHz = readU32LE(p);      p += 4;
        range.resolutionHz = readU32LE(p); p += 4;
        ranges.push_back(range);
    }
    return ranges;
}
```

Poblar `ClockSourceInfo::sampleRateRanges` en el snapshot para exponer al Kotlin.

**Importante.** El device puede responder STALL si el clock source no tiene `CS_SAM_FREQ_CONTROL` bit seteado en `bmControls`. Antes de consultar, verificar `clockSource.hasFrequencyControl`.

### 3.3 Implementar `setClockSource()` en `LibusbBackend`

```cpp
bool LibusbBackend::selectClockSource(int terminalId, int clockSourceId) {
    if (mUsbDevice->uacVersion != 2) {
        LOGW("selectClockSource only applies to UAC2");
        return false;
    }
    UsbClockGraph graph(*mUsbDevice);
    auto reachable = graph.reachableSourcesFor(terminalId);
    auto it = std::find_if(reachable.begin(), reachable.end(),
                            [=](const UsbClockSource* s) { return s->clockId == clockSourceId; });
    if (it == reachable.end()) {
        LOGE("Clock source %d not reachable from terminal %d", clockSourceId, terminalId);
        return false;
    }

    // If there's a selector in the path, set its CUR to point to the requested source
    if (graph.requiresSelectorConfiguration(terminalId)) {
        auto path = graph.resolvePath(terminalId);
        if (path && path->selector) {
            // Find the pin index matching clockSourceId
            uint8_t pinIndex = 0xFF;
            for (uint8_t i = 0; i < path->selector->sourceIds.size(); ++i) {
                if (path->selector->sourceIds[i] == clockSourceId) { pinIndex = i + 1; break; }
            }
            if (pinIndex == 0xFF) return false;
            UsbControlRequests req(mDeviceHandle, mUsbDevice->controlInterface);
            auto r = req.setClockSelectorCur(path->selector->unitId, pinIndex);
            if (!r) return false;
            LOGI("Clock selector %d → source %d (pin %d)",
                 path->selector->unitId, clockSourceId, pinIndex);
        }
    }

    // Verify source validity if control is present
    if (it->hasValidityControl) {
        UsbControlRequests req(mDeviceHandle, mUsbDevice->controlInterface);
        auto valid = req.getClockSourceValid(clockSourceId);
        if (valid && !*valid) {
            LOGW("Clock source %d reports invalid", clockSourceId);
            return false;
        }
    }

    mActiveClockSourceId = clockSourceId;
    return true;
}
```

### 3.4 Integrar en la secuencia de `start()`

Nueva secuencia normativa en `LibusbBackend::start()`:

```
1. selectBestInterfaces()            [stage 2]
2. If UAC2:
     selectClockSource(terminalId, defaultOrUserChoice)  [this stage]
3. configureSampleRate()              [stage 1, now tocando la fuente activa]
4. setupTransferManager()
5. transferManager->start()
6. DSP thread
```

Si `selectClockSource` falla pero hay una sola fuente alcanzable, continuar (no es error fatal). Log de cada decisión con `LOGI`.

### 3.5 Mejorar `configureSampleRate()` para usar la fuente activa

Modificar la implementación de stage 1 para usar `mActiveClockSourceId` en vez del `clockSources.front()`:

```cpp
// UAC2 branch
uint16_t wIndex = static_cast<uint16_t>((mActiveClockSourceId << 8) | mUsbDevice->controlInterface);
```

### 3.6 Exponer drift PPM y clock health en las stats

Añadir a `UsbTransferStats`:

```cpp
struct TransferStatistics {
    // ... existing fields
    std::atomic<float>  currentSampleRateHz{0.0f};  // measured
    std::atomic<float>  driftPpm{0.0f};
    std::atomic<float>  feedbackEffectiveFramesPerPacket{0.0f};
    std::atomic<uint32_t> feedbackPacketsReceived{0};
    std::atomic<uint32_t> feedbackPacketsInvalid{0};
    std::atomic<int>    activeClockSourceId{-1};
};
```

Actualizar `handleFeedbackComplete` para incrementar los counters, y `handleOutputComplete`/`handleInputComplete` para tomar snapshots del clock controller.

### 3.7 Evento de drift threshold

En `UsbTransferManager`, agregar:

```cpp
struct ThresholdConfig {
    float driftPpmWarning = 100.0f;
    float driftPpmCritical = 500.0f;
    int   underrunPerMinuteWarning = 5;
    int   underrunPerMinuteCritical = 30;
};
```

Desde el event loop thread (no RT thread), cada 500 ms comparar el estado actual con los thresholds y disparar el error callback con `UsbHealthEvent` cuando aplique. No despachar sincrónico; meter a una `SPSC queue` consumible desde el manager.

### 3.8 Exposición Kotlin

Añadir al `UsbTransferStats` commonMain los campos de clock. Publicar un nuevo Flow en el manager:

```kotlin
// IUsbAudioManager.kt
val healthEvents: Flow<UsbHealthEvent>

sealed class UsbHealthEvent {
    data class DriftWarning(val ppm: Float) : UsbHealthEvent()
    data class DriftCritical(val ppm: Float) : UsbHealthEvent()
    data class UnderrunRate(val perMinute: Int, val severity: Severity) : UsbHealthEvent()
    data class ClockSourceChanged(val oldId: Int, val newId: Int) : UsbHealthEvent()
    enum class Severity { INFO, WARNING, CRITICAL }
}
```

---

## 4. Tests

### 4.1 `UsbClockGraph_test.cpp`

- `buildsGraphFromParsedTopology`
- `reachableSourcesReturnsAllSelectorInputs`
- `resolvePathFollowsMultiplier`
- `requiresSelectorConfigurationTrueWithMultipleSources`
- `pickDefaultSourcePrefersInternalProgrammable`

### 4.2 `UsbControlRequests_test.cpp`

Tests con mock de `libusb_control_transfer` que capturan los bitfields exactos:

```cpp
TEST(UsbControlRequests, GetClockSourceRangeDecodesMultipleSubRanges) {
    MockLibusb mock;
    // Simulate device with 3 sub-ranges: 44100, 48000, 96000
    mock.whenControlTransfer(0xA1, UAC2_REQUEST_RANGE, /*...*/)
        .thenRespond({
            0x03, 0x00,                         // 3 sub-ranges
            0x44, 0xAC, 0x00, 0x00,  0x44, 0xAC, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  // 44100
            0x80, 0xBB, 0x00, 0x00,  0x80, 0xBB, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  // 48000
            0x00, 0x77, 0x01, 0x00,  0x00, 0x77, 0x01, 0x00,  0x00, 0x00, 0x00, 0x00,  // 96000
        });
    UsbControlRequests req(mock.handle(), 0);
    auto r = req.getClockSourceRangeSampleRates(5);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->size(), 3);
    EXPECT_EQ(r->at(0).minHz, 44100u);
    EXPECT_EQ(r->at(2).maxHz, 96000u);
}
```

### 4.3 Integration test en runner

Preset `CLOCK_SWITCH`: si el device tiene múltiples clock sources, cambiar entre ellas cada 2 segundos durante 20 segundos, validando que:
- No hay underruns > 10/s durante la transición.
- El drift converge a < 100 PPM dentro de 500 ms tras cada switch.
- El `healthEvents` Flow emite `ClockSourceChanged` en cada switch.

### 4.4 Drift stability test

Preset `DRIFT_STABILITY`: streaming continuo 5 minutos, monitorear `driftPpm` cada segundo, validar que:
- Drift promedio < 20 PPM.
- Drift máximo < 100 PPM.
- No hay eventos `DriftCritical`.

---

## 4.5 Cierre de validación manual

Resultado reportado el 2026-05-04: validación manual de hardware OK para el avance de Stage 3. Con esto quedan cerrados para entrada a Stage 4:

- Resolución de clock graph real desde terminales hacia clock source final.
- Clock Selector `GET_CUR`/`SET_CUR` antes de `SET_CUR` de sample rate.
- `selectClockSource(clockSourceId)` para aplicar en el próximo `startStreaming()`.
- Observabilidad básica de clock/feedback desde `UsbTransferStats` y `healthEvents`.
- Serialización de `bInterval` y pacing de paquetes derivado de velocidad USB + `bInterval`.

Pendientes intencionales que no bloquean Stage 4:

- Validación prolongada de drift/jitter (10 minutos o más) y preset `DRIFT_STABILITY` en al menos 2 devices.
- Más hardware UAC2 con `bInterval > 1` para confirmar pacing en endpoints de cadencia reducida.
- Refinar thresholds de `healthEvents` con datos reales; valores actuales: drift warning 100 PPM, drift critical 500 PPM, underrun warning 5/min, underrun critical 30/min.
- Modelar rangos completos de clock source en snapshot si NoisyPad necesita mostrar sub-ranges, no solo sample rates/min/max/continuous.
- Considerar `UsbControlRequests` centralizado cuando Stage 4 toque más controles class-specific; no es requisito para cerrar esta etapa.

---

## 5. Criterios de aceptación

- [x] `UsbClockGraph` implementado con tests de construcción y navegación.
- [ ] **Parcial:** `UsbControlRequests::getClockSourceRangeSampleRates()` implementado y testeado con mocks. Estado real: existe `ClockSourceRangeParser` + consulta directa en `LibusbBackend::populateClockSourceRates()`, pero no helper centralizado `UsbControlRequests`.
- [x] Clock Selector `CUR` implementado en `LibusbBackend` con `GET_CUR`/`SET_CUR` directo; helper centralizado `UsbControlRequests` sigue diferido.
- [x] `LibusbBackend::selectClockSource()` funcional para selección manual del próximo start; el graph valida reachability por terminal y hardware validation fue reportada como pasada.
- [x] `configureSampleRate()` usa clock IDs finales resueltos por graph desde playback/capture en UAC2, incluyendo selectors/multipliers.
- [x] `UsbTransferStats` incluye `driftPpm`, `currentSampleRateHz`, `activeClockSourceId`, `feedbackPacketsReceived`, `feedbackPacketsInvalid`.
- [ ] **Parcial:** `UsbCapabilitySnapshot.ClockSourceInfo.sampleRateRanges` poblado en UAC2. Estado real: se serializan sample rates/min/max/continuous; falta modelar rangos completos y graph.
- [x] `IUsbAudioManager.healthEvents: Flow<UsbHealthEvent>` expuesto y operacional desde el health loop Android.
- [x] Validación manual de hardware reportada OK para cerrar Stage 3 funcional.
- [ ] Pendiente largo: drift sostenido < 50 PPM durante 10 minutos con clock source single. Logs confirman SET_CUR al clock selector efectivo.
- [ ] Pendiente largo: un device UAC1 sigue funcionando sin regresiones tras los cambios (no debe intentar `selectClockSource`).
- [ ] Preset `DRIFT_STABILITY` pasa en al menos 2 devices del allowlist.

---

## 6. Riesgos específicos

1. **Devices con selector pero sin control escribible.** Algunos devices exponen un selector read-only (solo GET_CUR). Detectar con GET y, si el setter responde STALL, solo loggear warning — no es fatal.
2. **Validity control no siempre presente.** Si `hasValidityControl` es false, saltar la verificación. No todos los devices lo exponen.
3. **RANGE request con sub-ranges mal declarados.** Algunos devices devuelven `wNumSubRanges=0` con un total length > 2, u otros violan el spec. Defensive parsing — abortar con warning.
4. **Clock multiplier raro.** Los multiplicadores son poco usados en devices consumer; testear con al menos uno si es posible (Motu M2 tiene uno). Si no, test unitario sintético.
5. **Thread safety del `mActiveClockSourceId`.** Usar `std::atomic<int>`. La lectura/escritura siempre es desde thread UI o desde start/stop, nunca desde el hot path.

---

## 7. Checklist de commit

Diff aproximado:

- `audio/src/main/cpp/usb/UsbClockGraph.{h,cpp}` **nuevo** ~280 líneas
- `audio/src/main/cpp/usb/UsbControlRequests.{h,cpp}` **nuevo** ~450 líneas
- `audio/src/main/cpp/usb/UsbAudioTypes.h` +15 líneas (ranges en ClockSourceInfo)
- `audio/src/main/cpp/backends/LibusbBackend.{h,cpp}` +90 líneas (selectClockSource + integration)
- `audio/src/main/cpp/usb/UsbTransferManager.{h,cpp}` +60 líneas (new stats + threshold watchdog)
- `audio/src/main/cpp/usb/UsbDescriptorParser.cpp` +30 líneas (populate ranges from RANGE request)
- `audio/src/main/cpp/jni/jni_audio_bridge.cpp` +40 líneas (expose health events + stats fields)
- `audio/src/commonMain/kotlin/.../domain/usb/UsbAudioTypes.kt` +80 líneas (extend stats + health)
- `audio/src/commonMain/kotlin/.../api/IUsbAudioManager.kt` +10 líneas (healthEvents flow)
- `audio/src/androidMain/kotlin/.../internal/usb/UsbAudioManagerImpl.kt` +60 líneas (wire health flow)
- `audio/src/test/cpp/usb/UsbClockGraph_test.cpp` **nuevo** ~220 líneas
- `audio/src/test/cpp/usb/UsbControlRequests_test.cpp` **nuevo** ~300 líneas

Commit messages sugeridos:
1. `feat(usb): add UsbClockGraph for UAC2 clock topology resolution`
2. `feat(usb): implement UsbControlRequests helper for class-specific transfers`
3. `feat(usb): query clock source sample rate ranges (UAC2 RANGE)`
4. `feat(usb): implement LibusbBackend::selectClockSource`
5. `feat(usb): expose drift PPM and clock health in transfer stats`
6. `feat(usb): emit health events (drift, underrun, clock switch) via Flow`
7. `test(usb): UsbClockGraph + UsbControlRequests unit tests`

---

## 8. Siguiente etapa

Stage 4 puede entrar en planificación con [stage_04_mixing_routing.md](stage_04_mixing_routing.md). No implementar todavía `SplitBackend`, resize atómico de ring buffers, per-channel volume ni routing matrix sin revisar alcance, porque Stage 4 cambia superficie de backend y API pública.

Antes de implementación Stage 4, confirmar:

- Alcance mínimo compatible con NoisyPad.
- Evidencia adicional de drift/jitter si el nuevo routing depende de clock reconciliation.
- Si el resize atómico de ring buffer debe ir primero por estabilidad antes de routing/mixing.
