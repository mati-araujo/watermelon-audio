# Plan de Mejora del Driver USB Audio — Julio 2026

**Fecha:** 2026-07-07
**Alcance:** `audio/src/main/cpp/usb/*`, `backends/LibusbBackend.*`, `backends/ClockController.h`, JNI/Kotlin USB API.
**Objetivo:** driver USB de primer nivel — mínima latencia a máxima calidad y estabilidad, con cobertura de tests, responsabilidad única y mantenibilidad.
**Documento hermano:** `NoisyPad/docs/usb-audio/PLAN_USO_DRIVER_2026-07.md` (lado consumidor).

---

## ⚠️ Estado de ejecución y addenda (2026-07-08)

| Etapa | Estado | Notas |
|---|---|---|
| E1 (quick wins) | ✅ Implementada y **auditada** | Ver `AUDITORIA_E1-E3_2026-07-08.md`; ADPF activo en debug y release |
| E2 (sanitizers + DspPacer) | ✅ Implementada y auditada | Bit-identidad verificada por diff. **Delta:** el item 2.4 (tests de secuencia duplex fade/splice) se **difirió a la Etapa 5** — se testea host-side al extraer `usb/DspLoop` |
| E3 (Fase 2 adaptativo) | 🟠 Core implementado, **fixes F1–F4 pendientes** (ver auditoría) | **Deltas de alcance:** (a) la **persistencia por dispositivo (3.3)** NO está — el lado driver (JNI `getConvergedTuning`/`setInitialTuning`) se implementa junto con **App D**; (b) los items 2.2 (timing por dirección) y 2.4 (hardening) del spec `fase_2_ajuste_fino_adaptativo.md` quedaron **fuera de E3** — reevaluarlos tras la campaña de validación: si el round-trip medido (E4) ya cumple ≤9 ms estables, se descartan |
| E4 (round-trip) | ⬜ Pendiente — **próxima**, en paralelo con App V | |
| E5 (SRP split) | 🟠 **En curso** — 1ª costura hecha 2026-07-09 (`usb/DspBlockOps.h`) | Incluye los tests duplex diferidos de E2.4. Ver addendum abajo |
| E6 (bit-perfect) / E7 (microframe) | ⬜ Pendientes, sin cambios | E7 mantiene su go/no-go tras E4 |

**Secuencia repriorizada** (razones en `NoisyPad/docs/usb-audio/PLAN_VALIDACION_ON_DEVICE_2026-07.md`): fixes F1–F4 → E4 ∥ App V → campaña de validación on-device (3 DACs) → App C → App D (incluye persistencia 3.3) → E5 → resto.

### Addendum Etapa 5 — primera costura 2026-07-09 (`usb/DspBlockOps.h`)

Estrategia: **una extracción por PR, bit-idéntica, validada con la suite host** (la única red disponible en el sandbox; el NDK no buildea acá). El orden lo dicta el valor/riesgo/testabilidad, no el tamaño.

- **Descartado como primera costura: `PacketCodec`** (fill/processTransfer de UsbTransferManager) — está fuertemente entrelazado con descriptores libusb + rings + estado miembro; las partes puras (float↔PCM, ChannelMap) ya están extraídas y testeadas → alto riesgo, bajo valor de test.
- **Costura 1 — transformaciones DSP puras del loop → `usb/DspBlockOps.h`** (`monoToStereo` −3dB, `spliceDeclickHead`, `fadeBlockToSilence`). El de-click de splice estaba **duplicado** inline (splice-pending + underrun-fade); ahora es una sola función. `test_dsp_block_ops.cpp` con golden values a mano, incl. la secuencia del underrun (hold→fade→declick) — cubre H6. Bit-idéntico. `LibusbBackend.cpp` −23 líneas. (build exitoso en AS confirmado por el usuario 2026-07-09).
- **Costura 2 — layout de packets de salida → `usb/PacketLayout.h`** (`computeOutputPacketLayout`: sizing + clamp al slot + re-derive con truncación entera), sacada de `UsbTransferManager::fillOutputTransfer`. El clamp es un **invariante de correctness** (byte length > slot → el kernel linux_usbfs camina offsets fuera de la allocation → distorsión brutal), antes inline y sin test. `test_packet_layout.cpp` (no-clamp, clamp+re-derive, boundary exacto, truncación). Bit-idéntico. **Suite host 490/490.**
- **Nota de estrategia:** las dos costuras hechas fueron **piezas puras host-testeables** (bit-identidad demostrable sin hardware). El pozo de piezas puras del loop está mayormente agotado (DspPacer+DspBlockOps cubren pacer/trim/splice/fade/mono; PacketLayout cubre el sizing). `UsbVolumeController` ya está esencialmente extraído (`usb/UsbVolumeControl.h` + `test_feature_unit_caps.cpp`).
- **Costuras siguientes (grandes, libusb/estado-pesadas — NO host-testeables):** `usb/StreamNegotiator` (`configureSampleRate` ~821+ = control transfers directos con `mDeviceHandle`, topología, callbacks a miembros → extracción de clase pesada) y `usb/DspLoop` (cuerpo del loop, la más delicada, última). **Estas dos requieren el gate del plan que no tengo en el sandbox: profiler stats antes/después + round-trip en hardware.** Hacerlas a ciegas (solo build) va contra la disciplina bit-idéntica de E5. Recomendación: hacerlas con el usuario en un loop de build/on-device.

---

Este plan **continúa** el programa `docs/usb_latency/00_indice.md` (derivado de `docs/auditoria_usb_uac_latencia.md`, 2026-06-12) — no lo reemplaza. Aquí se consolida el estado real verificado en código a 2026-07-07 y se agrega lo que el programa de latencia no cubre: refactoring SRP, deuda de mecanismos duplicados, sanitizers y plan de tests.

---

## 1. Estado actual verificado (qué ya es primer nivel)

Verificado leyendo el código actual (no solo docs):

| Área | Estado | Evidencia |
|---|---|---|
| RT-safety del hot path | ✅ Sólido | `dspThreadFunc` (`LibusbBackend.cpp:1676–2200`): sin mutex, sin alloc, solo atomics + semáforo `mDspWake` (counting_semaphore, release wait-free). Buffers preasignados en `start()` con `MemoryUtils::prepareVectorForRealtime()` |
| Handoff DSP↔USB | ✅ Lock-free | `LockFreeRingBuffer` SPSC con acquire/release correctos (`dsp/LockFreeRingBuffer.h:49–71`) |
| Prioridades/afinidad | ✅ | SCHED_FIFO en DSP y event loop, pinned a big cores (`LibusbBackend.cpp:1682–1688`, `UsbTransferManager.cpp:1447–1456`) |
| Fase 0 (clock sync) | ✅ Implementada | `ClockController.h`: acumulador fraccional puro con Ff como consigna (sin PID; el signo invertido C1 de la auditoría está corregido) |
| Fase 1 (perfiles de latencia) | ✅ Implementada | `usb/LatencyProfile.h`: SAFE (8 ms transfer + 24 ms jitter = 32 ms standing) y LOW_LATENCY (1 ms + 4 ms = 5 ms de salida; ~10–14 ms round-trip). Pacer por nivel de ring en el DSP loop (`LibusbBackend.cpp:1841–1895`) con gate de input, producción de emergencia (`outputCritical`) y trim de exceso combinado con splice fade |
| Fase 2 (adaptativo) | 🟡 Parcial | Ratchet de jitter budget ante underrun (`UsbTransferManager.cpp:846–866`, `mJitterExtraMs` +1 ms con warmup y cap). Falta: convergencia hacia abajo, persistencia por dispositivo, retiro del `AdaptiveBufferController` legacy |
| Formatos asimétricos in/out | ✅ | `inputPcmFormat` separado del de salida (fix GHW 24-out/16-in, `UsbTransferManager.cpp:1320–1321`) |
| Feedback implícito | ✅ | `ImplicitFeedbackEstimator` alimenta el ClockController cuando hay capture y no hay feedback EP explícito (`LibusbBackend.cpp:1591–1627`) |
| Robustez | ✅ Buena | Watchdog 500 ms, `RecoveryPolicy`, detección de disconnect por dos vías, teardown con cancel+drain de transfers, burst de semáforo para destrabar el DSP |
| Tests | 🟡 Buena base | 21 suites host-side en `usb/tests/` (googletest, corren en CI vía `scripts/run-cpp-tests.sh` en cada PR). Lógica pura bien cubierta; falta el pacer/trim/duplex y sanitizers |

**Conclusión del review:** la arquitectura es correcta y el trabajo pesado de latencia (Fases 0–1) ya está hecho. Lo que separa esto de "primer nivel" no son bugs estructurales sino: (a) dos mecanismos adaptativos coexistiendo, uno de los cuales ajusta la variable equivocada; (b) el tail de stalls de scheduling >8 ms que el propio código señala como "necesita ADPF"; (c) dos god-files que concentran todo; (d) huecos de test justo en la lógica más delicada (pacer/trim/duplex); (e) sin medición round-trip real (Fase 5) para verificar nada de lo anterior en hardware.

---

## 2. Hallazgos

### H1 — Mecanismos adaptativos duplicados y contradictorios (ALTO, deuda estructural)
`LibusbBackend.cpp:1810–1826` sigue evaluando `AdaptiveBufferController` + `requestBufferResize()`/`performBufferResize()` (redimensiona la **capacidad** del ring 50–200 ms), mientras `UsbTransferManager.cpp:846–866` ya ajusta el **jitter budget** (la palanca real de latencia, hallazgo L5 de la auditoría). La capacidad del ring no controla la latencia — el controller legacy ajusta la variable equivocada, agrega un camino de resize en caliente (`ResizableRingBuffer`) que ya no hace falta, y sus stats se exportan por JNI a la UI de NoisyPad como si fueran "el" mecanismo.

### H2 — Sin ADPF: el tail de stalls >8 ms está identificado y sin atacar (ALTO, latencia)
El propio código lo dice: `LatencyProfile.h:50–59` ("the residual tail (>8 ms) needs ADPF, not more queue") y `UsbTransferManager.h:799` ("15 ms+ means scheduling pressure that only ADPF/priority can fix"). LOW_LATENCY hoy compensa con 8 URBs en vuelo. `APerformanceHintSession` (API 31+) reduce el riesgo dominante: DVFS bajando la frecuencia del core ante cargas periódicas cortas. Ya está especificado en `fase_3_experimental_microframe.md` §3.2.6 pero no depende de la Fase 3 — aplica ya a LOW_LATENCY.

### H3 — God-files (MEDIO, mantenimiento/SRP)
- `LibusbBackend.cpp` (2724 LOC): inicialización libusb + negociación de sample rate + selección de interfaces + DSP loop + volumen/feature units + stats + resize. El DSP loop (~520 líneas) mezcla pacing, trim, splice, mono→stereo, volumen y evaluación adaptativa.
- `UsbTransferManager.cpp` (1734 LOC): scheduling iso + conversión de formato + feedback + estadísticas + recovery + event loop.
La lógica de pacing/trim/splice es exactamente la más delicada y hoy es intesteable porque vive inline en el loop.

### H4 — Ratchet de jitter solo sube; sin persistencia por dispositivo (MEDIO, latencia)
`mJitterExtraMs` sube +1 ms por underrun (cap) y nunca baja: una ráfaga transitoria (p. ej. termal) deja la sesión con latencia extra permanente. El diseño completo ya está en `fase_2_ajuste_fino_adaptativo.md` (histéresis asimétrica, piso por dispositivo, persistencia VID:PID).

### H5 — Fase 5 (round-trip real) sin implementar (ALTO, verificación)
`runLoopbackTest` devuelve SKIPPED (`UsbAudioTestRunner.kt:373`); `jni_benchmark.cpp:129–164` es scaffolding muerto. Sin medición round-trip analógica no hay criterio de aceptación objetivo para H2/H4 ni números reales por dispositivo. El diseño (chirp + correlación + callback swap) está completo en `fase_5_test_roundtrip_dispositivo.md`.

### H6 — Huecos de test en la lógica crítica (MEDIO, tests)
Bien cubierto: descriptores, iso timing, clock, rate requests, recovery, ring resizable. Sin cubrir:
- El **pacer** (outputReady/inputReady/outputCritical) y el **trim de exceso combinado** (`LibusbBackend.cpp:1841–1895` y 1920–1950) — lógica pura pero inline en el loop.
- Secuencias duplex: underrun de input → last-valid-block fade → splice al reanudar.
- Sanitizers: la suite host corre sin ASan/UBSan/TSan; los bugs de ordering/UAF en teardown son justamente los que un TSan pesca.

### H7 — Menores (BAJO)
- Mapeo bitDepth→PcmFormat duplicado (`UsbTransferManager.cpp:1477–1486` vs `AudioFormatConverter.h:52–67`).
- Mono→stereo con −3 dB hardcodeado (`LibusbBackend.cpp:1971`); dither TPDF con seed fija y sin control (cubierto por `fase_4_calidad_bitperfect.md`).
- Watchdog 500 ms fijo, no correlacionado con la duración de transfer del perfil activo (con transfers de 1 ms, 500 ms = 500 transfers perdidos antes de reaccionar).
- `ThreadUtils::setCurrentThreadRealtime` no reporta si SCHED_FIFO fue concedido o cayó al fallback de nice (la Fase 3 lo requiere como precondición consultable).

---

## 3. Plan de implementación incremental

Orden elegido para que **cada etapa deje el sistema verificable y shippeable**. La regla transversal del programa sigue vigente: nada de mutex/alloc/syscalls en RT path; todo lo nuevo extraíble se testea host-side; SAFE queda bit-idéntico como red de seguridad.

### Etapa 1 — Quick wins (1–2 días, bajo riesgo)

| # | Cambio | Archivos | Valor |
|---|---|---|---|
| 1.1 | **ADPF**: registrar DSP thread y USB event thread en `APerformanceHintSession` con target = duración de bloque; guard por API level (dlsym / `__builtin_available`, minSdk 29). Reportar `reportActualWorkDuration` desde el DSP loop (una llamada wait-free por iteración) | `common/ThreadUtils.*`, `LibusbBackend.cpp`, `UsbTransferManager.cpp` | Ataca directamente el tail >8 ms; es EL quick win de latencia señalado por el propio código |
| 1.2 | `ThreadUtils::setCurrentThreadRealtime` devuelve el resultado real del scheduling (FIFO concedido / fallback nice) y se expone en telemetría | `common/ThreadUtils.*` | Precondición de Fase 3; diagnóstico en campo |
| 1.3 | Unificar bitDepth→PcmFormat en un solo helper en `AudioFormatConverter.h` | `UsbTransferManager.cpp` | Elimina divergencia futura |
| 1.4 | Watchdog en múltiplos del transfer del perfil activo (p. ej. `max(100 ms, 50 × transferMs)`) en vez de 500 ms fijo | `RecoveryPolicy.h`, `UsbTransferManager.cpp` | Detección proporcional al régimen |
| 1.5 | Borrar scaffolding muerto de round-trip (`jni_benchmark.cpp:129–164`, estado que nunca avanza) dejando stubs deprecated para no romper `LatencyAnalyzer.kt` | `jni/jni_benchmark.cpp` | Limpieza previa a Fase 5 |

**Verificación:** suite host verde + WMA_AUDIT en los 3 DACs con LOW_LATENCY; comparar `outputWireGaps`/`maxGapMs` antes/después de ADPF (esperable: tail >8 ms colapsa).

### Etapa 2 — Sanitizers y tests del pacer (2–3 días, solo tests/CI)

| # | Cambio | Valor |
|---|---|---|
| 2.1 | Job de CI adicional corriendo la suite host con **ASan+UBSan** y otro con **TSan** (los rings lock-free y el teardown son los candidatos #1 a data races latentes) | Red de seguridad permanente para todo lo que sigue |
| 2.2 | **Extraer el pacer a `usb/DspPacer.h`** (header puro, sin libusb): entradas = niveles de ring/targets/flags; salida = decisión {WAIT, PRODUCE, PRODUCE_CRITICAL} + cantidad a trimear. El DSP loop lo consume; comportamiento bit-idéntico | Convierte las ~110 líneas más delicadas del driver en lógica testeable |
| 2.3 | Tests de `DspPacer`: PLAYBACK_ONLY paced por ring, duplex gated por input, `outputCritical`, trim de exceso combinado (ley de conservación: input target + output target constantes), arranque duplex con `startupLeadFrames` | Cubre H6 |
| 2.4 | Test de secuencia duplex con transfer manager simulado (completions inyectadas): underrun input → fade → splice; disconnect mid-stream → salida limpia del loop | Cubre las secuencias que hoy solo se validan en hardware |

**Nota:** 2.2 es el primer paso del refactor SRP (Etapa 5) — se hace antes porque desbloquea los tests sin esperar al split grande.

### Etapa 3 — Completar Fase 2: adaptativo convergente + persistencia (3–5 días)

Implementar `fase_2_ajuste_fino_adaptativo.md` tal como está especificada, con estas precisiones:

1. **`mJitterBudgetMs` atómico vivo** en UsbTransferManager con `setJitterBudgetMs()` (clamp [1,16]); `getOutputRingTargetLevel()` lo lee por iteración → ajuste en caliente sin resize ni glitch (el diseño 2.1a del spec).
2. **Retirar `AdaptiveBufferController` del camino activo** (H1): `dspThreadFunc` deja de evaluarlo; `requestBufferResize`/`performBufferResize`/`ResizableRingBuffer` quedan deprecated (no borrar aún — la UI de NoisyPad consume sus stats; coordinar con el plan hermano). El lazo nuevo (XRUN sube ×2 rápido / ESTABLE baja −1 ms lento / PISO por sesión) reemplaza al ratchet actual de solo-subida (H4).
3. **Persistencia por dispositivo**: API JNI/Kotlin `getConvergedTuning()/setInitialTuning(vidPid, budget, measuredRtt)`; el almacenamiento (DataStore) vive en NoisyPad. Arranque de sesión con el valor convergido → el dispositivo conocido arranca ya en su mínimo.
4. Ventanas de evaluación **por tiempo** (2 s), no por conteo de callbacks (con bloques de 2 ms, 100 callbacks = 200 ms).
5. Tests host del lazo de control (histéresis, cooldown, piso) — es lógica pura.

**Verificación:** en los 3 DACs, budget converge y se estabiliza; tras ráfaga inducida de carga, vuelve a bajar (H4 resuelto); round-trip estable ~6–9 ms en LOW_LATENCY.

### Etapa 4 — Fase 5: medición round-trip real (3–4 días)

Implementar `fase_5_test_roundtrip_dispositivo.md`: `usb/RoundTripMeasurer.{h,cpp}` (chirp Hann 10 ms 500 Hz–6 kHz, correlación en worker thread, plano RT sin análisis), `mCallback` atómico + `swapCallback()` en LibusbBackend, API Kotlin (`runRoundTripTestFlow` migrado, `runLoopbackTest` real). 

**Por qué en esta posición:** es la herramienta de aceptación de las Etapas 1 y 3 y la fuente del `measuredRoundTripMs` que persiste la Etapa 3.3. Antes de tocar más el driver, conviene poder medirlo de verdad. (Puede paralelizarse con la Etapa 3 si hay capacidad — no comparten archivos calientes salvo LibusbBackend.h.)

**Verificación:** loopback físico miniplug en DAC full-duplex; resultado reproducible ±0.5 ms; correlación con el desglose del profiler.

### Etapa 5 — Refactor SRP de los god-files (4–6 días, sin cambio de comportamiento)

Split guiado por las costuras que ya existen, en PRs chicos y bit-idénticos (validar con la suite + round-trip test de la Etapa 4):

- **`LibusbBackend`** queda como orquestador fino. Se extraen:
  - `usb/StreamNegotiator` — negociación de sample rate/coerción/retry + selección de interfaces (hoy `configureSampleRate()` líneas 817–1060 + setup).
  - `usb/DspLoop` — el cuerpo del loop, consumiendo `DspPacer` (ya extraído en 2.2) + splice/trim/mono→stereo/volumen como funciones puras testeables.
  - `usb/UsbVolumeController` — feature units (SET_CUR/GET_CUR de volumen/mute), hoy entrelazado en el backend.
- **`UsbTransferManager`** se parte en: transfer scheduling/event loop (queda) + `usb/PacketCodec` (layout de packets + conversión, hoy `fillOutputTransfer`/`processInputTransfer` líneas 1157–1410) — el codec es puro y gana tests de golden-data con los traces reales que ya usan los tests de descriptores.

**Regla:** un PR por extracción, `git diff` del ensamblado final revisable, cero cambios de timing (verificar con profiler stats antes/después en hardware).

### Etapa 6 — Calidad bit-perfect (Fase 4) (2–3 días, independiente)

`fase_4_calidad_bitperfect.md` tal cual: dither configurable (off/TPDF) + redondeo correcto, gain mono→stereo configurable (retirar −3 dB hardcodeado como único comportamiento), verificación de round-trip bit-exacto 24-bit con el codec extraído en la Etapa 5 (test host de ida y vuelta float↔PCM).

### Etapa 7 — Fase 3 experimental: microframe <5 ms (opt-in) (1 semana, riesgo alto)

Solo después de ADPF (Etapa 1) + medición real (Etapa 4): URBs de 125 µs en UAC2 HS, gated por perfil experimental opt-in, precondición = SCHED_FIFO confirmado (1.2). Criterio go/no-go: si tras ADPF el p99 de `maxGapMs` sigue >4 ms en los devices objetivo, el piso es de scheduling y esta fase se pospone (está documentado como el límite duro probable en el spec §DVFS).

---

## 4. Secuencia y dependencias

```
Etapa 1 (quick wins: ADPF, watchdog, dedup)      ──┐
Etapa 2 (sanitizers + DspPacer + tests)          ──┼──► Etapa 3 (Fase 2 completa: adaptativo+persistencia)
                                                    │         │
                                                    └──► Etapa 4 (Fase 5: round-trip real)  [paralelizable con 3]
                                                              │
                                              Etapa 5 (refactor SRP, validado con 4)
                                                              │
                                        Etapa 6 (Fase 4 bit-perfect)   Etapa 7 (Fase 3 experimental, go/no-go)
```

Coordinación con NoisyPad (ver plan hermano): la Etapa 3.2 deprecia los stats del AdaptiveBufferController que la UI muestra; la 3.3 y la 4 requieren plumbing Kotlin/UI del lado app. Publicar cada etapa como versión SNAPSHOT y validar con `includeBuild` local antes de release.

## 5. Criterios de aceptación globales

- **Latencia:** LOW_LATENCY round-trip medido (Fase 5, analógico) ≤ 9 ms estable en los 3 DACs de referencia; p99 de `maxGapMs` < 4 ms con ADPF.
- **Estabilidad:** 0 underruns/hora en SAFE; < 1 evento de ratchet/hora en LOW_LATENCY tras convergencia; disconnect/replug sin crash ni leak (TSan/ASan verdes).
- **Calidad:** round-trip 24-bit bit-exacto con dither off; sin clicks en trim/splice (verificable por inspección del capture del round-trip test).
- **Tests:** pacer, trim, lazo adaptativo y codec con suites host; CI con sanitizers en verde.
- **SRP:** ningún archivo del subsistema USB > ~800 LOC; hot path íntegramente compuesto de unidades puras testeadas.

## 6. Riesgos

| Riesgo | Mitigación |
|---|---|
| ADPF no disponible / sin efecto en devices viejos (API < 31) | Guard por dlsym; el comportamiento actual queda intacto; medir con Fase 5 |
| Refactor SRP introduce regresión de timing | PRs bit-idénticos + profiler stats antes/después + round-trip test como gate |
| Retiro del AdaptiveBufferController rompe la UI de NoisyPad | Deprecar (no borrar) JNI; coordinar con plan hermano antes del release |
| Fase 3 inviable por DVFS | Go/no-go explícito tras Etapa 4; el spec ya lo anticipa |
