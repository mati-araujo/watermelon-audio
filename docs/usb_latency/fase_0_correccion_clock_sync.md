# Fase 0 — Corrección del clock sync y métricas de latencia

**Objetivo:** que la sincronización de clock (explícita e implícita, UAC1 y UAC2) sea *correcta* antes de bajar buffers. Con buffers de 32 ms los bugs actuales se disimulan; con buffers de 4–5 ms (Fase 1) cualquier error de rate produce xruns en segundos. Esta fase es prerequisito duro de todo lo demás.

**Hallazgos cubiertos:** C1, C2, C3, C5, L7 y M2/M3 de la auditoría.
**Estimación:** 2–4 días + validación en hardware.

---

## 0.1 — Reemplazo del PID por seguimiento directo de Ff (C1 + C3 + M2 + M3)

### Problema
- `ClockController.h:242`: `mPid.calculate(expectedRate, avgRate)` produce ajuste con **signo invertido** (dispositivo rápido → host envía menos frames).
- Diseño conceptual erróneo: un PID contra el nominal intenta "corregir" una medición exógena. El feedback Ff **es** la consigna de frames por intervalo de servicio (así lo implementa snd-usb-audio).
- `UsbIsoTiming.h:41` trunca `framesPerPacket` (44.1 kHz → 44 → −2268 ppm sin compensación cuando no hay feedback).
- Constantes inconsistentes: PID limita ±8, `getAdjustedFrameCount` clampea ±4, el headroom de buffer reserva ±4 (`UsbTransferManager.cpp:508`).
- `expectedRate` UAC2 asume high-speed (`/8000`) en vez de derivar de `packetsPerSecond` real.

### Diseño

Refactor **in-place** de `ClockController` (mismo archivo, misma clase, para no tocar call sites). Se elimina `PIDController` (solo lo usa este archivo) y el buffer de moving-average de 16 taps; se reemplaza por EMA + acumulador fraccional.

```cpp
class ClockController {
public:
    // NUEVO: el nominal se define en frames-por-packet FRACCIONAL.
    // packetsPerSecond viene de TransferConfig (ya existe), NO se asume
    // 1000/8000 por versión UAC.
    void configure(int sampleRateHz, int packetsPerSecond) {
        mNominalFramesPerPacket = double(sampleRateHz) / double(packetsPerSecond);
        mSampleRate = sampleRateHz;
        mPacketsPerSecond = packetsPerSecond;
        reset();
    }

    // processFeedback: parsea Ff (10.14 UAC1 / 16.16 UAC2, sin cambios en
    // parseFeedbackValue) y lo convierte a frames-por-PACKET del data EP:
    //
    //   unitsPerPacket = unidadDeFf_por_segundo / packetsPerSecond
    //     UAC1 FS : Ff en samples/frame(1ms)      → units/s = 1000
    //     UAC2 HS : Ff en samples/microframe(125µs)→ units/s = 8000
    //
    //   targetFramesPerPacket = Ff * (unitsPerSecond / packetsPerSecond)
    //
    // Validación: descartar si |target/nominal - 1| > 0.1 (10 %); contar en
    // feedbackPacketsInvalid. Suavizado: EMA con alpha = 0.10.
    void processFeedback(const uint8_t* data, int length, UacVersion version);

    // NUEVO: entrada para feedback implícito (0.2). Mismo camino que el
    // explícito, pero la medición viene del rate de captura.
    void setMeasuredFramesPerPacket(double framesPerPacket);

    // Acumulador fraccional puro — SIN PID:
    int getAdjustedFrameCount(int nominalFrames) {
        const double target = mHasMeasurement
            ? mTargetFramesPerPacket.load(std::memory_order_acquire)
            : mNominalFramesPerPacket;   // ← arregla 44.1 kHz sin feedback
        mAccum += target;
        int frames = static_cast<int>(mAccum);   // floor (mAccum siempre > 0)
        mAccum -= frames;
        // Clamp a una ÚNICA constante compartida con el headroom de buffer.
        const int lo = nominalFrames - kClockAdjustFramesMax;
        const int hi = nominalFrames + kClockAdjustFramesMax;
        if (frames < lo) { mAccum += (frames - lo); frames = lo; }
        if (frames > hi) { mAccum += (frames - hi); frames = hi; }
        // Anti-windup del catch-up: el residuo devuelto al acumulador se
        // limita a ±2*kClockAdjustFramesMax para que un transitorio no
        // produzca ráfagas largas de packets clampleados.
        mAccum = std::clamp(mAccum, -2.0*kClockAdjustFramesMax,
                                     +2.0*kClockAdjustFramesMax);
        return frames;
    }
};
```

**Constante única:** mover `CLOCK_ADJUST_FRAMES_MAX = 4` a `UsbConstants.h` como `kClockAdjustFramesMax` y usarla en `ClockController` y en `UsbTransferManager::allocateTransfers()` (hoy duplicada en `UsbTransferManager.cpp:508`). El headroom de buffer de salida sigue siendo `kClockAdjustFramesMax * bytesPerFrame` por packet — sin cambios de layout.

**Notas de borde:**
- `getAdjustedFrameCount` se llama N veces por transfer (una por fill de transfer completo en `fillOutputTransfer`, que hoy usa un único valor para todos los packets del transfer). Mantener ese contrato: el valor devuelto se aplica uniforme al transfer y el acumulador debe avanzar `packetCount × target` por fill. **Cambiar la firma a `getAdjustedFrameCount(int nominalFrames, int packetCount)`** que acumule `packetCount * target` y reparta el entero — evita el sesgo de redondear una vez y multiplicar.
- Estados sin medición (`mHasMeasurement == false`): arranque, endpoints sync/adaptive, feedback inválido sostenido. El nominal fraccional ya genera el patrón 44-44-44-45 correcto a 44.1 kHz FS.
- `mFrameAdjustment`/`getDriftPpm`/`getCurrentSampleRate` se mantienen para estadísticas (derivados de la EMA), mismas firmas.

### Archivos
- `backends/ClockController.h` — refactor descrito; eliminar `PIDController`.
- `usb/UsbConstants.h` — `kClockAdjustFramesMax`.
- `usb/UsbTransferManager.cpp` — `configure()` llama `mClockController->configure(rate, packetsPerSecond)`; `fillOutputTransfer` usa la firma nueva con `packetCount`.

### Tests (`usb/tests/test_clock_controller.cpp` — reescritura parcial)
1. **Dirección** (el test que faltaba): dispositivo +100 ppm via feedback UAC1 → tras converger, la suma de `getAdjustedFrameCount` sobre 10 000 packets ≈ `48.0048 × 10000` ± 2 frames. Espejo a −100 ppm.
2. **44.1 kHz sin feedback, FS**: 1000 packets → total = 44100 ± 1.
3. **44.1 kHz sin feedback, HS bInterval=1**: 8000 packets → total = 44100 ± 1 (nominal 5.5125 frames/packet).
4. **UAC2 HS con packetsPerSecond=1000** (bInterval=4): verifica que la conversión de unidades usa `packetsPerSecond` y no `/8000`.
5. **Clamp + anti-windup**: feedback aberrante (+5 %) → frames clampeados a nominal+4 y acumulador acotado; al volver el feedback a nominal, recuperación sin ráfaga > `2*kClockAdjustFramesMax`.
6. **Rechazo**: Ff fuera de ±10 % no altera el target (cuenta como inválido).
7. Mantener los tests de parsing 10.14/16.16 existentes.

---

## 0.2 — Feedback implícito real (C2)

### Problema
`UsbTransferManager.cpp:182-192` detecta el feedback implícito y lo descarta ("clock sync via packet timing") pero **nada mide ese timing**. Interfaces full-duplex asíncronas sin EP de feedback explícito corren a nominal → drift libre.

### Diseño

Medir el rate real del stream de **entrada** (la captura es síncrona al clock del dispositivo: éste envía exactamente los frames que su clock produce por intervalo de servicio) y usarlo como Ff para la salida.

En `UsbTransferManager`:

```cpp
// Estado nuevo (solo tocado desde el event thread — sin atomics extra):
struct ImplicitFeedbackEstimator {
    uint64_t frames = 0;        // frames de entrada acumulados
    uint64_t packets = 0;       // packets de entrada (intervalos de servicio)
    static constexpr uint64_t kWindowPackets = 256;  // ~256 ms FS / 32 ms HS biv=1

    // Devuelve nullopt hasta completar la primera ventana.
    std::optional<double> onPackets(uint64_t newFrames, uint64_t newPackets);
};
```

En `processInputTransfer` (`UsbTransferManager.cpp:1157`): tras el loop de packets, si `mImplicitFeedbackActive`, acumular `totalFrames = totalSamples / inputChannelCount` y `ctx->packetCount` packets (los packets con `actual_length == 0` **cuentan como intervalo transcurrido con 0 frames** — eso es exactamente la información de rate). Al cerrar ventana:

```cpp
double inFramesPerInPacket = frames / double(packets);
// Normalizar al cadence del EP de SALIDA (puede diferir si los bInterval
// difieren — ver L6/Fase 2; hasta entonces packetsPerSecond es común):
double outFramesPerOutPacket = inFramesPerInPacket
    * (inputPacketsPerSecond / double(outputPacketsPerSecond)); // hoy = 1.0
mClockController->setMeasuredFramesPerPacket(outFramesPerOutPacket);
```

`setMeasuredFramesPerPacket` aplica la misma validación ±10 % y EMA (alpha 0.05 — ventanas largas, medición más limpia que el feedback por packet).

**Activación** (en `LibusbBackend::setupTransferManager`, `LibusbBackend.cpp:1466-1485`):
- `mSelectedPlayback->dataEndpoint.isAsync()` **y** sin feedback explícito **y** `mSelectedCapture` activo → `mTransferManager->setImplicitFeedbackEnabled(true)`.
- Caso degradado: PLAYBACK_ONLY asíncrono con feedback marcado implícito pero sin stream de entrada → queda en nominal fraccional (sin regresión vs hoy, y 0.1 ya arregla 44.1).
- En FULL_DUPLEX **aunque el EP no declare implícito**: si el dispositivo es asíncrono y de un solo clock (lo normal), esclavizar la salida al rate de captura es válido y además mantiene los dos rings balanceados. Activarlo por default en duplex cuando no hay feedback explícito; flag para desactivar por dispositivo si aparece hardware con clocks separados (`UsbClockGraph` ya sabe si los terminales comparten clock source — usarlo como gate cuando esté disponible).

**Resolución de la medición:** con ventana de 256 packets FS (256 ms), la cuantización de 1 frame ≈ 81 ppm por ventana; la EMA reduce el ruido por debajo de ±20 ppm en ~5 ventanas. Suficiente: el ring de la Fase 1 (4–5 ms ≈ 200 frames) tolera ese residuo durante horas.

### Archivos
- `usb/UsbTransferManager.{h,cpp}` — estimador, `setImplicitFeedbackEnabled()`, hook en `processInputTransfer`.
- `backends/LibusbBackend.cpp` — lógica de activación.
- Estadísticas: reusar `feedbackEffectiveFramesPerPacket` / `driftPpm` (mismos atomics, la UI no cambia).

### Tests
- Test unitario del `ImplicitFeedbackEstimator` (extraíble, sin libusb): secuencias de (frames, packets) simulando +200 ppm → estimación converge a 48.0096±0.001 frames/packet en N ventanas.
- Test integrado con `ClockController`: estimador → `setMeasuredFramesPerPacket` → la suma de `getAdjustedFrameCount` sigue el rate simulado (mismo criterio que test 1 de 0.1).

---

## 0.3 — Parser UAC1: feedback EP por bSynchAddress/bRefresh y protección del data EP (C4)

### Problema
`UsbDescriptorParser.cpp:133-176`:
1. Solo clasifica feedback por usage bits `01`. Dispositivos UAC1 legacy (USB 1.1) usan `bmAttributes=0x01` (usage 00) + `bSynchAddress`/`bRefresh` en el data EP.
2. La rama `else` asigna `dataEndpoint = endpoint` sin verificar dirección → un EP iso IN (sync legacy) en una interfaz de playback **sobrescribe** el data EP OUT.
3. Los descriptores de endpoint de audio UAC1 son de **9 bytes** (`bLength=9`: `bRefresh` en offset 7, `bSynchAddress` en offset 8) y el parser los lee como estándar de 7.

### Diseño

**a) Parseo de los campos de audio-endpoint.** En `parseEndpointDescriptor`:

```cpp
struct UsbEndpointInfo {
    // ... existentes ...
    uint8_t refresh = 0;        // bRefresh (UAC1, 0 si no presente)
    uint8_t synchAddress = 0;   // bSynchAddress (UAC1, 0 si no presente)
};
// en el parser:
if (desc->bLength >= 9) {
    ep.refresh      = data[7];
    ep.synchAddress = data[8];
}
```

**b) Clasificación en dos pasadas por altsetting.** Hoy la clasificación es greedy por orden de aparición. Cambiar a: acumular los endpoints iso del altsetting en `mContext.pendingEndpoints` y resolver al cerrar el altsetting (donde hoy se hace el push a `playbackInterfaces/captureInterfaces`, `UsbDescriptorParser.cpp:200-211` y `252-263`):

```
1. dataEndpoint  = primer EP iso cuyo usage != FEEDBACK
                   (si hay dos candidatos con direcciones opuestas, el que
                    tenga usage == DATA(00) y maxPacketSize coherente con
                    los formatos parseados; ver regla c)
2. feedbackEndpoint (explícito) si existe EP con usage == FEEDBACK,
   dirección OPUESTA al data EP, iso.
3. feedbackEndpoint (legacy UAC1) si NO hubo (2) y:
   dataEndpoint.synchAddress != 0 y existe EP iso cuyo address ==
   dataEndpoint.synchAddress → ese EP es feedback explícito.
4. feedbackEndpoint (implícito) si usage == IMPLICIT_FB en el data EP
   (lógica actual, sin cambios).
```

**c) Regla de dirección del data EP:** la dirección de la interfaz la define su terminal link / el primer EP de datos. Una vez fijada (primer EP con usage != FEEDBACK), **ningún EP de dirección opuesta puede reemplazar `dataEndpoint`**. Esto solo elimina el clobbering; no rechaza topologías raras.

**d) Cadencia del transfer de feedback (UAC1):** propagar `refresh` a `UsbFeedbackEndpoint` y, en `UsbTransferManager::allocateTransfers` (`:636-665`), documentar que el período real de actualización es `2^(bRefresh-1)` ms; el transfer de 1 packet re-submitido a completion ya respeta ese ritmo (el host controller solo completa cuando el dispositivo transmite), así que **no se requiere cambio funcional** — solo registrar `refresh` en el log y en stats para diagnóstico.

### Archivos
- `usb/UsbAudioTypes.h` — campos `refresh`/`synchAddress`.
- `usb/UsbDescriptorParser.{h,cpp}` — parseo de 9 bytes + clasificación en dos pasadas.
- `usb/UsbTransferManager.cpp` — log de refresh.

### Tests (`usb/tests/test_usb_descriptor_parser.cpp` — extender)
1. **Dispositivo UAC1 legacy**: playback alt con data EP OUT (`bLength=9`, `bSynchAddress=0x81`, `bRefresh=4`) + EP IN `0x81` iso usage 00 → debe producir `dataEndpoint=OUT` y `feedbackEndpoint` explícito en `0x81`. (Hoy este fixture produce dataEndpoint=0x81 — test de regresión del clobbering.)
2. **Orden inverso**: el EP IN aparece *antes* que el OUT en el descriptor → mismo resultado.
3. **Usage bits modernos**: los fixtures existentes (`test_usb_descriptor_parser.cpp:191-298`) siguen pasando sin cambios.
4. **EP de 7 bytes**: `refresh/synchAddress` quedan en 0, sin feedback fantasma.

---

## 0.4 — Coerción de sample rate: abortar y reconfigurar (C5)

### Problema
`LibusbBackend.cpp:867-871`: si el GET_CUR revela que el dispositivo coercionó el rate, se actualiza `mRequestedSampleRate` **después** de que `TransferConfig` (framesPerPacket, rings, ClockController) fue calculado con el rate viejo → drift sistemático del cociente de rates.

### Diseño

El hook de clock (`configureSampleRate`, ejecutado dentro de `UsbTransferManager::start()`) ya no "acepta" la coerción en silencio:

```cpp
// LibusbBackend
std::atomic<int> mNegotiatedSampleRate{0};   // escrito por el hook

// en configureSampleRate(), rama UAC1 y UAC2:
if (actual != requested) {
    LOGW("Device coerced %u -> %u Hz; restart required", requested, actual);
    mNegotiatedSampleRate.store(int(actual));
    return false;            // ← aborta el start del transfer manager
}
```

En `LibusbBackend::start()` (`:1110-1115`), tras el fallo de `mTransferManager->start()`:

```cpp
for (int attempt = 0; attempt < 2; ++attempt) {
    if (mTransferManager->start()) break;
    const int coerced = mNegotiatedSampleRate.exchange(0);
    if (coerced > 0 && coerced != mRequestedSampleRate && attempt == 0) {
        LOGI("Retrying start at device rate %d Hz", coerced);
        mRequestedSampleRate = coerced;
        teardownTransferManager();
        if (!setupTransferManager()) return ERROR_USB_INIT_FAILED;
        continue;            // segundo intento con el rate real
    }
    teardownTransferManager();
    return BackendResult::ERROR_STREAM_FAILED;
}
```

- Máximo 1 reintento (si el dispositivo vuelve a coercionar, error).
- El rate efectivo queda visible en `StreamInfo.sampleRate` — el consumidor (NoisyPad) ya lo lee tras `start()` según el contrato de `IAudioBackend::getStreamInfo()`.
- Caso UAC2: la coerción se chequea por clock source (el GET_CUR de `:883+` ya existe); aplicar la misma regla.

### Tests
- Lógica de retry extraída a helper puro testeable (decisión `(resultado hook, coercedRate, attempt) → acción`) en `usb/` con unit test; el flujo completo se valida en hardware (forzar 44.1 kHz en un DAC que solo acepta 48 kHz).

---

## 0.5 — Latencia reportada real (L7)

### Problema
`TransferStatistics::currentLatencyMs` solo se escribe en `reset()`; `getOutputLatencyMs()/getInputLatencyMs()` (`LibusbBackend.cpp:1273-1288`) devuelven 0 con el stream corriendo.

### Diseño

Calcular en el **event thread** (donde ya se actualizan las stats, costo O(1)):

```cpp
// UsbTransferManager, al final de handleOutputComplete():
const double framesPerTransfer = double(mConfig.packetsPerTransfer)
                               * mConfig.framesPerPacket;
const double ringFrames   = mOutputRingBuffer.availableToRead()
                          / double(mConfig.channelCount);
// En vuelo: transfers pendientes; en promedio la mitad ya fue consumida.
const double inflight     = mOutputPendingCount.load() * framesPerTransfer * 0.5;
const float outMs = float((ringFrames + inflight) * 1000.0 / mConfig.sampleRate);
mStats.currentLatencyMs.store(outMs, std::memory_order_relaxed);
// EMA para avgLatencyMs (alpha 0.05):
mStats.avgLatencyMs.store(0.95f*avg + 0.05f*outMs, std::memory_order_relaxed);
```

```cpp
// Nuevo atomic en TransferStatistics:
std::atomic<float> currentInputLatencyMs{0.0f};
// al final de handleInputComplete():
inputMs = (inputRingFrames + framesPerTransfer * 0.5) * 1000.0 / sampleRate;
```

`LibusbBackend::getInputLatencyMs()` pasa a leer `currentInputLatencyMs` (hoy devuelve la de salida, `:1285`). Documentar en el header que esto es **latencia de software del lado host**; el round-trip total medido (con conversores y URB scheduling) lo da la Fase 5.

Exponer también `getRoundTripLatencyMs()` (override del default de `IAudioBackend.h:374`) = out + in.

### Tests
- Aritmética extraída a helper puro (`usb/UsbLatencyMath.h`): `(ringSamples, channels, pendingTransfers, framesPerTransfer, sampleRate) → ms`, con unit tests de casos típicos FS/HS.
- Validación cruzada en Fase 5: `medido_analógico ≈ outMs + inMs + (1–3 ms de conversores/URB)`.

---

## Criterios de aceptación de la fase

1. Suite `usb/tests` verde, incluyendo los tests nuevos de dirección de ajuste, 44.1 kHz y clobbering del parser.
2. En los 3 DACs: stream duplex de 30 min a 48 kHz y a 44.1 kHz **sin** underruns/overruns acumulados (delta de `WMA_AUDIT` = 0 en régimen), con `driftPpm` estable y `feedbackEffectiveFramesPerPacket` coherente con el rate nominal.
3. `getOutputLatencyMs()/getInputLatencyMs()` reportan valores > 0 consistentes con la configuración (≈ 32 ms / ≈ 10 ms con la config por defecto actual).
4. Dispositivo coercionador de rate: el stream arranca al rate del dispositivo, sin distorsión ni drift.

## Riesgos
- Hardware con feedback fuera de spec (Ff en formato erróneo): mitigado por la validación ±10 % + fallback a nominal fraccional.
- El reintento de start (0.4) duplica el tiempo de arranque en el caso coercionado (~+100–200 ms): aceptable, es excepcional.
- La clasificación de endpoints en dos pasadas cambia el parser para *todos* los dispositivos: cubrir con los fixtures existentes antes de merge (test 3 de 0.3).
