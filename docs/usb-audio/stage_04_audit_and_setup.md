# Stage 4 audit and setup

**Estado:** setup previo listo para desarrollo.
**Fecha:** 2026-05-04.
**Base asumida:** Stage 3 cerrado funcionalmente, con validacion manual de hardware OK y pendientes largos documentados.

Este documento audita `stage_04_mixing_routing.md` contra el codigo real y fija el orden recomendado antes de implementar. La idea es reducir superficie de riesgo: primero estabilidad y pruebas puras, despues routing/control, y recien al final composicion de backends.

---

## 1. Hallazgos de auditoria

### Criticos

1. **El resize del ring buffer es el primer bloqueo real.**
   - Confirmado en codigo: `UsbTransferManager::reconfigureBufferSize()` llama `LockFreeRingBuffer::resize()` mientras el DSP thread y el USB event thread pueden leer/escribir.
   - `LockFreeRingBuffer::resize()` documenta "only when not in use" y realloca el `std::vector`, por lo que el plan de Stage 4 debe atacar esto antes de `SplitBackend`.
   - Accion: implementar un swap atomico de buffers o un resize coordinado por pausa/restart corto. No agregar mas consumidores/productores hasta cerrar esto.

2. **`SplitBackend` depende de input real de Oboe, pero Oboe todavia no entrega input al callback.**
   - Confirmado en codigo: `OboeBackend::onAudioReady()` pasa `inputData = nullptr`; `openInputStream()` existe, pero no hay puente de lectura input -> callback.
   - Consecuencia: `SplitBackend` Oboe-in/USB-out no puede validar el caso principal si antes no se implementa input capture real en Oboe o una interfaz de input-source separada.
   - Accion: separar Stage 4 en dos gates: `Backend input source contract` y luego `SplitBackend`.

3. **El contrato `IAudioBackend` no distingue source/sink.**
   - Hoy un backend es "ambas cosas" y `onAudioReady(output, input, frames)` asume un callback unico. Para split, el output sink debe ser el unico que llama al callback del usuario, mientras el input source solo produce frames.
   - Consecuencia: componer dos `IAudioBackend` sin contrato adicional puede duplicar callbacks, arrancar dos DSP loops compitiendo, o descartar input real.
   - Accion: definir primero una politica de roles (`INPUT_SOURCE`, `OUTPUT_SINK`, `FULL_DUPLEX`) o interfaces internas pequenas para fuentes/sinks antes de crear `SplitBackend`.

4. **Recovery automatico puede confundirse con disconnect real.**
   - `UsbTransferManager::reportError(DEVICE_DISCONNECTED, ...)` marca `mDeviceDisconnected=true`, y varios paths tratan `LIBUSB_ERROR_IO` como disconnect.
   - Consecuencia: un retry tardio puede quedar bloqueado por estado desconectado, o un restart agresivo puede enmascarar un device muerto.
   - Accion: introducir un estado intermedio `TRANSIENT_ERROR/RESTART_REQUESTED` antes de tocar `DEVICE_DISCONNECTED`.

### Importantes

1. **El plan de double-buffer usa `std::thread(...).detach()` en el ejemplo.**
   - Esto no conviene en el motor real: complica lifetime si `UsbTransferManager` se destruye mientras el thread diferido aun referencia `this`.
   - Accion: preferir epoch/refcount interno, deferred free en event loop, o mantener ambos slots vivos hasta `stop()`.

2. **Los buffers temporales de conversion tambien son parte del resize.**
   - `mFloatBuffer` y `mPcmBuffer` se resizean en `configure()`. Si Stage 4 permite cambiar formatos/channels en caliente, tambien necesitan staging fuera del hot path.
   - Accion: Stage 4 resize solo debe cambiar `ringBufferMs`; cambios de formato/channels requieren stop/start hasta que haya staging completo.

3. **Per-channel volume no debe exponer fallback digital como si fuera control hardware por canal.**
   - Hoy `getCapabilities()` marca volumen como disponible por fallback digital. Para UI/API de canales fisicos, eso puede mentir.
   - Accion: separar `hardwarePerChannelCaps` de `digitalFallbackCaps`.

4. **Channel routing necesita semantica exacta por direccion.**
   - Output routing y input mix matrix tienen reglas distintas. Un solo tipo `ChannelRouting.Custom(map, matrix)` puede ser ambiguo para NoisyPad.
   - Accion: modelar `OutputChannelRouting` e `InputChannelRouting` internamente, aunque Kotlin exponga presets simples al principio.

5. **Los tests host actuales cubren USB puro, no backends.**
   - `audio/src/main/cpp/usb/tests` evita Oboe/libusb runtime real. `SplitBackend` necesita test doubles sin Android/Oboe.
   - Accion: antes de `SplitBackend`, crear un harness host de backends con `FakeAudioBackend` y clock virtual.

### Worth knowing

- El resampler lineal alcanza para tolerancia pequena, pero no para monitoreo de alta calidad a largo plazo.
- Split Oboe/USB probablemente tendra latencia variable por scheduler Android; los acceptance tests deben medir percentiles, no solo promedio.
- Routing multicanal requerira nombres/caps por canal si NoisyPad quiere UI profesional; no bloquear MVP por nombres.
- Recovery y adaptive buffer pueden pelearse: si ambos reaccionan al mismo underrun/error, pueden oscilar.

---

## 2. Orden recomendado de implementacion

### Slice 0 - Setup previo

- Congelar Stage 3 como baseline.
- Mantener `UsbIsoTiming` y sus tests como guardrail de pacing.
- No tocar migraciones AGP/KMP ni externalNativeBuild.
- Crear esta auditoria como fuente de verdad para Stage 4.

### Slice 1 - Ring buffer resize seguro

Objetivo: eliminar el riesgo de corrupcion sin cambiar API publica.

Entregables:
- Reemplazar `LockFreeRingBuffer::resize()` en caliente por swap/slots o resize coordinado.
- Mantener el hot path sin mutex, allocation, logging ni sleep.
- Test host de stress: writer + reader + resize repetido.
- Confirmar que `performBufferResize()` ya no depende de "resize immediately" como supuesto de seguridad.

Gate para avanzar:
- `usb_tests.exe` verde.
- Stress test 5 segundos sin crash/corrupcion.
- `assembleDebug` verde en 4 ABIs.

### Slice 2 - ChannelMap puro

Objetivo: routing/mixing testable sin USB ni JNI.

Entregables:
- `ChannelMap`/`ChannelMatrix` header-only o cpp pequeno.
- Presets: identity, swap L/R, left-only, mono downmix.
- Tests de buffers pequenos con valores deterministas.
- Integracion inicial solo dentro de conversion float PCM de USB, con default identity.

Gate para avanzar:
- Sin cambios de API Kotlin todavia.
- Default behavior bit-equivalent para stereo identity.

### Slice 3 - Per-channel hardware volume caps

Objetivo: exponer controles reales sin mentir con fallback digital.

Entregables:
- Extender `UsbVolumeControl` para GET/SET por canal fisico.
- Modelar caps por canal con `hasHardwareVolume`, `hasHardwareMute`, current state.
- Mantener fallback digital master existente.

Gate para avanzar:
- Si un canal no responde GET_CUR/SET_CUR, queda deshabilitado, no fatal.
- Compatibilidad NoisyPad: API nueva opcional, defaults anteriores intactos.

### Slice 4 - RecoveryPolicy

Objetivo: distinguir errores transitorios de disconnect.

Entregables:
- Estado `RESTART_REQUESTED`/evento interno, sin marcar `mDeviceDisconnected` salvo NO_DEVICE/watchdog final.
- Rate limit estricto de restarts.
- Test con error transient -> recovery -> stream sigue.

Gate para avanzar:
- No hay retry desde callback hot path.
- No hay loops infinitos; hard ceiling por minuto.

### Slice 5 - Backend input-source contract

Objetivo: desbloquear SplitBackend con input real.

Entregables:
- Definir si se agrega interfaz interna (`IAudioInputSource`) o roles en `IAudioBackend`.
- Implementar lectura de input real en Oboe o un adapter testable.
- Test double host con clock virtual.

Gate para avanzar:
- Un fake input source puede producir frames y un fake sink puede consumirlos sin Android.

### Slice 6 - SplitBackend MVP

Objetivo: Oboe input -> USB output como caso principal.

Entregables:
- Bridge SPSC preallocado.
- DriftResampler lineal preallocado.
- Solo output sink llama al callback del usuario.
- Stats: bridge underruns/overruns, source/sink latency, drift correction.

Gate para avanzar:
- Smoke test con fakes pasa.
- Manual hardware test Pixel + USB device.
- Documentar latencia esperada como compromiso, no como low-latency full-duplex.

### Slice 7 - API Kotlin/JNI

Objetivo: exponer solo lo ya probado nativamente.

Entregables:
- `ChannelRouting`/caps/volume por canal.
- Opciones de split backend si el MVP nativo paso.
- JNI siguiendo mutex por categoria y `Result<T>` para fallos.

Gate para avanzar:
- NoisyPad compila con API anterior sin cambios.
- Nuevas APIs son opt-in.

---

## 3. Setup de pruebas previo

### Host C++

- Mantener `audio/src/main/cpp/usb/tests` para componentes USB puros.
- Agregar tests nuevos por slice antes de integrar:
  - `RingBufferSwap` o equivalente.
  - `ChannelMap`.
  - `RecoveryPolicy` con estado puro o manager stub.
- Para `SplitBackend`, crear un harness separado con `FakeAudioBackend` sin Oboe/libusb.

### Android/Kotlin

- No crear tests JVM para USB Manager hasta que haya una capa pura testeable; hoy `UsbAudioManagerImpl` depende fuerte de Android.
- Agregar smoke preset Android solo cuando el nativo ya tenga fake tests.

### Manual hardware

- Mantener una matriz minima:
  - UAC1 GHW host fixture/manual smoke.
  - UAC2 UGREEN CM720.
  - Device con capture real para split.
  - Device multicanal o al menos feature-unit con canales separados para volume/routing.

---

## 4. Impacto esperado en NoisyPad

- Slice 1 y 2: sin impacto API si defaults se mantienen.
- Slice 3: API nueva opcional para caps/volume por canal.
- Slice 4: mejora comportamiento ante errores; puede cambiar eventos/logs, no API publica necesariamente.
- Slice 5 y 6: requiere decision de UX/API para elegir input/output backend; no exponer hasta tener smoke hardware.
- Slice 7: mantener compatibilidad binaria/semantica de llamadas existentes; nuevas capacidades deben tener defaults seguros.

---

## 5. No-go antes de empezar codigo grande

No avanzar a `SplitBackend` si cualquiera de estos puntos sigue abierto:

- `OboeBackend` sigue sin entregar input real al callback o adapter.
- No existe test double host para backends.
- El resize en caliente sigue usando `LockFreeRingBuffer::resize()`.
- Recovery sigue tratando todo `LIBUSB_ERROR_IO` como disconnect definitivo.
- No esta decidido si routing se aplica en USB conversion, DSP graph o ambos.

---

## 6. Comandos de verificacion

```powershell
audio\src\main\cpp\usb\tests\build\Debug\usb_tests.exe
.\gradlew :audio:compileDebugKotlin --no-configuration-cache
.\gradlew :audio:assembleDebug
```

Si MSBuild falla con `Path`/`PATH` duplicados al compilar tests host, normalizar variables del proceso antes de `cmake --build`, como se hizo durante cierre de Stage 3.
