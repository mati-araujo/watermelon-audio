# Etapa 1 — Fundamentos críticos

**Estado:** propuesta — no iniciada.
**Dependencias:** ninguna. Es la primera etapa y bloquea todas las demás.
**Duración estimada:** 3–5 días de trabajo concentrado.
**Severidad de los bugs que resuelve:** 2× Crítico, 1× Mayor.

---

## 1. Objetivo

Dejar funcionando "de verdad" el path feliz del backend USB eliminando los tres defectos que hoy le impiden ser profesional aun con devices compatibles en el allowlist:

1. **Implementar la negociación de sample rate** vía `libusb_control_transfer` (SET_CUR) tanto en UAC 1.0 (endpoint request) como en UAC 2.0 (clock source interface request). Hoy está declarado (`LibusbBackend.h:387`) pero sin implementación, y el device se queda en su default interno.
2. **Corregir el procesamiento del feedback endpoint para UAC 1.0**: pasar la versión UAC explícita (no inferir por tamaño), usar 3 bytes en UAC1 y 4 bytes en UAC2, y validar que el endpoint realmente sea `SYNCH_FEEDBACK`.
3. **Reemplazar el polling con sleep 200 µs del DSP thread** por una sincronización event-driven que reciba señal directa desde las callbacks de transfer USB cuando hay datos consumibles / espacio disponible.

Al terminar la etapa, un device UAC 1.0 con feedback endpoint debe poder abrirse a 48 kHz y mantener drift < 50 PPM durante al menos 10 minutos sin clicks; un device UAC 2.0 debe poder alternar entre 44.1 y 48 kHz sin reconexión; y el jitter p99 del DSP callback debe ser < 500 µs sostenido.

---

## 2. Archivos tocados

| Archivo | Tipo de cambio |
|---|---|
| `audio/src/main/cpp/backends/LibusbBackend.cpp` | **nuevo método** `configureSampleRate()`; reemplazo del DSP loop por espera event-driven |
| `audio/src/main/cpp/backends/LibusbBackend.h` | declarar `mRateReadySem`, `waitForStreamData()`, remover el comentario stub del declarador existente |
| `audio/src/main/cpp/usb/UsbTransferManager.cpp` | fix de `handleFeedbackComplete`; llamadas al semáforo desde `handleOutputComplete`/`handleInputComplete` |
| `audio/src/main/cpp/usb/UsbTransferManager.h` | acepta `UacVersion` en el constructor; exponer `setDataReadySignal(std::counting_semaphore*)` |
| `audio/src/main/cpp/backends/ClockController.h` | mantener firma, pero añadir test hooks para tuning del PID |
| `audio/src/main/cpp/usb/UsbConstants.h` | añadir constantes `UAC_FEEDBACK_LENGTH_UAC1 = 3`, `UAC_FEEDBACK_LENGTH_UAC2 = 4` |
| **Nuevo**: `audio/src/test/cpp/usb/ClockController_test.cpp` | test con feedback stubbed |
| **Nuevo**: `audio/src/test/cpp/usb/SampleRateNegotiation_test.cpp` | test con libusb mock |

---

## 3. Tareas

### 3.1 Implementar `LibusbBackend::configureSampleRate()`

**Problema.** El método existe en el header (`LibusbBackend.h:387`) pero no tiene cuerpo en el `.cpp`. Nada en el subsistema envía jamás un SET_CUR al clock.

**Diseño del fix.**

El método debe recibir el sample rate deseado, usar `mUsbDevice->uacVersion` para decidir la ruta, y hacer la validación post-escritura con GET_CUR.

```cpp
// Nuevo en LibusbBackend.cpp
bool LibusbBackend::configureSampleRate() {
    if (!mDeviceHandle || !mUsbDevice) return false;

    const int version = mUsbDevice->uacVersion;
    const uint32_t rate = static_cast<uint32_t>(mRequestedSampleRate);

    if (version == 1) {
        // UAC 1.0: class-specific endpoint request to the data endpoint
        // bmRequestType = 0x22 (H2D | Class | Endpoint)
        // bRequest      = SET_CUR (0x01)
        // wValue        = SAMPLING_FREQ_CONTROL << 8 (0x0100)
        // wIndex        = data endpoint address
        // wLength       = 3, data = 24-bit LE rate
        if (!mSelectedPlayback) return false;
        uint8_t payload[3] = {
            static_cast<uint8_t>(rate & 0xff),
            static_cast<uint8_t>((rate >> 8) & 0xff),
            static_cast<uint8_t>((rate >> 16) & 0xff),
        };
        int r = libusb_control_transfer(
            mDeviceHandle,
            /*bmRequestType*/ 0x22,
            /*bRequest*/      UAC_REQUEST_SET_CUR,
            /*wValue*/        static_cast<uint16_t>(UAC1_EP_SAMPLING_FREQ_CONTROL << 8),
            /*wIndex*/        mSelectedPlayback->dataEndpoint.address,
            payload, 3,
            /*timeout*/       1000);
        if (r < 0) {
            LOGE("UAC1 SET_CUR sample rate failed: %s", libusb_error_name(r));
            return false;
        }

        // Verify with GET_CUR — some devices coerce to nearest supported rate
        uint8_t read[3] = {0};
        int g = libusb_control_transfer(
            mDeviceHandle, 0xA2, UAC_REQUEST_GET_CUR,
            static_cast<uint16_t>(UAC1_EP_SAMPLING_FREQ_CONTROL << 8),
            mSelectedPlayback->dataEndpoint.address,
            read, 3, 1000);
        if (g == 3) {
            uint32_t actual = read[0] | (read[1] << 8) | (read[2] << 16);
            if (actual != rate) {
                LOGW("Device coerced rate %u -> %u", rate, actual);
                mRequestedSampleRate = static_cast<int>(actual);
            }
        }
        return true;
    }

    if (version == 2) {
        // UAC 2.0: interface request to the clock source unit
        // bmRequestType = 0x21 (H2D | Class | Interface)
        // bRequest      = CUR (0x01)
        // wValue        = CS_SAM_FREQ_CONTROL << 8 | channel (0)
        // wIndex        = clockSourceID << 8 | controlInterfaceNum
        // wLength       = 4, data = 32-bit LE rate
        if (mUsbDevice->clockSources.empty()) {
            LOGE("UAC2 device has no parsed clock sources");
            return false;
        }
        // For now pick the first clock source; stage 3 introduces selection.
        const auto& clockSrc = mUsbDevice->clockSources.front();
        uint8_t payload[4] = {
            static_cast<uint8_t>(rate & 0xff),
            static_cast<uint8_t>((rate >> 8) & 0xff),
            static_cast<uint8_t>((rate >> 16) & 0xff),
            static_cast<uint8_t>((rate >> 24) & 0xff),
        };
        uint16_t wIndex = static_cast<uint16_t>(
            (clockSrc.clockId << 8) | mUsbDevice->controlInterface);
        int r = libusb_control_transfer(
            mDeviceHandle,
            /*bmRequestType*/ 0x21,
            /*bRequest*/      UAC2_REQUEST_CUR,
            /*wValue*/        static_cast<uint16_t>(UAC2_CS_SAM_FREQ_CONTROL << 8),
            wIndex,
            payload, 4,
            /*timeout*/       1000);
        if (r < 0) {
            LOGE("UAC2 SET_CUR sample rate failed: %s", libusb_error_name(r));
            return false;
        }
        // Verify
        uint8_t read[4] = {0};
        int g = libusb_control_transfer(
            mDeviceHandle, 0xA1, UAC2_REQUEST_CUR,
            static_cast<uint16_t>(UAC2_CS_SAM_FREQ_CONTROL << 8),
            wIndex, read, 4, 1000);
        if (g == 4) {
            uint32_t actual = read[0] | (read[1] << 8) | (read[2] << 16) | (read[3] << 24);
            if (actual != rate) {
                LOGW("Device coerced rate %u -> %u", rate, actual);
                mRequestedSampleRate = static_cast<int>(actual);
            }
        }
        return true;
    }

    LOGE("Unknown UAC version %d — cannot negotiate sample rate", version);
    return false;
}
```

**Invocación.** En `LibusbBackend::start()`, insertar la llamada **después** de `selectBestInterfaces()` y **antes** de `setupTransferManager()`. Si falla, retornar `BackendResult::ERROR_INVALID_CONFIG` con el rate loggeado para que el Kotlin pueda decidir si fallback a Oboe.

**Constantes que faltan.** Agregar en `usb/UsbConstants.h`:
```cpp
constexpr uint8_t UAC1_EP_SAMPLING_FREQ_CONTROL = 0x01;  // UAC1 spec 5.2.3.2.3.1
constexpr uint8_t UAC2_REQUEST_CUR              = 0x01;  // UAC2 spec 5.2.1
```
(ya existe `UAC2_CS_SAM_FREQ_CONTROL = 0x01` en la línea 111, reusar.)

### 3.2 Propagar versión UAC al `UsbTransferManager`

**Problema.** `handleFeedbackComplete()` en `UsbTransferManager.cpp:637-641` infiere la versión por el tamaño del paquete (`if (length >= 4) version = UAC_2_0`), lo cual es frágil — un device UAC1 con padding devolverá 4 y será tratado como UAC2.

**Fix.**

1. Añadir un campo miembro `UacVersion mUacVersion = UacVersion::UNKNOWN;` en `UsbTransferManager.h`.
2. Exponer un setter `void setUacVersion(UacVersion v) { mUacVersion = v; mClockController->setUacVersion(v); }` llamado desde `LibusbBackend::setupTransferManager()` con `mUsbDevice->uacVersion`.
3. En `handleFeedbackComplete` usar `mUacVersion` en lugar de la heurística por longitud.
4. En `allocateTransfers`, calcular el packet length del feedback según versión:
   ```cpp
   int feedbackLen = (mUacVersion == UacVersion::UAC_1_0)
                         ? UAC_FEEDBACK_LENGTH_UAC1  // 3
                         : UAC_FEEDBACK_LENGTH_UAC2; // 4
   libusb_set_iso_packet_lengths(mFeedbackTransfer, feedbackLen);
   ```
5. Antes de `libusb_fill_iso_transfer(mFeedbackTransfer, ...)`, verificar que el endpoint guardado en `mFeedbackEndpoint->endpoint.attributes` tenga los bits de sync type en `LIBUSB_ISO_USAGE_TYPE_FEEDBACK` (`= 0x10`). Si no, loggear warning y desactivar feedback.

### 3.3 Validar endpoint como feedback real

**Problema.** `selectBestInterfaces()` asigna `mTransferManager->setFeedbackEnabled(true, &(*mSelectedPlayback->feedbackEndpoint))` sin verificar que el endpoint sea isochronous-in con usage type `Feedback`.

**Fix.** Añadir en `UsbTransferManager::setFeedbackEnabled`:

```cpp
if (endpoint) {
    const uint8_t xfer = endpoint->endpoint.attributes & 0x03;  // transfer type
    const uint8_t sync = (endpoint->endpoint.attributes >> 2) & 0x03;  // sync type
    const uint8_t usage = (endpoint->endpoint.attributes >> 4) & 0x03; // usage type
    const bool isIn = (endpoint->endpoint.address & 0x80) != 0;
    const bool isIsoc = (xfer == 0x01);
    const bool isFeedbackUsage = (usage == 0x01);  // USB 9.6.6 Table 9-14

    if (!isIn || !isIsoc || !isFeedbackUsage) {
        LOGW("Endpoint 0x%02X is not a feedback endpoint (xfer=%u sync=%u usage=%u dir=%c)",
             endpoint->endpoint.address, xfer, sync, usage, isIn ? 'I' : 'O');
        mFeedbackEnabled = false;
        mFeedbackEndpoint.reset();
        return;
    }
    mFeedbackEndpoint = *endpoint;
}
mFeedbackEnabled = enabled;
```

### 3.4 DSP thread event-driven

**Problema.** `LibusbBackend::dspThreadFunc()` en `LibusbBackend.cpp:705` hace polling con `sleep_for(200µs)` cuando el ring buffer no tiene suficientes datos. Esto introduce jitter y no escala a buffers pequeños.

**Diseño.** Usar `std::counting_semaphore` (C++20, disponible con `minSdk 29` y NDK 26+). El semáforo se posta desde `handleOutputComplete()` después de `fillOutputTransfer()` (hay espacio en el ring para más DSP output) y desde `handleInputComplete()` después de `processInputTransfer()` (hay más input disponible para el DSP).

**Pasos concretos.**

1. En `LibusbBackend.h`, añadir:
   ```cpp
   #include <semaphore>

   private:
       // Signaled by the USB event thread when data is consumable/space is free.
       // max count bounded by the number of pending transfers (safe upper bound).
       std::counting_semaphore<64> mDspWake{0};

       // Bail-out flag for spurious wakeups during shutdown
       std::atomic<bool> mDspWakeDueToStop{false};
   ```

2. En `LibusbBackend::dspThreadFunc()`, reemplazar el bucle `if (!outputReady || !inputReady) sleep(200µs); continue;` por:
   ```cpp
   // Wait for either space/data or a stop request. Timeout is a safety net.
   (void) mDspWake.try_acquire_for(std::chrono::milliseconds(5));
   if (!mDspRunning.load(std::memory_order_acquire)) break;
   // Recompute readiness — a wake can be for any of input/output/feedback
   bool outputReady = !mSelectedPlayback ||
       (mTransferManager->getOutputBufferAvailable() >= outputSamples);
   bool inputReady = !mSelectedCapture ||
       (mTransferManager->getInputBufferAvailable() >= inputSamples);
   if (!outputReady || !inputReady) continue;
   ```

3. En `UsbTransferManager::handleOutputComplete`, después de `fillOutputTransfer(ctx)`, si el `LibusbBackend` registró un semáforo, llamarlo con `release(1)`. Idem en `handleInputComplete` y `handleFeedbackComplete` (por si la clock adjust hace que el frame count cambie suficientemente como para que el DSP deba corregir).

4. Exponer desde `UsbTransferManager` un setter:
   ```cpp
   void setDataReadySignal(std::counting_semaphore<64>* sem) { mDataReadySignal = sem; }
   ```
   El `LibusbBackend::setupTransferManager()` lo registra tras `configure()`.

5. En `LibusbBackend::stop()`, después de `mDspRunning.store(false)`, llamar `mDspWake.release(8)` (signal burst) para desbloquear cualquier espera pendiente antes del `join()`.

**Consideración RT.** `std::counting_semaphore::release()` en libc++ para Android usa `futex` y es wait-free en el path feliz (si no hay waiters). Es seguro desde la callback de libusb, que corre en un thread no-RT pero priorizado.

**Métrica verificable.** Antes del fix, medir jitter p99 del DSP callback durante 60 segundos con buffer 256. Después del fix, el mismo test debe mostrar p99 < 500 µs (era ≥ 1 ms antes).

### 3.5 Stop-time waking

El DSP loop corre con `try_acquire_for(5 ms)` para no quedarse dormido si algo falla upstream. Al hacer `stop()`, además del `mDspRunning = false`, se hace `mDspWake.release(N)` donde `N` cubre cualquier cantidad razonable de iteraciones pendientes. Esto evita esperar hasta 5 ms por iteración durante el shutdown.

### 3.6 Logging mínimo de negociación

Incluir en `configureSampleRate()`:
```cpp
LOGI("Rate negotiation: UAC%d req=%u actual=%u clockSrc=%d",
     version, rate, negotiatedRate, clockSrcId);
```

Esto alimenta la compatibility matrix y la depuración de devices nuevos.

---

## 4. Tests verificables

### 4.1 Unit tests C++ (nuevo directorio `audio/src/test/cpp/usb/`)

**`ClockController_test.cpp`**

```cpp
TEST(ClockController, Uac1FeedbackParsedAsThreeBytes) {
    ClockController ctrl(48000);
    // 48.0 samples per frame in 10.14 format = 48 * 16384 = 786432 = 0x0C0000
    uint8_t feedback[3] = {0x00, 0x00, 0x0C};
    ctrl.processFeedback(feedback, 3, UacVersion::UAC_1_0);
    EXPECT_NEAR(ctrl.getCurrentSampleRate(), 48000.0f, 1.0f);
    EXPECT_LT(std::abs(ctrl.getDriftPpm()), 50.0f);
}

TEST(ClockController, Uac2FeedbackParsedAsFourBytes) {
    ClockController ctrl(48000);
    // 6.0 samples per microframe in 16.16 = 6 * 65536 = 393216 = 0x00060000
    uint8_t feedback[4] = {0x00, 0x00, 0x06, 0x00};
    ctrl.processFeedback(feedback, 4, UacVersion::UAC_2_0);
    EXPECT_NEAR(ctrl.getCurrentSampleRate(), 48000.0f, 1.0f);
}

TEST(ClockController, DriftConvergesUnderNoise) {
    ClockController ctrl(48000);
    // Simulate a device running 100 PPM fast
    for (int i = 0; i < 1000; ++i) {
        // 48.0048 samples per frame (100 PPM fast)
        float rate = 48.0048f + (i % 3 - 1) * 0.001f;  // small noise
        uint32_t raw = static_cast<uint32_t>(rate * 16384.0f);
        uint8_t bytes[3] = {
            static_cast<uint8_t>(raw & 0xff),
            static_cast<uint8_t>((raw >> 8) & 0xff),
            static_cast<uint8_t>((raw >> 16) & 0xff),
        };
        ctrl.processFeedback(bytes, 3, UacVersion::UAC_1_0);
    }
    // After convergence, drift should be < 10 PPM residual
    EXPECT_LT(std::abs(ctrl.getDriftPpm() - 100.0f), 10.0f);
}
```

**`SampleRateNegotiation_test.cpp`** — usa un mock de `libusb_control_transfer` (puede ser un `#define` redirigible en el header para compilar en modo test).

```cpp
TEST(SampleRateNegotiation, Uac1SendsCorrectWindowAndLength) {
    MockLibusb mock;
    mock.expectControlTransfer(0x22, 0x01, 0x0100, /*wIndex=*/0x81,
                                /*wLength=*/3, {0x80, 0xBB, 0x00});  // 48000 = 0x00BB80
    LibusbBackend backend;
    // ... configure with mock device, mUacVersion = 1
    backend.setSampleRate(48000);
    EXPECT_TRUE(backend.configureSampleRate());
}

TEST(SampleRateNegotiation, Uac2UsesClockSourceIdInWIndex) {
    MockLibusb mock;
    mock.expectControlTransfer(0x21, 0x01, 0x0100,
                                /*wIndex=*/(0x09 << 8) | 0x00,  // clockId=9, intf=0
                                /*wLength=*/4, {0x80, 0xBB, 0x00, 0x00});
    LibusbBackend backend;
    backend.setSampleRate(48000);
    // mUacVersion = 2, clockSources[0].clockId = 9, controlInterface = 0
    EXPECT_TRUE(backend.configureSampleRate());
}
```

### 4.2 Smoke test de device real

Añadir a `UsbAudioTestRunner.kt` un test presset nuevo: **`RATE_NEGOTIATION_SWEEP`**.

Itera sobre `[44100, 48000, 88200, 96000, 176400, 192000]`, llama `startStreaming(rate, 2, 24)` por cada uno durante 2 segundos, valida que:
- El device no rechazó la apertura.
- La transfer statistics muestran `packetsCompleted > 0`.
- El `getCurrentSampleRate()` del clock controller converge a ± 0.1% del requested.
- Los underrun counters no crecen sostenidamente.

Genera un reporte por device con qué rates aceptó.

### 4.3 Medición de jitter p99

Con el device referencia (Scarlett Solo), correr 60 segundos de playback 48 kHz/24 bit con buffer 256. Comparar `getProfilingStats().dspCallback.p99LatencyUs` antes y después del fix del DSP thread. Debe caer de algún valor ≥ 1000 µs a < 500 µs.

---

## 5. Criterios de aceptación

Esta etapa se considera **completada** cuando:

- [ ] `LibusbBackend::configureSampleRate()` implementado y llamado desde `start()` con branching UAC1/UAC2.
- [ ] Unit tests `ClockController_test.cpp` y `SampleRateNegotiation_test.cpp` pasando en CI (requiere hookear `gtest` si no está aún; ver stage 6 si no).
- [ ] `UsbTransferManager` recibe `UacVersion` explícita y el feedback endpoint usa el length correcto (3 vs 4 bytes) según versión.
- [ ] Validación de endpoint implementada — un endpoint que no sea isochronous-in con usage-type feedback es rechazado con log WARN y el stream continúa sin feedback.
- [ ] `dspThreadFunc()` ya no contiene `sleep_for(200µs)`; usa `std::counting_semaphore` con timeout de seguridad de 5 ms.
- [ ] En Scarlett Solo 3rd Gen + Pixel 6, streaming sostenido 10 minutos sin underruns, drift < 50 PPM, jitter p99 < 500 µs, con buffer 256 @ 48 kHz.
- [ ] En un device UAC 1.0 (C-Media o equivalente del allowlist), streaming sostenido 10 minutos sin clicks y con feedback endpoint procesándose correctamente (verificar en logs que el length del feedback transfer es 3).
- [ ] Preset `RATE_NEGOTIATION_SWEEP` del `UsbAudioTestRunner` pasa en al menos dos devices del allowlist con al menos dos rates cada uno.

---

## 6. Riesgos específicos de esta etapa

1. **Devices que no implementan GET_CUR.** Algunos devices aceptan SET_CUR pero responden STALL en GET_CUR. Mitigación: si GET_CUR falla, no fallar el setup — solo saltar la verificación y loggear warning.
2. **Clock source ID desconocido en UAC2.** Si el parser no encontró clock sources (devices mal descritos), `configureSampleRate()` debe intentar el fallback de "interfaz 0, clockId 0" con log, no abort.
3. **`std::counting_semaphore` no disponible en versiones antiguas de libc++.** NDK 26+ lo tiene. Si hay problemas de build con NDK previo, fallback a `std::condition_variable` con mutex (menos eficiente, pero funcional).
4. **Semáforo sobre-signaleado.** El release se dispara en cada transfer completion. Si hay 3 transfers in-flight, puede haber 3 releases acumulados. El counter está acotado a 64, pero el DSP solo necesita uno para correr una vuelta. La lógica de "recompute readiness after wake" absorbe los wakes espurios.

---

## 7. Checklist de commit

Cuando se mergea esta etapa el diff debe ser aproximadamente:

- `audio/src/main/cpp/backends/LibusbBackend.cpp`: +140 líneas (configureSampleRate), -8 líneas (sleep loop), +15 (semaphore integration)
- `audio/src/main/cpp/backends/LibusbBackend.h`: +6 líneas (semaphore member + prototypes)
- `audio/src/main/cpp/usb/UsbTransferManager.cpp`: -8 líneas (heurística UAC), +12 líneas (versión explícita + packet length condicional), +6 líneas (semaphore release)
- `audio/src/main/cpp/usb/UsbTransferManager.h`: +3 líneas (setter para versión + semáforo)
- `audio/src/main/cpp/usb/UsbConstants.h`: +4 líneas (constantes de feedback length + UAC1 EP control)
- `audio/src/test/cpp/usb/ClockController_test.cpp`: **nuevo** ~80 líneas
- `audio/src/test/cpp/usb/SampleRateNegotiation_test.cpp`: **nuevo** ~120 líneas
- `audio/src/androidMain/kotlin/.../UsbAudioTestRunner.kt`: +40 líneas (nuevo preset)
- `audio/src/commonMain/kotlin/.../UsbTestResult.kt`: +8 líneas (nuevo enum de test type)

Commit messages sugeridos:
1. `feat(usb): implement UAC1/UAC2 sample rate negotiation (SET_CUR)`
2. `fix(usb): use explicit UAC version for feedback endpoint parsing`
3. `fix(usb): validate feedback endpoint attributes before enabling`
4. `perf(usb): replace DSP thread polling with counting_semaphore`
5. `test(usb): add ClockController feedback parsing tests`
6. `test(usb): add sample rate negotiation control transfer tests`

---

## 8. Siguiente etapa

Con los fundamentos sólidos, [stage_02_discovery.md](stage_02_discovery.md) rediseña el descubrimiento para permitir múltiples formatos por altsetting y una API de selección dirigida por preferencias. Es prerrequisito para stages 3, 4 y 5.
