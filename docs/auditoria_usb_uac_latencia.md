# Auditoría USB Audio (UAC 1.0 / UAC 2.0) — Latencia, Estabilidad y Bit-Perfect

**Fecha:** 2026-06-12
**Alcance:** `audio/src/main/cpp/usb/*`, `audio/src/main/cpp/backends/LibusbBackend.*`, `ClockController.h`, `UsbIsoTiming.h`, soporte relacionado.
**Caso de uso objetivo:** loop full-duplex (guitarra → efectos → salida) con round-trip < 5 ms de procesamiento + transporte.

---

## 1. Resumen ejecutivo

La implementación es sólida en robustez (recovery, watchdog, hot-plug, layout correcto de buffers iso para linux_usbfs, formatos input/output independientes), pero **la arquitectura actual de buffering hace estructuralmente imposible el objetivo de < 5 ms**: el presupuesto de latencia round-trip real con la configuración por defecto es **~55–70 ms**, dominado por tres decisiones de diseño, no por bugs puntuales:

1. **Transfers de 8 ms** (`targetTransferMs = 8` en `UsbIsoTiming.h`) — granularidad de completion de 8 ms en ambas direcciones.
2. **Target del ring de salida = 4 transfers = 32 ms** (`getOutputRingTargetLevel()`).
3. **Bloque DSP de 256 frames = 5.3 ms** (hardcodeado en el JNI bridge).

Además hay **5 hallazgos críticos de corrección** que afectan estabilidad y sincronización asíncrona (prioridades 2 y 3 del pedido), el más grave: **el lazo de feedback asíncrono tiene el signo invertido** — el host responde a un dispositivo "rápido" enviando *menos* samples, acelerando el drift en lugar de corregirlo.

Con los cambios de la Fase 1 (solo configuración + pacing) la latencia round-trip baja a **~10–14 ms**; con la Fase 2 (transfers de 1 ms reales + ring target mínimo) a **~6–9 ms**. El piso práctico en Android vía usbfs/libusb (sin driver kernel dedicado) está en ese orden; < 5 ms estrictos requeriría servicing por microframe (URBs de 125 µs en high-speed) y es alcanzable solo en dispositivos UAC2 HS con CPU sobrada — se documenta como Fase 3 experimental.

---

## 2. Presupuesto de latencia actual (medido sobre el código)

Configuración por defecto: 48 kHz, UAC2 high-speed (bInterval=1), `framesPerPacket=6`, `packetsPerTransfer=64` (8 ms), `numTransfers=3`, bloque DSP 256 frames.

| Etapa | Mecanismo | Latencia |
|---|---|---|
| ADC → completion de transfer IN | El URB de entrada solo completa cuando sus 64 packets (8 ms) terminan | 4–8 ms (promedio ~8 ms para el primer sample del transfer) |
| Ring de entrada → DSP | El DSP exige `inputSamples` (256 frames) disponibles | 0–5.3 ms |
| Bloque DSP | 256 frames @ 48 kHz | 5.3 ms |
| Ring de salida | Pacer mantiene `(numTransfers+1)` transfers = **32 ms** (`UsbTransferManager.h:396-402`) | ~28–32 ms |
| Transfers OUT en vuelo | 3 × 8 ms encolados en el host controller | 8–24 ms |
| Conversores del DAC/ADC | hardware | ~1 ms |
| **Total round-trip** | | **~55–70 ms** |

En UAC1 full-speed (packets de 1 ms, `packetsPerTransfer=8`) los números son casi idénticos porque la estructura (8 ms/transfer, target 4 transfers) es la misma.

**Puntos clave de la arquitectura:**

- El **tamaño del ring (`ringBufferMs=100`) NO es la latencia** — el comentario del código lo entiende bien. La latencia de salida la fija el *pacer target* (`getOutputRingTargetLevel`). Por eso el `AdaptiveBufferController` (que redimensiona la *capacidad* entre 50–200 ms, `AdaptiveBufferController.h:73-75`) **no controla la latencia en absoluto** — ajusta la variable equivocada. La palanca real es el target del pacer.
- El prefill de arranque escribe **2× el contenido en vuelo de silencio** (`UsbTransferManager.cpp:300-305`): 48 ms de silencio al inicio. El pacer lo drena hasta el target, pero alarga el arranque y no aporta nada sobre prefill = 1× en vuelo.

---

## 3. Hallazgos críticos (corrección / estabilidad)

### C1 — Signo invertido en el lazo de feedback asíncrono ⚠️ CRÍTICO
`ClockController.h:242` + `ClockController.h:77-96`

```cpp
float adjustment = mPid.calculate(expectedRate, avgRate);
// error = setpoint - measurement = expected - measured
```

Si el dispositivo corre rápido (avgRate > expectedRate), `error < 0` → `adjustment < 0` → `getAdjustedFrameCount()` devuelve **menos** frames por packet. Lo correcto es enviar **más** (seguir el Ff que reporta el dispositivo). El resultado es que el feedback asíncrono **duplica el drift en vez de anularlo**: el ring del dispositivo se vacía/llena al doble de velocidad que sin feedback, y el "soporte asíncrono" es hoy contraproducente.

Conceptualmente el diseño también es erróneo aunque se corrija el signo: un PID contra el *nominal* intenta llevar la medición al setpoint, pero la medición (el clock del dispositivo) es exógena — no se puede controlar. **El feedback Ff ES directamente la consigna de frames/packet.** La implementación estándar (la que usa snd-usb-audio) es:

```cpp
// por packet:
mAccum += avgFf;                       // Ff medido (samples/intervalo de servicio)
int frames = (int)mAccum;
mAccum -= frames;
```

Sin PID. Los tests de `test_clock_controller.cpp` solo verifican el parsing/medición, nunca la dirección del ajuste — por eso el bug no se detectó. Agregar un test: "dispositivo +100 ppm → media de `getAdjustedFrameCount` > nominal".

### C2 — Feedback implícito detectado pero no implementado ⚠️ CRÍTICO para duplex
`UsbTransferManager.cpp:182-192`, `UsbDescriptorParser.cpp:161-172`

El parser detecta endpoints de feedback implícito y el transfer manager loguea *"clock sync via packet timing"*, pero **nada alimenta al ClockController desde `processInputTransfer`**. Las interfaces de salida asíncronas con feedback implícito (muy comunes en interfaces de grabación full-duplex, justo el target de este proyecto) quedan **sin sincronización alguna**: drift libre hasta el xrun, típicamente cada pocos minutos según el desvío del cristal.

Implementación: en `processInputTransfer`, acumular `actual_length/inputBytesPerFrame` por ventana de servicio y usar ese rate medido como Ff para el ajuste de salida (mismo acumulador fraccional de C1). En full-duplex con un solo clock físico (el caso de las 3 DACs de prueba), esclavizar la salida al rate de consumo de entrada resuelve además el balance entre rings.

### C3 — 44.1 kHz sin acumulador fraccional nominal ⚠️ CRÍTICO en endpoints sin feedback
`UsbIsoTiming.h:41` (`framesPerPacket = sampleRate / packetsPerSecond`, división entera)

A 44.1 kHz FS: 44100/1000 = **44** frames/packet → el host transmite a 44 000 Hz efectivos (−2268 ppm). El comentario en `LibusbBackend.cpp:1441-1443` dice que "el acumulador fraccional del clock controller compensa vía feedback" — pero:
- En endpoints **adaptive/sync** no hay feedback: deriva permanente de 0.23 % → overrun/underrun garantizado en segundos/minutos. (El `AltsettingSelector` puntúa adaptive 0.5, así que esto se selecciona en hardware real.)
- Incluso con feedback, el PID limitado y promediado tardaría en absorber un sesgo sistemático que es **conocido de antemano**.

Fix: el acumulador fraccional debe trabajar sobre el nominal exacto (`44.1` frames/packet → patrón 44-44-44-...-45), con el feedback como corrección *encima* de eso. Es el mismo acumulador de C1; la base debe ser `sampleRate / (float)packetsPerSecond`, no el entero truncado.

### C4 — Detección de feedback EP UAC1 incompleta: puede clobberear el data endpoint ⚠️ CRÍTICO
`UsbDescriptorParser.cpp:133-176`

El parser clasifica como feedback solo endpoints con usage bits `01` (bmAttributes 5:4). Muchos dispositivos UAC1 reales (diseñados contra USB 1.1, donde no existían los usage bits) exponen el endpoint de sincronización con `bmAttributes = 0x01` (iso, usage 00) y lo señalizan vía `bSynchAddress`/`bRefresh` del data endpoint — campos que el parser **no lee** (los descriptores de endpoint de audio UAC1 son de 9 bytes; bRefresh está en el offset 8).

Peor: la rama `else` asigna `dataEndpoint = endpoint` **sin verificar dirección**. En una interfaz de playback (data EP OUT), un EP iso IN con usage 00 que aparezca después **sobrescribe el data endpoint** → streaming roto en ese dispositivo, y además se pierde el feedback.

Fix mínimo: en la rama `else`, aceptar como data endpoint solo si la dirección coincide con el rol de la interfaz (terminal de la interfaz / dirección del primer EP). Fix completo: parsear `bSynchAddress` + `bRefresh` y resolver el feedback EP por dirección cruzada como hace snd-usb-audio; usar `bRefresh` para dimensionar la cadencia del transfer de feedback.

### C5 — Coerción de sample rate después de configurar el transfer manager
`LibusbBackend.cpp:867-871` (hook ejecutado dentro de `UsbTransferManager::start()`)

Si el dispositivo coerce el rate en el GET_CUR (`mRequestedSampleRate = actual`), el transfer manager **ya fue configurado** con el rate solicitado: `framesPerPacket`, ring sizing y el nominal del ClockController quedan calculados para el rate equivocado → drift sistemático equivalente al cociente de rates. Hay que abortar y reconfigurar (o renegociar antes de `configure()` usando un claim temporal del altsetting).

---

## 4. Hallazgos de latencia (prioridad 1)

### L1 — `targetTransferMs = 8`: la granularidad de TODO el pipeline
`UsbIsoTiming.h:37`, `LibusbBackend.cpp:1447-1449`

8 ms por URB implica: entrada visible cada 8 ms, salida consumida del ring en bloques de 8 ms, underruns de 8 ms completos (el `fillOutputTransfer` es todo-o-silencio), y un target de pacer expresado en múltiplos de 8 ms. **Es el parámetro individual más importante del sistema.**

Propuesta: `targetTransferMs = 1` (HS: 8 packets/URB; FS: 1 packet/URB) en un perfil "low latency", con `numTransfers = 4–6` para mantener la profundidad de cola en el host controller (~4–6 ms de pipeline en vuelo, en vez de 24 ms). El costo es más completions/segundo (1000/s vs 125/s) — el event loop ya corre con timeout de 1 ms y prioridad RT, así que el headroom existe; medir CPU en los 3 DACs de prueba.

### L2 — Target del ring de salida: 4 transfers de margen
`UsbTransferManager.h:396-402`

`(numTransfers + 1) × packetsPerTransfer × framesPerPacket × channels` = 32 ms con la config actual. El razonamiento del comentario (absorber jitter de scheduling) es correcto, pero el margen debe expresarse en **ms absolutos de jitter tolerado**, no en múltiplos del tamaño de transfer — al achicar los transfers a 1 ms, la fórmula actual daría 5 ms de target (¡bien!), pero conviene hacerlo explícito y configurable:

```
targetSamples = (jitterBudgetMs + transferMs) * sampleRate/1000 * channels
```

con `jitterBudgetMs` ~2–4 ms en modo low-latency (ajustable por el AdaptiveBufferController, ver L5). Con transfers de 1 ms y budget de 3 ms → 4 ms de ring + 4–6 ms en vuelo.

### L3 — Bloque DSP de 256 frames hardcodeado
`jni_audio_bridge.cpp:1652` (`backend->setBufferSize(256)`), `LibusbBackend.h:386`

5.3 ms de bloque + el requisito de `inputReady ≥ 256 frames` añade hasta 5.3 ms de espera de entrada. Para el loop de guitarra: 48–96 frames (1–2 ms). El motor DSP ya procesa por bloques arbitrarios. Exponer esto al consumidor (NoisyPad) vía la API en vez de hardcodearlo en el bridge.

### L4 — Prefill de arranque 2× en vuelo
`UsbTransferManager.cpp:300-305`

48 ms de silencio inicial (HS). Con el pacer drena hasta el target, pero en FULL_DUPLEX el gating combinado (`outputReady && inputReady`) hace que el exceso tarde en drenar y puede dejar backlog en el ring de entrada. Prefill correcto: exactamente lo que consumen los `numTransfers` fills iniciales + el target del pacer, ni más ni menos.

### L5 — AdaptiveBufferController ajusta capacidad, no latencia
`AdaptiveBufferController.h:73-75`, `UsbTransferManager.cpp:1482-1533`

Redimensiona la *capacidad* del ring (50–200 ms) que, en esta arquitectura, es solo headroom de memoria — la latencia la fija el pacer target. Rediseño: que el controlador adapte `jitterBudgetMs` (L2) según la tasa de underruns observada (subir ante xruns, bajar lentamente en estabilidad). Eso convierte el componente en un controlador de latencia real: arranca conservador (p.ej. 8 ms) y converge al mínimo estable por dispositivo. Persistir el valor convergido por VID:PID (DataStore en el lado Kotlin) para arrancar óptimo en sesiones siguientes.

### L6 — Timing derivado de un solo endpoint en duplex
`LibusbBackend.cpp:1426-1429`

`calculateIsoTransferTiming` usa el `bInterval` de **un** `configInterface` para ambas direcciones. Si entrada y salida declaran bInterval distinto (p.ej. OUT bInterval=1 → 125 µs, IN bInterval=4 → 1 ms, combinación real en hardware UAC2), el `packetsPerTransfer` queda mal para una dirección: con la config actual, un IN de 1 ms con 64 packets/URB = **un URB de entrada de 64 ms**. Calcular timing por dirección (`framesPerPacket`/`packetsPerTransfer` separados para input y output en `TransferConfig`).

### L7 — Latencia reportada siempre 0
`TransferStatistics::currentLatencyMs` nunca se actualiza (solo `reset()`); `LibusbBackend::getOutputLatencyMs()/getInputLatencyMs()` (`LibusbBackend.cpp:1273-1288`) devuelven ese 0 cuando el stream corre. Cualquier compensación de latencia río arriba (looper, métrónomo, UI) trabaja con datos falsos. Calcularla de verdad: `outputLatency = (ringLevel/channels + framesEnVuelo) / sampleRate`; `inputLatency = (inputRingLevel/inCh + transferDepth) / sampleRate`. Todos los datos ya existen como atomics.

### L8 — Diagnósticos en el hot path
`UsbTransferManager.cpp:1054-1143` (peak scan + decode-back + hexdump cada 300 fills, en el event thread RT) y `LibusbBackend.cpp:1876-1902` (fingerprint de 512 samples por callback DSP). Son baratos pero no gratis a 1000 completions/s con transfers de 1 ms. Compilarlos bajo un flag (`WMA_USB_DIAG`) o gatearlos en runtime, apagados por defecto en release.

---

## 5. Calidad e integridad (bit-perfect, prioridad 3)

### Q1 — Soft clip SIEMPRE activo: no es bit-perfect
`AudioFormatConverter.h:248-257` + todas las rutas `floatTo*`

`softClip()` con umbral 0.95 se aplica incondicionalmente: **cualquier sample > 0.95 se altera** (y mete `tanh` por sample en el RT path cuando ocurre). Para un motor que promete transporte limpio esto viola bit-perfect incluso con material legal a 0 dBFS. Recomendación: default `softClipThreshold = 1.0` (= solo hard clamp) y exponer el soft clip como opción explícita.

### Q2 — Cuantización por truncamiento
`static_cast<int32_t>(sample * scale)` redondea hacia cero (sesgo asimétrico + distorsión de bajo nivel correlacionada). Usar `lrintf()` (round-to-nearest-even). Con dither activo en 16-bit el efecto se enmascara; en 24/32-bit (sin dither) el truncamiento es la única no-linealidad y es gratuito evitarlo.

### Q3 — Dither: correcto pero global
TPDF a ±0.5 LSB bien implementado (`TpdfDither::get`), default ON. Para 16-bit playback es la elección correcta; para un modo "bit-perfect estricto" debe poder apagarse junto con Q1 (ya existe el setter, falta exponerlo en la API pública/Kotlin).

### Q4 — Volumen digital
Bien resuelto: el multiply se saltea con vol ≥ 0.999 y la ruta hardware (UAC Feature Unit) tiene prioridad. Sin observaciones.

### Q5 — Conversión mono→estéreo con −3 dB fijo
`LibusbBackend.cpp:1729-1736`: el `monoGain = 0.707` altera nivel sin necesidad (duplicar un canal mono a L+R no clippea). Para procesamiento de guitarra el nivel de entrada importa; hacer el gain configurable o eliminarlo.

---

## 6. Observaciones menores / robustez

| # | Observación | Referencia |
|---|---|---|
| M1 | `BackendError::UNDERRUN` reportado cuando `writeOutput` falla — eso es un **overrun** del ring de salida. Etiqueta engañosa para telemetría. | `LibusbBackend.cpp:1917-1926` |
| M2 | `getAdjustedFrameCount` clampea ±4 pero el PID limita ±8 y el margen de buffer se reserva para ±4 (`CLOCK_ADJUST_FRAMES_MAX`). Tres constantes que deben ser una sola. | `ClockController.h:180,287`, `UsbTransferManager.cpp:508` |
| M3 | `expectedRate` UAC2 asume high-speed (`/8000`). Un dispositivo UAC2 enumerado a full-speed (raro pero legal) mediría 8× mal. Derivar de `packetsPerSecond` real, que ya está en `TransferConfig`. | `ClockController.h:233-239` |
| M4 | Afinidad de CPU: `numCpus-1`/`numCpus-2` asume que los últimos cores son "big". El orden little/big varía por SoC (en algunos Qualcomm el core 0 es prime). Leer `cpu_capacity` de sysfs o usar `sched_setaffinity` con la máscara de los cores de mayor capacidad. | `LibusbBackend.cpp:1539-1545`, `UsbTransferManager.cpp:1258-1264` |
| M5 | `processInputTransfer` descarta el transfer completo (8 ms) si el ring de entrada no tiene espacio; con transfers de 1 ms el daño se reduce solo. Considerar escritura parcial. | `UsbTransferManager.cpp:1217-1220` |
| M6 | `fillOutputTransfer` es todo-o-silencio: un déficit de 1 sample produce 8 ms de silencio. Leer lo disponible y completar el resto con silencio reduce el artefacto (especialmente mientras los transfers sigan siendo de 8 ms). | `UsbTransferManager.cpp:1037-1042` |
| M7 | El transfer de feedback no usa `bRefresh` (UAC1): se resubmite a ciclo libre. Funciona, pero con C4 resuelto conviene respetar la cadencia declarada. | `UsbTransferManager.cpp:636-665` |
| M8 | `notifyDataReady()` lee `mDataReadyCallback` sin lock por diseño (comentario honesto). Aceptable dado el ciclo de vida actual; documentado correctamente. | `UsbTransferManager.h:655-662` |
| M9 | El pipeline de stop/drain (drenar CANCELLED antes de liberar transfers, deadline 500 ms) está **bien resuelto** — es un punto fuerte poco común. | `UsbTransferManager.cpp:1276-1347` |
| M10 | Memoria: `mlock` de rings y buffers temporales (`MemoryUtils::prepareForRealtime`) — correcto para evitar page faults en RT. | `UsbTransferManager.cpp:79-104` |

---

## 7. Plan de acción priorizado

> **Especificaciones detalladas por fase:** ver [`docs/usb_latency/00_indice.md`](usb_latency/00_indice.md).

### Fase 0 — Corrección (antes de tocar latencia; 2–4 días)
1. **C1**: reemplazar PID por acumulador fraccional directo sobre Ff; tests de dirección del ajuste.
2. **C3**: base fraccional nominal (`sampleRate / packetsPerSecond` en float) — arregla 44.1 kHz.
3. **C4**: check de dirección en data endpoint + parsing de `bSynchAddress`/`bRefresh`.
4. **C2**: feedback implícito → medir rate de entrada y esclavizar salida (cubre además el balance duplex).
5. **C5**: renegociar/abortar si el dispositivo coerce el rate.
6. **L7**: latencia reportada real (necesaria para *medir* las fases siguientes).

### Fase 1 — Latencia por configuración (1–2 días tras Fase 0)
- `targetTransferMs = 1`, `numTransfers = 4` (perfil "lowLatency" junto al actual como "safe").
- Pacer target en ms absolutos: transferMs + jitterBudget 3–4 ms (L2).
- Bloque DSP 48–96 frames expuesto por API (L3).
- Prefill = 1× en vuelo + target (L4).
- Diagnósticos tras flag (L8).
- **Resultado esperado: round-trip ~10–14 ms**, verificable con L7 + loopback físico (jack out→in).

### Fase 2 — Ajuste fino (después de validar en los 3 DACs)
- AdaptiveBufferController re-apuntado al jitter budget con persistencia por dispositivo (L5).
- Timing por dirección (L6).
- jitterBudget 1.5–2 ms en dispositivos que lo aguanten.
- **Resultado esperado: ~6–9 ms round-trip.**

### Fase 3 — Experimental (< 5 ms, solo UAC2 HS)
- URBs de 1–2 packets (125–250 µs) con 8–16 en vuelo; DSP de 16–32 frames.
- Requiere medir CPU/energía; probablemente viable solo en SoCs grandes. Documentar como modo opt-in.

### Calidad (independiente, 0.5 día)
- Q1 (soft clip off por defecto), Q2 (`lrintf`), Q3 (API de dither), Q5 (gain mono configurable).

---

## 8. Qué está bien y no hay que tocar

- Layout contiguo de buffers iso conforme a linux_usbfs (longitudes acumuladas, no slots fijos) — correcto y bien documentado, incluida la asimetría OUT (longitud propia) vs IN (stride = slot fijo).
- Formatos de entrada/salida independientes (`inputPcmFormat`) — el fix del caso GHW está bien protegido.
- SPSC ring buffers con memoria acquire/release correcta; semáforo de wake en vez de polling.
- Secuencia de arranque: claim → altsetting → hook de clock (SET_CUR con endpoints vivos) → allocate → submit. Orden correcto para UAC1 y UAC2.
- Recovery policy con reset de altsetting 0→N y drain seguro de cancelaciones.
- Validación defensiva del feedback endpoint en `setFeedbackEnabled`.
