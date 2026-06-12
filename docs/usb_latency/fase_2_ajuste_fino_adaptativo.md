# Fase 2 — Ajuste fino: jitter budget adaptativo, timing por dirección y persistencia

**Objetivo:** round-trip ~6–9 ms estable por dispositivo. La latencia deja de ser un número fijo elegido a mano y pasa a converger automáticamente al mínimo que cada combinación dispositivo+teléfono sostiene sin xruns, recordándolo entre sesiones.

**Hallazgos cubiertos:** L5, L6 + endurecimiento de la Fase 1.
**Depende de:** Fases 0 y 1 validadas en hardware.
**Estimación:** 3–5 días.

---

## 2.1 — Re-apuntar el AdaptiveBufferController al jitter budget (L5)

### Problema
`AdaptiveBufferController` ajusta la **capacidad** del ring (50–200 ms, `AdaptiveBufferController.h:73-75`), que en esta arquitectura no controla la latencia. La palanca real es `jitterBudgetMs` (Fase 1). Además `reconfigureBufferSize()` (`UsbTransferManager.cpp:1482-1533`) hace un swap de ring en caliente que ya no es necesario si la capacidad se dimensiona por el máximo desde el arranque.

### Diseño

**a) Variable de control nueva.** `TransferConfig.jitterBudgetMs` pasa a ser respaldado por un atomic vivo:

```cpp
// UsbTransferManager
std::atomic<int> mJitterBudgetMs;   // init = config.jitterBudgetMs

size_t getOutputRingTargetLevel() const {
    const int framesPerTransfer = mConfig.packetsPerTransfer * mConfig.framesPerPacket;
    const int jitterFrames = mJitterBudgetMs.load(std::memory_order_relaxed)
                           * mConfig.sampleRate / 1000;
    return size_t(framesPerTransfer + jitterFrames) * mConfig.channelCount;
}

void setJitterBudgetMs(int ms);  // clamp [jitterMin, jitterMax]; thread-safe
```

El DSP loop ya lee el target por iteración (cambio hecho en Fase 1) → **el ajuste es en caliente, sin resize de ring, sin glitch**: subir el target hace que el DSP produzca algunos bloques extra hasta alcanzarlo; bajarlo deja de producir hasta drenar. Transición suave por construcción.

**b) Rango y capacidad.** `jitterBudgetMs ∈ [1, 16]`. La capacidad del ring se fija al arranque para el máximo: `ringCapacityMs ≥ transferMs + 16 + 2×transferMs` → con `ringCapacityMs=50` de la Fase 1 sobra. **Eliminar** `reconfigureBufferSize`/`ResizableRingBuffer` del camino adaptativo (la clase puede quedar, pero el controlador deja de invocarla; marcar deprecated).

**c) Lazo de control** (reescritura del cuerpo de `AdaptiveBufferController`, conservando la interfaz `evaluate()/Recommendation`):

```
Señales (por ventana de evaluación de 2 s, alimentadas desde el DSP loop):
  - underrunsDelta      (stats.underruns, delta de ventana)
  - overrunsDelta
  - inputReadFailDelta  (fallos de readInput del DSP)
  - dspCallbackP99Us    (UsbLatencyProfiler, ya disponible)

Reglas (hysteresis asimétrica — subir rápido, bajar lento):
  XRUN:    underrunsDelta > 0 || inputReadFailDelta > umbral(2)
           → budget = min(budget * 2, jitterMax); cooldownVentanas = 15 (30 s)
  ESTABLE: 30 ventanas (60 s) consecutivas sin XRUN y cooldown == 0
           → budget = budget - 1 ms (si > jitterMin)
  PISO:    si tras bajar a B-1 hay XRUN en < 60 s
           → volver a B y marcar B como piso (no volver a bajar de B
             durante la sesión; persistir B como valor convergido)
```

- Arranque de sesión: budget inicial = valor persistido del dispositivo (2.3) o `4 ms` por defecto.
- El controlador corre donde corre hoy (`dspThreadFunc`, evaluación cada `ADAPTIVE_EVAL_INTERVAL` callbacks — recalibrar el intervalo a tiempo real: con bloques de 2 ms, 100 callbacks = 200 ms; usar ventanas por tiempo, no por conteo).
- `requestBufferResize`/`performBufferResize` (`LibusbBackend`) se reemplazan por `mTransferManager->setJitterBudgetMs(n)` — sin punto de sincronización adicional.

**d) Telemetría:** exponer en stats `currentJitterBudgetMs` y `convergedFloorMs` para que NoisyPad los muestre (pantalla de diagnóstico) y para el test de Fase 5.

### Tests
- `AdaptiveBufferController` es lógica pura → unit tests host-side: secuencias de ventanas (clean/xrun) verifican duplicación, cooldown, descenso, detección de piso.
- Test de integración manual: provocar carga (termal/CPU burner) durante stream y verificar escalada de budget sin glitch audible sostenido (un glitch en el evento que dispara la subida es esperado).

---

## 2.2 — Timing por dirección (L6)

### Problema
`calculateIsoTransferTiming` se evalúa una vez con el `bInterval` de un solo endpoint (`LibusbBackend.cpp:1426-1429`). Con OUT bInterval=1 (125 µs) e IN bInterval=4 (1 ms) — combinación real en UAC2 — el `packetsPerTransfer` común produce URBs de entrada de 64 ms.

### Diseño

**a) `TransferConfig` gana el bloque de entrada:**

```cpp
struct TransferConfig {
    // ... output: framesPerPacket, packetsPerTransfer, packetsPerSecond,
    //             endpointInterval (existentes, pasan a ser SOLO de salida)
    int inputFramesPerPacket   = 48;
    int inputPacketsPerTransfer= 8;
    int inputPacketsPerSecond  = 1000;
    int inputEndpointInterval  = 1;
    // inputBytesPerPacket()/inputBytesPerTransfer() pasan a usar
    // inputFramesPerPacket (hoy usan framesPerPacket: UsbTransferManager.h:124-130)
};
```

**b) `setupTransferManager`** calcula dos timings:

```cpp
const auto outTiming = calculateIsoTransferTiming(rate, isHighSpeed,
    mSelectedPlayback->dataEndpoint.interval, tuning.targetTransferMs);
const auto inTiming  = calculateIsoTransferTiming(rate, isHighSpeed,
    mSelectedCapture->dataEndpoint.interval,  tuning.targetTransferMs);
```

Ambas direcciones apuntan al mismo `targetTransferMs` → URBs de duración pareja aunque el conteo de packets difiera.

**c) Consumidores a actualizar (auditoría de usos de `mConfig.framesPerPacket`/`packetsPerTransfer` en el camino de entrada):**
- `allocateTransfers`: input usa `inputPacketsPerTransfer` para `libusb_alloc_transfer` y el sizing de slots (`UsbTransferManager.cpp:594-633`).
- `processInputTransfer`: ya itera `ctx->packetCount` — sin cambios.
- `configure()`: sizing de `mFloatBuffer`/`mPcmBuffer` usa el máximo de ambas direcciones (ya lo hace con bytes; revisar samples, `UsbTransferManager.cpp:86-99`).
- Estimador de feedback implícito (Fase 0.2): la normalización `inputPacketsPerSecond / outputPacketsPerSecond` deja de ser 1.0 — el código ya lo contempla.
- `ClockController::configure(rate, packetsPerSecond)` usa el **de salida** (ajusta packets de salida).
- `getOutputRingTargetLevel`: solo salida — sin cambios.

**d) `UsbLatencyProfiler.configureFromTransfer`**: recibir ambos timings para que las ventanas de input/output usen su cadencia real.

### Tests
- Host-side: fixture con OUT bInterval=1 + IN bInterval=4 → `inTiming.packetsPerTransfer=1` y `outTiming.packetsPerTransfer=8` con `targetTransferMs=1`; verificación del sizing de buffers con los nuevos campos.
- Hardware: el GHW USB AUDIO (24/16 asimétrico) es el candidato a tener bIntervals distintos — verificar logs de enumeración.

---

## 2.3 — Persistencia de tuning por dispositivo (Kotlin)

### Diseño

Nuevo repositorio androidMain siguiendo el patrón de `TrustedUsbDevicesRepository`/`UsbVolumeRepository` (DataStore):

```kotlin
// internal/usb/UsbLatencyTuningRepository.kt
@Serializable
data class UsbDeviceTuning(
    val jitterBudgetMs: Int,        // piso convergido de 2.1
    val profile: UsbLatencyProfile, // último perfil usado
    val measuredRoundTripMs: Float?,// resultado de Fase 5, si se corrió
    val updatedAtEpochMs: Long,
)

interface IUsbLatencyTuningRepository {
    suspend fun load(vendorId: Int, productId: Int, sampleRate: Int): UsbDeviceTuning?
    suspend fun save(vendorId: Int, productId: Int, sampleRate: Int, t: UsbDeviceTuning)
}
```

- Clave: `"$vendorId:$productId:$sampleRate"` (el piso de jitter depende del rate).
- Flujo: `UsbAudioManagerImpl` carga el tuning al conectar el dispositivo → `bridge.setUsbLatencyTuning(...)` antes de `start()`; al detectar convergencia (callback/stat `convergedFloorMs`), persiste.
- La interfaz va en commonMain solo si NoisyPad la necesita observar; el default es interno.
- JNI: lectura de `convergedFloorMs` vía el getter de stats existente (extender el array de `nativeGetUsbTransferStats` o equivalente — revisar el getter de stats actual en `AudioNativeBridge.kt` y añadir los dos campos al final para no romper offsets).

---

## 2.4 — Endurecimiento (backlog de la Fase 1)

1. **Prefill en recovery restart** (detectado en Fase 1): `performRecoveryRestart` (`UsbTransferManager.cpp:945-1005`) re-submitea sin re-prefill → tras un restart el ring puede quedar bajo target con xrun inmediato. Añadir el mismo prefill exacto de `start()` (silencio) antes de los fills.
2. **`fillOutputTransfer` parcial** (M6): si `availableToRead ∈ (0, samplesNeeded)`, leer lo disponible y completar con silencio en vez de descartar todo. Con URBs de 1 ms el beneficio es menor pero el costo es trivial (la API de `LockFreeRingBuffer` necesita `readUpTo()` — añadirlo con el mismo contrato SPSC).
3. **`processInputTransfer` parcial** (M5): espejo — `writeUpTo()` y descartar solo el excedente.
4. **Afinidad por capacidad de CPU** (M4): reemplazar `numCpus-1/-2` por lectura de `/sys/devices/system/cpu/cpu*/cpu_capacity` (con fallback al heurístico actual si no existe). Helper en `ThreadUtils`: `getCoresByCapacityDesc()`. DSP → core de mayor capacidad; event loop → segundo.
5. **Etiqueta de error** (M1): `BackendError::UNDERRUN` por fallo de `writeOutput` → nuevo `BackendError::OUTPUT_OVERRUN` (añadir al enum y propagar por JNI; mantener el viejo para Oboe).

---

## Presupuesto esperado (LOW_LATENCY convergido, 48 kHz HS, budget 2 ms)

| Etapa | ms |
|---|---|
| Transfer IN | 0.5–1 |
| Espera bloque entrada | 0–2 |
| Bloque DSP | 2 |
| Ring salida (1 + 2) | 3 |
| URBs OUT en vuelo | 2–4 |
| Conversores | ~1 |
| **Total** | **~6–9 ms** |

## Criterios de aceptación

1. Budget converge en los 3 DACs a ≤ 4 ms y se mantiene 30 min sin xrun; el valor persiste y se reaplica al reconectar.
2. Bajo carga sintética (CPU burner 4 threads, 5 min) el budget escala sin xruns sostenidos y vuelve a bajar al retirar la carga.
3. Dispositivo con bIntervals asimétricos enumera con URBs de duración pareja en ambas direcciones (verificar log).
4. Round-trip medido (Fase 5) ≤ 9 ms en al menos uno de los 3 DACs.
5. Unit tests nuevos verdes (controlador adaptativo, timings por dirección, readUpTo/writeUpTo).

## Riesgos
- **Oscilación del lazo adaptativo**: mitigada por hysteresis asimétrica + piso de sesión. Validar con el escenario de carga.
- **Cambio de cadencia del profiler** (2.2d): revisar que las stats p99 sigan calibradas — son entrada del lazo.
- **DataStore en el camino de conexión**: la carga del tuning es suspend; no bloquear el hilo de USB attach (el flujo de `UsbAudioManagerImpl` ya es coroutine-based).
