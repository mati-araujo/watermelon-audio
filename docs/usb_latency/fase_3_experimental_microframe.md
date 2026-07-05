# Fase 3 — Modo experimental sub-5 ms (servicing por microframe, solo UAC2 HS)

**Objetivo:** explorar el piso físico de usbfs/libusb en Android: URBs de 125–250 µs, bloques DSP de 16–32 frames, jitter budget de 1 ms. **Opt-in explícito**, gated por hardware probado, nunca default. Si el resultado en hardware real no baja de ~6 ms o el costo de CPU es inaceptable, la fase se cierra documentando el límite — eso también es un resultado válido.

**Depende de:** Fases 0–2 validadas; Fase 5 disponible (sin el test de round-trip automatizado no se puede evaluar honestamente).
**Estimación:** 3–4 días de experimentación instrumentada.

---

## 3.1 — Configuración objetivo

Nuevo preset en `UsbLatencyTuning`:

```cpp
static UsbLatencyTuning ultraLowLatency() {
    return {
        .targetTransferMs = 0,    // 0 = "mínimo posible": packetsPerTransfer
                                  //     forzado a 1–2 (ver 3.2)
        .numTransfers     = 12,   // 1.5–3 ms de cola en vuelo
        .jitterBudgetMs   = 1,
        .dspBlockFrames   = 32,   // ~0.67 ms @48k
        .ringCapacityMs   = 20,
    };
}
```

Presupuesto teórico (48 kHz HS, bInterval=1, 6 frames/packet):

| Etapa | ms |
|---|---|
| Transfer IN (URB de 2 packets) | 0.25–0.5 |
| Espera bloque entrada (32 fr) | 0–0.7 |
| Bloque DSP | 0.67 |
| Ring salida (0.25 + 1) | 1.25 |
| URBs OUT en vuelo (promedio) | 0.75–1.5 |
| Conversores | ~1 |
| **Total** | **~4–6 ms** |

El término irreducible son los conversores + el scheduling de URBs del xHCI (los URBs se encolan con ≥1 microframe de antelación). **< 5 ms es alcanzable solo si todo lo demás coopera.**

## 3.2 — Cambios de código

1. **`calculateIsoTransferTiming`**: soportar `targetTransferMs=0` → `packetsPerTransfer = clamp(2, ...)` en HS (2 packets = 250 µs amortigua el costo por URB; bajar a 1 solo si la medición lo justifica). En FS este preset **se rechaza** (`start()` falla con error claro: el packet de 1 ms ya es el piso).
2. **Gating duro en `setLatencyTuning`**: aceptar `ultraLowLatency` solo si `libusb_get_device_speed ∈ {HIGH, SUPER, SUPER_PLUS}` y `uacVersion == 2`. Si no, degradar a `lowLatency` con warning en el resultado (el wrapper Kotlin devuelve el perfil efectivo).
3. **Event loop**:
   - 8000 completions/s con URBs de 2 packets = 4000 URB completions/s. `libusb_handle_events_timeout_completed` con timeout de 1 ms introduce hasta 1 ms de retardo de servicing → **bajar el timeout a 200 µs** en este perfil (campo en TransferConfig), o mejor: usar el fd-set de libusb con `poll()` directo y timeout fino. Mantenerlo simple primero: timeout 200 µs y medir.
   - El drain de wakes del DSP (`mDspWake`) recibe 4000 releases/s — ya está amortizado por el drain loop; sin cambios.
4. **Probe de viabilidad en runtime** (clave del opt-in): antes de aceptar el perfil, correr automáticamente un stream silencioso de 10 s con la config ultra y evaluar `packetsErrors + underruns + overruns == 0` y CPU del event thread < 15 %. API: `wma_usb_probe_latency_profile(profile) → bool`, expuesta a Kotlin (`suspend fun probeUsbLatencyProfile(...)`). NoisyPad la usa para habilitar/deshabilitar la opción en UI.
5. **Prioridades**: este perfil **requiere** que `pthread_setschedparam(SCHED_FIFO)` haya tenido éxito en DSP y event thread (en Android moderno suele concederse a apps con `android.permission.HIGH_SAMPLING_RATE_SENSORS`-class exemptions vía `performance hint`/`ADPF`; si `setCurrentThreadRealtime` cayó al fallback de nice, el probe debe fallar). Añadir el resultado real del scheduling a `ThreadUtils` como valor de retorno consultable.
6. **ADPF (Android Dynamic Performance Framework)**: registrar los dos threads en un `APerformanceHintSession` con target duration = duración de bloque. Disponible desde API 31+ (min SDK 29 → guard por `__builtin_available`/dlsym). Esto reduce el riesgo dominante (DVFS bajando la frecuencia del core en cargas periódicas cortas).

## 3.3 — Instrumentación de la experimentación

Para cada DAC y cada combinación `{packetsPerTransfer ∈ 1,2,4} × {numTransfers ∈ 8,12,16} × {dspBlock ∈ 16,32,48}`:

- Round-trip medido (Fase 5), p50/p95 sobre 20 mediciones.
- `packetsErrors`, xruns por hora.
- CPU por thread (`simpleperf stat -t <tid>`), consumo (batterystats delta).
- Registrar en una tabla en `docs/usb_latency/resultados_fase3.md` — el preset final se decide con datos.

## 3.4 — Criterios de salida (cualquiera cierra la fase)

- **Éxito:** algún DAC sostiene round-trip p95 < 5 ms, 30 min sin xrun, CPU event+DSP < 25 % total → el preset queda publicado tras el probe.
- **Límite documentado:** ninguna combinación baja de ~6 ms o el costo es inaceptable → se documenta el piso medido por dispositivo y el preset queda detrás de un flag de build (no de producto).

## Riesgos
- **DVFS/thermal**: cargas periódicas de <1 ms invitan al governor a bajar frecuencia → deadline misses intermitentes. Mitigación: ADPF (3.2.6); si no alcanza, este es probablemente el límite duro.
- **Costo por URB de usbfs**: cada submit/reap es un ioctl; 8000/s ≈ presupuesto de syscalls no trivial en cores LITTLE. La medición decide.
- **Variabilidad entre teléfonos**: el probe de runtime es la única defensa realista; nunca publicar el preset sin probe.
