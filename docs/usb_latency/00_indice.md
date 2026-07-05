# Especificaciones — Programa de Latencia USB (UAC 1.0 / 2.0)

Derivadas de la auditoría [`docs/auditoria_usb_uac_latencia.md`](../auditoria_usb_uac_latencia.md).
Cada fase tiene su especificación técnica autocontenida, pensada como base de referencia durante el desarrollo.

| Fase | Spec | Objetivo | Depende de |
|---|---|---|---|
| 0 | [fase_0_correccion_clock_sync.md](fase_0_correccion_clock_sync.md) | Corrección del clock sync (signo PID, feedback implícito, 44.1 kHz, parser UAC1, coerción de rate, latencia reportada) | — |
| 1 | [fase_1_latencia_configuracion.md](fase_1_latencia_configuracion.md) | Round-trip ~10–14 ms vía configuración (transfers 1 ms, pacer en ms, bloque DSP chico) | Fase 0 |
| 2 | [fase_2_ajuste_fino_adaptativo.md](fase_2_ajuste_fino_adaptativo.md) | Round-trip ~6–9 ms: jitter budget adaptativo + timing por dirección + persistencia por dispositivo | Fases 0–1 |
| 3 | [fase_3_experimental_microframe.md](fase_3_experimental_microframe.md) | Modo experimental < 5 ms (URBs por microframe, solo UAC2 HS, opt-in) | Fases 0–2 |
| 4 | [fase_4_calidad_bitperfect.md](fase_4_calidad_bitperfect.md) | Bit-perfect: soft clip, redondeo, dither configurable, gain mono | Independiente |
| 5 | [fase_5_test_roundtrip_dispositivo.md](fase_5_test_roundtrip_dispositivo.md) | Test de round-trip en dispositivo real (loopback físico miniplug), API para UI en NoisyPad | Fase 0 (L7); ideal tras Fase 1 |

## Reglas transversales (aplican a todas las fases)

- **RT-safety**: nada de mutex/alloc/syscalls en el DSP thread ni en los callbacks de libusb (event thread). Logging solo fuera del hot path o rate-limited tras flag de diagnóstico.
- **Convención del repo**: nuevas funciones JNI en `jni/jni_audio_bridge.cpp` + `AudioNativeBridge.kt` con wrapper `Result<T>` y mutex de categoría; API pública en `IAudioNativeBridge` (commonMain); considerar espejo en C API `watermelon_audio.h/cpp`.
- **Tests nativos**: la suite host-side vive en `audio/src/main/cpp/usb/tests/` (googletest, CMake standalone). Toda lógica nueva extraíble (sin libusb) debe ser testeable ahí, siguiendo el patrón de `UsbIsoTiming.h` / `SampleRateRequest.h`.
- **Verificación en hardware**: cada fase cierra con validación en los 3 DACs de prueba (incluye el GHW USB AUDIO 24-bit out / 16-bit in) usando los logs `WMA_AUDIT` y, desde Fase 5, el test de round-trip automatizado.
- **Compatibilidad**: el comportamiento actual queda disponible como perfil `SAFE`; los cambios de latencia se activan por perfil para no regresionar dispositivos no probados.
