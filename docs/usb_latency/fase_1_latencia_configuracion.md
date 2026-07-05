# Fase 1 — Latencia por configuración: perfiles, transfers de 1 ms y pacer en ms

**Objetivo:** round-trip ~10–14 ms sin cambios estructurales — solo re-parametrizar el pipeline existente y exponer las palancas correctas. Todo detrás de un **perfil de latencia** seleccionable; el comportamiento actual queda como `SAFE`.

**Hallazgos cubiertos:** L1, L2, L3, L4, L8.
**Depende de:** Fase 0 completa (con buffers de 4–5 ms, el clock sync incorrecto produce xruns en segundos).
**Estimación:** 1–2 días + validación.

---

## 1.1 — Perfil de latencia (`UsbLatencyProfile`)

Tipo nuevo en `usb/StreamPreference.h` (o archivo propio `usb/LatencyProfile.h`):

```cpp
struct UsbLatencyTuning {
    int targetTransferMs = 8;   // duración de cada URB iso
    int numTransfers     = 3;   // URBs en vuelo por dirección
    int jitterBudgetMs   = 24;  // margen del pacer POR ENCIMA de 1 transfer
    int dspBlockFrames   = 256; // bloque del callback de usuario
    int ringCapacityMs   = 100; // capacidad (no latencia) del ring

    static UsbLatencyTuning safe() { return {}; }  // == comportamiento actual

    static UsbLatencyTuning lowLatency() {
        return {
            .targetTransferMs = 1,
            .numTransfers     = 4,
            .jitterBudgetMs   = 4,
            .dspBlockFrames   = 96,   // 2 ms @48k
            .ringCapacityMs   = 50,
        };
    }
};
```

Justificación de `lowLatency`:
- **targetTransferMs=1**: HS → 8 packets/URB (bInterval=1) o 1 packet (bInterval=4); FS → 1 packet/URB. `calculateIsoTransferTiming` ya recibe este parámetro (`UsbIsoTiming.h:33-46`) — solo hay que dejar de hardcodearlo.
- **numTransfers=4**: con URBs de 1 ms, mantiene 3 URBs encolados en el host controller mientras se rellena el cuarto. Profundidad de cola en vuelo: 4 ms (vs 24 ms actual). Mínimo seguro: el kernel necesita ≥2 URBs encolados para no introducir gaps si la completion + resubmit tarda >1 ms (ISO_ASAP re-engancha pero con un hueco audible).
- **jitterBudgetMs=4**: tolerancia a scheduling jitter del DSP thread. Valor de arranque conservador; la Fase 2 lo hace adaptativo.
- **dspBlockFrames=96**: 2 ms @ 48 kHz. 48 (1 ms) es viable pero duplica el overhead de callback; 96 equilibra. Debe ser ≤ jitterBudget para que el gating de entrada no sume espera extra.
- **ringCapacityMs=50**: la capacidad es solo headroom de memoria (la latencia la fija el pacer), pero achicarla acota el peor caso de overrun y la huella de mlock.

### Plumbing

| Capa | Cambio |
|---|---|
| `LibusbBackend` | `void setLatencyTuning(const usb::UsbLatencyTuning&)` antes de `start()`. Guardar en `mTuning`. `setBufferSize()` existente pasa a alimentar `mTuning.dspBlockFrames` (compat). |
| `setupTransferManager` (`LibusbBackend.cpp:1426-1459`) | `calculateIsoTransferTiming(rate, isHighSpeed, bInterval, mTuning.targetTransferMs)`; `config.numTransfers = mTuning.numTransfers`; `config.ringBufferMs = mTuning.ringCapacityMs`; `config.jitterBudgetMs = mTuning.jitterBudgetMs` (campo nuevo en `TransferConfig`). |
| JNI | `nativeSetUsbLatencyProfile(jint profile)` (0=SAFE, 1=LOW_LATENCY) + `nativeSetUsbLatencyTuning(...)` con los 5 enteros para tuning fino. En `jni/jni_audio_bridge.cpp`, mutex `lifecycleMutex`. Eliminar el hardcodeo `backend->setBufferSize(256)` de `jni_audio_bridge.cpp:1652` → tomar el valor del perfil activo. |
| Kotlin | `IAudioNativeBridge` (commonMain): `setUsbLatencyProfile(profile: UsbLatencyProfile): Result<Unit>`; enum en `domain/usb/`. Implementación en `AudioNativeBridge.kt` (androidMain). Exponer en `IUsbAudioManager` para que NoisyPad lo setee desde settings. |
| C API | `wma_usb_set_latency_profile(int)` en `watermelon_audio.h/cpp` (opcional pero recomendado por la regla del repo). |

**Restricción (actualizada en implementación):** el setter no rechaza con el stream corriendo — latchea el perfil (persistido en `BackendManager`) y lo aplica en el próximo `start()`, igual que `setUsbStreamingMode`. Además, `setupTransferManager` auto-deriva LOW_LATENCY para modos con input (`streamMode != PLAYBACK_ONLY`) cuando el tuning configurado no es agresivo, porque un push asíncrono desde la app puede llegar después del start (race observada con NoisyPad).

---

## 1.2 — Pacer target en milisegundos absolutos (L2)

### Problema
`getOutputRingTargetLevel()` (`UsbTransferManager.h:396-402`) = `(numTransfers+1) × transfer` → el margen escala con el tamaño de transfer en vez de con el jitter real del OS. Con la config SAFE da 32 ms; con transfers de 1 ms daría 5 ms por accidente. Hacerlo explícito:

### Diseño

```cpp
// UsbTransferManager
size_t getOutputRingTargetLevel() const {
    const int framesPerTransfer = mConfig.packetsPerTransfer * mConfig.framesPerPacket;
    const int jitterFrames = mConfig.jitterBudgetMs * mConfig.sampleRate / 1000;
    return size_t(framesPerTransfer + jitterFrames) * mConfig.channelCount;
}
```

- SAFE: `jitterBudgetMs=24` reproduce exactamente el target actual (8 ms transfer + 24 ms = 32 ms) → **cero cambio de comportamiento** para el perfil por defecto.
- LOW_LATENCY: 1 ms + 4 ms = 5 ms de ring de salida.
- El valor se lee por iteración en el DSP loop (hoy se cachea al inicio del thread, `LibusbBackend.cpp:1577-1579`): **cambiar a lectura por iteración** (es un cálculo de enteros sobre config inmutable + un campo que la Fase 2 hará atómico). Esto prepara el ajuste dinámico sin re-arrancar el stream.

### Invariantes a verificar
- `ringCapacity ≥ target + 2×framesPerTransfer` (asserts en `configure()`): el pacer necesita poder sobrepasar el target transitoriamente sin overrun.
- `prefill` (1.3) ≤ capacidad.

---

## 1.3 — Prefill exacto (L4)

### Problema
`UsbTransferManager::start()` (`UsbTransferManager.cpp:300-305`) pre-llena `2 × inflight` de silencio (48 ms en HS SAFE). Los fills iniciales consumen `numTransfers × transfer`; el resto queda como nivel de ring inicial ≠ target.

### Diseño

```cpp
// start(), antes de los fills iniciales:
const size_t inflightSamples = size_t(mConfig.numTransfers)
    * mConfig.packetsPerTransfer * mConfig.framesPerPacket * mConfig.channelCount;
const size_t prefillSamples = inflightSamples + getOutputRingTargetLevel();
```

Tras los `numTransfers` fills iniciales el ring queda exactamente en `target`: el DSP arranca en régimen, sin transitorio de drenado ni backlog de entrada en duplex. El silencio inicial audible pasa de ~48 ms a `target` (5 ms en LOW_LATENCY).

---

## 1.4 — Bloque DSP configurable (L3)

- `mRequestedBufferSize` ya existe y el DSP loop ya lo usa (`LibusbBackend.cpp:1552`); el único hardcodeo es `jni_audio_bridge.cpp:1652`. Reemplazar por el valor del perfil/tuning activo.
- Validación en `start()`: `dspBlockFrames` clampleado a `[16, 1024]` y **redondeado a múltiplo de framesPerPacket** cuando sea posible (96 = 16×6 en HS; evita que `inputReady` oscile entre 1 y 2 transfers de espera).
- Documentar en `IAudioBackend::setBufferSize` que en USB el costo de bloques chicos es overhead de callback, no xruns (los rings desacoplan).

### Gating de entrada con bloques chicos
`inputReady = available ≥ inputSamples` (`LibusbBackend.cpp:1682-1683`) no cambia, pero con URBs de 1 ms la entrada llega en porciones de 1 ms → la espera máxima de entrada baja de 8 ms a 1 ms. Ningún cambio de código adicional.

---

## 1.5 — Diagnósticos tras flag (L8)

- Nuevo define `WMA_USB_DIAG` (CMake option, OFF en release):
  - `UsbTransferManager.cpp:1054-1143` (peak scan + decode-back + hexdump `USB_FMT`).
  - `LibusbBackend.cpp:1788-1845` (log `USB_DSP`) y `:1876-1902` (fingerprint de 512 samples por callback).
- Los contadores baratos (underruns delta, ring min/max) se mantienen siempre activos: son la telemetría de las fases 2 y 5.
- Razón: a 1000 completions/s el decode-back periódico y los scans dejan de ser despreciables y contaminan la medición de CPU.

---

## 1.6 — Interacciones a revisar (checklist de implementación)

| Ítem | Riesgo con transfers de 1 ms | Acción |
|---|---|---|
| `try_acquire_for(5ms)` del DSP (`LibusbBackend.cpp:1690`) | El timeout de seguridad es 5× el transfer; podría retrasar la detección de "hay que producir" si se pierde un wake | Mantener 5 ms (solo es fallback; los wakes llegan a 1 kHz). No reducir: más wakeups espurios = más CPU |
| Watchdog `WATCHDOG_TIMEOUT_MS=500` | OK (≫ transfer) | Sin cambio |
| `transferTimeoutMs=100` | OK | Sin cambio |
| `DRAIN_DEADLINE_MS=500` en stop | Con 4 URBs de 1 ms drena en <10 ms | Sin cambio |
| Semáforo `mDspWake` cap=64 | 1000 releases/s, drenados por iteración | Sin cambio (saturación inocua) |
| `AdaptiveBufferController` | Opera sobre `ringBufferMs` (capacidad) — irrelevante para latencia | **Deshabilitarlo en LOW_LATENCY** hasta Fase 2 (`setAdaptiveBufferingEnabled(false)` forzado) |
| `RecoveryPolicy` restart | El restart re-llena con prefill nuevo (1.3) | Verificar que `performRecoveryRestart` use el mismo prefill — hoy NO pre-llena el ring (gap conocido): añadir prefill al restart |
| usbfs `MAX_ISO_BUFFER` / límites de URB | 8 packets × ~300 bytes ≪ límites | Sin riesgo |

---

## 1.6b — Estabilidad del pipeline duplex (hallazgos de la validación en DAC1)

La primera validación en hardware (UAC1 FS, duplex) mostró underruns persistentes del
output ring en régimen, insensibles a jitterBudget/numTransfers. Causa raíz y mecanismos
añadidos:

1. **Ley de conservación / trim por exceso combinado.** La producción del DSP está gateada
   1:1 por input disponible, así que `inputRing + outputRing` se conserva: un stall del DSP
   drena el output ring y estaciona ese mismo audio como backlog de input, que las
   iteraciones de catch-up del pacer reconvierten en nivel de output. Un trim incondicional
   del backlog (el intento original de acotar `inLatMs`) destruye ese combustible y vuelve
   PERMANENTE cada déficit transitorio (ratchet → ring en cero → underruns continuos).
   El trim ahora descarta solo el **exceso combinado** en frames:
   `(in − inTarget) + (out − outTarget) ≥ 1 bloque`. En régimen recorta el backlog de
   startup (acota `inLatMs`); tras un stall el déficit de output cancela el backlog y no se
   descarta nada. Telemetría: `inputSamplesTrimmed` (delta `trimDelta` en `USB_CLOCK`) —
   en régimen debe ser 0.
2. **Producción de emergencia.** Si el output ring cae bajo `max(1 bloque, target/2)` y el
   input no tiene un bloque completo (hipo de captura, paquetes perdidos), el DSP produce
   igual: `readInput` falla sin consumir y el fade del último bloque válido mantiene la
   salida continua. Un fade breve de input es mucho menos audible que un packet de silencio
   en el DAC.
3. **Startup lead** (`TransferConfig.startupLeadFrames`). En duplex el output drena a wire
   rate desde el primer SOF mientras el DSP espera el primer bloque de input (~1 transfer +
   1 bloque); con producción input-gated ese drenado inicial nunca se recupera y comería
   ~la mitad del jitter budget. Se prefill-ea (bloque + 1 transfer) solo en régimen
   low-latency; SAFE pasa 0 (bit-idéntico).
4. **Event thread a REALTIME.** A 1000 completions/s por dirección con URBs de 1 ms y
   `fillOutputTransfer` en su hot path, el deadline de resubmit es `numTransfers` ms;
   `Priority::HIGH` (nice −10) pierde esa carrera bajo presión de scheduling.
5. **Feedback implícito también para playback ADAPTIVE** (régimen low-latency, duplex,
   sin feedback endpoint explícito). Con producción input-gated, cualquier surplus del
   clock de captura vs SOF aparece como trim periódico de 96 frames (empalme de 2 ms
   audible cada pocos segundos) y cualquier déficit como drenado lento del output. Los EP
   adaptive recuperan su clock de la tasa de datos entrante, así que esclavizar el sizing
   de paquetes de salida a la tasa de captura (la maquinaria de Fase 0) es exactamente lo
   que saben trackear → `inRing + outRing` conservado con deriva neta cero. SAFE no cambia
   (siempre corrió adaptive-at-nominal y su umbral de trim de 24 ms esconde la deriva).
6. **Jitter budget adaptativo** (`mJitterExtraMs`, telemetría `jbExtra` en `USB_CLOCK`).
   El perfil arranca en el absorber mínimo (4 ms) y por cada underrun de output sube 1 ms
   (cap +12). No decae en la sesión (sesgo a estabilidad); se resetea en `configure()`.
   El refill al nuevo target es automático: el mismo stall que causó el underrun estacionó
   el audio faltante como backlog de input, y el target más alto evita que el trim lo
   descarte — se convierte en nivel de output. Resultado: la latencia converge a la mínima
   que ese dispositivo/sistema realmente sostiene sin glitches.
7. **De-click de empalmes de input.** Cada evento de trim (descarte de bloque) o de
   dropout/reanudación (fade del último bloque válido) era una discontinuidad dura en la
   señal monitoreada — el "click" audible que queda incluso con xruns en cero. Ahora: el
   bloque de fade arranca mezclado desde el último sample entregado (era una repetición del
   bloque anterior → step), y el primer bloque real tras un trim/fade hace fade-in de ~1 ms
   desde el valor sostenido (tras un fade completo ese valor es ~0 → reanudación limpia).
8. **Warmup del budget adaptativo** (`kAdaptiveWarmupMs=1000`). Los underruns del primer
   segundo tras (re)start son estructurales (gap entre el submit inicial y el DSP thread
   llegando a su loop, enmascarado por el fade de la app) — no suben `jbExtra`. Sin esto,
   cada sesión pagaba ~2 ms de latencia permanente por un artefacto de arranque inaudible.
9. **Detector de wire gaps** (`outputWireGaps`, `wireGapsDelta=` en `USB_CLOCK`). La clase
   de glitch invisible para todos los contadores de ring: iso OUT no tiene ACK, así que si
   el event thread pierde la carrera de resubmit y la cola del kernel se vacía, el DAC pasa
   ≥1 intervalo de servicio sin datos con el ring sano y el URB completando OK. Se detecta
   midiendo el spacing entre completions: ≥ `numTransfers × transferMs` ⇒ gap seguro.
   Tras observar artefactos audibles con TODOS los counters en cero en hardware real,
   `numTransfers` volvió a 6 (deadline de resubmit 6 ms — el seguro contra la latencia de
   userspace bajo scheduling Android; cuesta +2 ms de round-trip).

## 1.7 — Presupuesto de latencia esperado (LOW_LATENCY, 48 kHz HS)

| Etapa | ms |
|---|---|
| Transfer IN (1 ms URB, promedio) | 0.5–1 |
| Espera de bloque de entrada (96 frames) | 0–2 |
| Bloque DSP | 2 |
| Ring de salida (target) | 5 |
| URBs OUT en vuelo (4×1 ms, promedio) | 2–4 |
| Conversores DAC/ADC | ~1 |
| **Total round-trip** | **~10–14 ms** |

---

## Criterios de aceptación

1. Perfil SAFE: comportamiento bit-idéntico al actual (mismos valores de target, prefill equivalente, logs).
2. Perfil LOW_LATENCY en los 3 DACs (48 kHz y 44.1 kHz, duplex):
   - 30 min de stream sin underruns/overruns en régimen (delta `WMA_AUDIT` = 0 tras el primer segundo).
   - `getOutputLatencyMs() + getInputLatencyMs()` ≤ 12 ms.
   - CPU del event thread < 5 % y del DSP thread < 15 % en un dispositivo mid-range (medir con `simpleperf`).
3. Música/efectos audibles sin glitches con la app real (NoisyPad via `includeBuild`).
4. Tests host-side: `calculateIsoTransferTiming` con `targetTransferMs=1` (FS y HS, bInterval 1 y 4); target/prefill helpers con ambos perfiles.

## Riesgos
- **Resubmit tardío con URBs de 1 ms**: si el event thread se retrasa >3 ms (3 URBs restantes), hay gap en el wire. Mitigación: `numTransfers=4` + prioridad RT del event thread + medición de `packetsErrors` en la validación. Si un DAC lo sufre, subir a `numTransfers=6` (6 ms en vuelo sigue ≪ SAFE).
- **Dispositivos FS con bInterval>1**: `targetTransferMs=1` con packets de 2–8 ms degrada a 1 packet/URB de duración bInterval — correcto pero la latencia mínima la fija el hardware. Documentar en `getCapabilities()`.
- **CPU/batería**: 8× más completions. Si la medición da >5 % event thread, ofrecer `targetTransferMs=2` como término medio en el mismo tuning struct.
