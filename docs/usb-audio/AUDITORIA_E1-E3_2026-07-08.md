# Auditoría de Implementación — Driver E1, E2, E3(core) — 2026-07-08

**Contra:** `docs/usb-audio/PLAN_DRIVER_2026-07.md` (Etapas 1–3).
**Commit auditado:** `38e2979` "usb improvements".
**Contexto:** E1 y E2 con build verificado; **E3 nunca se compiló** (sandbox sin gradlew) — auditoría profunda de compilabilidad y corrección.
**Plan de validación on-device:** `NoisyPad/docs/usb-audio/PLAN_VALIDACION_ON_DEVICE_2026-07.md`.

---

## Veredicto general

| Etapa | Veredicto | Notas |
|---|---|---|
| E1 (ADPF, SchedResult, dedup, watchdog, stubs) | ✅ OK | 5/5 items fieles al plan; ADPF activo en debug **y** release (dlsym runtime) |
| E2 (sanitizers, DspPacer, tests) | ✅ OK | Bit-identidad del pacer/trim **verificada por diff** contra `a116b5b`; suite usb instrumentada por ASan/UBSan/TSan en CI |
| E3 core (jitter budget convergente) | 🟠 Compila, pero **1 bug de contrato** + 2 menores | Ver F1–F3. La lógica del controller y el CAS de 2 escritores son correctos |

**E3 sí compila** (verificado símbolo a símbolo: includes, `std::clamp` resuelto vía `JitterBudgetController.h:24` → `UsbTransferManager.h:43`, designated initializers en C++20 en ambos builds, tests registrados en `usb/tests/CMakeLists.txt:61-62`, sin referencias stale a `mJitterExtraMs`).

---

## Hallazgos a corregir ANTES de la validación on-device

### F1 — SAFE no es bit-idéntico tras un underrun (BUG de contrato) 🟠
`UsbTransferManager.cpp:74-76` + `:872-882` + `JitterBudgetController.h:20-22`.

- El up-ratchet (+1 ms/underrun) **no está gateado por perfil** — igual que antes (comportamiento viejo preservado ✅): en SAFE puede subir 24→36.
- Lo nuevo: `mJitterBudgetMinMs = initialBudget` (24) **no congela nada** cuando el budget subió por ratchet — `onWindow()` devuelve `LOWER` mientras `currentBudget > floor`, así que el converger baja 36→24 en ventanas limpias. **El SAFE viejo nunca bajaba.**
- Los comentarios afirman un invariante falso ("min == initial, so the down-convergence can never lower it") en 3 lugares: `JitterBudgetController.h:20-22`, `UsbTransferManager.cpp:69-72`, `UsbTransferManager.h:471-472`.
- El test `SafeProfileIsFrozen` no simula el ratchet previo, por eso pasa.

**Fix (fiel al comportamiento viejo):** deshabilitar el down-converger cuando el perfil está en régimen congelado — early-return en `maybeConvergeJitterBudget()` si `mJitterBudgetMinMs >= mConfig.jitterBudgetMs` (o flag explícito `downConvergenceEnabled` derivado del perfil en `configure()`). Corregir los 3 comentarios. Agregar test: *SAFE con ratchet previo a 27 → N ventanas limpias → sigue en 27*.

Impacto práctico bajo (SAFE tiene 24 ms de margen y converger hacia abajo es hasta deseable), pero el contrato "SAFE = red de seguridad bit-idéntica" es la base de todo el programa — o se cumple o se re-declara explícitamente en el plan.

### F2 — Data race en telemetría del piso (menor) 🟡
`JitterBudgetController::mFloor` es `int` no-atómico; lo escribe el DSP thread (`onWindow`) y lo leería un thread JNI vía `getConvergedFloorMs()`. TSan lo flaggearía en device (los jobs de CI no lo ejercitan multi-thread). **Fix:** espejo atómico del floor para el getter (o documentar el getter como DSP-thread-only y no exponerlo a JNI así).

### F3 — Código muerto residual del retiro del AdaptiveBufferController (menor) 🟡
`LibusbBackend.cpp:1832-1834` mantiene `if (mBufferResizePending) performBufferResize()`, pero `requestBufferResize` (único setter) ya no tiene **ningún** caller. Inofensivo pero confuso. **Fix:** eliminar el check del loop; `performBufferResize`/`ResizableRingBuffer` quedan solo tras el JNI deprecated hasta App D.

### F4 — Menores de higiene
- `LibusbBackend.cpp:1717` usa `syscall(SYS_gettid)` sin `#include <sys/syscall.h>`/`<unistd.h>` explícitos (resuelve por includes transitivos — agregar explícitos).
- `test_jitter_budget_controller.cpp` tiene 7 casos, no los 8 reportados; el caso SAFE-con-ratchet falta (se agrega con F1).
- Nota ADPF: el tid del event loop se co-registra best-effort (si el event thread aún no llegó a guardar su tid, solo se registra el DSP). Aceptable; el orden actual (`mTransferManager->start()` antes de crear el DSP thread) lo hace improbable.

---

## Verificaciones que dieron OK (sin acción)

- **E2 bit-identidad**: `evaluatePacer`/`computeTrimBlocks` producen decisiones idénticas al código inline anterior (mismo predicado WAIT, misma ley signed de excess y decremento; el for-con-break del caller equivale al while viejo).
- **CAS de 2 escritores** (`adjustJitterBudgetMs`, event thread ↑ / DSP ↓): compare_exchange_weak correcto, sin pérdida de updates; relaxed es adecuado (escalar independiente).
- **RT-safety de lo nuevo**: `maybeConvergeJitterBudget` lee `steady_clock` (vDSO, sin trap) con early-out por iteración; `reportActualWorkDuration` es un forward de function pointer; sin allocs/locks nuevos en el hot path. El `LOGI` de LOWER (≤1/30–60 s) es de la misma clase que la línea WMA_CLOCK existente.
- **Debug vs release**: sin `assert()` en código nuevo; `WMA_USB_DIAG` compila fuera en ambos salvo opt-in; telemetría `jbMs` está en la línea clock-health **siempre activa** (visible en logcat release); ADPF activo en ambos build types.
- **Interacción ratchet↑/converger↓**: el pin del piso lee el valor ya ratcheteado, tal como modelan los tests.

## Orden sugerido

F1–F4 son un PR chico (~2 h) sobre `usb/` + 1 test nuevo. Hacerlo **antes** del primer build de validación on-device para no perseguir fantasmas de comportamiento SAFE durante la campaña de pruebas.
