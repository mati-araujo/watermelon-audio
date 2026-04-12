# USB Audio Backend — Auditoría, propuesta y roadmap

**Proyecto:** `watermelon-audio` (Watermelon Studios)
**Versión del documento:** 1.1 — 2026-04-11
**Estado de la etapa 1:** MERGED en v1.2.2. Los tres defectos críticos originales (sample rate negotiation, feedback endpoint UAC1, DSP thread polling) están corregidos. La validación hardware reveló 5 bugs adicionales (3 regresiones de stage 1 + 2 pre-existentes) todos resueltos en v1.1.1–v1.2.2. Ver `stage_01_foundations.md` para detalles.
**Alcance:** `audio/src/main/cpp/backends/{LibusbBackend, BackendManager, ClockController, OboeBackend}`, `audio/src/main/cpp/usb/*`, `audio/src/main/cpp/jni/jni_usb.cpp`, `audio/src/androidMain/kotlin/.../internal/usb/*`, `audio/src/commonMain/kotlin/.../{api/IUsbAudioManager, domain/usb/*}`.

---

## 1. Contexto y objetivos del audit

`watermelon-audio` es una librería profesional de síntesis en tiempo real (C++20 + Oboe + Kotlin Multiplatform) que ya expone un backend USB basado en `libusb` como alternativa al HAL de Android. El objetivo declarado en el propio header del backend es alcanzar **latencia round-trip < 10 ms** con tolerancia a hot-plug y full-duplex (`audio/src/main/cpp/backends/LibusbBackend.h:11`).

Este audit evalúa la implementación actual respecto a cuatro ejes:

1. **Baja latencia** — caminos lock-free, tamaños de buffer, sincronización thread↔transfer, prioridad de hilos.
2. **Estabilidad del streaming** (sin glitches) — clock sync, manejo de underrun/overrun, recuperación de errores, resize seguro.
3. **Robustez y descubrimiento** — enumeración de dispositivos, altsettings, endpoints, formatos, clock sources, feature units.
4. **Composición de streams** — combinar entrada de Oboe (mic interno) con salida USB, soportar full-duplex asimétrico, routing entre backends.

El cierre del audit propone una arquitectura objetivo y un roadmap incremental dividido en siete etapas entregables de manera independiente (un archivo `stage_XX_*.md` por etapa, enlazados al final del documento).

---

## 2. Mapa de la implementación actual

### 2.1 Capa C++ (subsistema USB)

| Módulo | Archivo | LOC | Rol |
|---|---|---|---|
| Interfaz de backend | `backends/IAudioBackend.h` | 404 | Contrato backend-agnóstico, `IAudioCallback`, `StreamConfig`, `StreamInfo` |
| Backend Oboe | `backends/OboeBackend.{h,cpp}` | 165 + 435 | Wrapper de `oboe::AudioStream` con full-duplex opcional |
| Backend libusb | `backends/LibusbBackend.{h,cpp}` | 445 + 1287 | Abre device via fd envuelto, parsea descriptores, lanza DSP y transfer threads |
| Manager dual | `backends/BackendManager.{h,cpp}` | 270 + 344 | Selección y switch entre backends; expone `initializeUsbBackend(fd, path)` |
| Clock controller | `backends/ClockController.h` | 422 | PID + moving average para ajustar `framesPerPacket` a partir del feedback endpoint |
| Transfer manager | `usb/UsbTransferManager.{h,cpp}` | 525 + 1003 | Isochronous triple buffering, ring buffers, watchdog, callbacks libusb |
| Descriptor parser | `usb/UsbDescriptorParser.{h,cpp}` | 404 + 983 | Parseo de descriptores UAC 1.0/2.0 a `UsbAudioDevice` |
| Tipos y constantes | `usb/{UsbAudioTypes.h, UsbConstants.h}` | 503 + 318 | Enums UAC, structs de topología, control selectors |
| Conversor de formato | `usb/AudioFormatConverter.{h,cpp}` | 405 + 122 | float ↔ S16/S24/S32, dither TPDF, soft clip |
| Volumen | `usb/UsbVolumeControl.{h,cpp}` | 294 + 365 | SET_CUR/GET_CUR sobre Feature Units (hw) con fallback digital atómico |
| Profiler | `usb/UsbLatencyProfiler.h` | 648 | Métricas lock-free de transferencias, DSP y jitter (p95/p99) |
| Buffer adaptativo | `usb/AdaptiveBufferController.h` | 468 | Hysteresis para ajustar tamaño de ring buffer según underrun/CPU |
| JNI USB | `jni/jni_usb.cpp` | 126 | Estado compartido y funciones de profiling expuestas al Kotlin |

### 2.2 Capa Kotlin (Android + commonMain)

| Área | Archivo | LOC | Rol |
|---|---|---|---|
| API pública | `commonMain/.../api/IUsbAudioManager.kt` | 356 | Contrato de manager con `Flow` de estado, permiso, volumen, streaming |
| Tipos de dominio | `commonMain/.../domain/usb/*.kt` | 940 | Devices, caps, events, test configs, volumen |
| Impl Android | `androidMain/.../internal/usb/UsbAudioManagerImpl.kt` | 1356 | Discovery, permiso, handoff al nativo, health check, wake-lock |
| Compatibilidad | `androidMain/.../internal/usb/UsbDeviceCompatibility.kt` | 257 | Allowlist VID/PID + gating DEBUG/RELEASE |
| Persistencia | `androidMain/.../internal/usb/{TrustedUsbDevicesRepository, UsbVolumeRepository}.kt` | 357 | DataStore: dispositivos de confianza y volumen por device |
| Test runner | `androidMain/.../internal/usb/UsbAudioTestRunner.kt` | 423 | Ejecuta tests de playback/capture/loopback/stress |

### 2.3 Flujo actual de alto nivel

```
┌────────────────────────────────────────────────────────────────────────┐
│                            App / NoisyPad                              │
├────────────────────────────────────────────────────────────────────────┤
│ IUsbAudioManager  ←─ Flows: connectedDevices, connectionState, events │
├────────────────────────────────────────────────────────────────────────┤
│  UsbAudioManagerImpl (androidMain)                                     │
│    • UsbManager.deviceList + BroadcastReceiver                          │
│    • Pide permiso con PendingIntent(FLAG_MUTABLE)                       │
│    • openDevice().fileDescriptor → AudioNativeBridge                    │
│    • Health check 1s → fallbackToOboe on 3s unhealthy                   │
│    • WakeLock PARTIAL durante streaming                                 │
├────────────────────────────────────────────────────────────────────────┤
│  AudioNativeBridge (JNI)                                                │
│    • initializeUsbDevice(fd, path) → BackendManager                     │
│    • startUsbStreamingWithMode(sr, ch, bit, mode)                       │
├────────────────────────────────────────────────────────────────────────┤
│  BackendManager                                                         │
│    • OboeBackend (siempre) + LibusbBackend (on-demand)                  │
│    • selectBackend() / fallbackToOboe()                                 │
├────────────────────────────────────────────────────────────────────────┤
│  LibusbBackend                                                          │
│    • libusb_wrap_sys_device(fd) → handle                                │
│    • parseDeviceDescriptors() vía control transfer GET_DESCRIPTOR       │
│    • selectBestInterfaces() → primer match por sample rate              │
│    • UsbTransferManager (triple buffered iso transfers + DSP thread)    │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 3. Hallazgos del audit

Cada hallazgo lleva severidad (**C**rítica — bloquea certificación profesional; **M**ayor — degrada experiencia u obstaculiza evolución; **m**enor — mejora incremental), archivo+línea donde se observa, y el impacto operativo. Las correcciones se referencian por stage (ver §6).

### 3.1 Sample rate negotiation — ausente **[C]**

**Observación.** `LibusbBackend::configureSampleRate()` está declarado en `LibusbBackend.h:387` pero **no tiene implementación** en `LibusbBackend.cpp`. Una búsqueda exhaustiva confirma que los únicos `libusb_control_transfer` en el proyecto son: (a) `LibusbBackend.cpp:183` para leer el descriptor raw, y (b) los ocho en `UsbVolumeControl.cpp` para volumen/mute. **Nunca se envía SET_CUR a `SAMPLING_FREQ_CONTROL` (UAC1, endpoint request 0x22) ni a `CS_SAM_FREQ_CONTROL` (UAC2, clock source interface request 0x21)**.

**Consecuencia.** El device nunca es instruido para cambiar su clock a la frecuencia pedida por el usuario. El código asume que el tamaño de paquete iso en `libusb_set_iso_packet_lengths()` es suficiente para forzar el rate, cuando en realidad solo declara cuántos bytes se envían por microframe. Devices multi-rate (prácticamente todas las interfaces profesionales) se quedan en su default interno, produciendo:

- Pitch incorrecto cuando el host cree que envía 48 kHz y el device reproduce a 44.1 kHz.
- Drift acumulativo no corregible por el feedback endpoint.
- Imposibilidad de alternar 44.1/48/96/192 en runtime sin reiniciar el stream — y aun así el problema persiste porque nadie negocia.
- Algunas interfaces UAC2 directamente se niegan a abrir el altsetting sin un SET_CUR previo al clock source.

**Evidencia.** `LibusbBackend.h:387` (declaración) + ausencia del símbolo en `LibusbBackend.cpp` + `Grep` confirmando que no hay llamadas class-specific con bRequest 0x01 fuera de volumen.

**Fix.** Etapa 1 — implementación del `SET_CUR` correcto según versión UAC, con lectura posterior de `GET_CUR` para validación (debe eco-retornar el rate aplicado).

### 3.2 Feedback endpoint UAC1/UAC2 — parcialmente incorrecto **[C]**

**Observación.** El camino de clock sync existe parcialmente y sorprende positivamente: `UsbTransferManager::handleFeedbackComplete()` (`UsbTransferManager.cpp:633-658`) llama `mClockController->processFeedback()` con un `length` calculado a partir de `iso_packet_desc[0].actual_length`, y `fillOutputTransfer()` usa `mClockController->getAdjustedFrameCount()` para variar `iso_packet_desc[i].length` por paquete. El `ClockController` (`backends/ClockController.h:68`) implementa PID + moving average + acumulador fraccional — un diseño sólido.

**Problemas concretos:**

1. **Packet length hardcoded a 4** en `UsbTransferManager.cpp:477` (`libusb_set_iso_packet_lengths(mFeedbackTransfer, 4)`). Esto solo funciona para UAC 2.0 (16.16). En UAC 1.0 full-speed son **3 bytes** (10.14), así que el device responderá `short packet` o el packet length configurado será incorrecto.
2. **Detección de versión UAC por heurística** en `UsbTransferManager.cpp:637-641`:
   ```cpp
   UacVersion version = UacVersion::UAC_1_0;
   int length = transfer->iso_packet_desc[0].actual_length;
   if (length >= 4) version = UacVersion::UAC_2_0;
   ```
   La versión real está en `mUsbDevice->uacVersion` (parseada) pero no se propaga al transfer manager. Un UAC1 device que devuelva 4 bytes por alineación será mal interpretado.
3. **No se valida que el endpoint sea realmente un feedback endpoint**: el parser marca un `UsbFeedbackEndpoint` si ve un endpoint con `bmAttributes & 0x20` (bit 5, implicit feedback) o el campo `bSynchAddress`. Esto está en `UsbDescriptorParser.cpp` pero no se verifica en el transfer manager antes de asignar la callback.
4. **UAC version por defecto en el constructor del `ClockController`**: se inicializa a 48000 (`UsbTransferManager.cpp:43`) y se reconfigura con `setNominalSampleRate()` en `configure()`, pero el `processFeedback()` no recibe la versión — la recibe por parámetro en cada callback, tomada de la heurística anterior.

**Consecuencia.** En devices UAC 1.0 asíncronos (muchos DACs de consumer-grade USB-C, Cmedia, etc.) el feedback se parsea mal — el PID ajusta hacia valores incorrectos, el drift aumenta en lugar de corregirse, y a los pocos minutos aparecen clicks/crackles intermitentes.

**Fix.** Etapa 3 — pasar la versión UAC conocida desde el parser al `UsbTransferManager`, calcular `feedbackPacketLength` según versión (3 vs 4), validar el tipo de endpoint antes de enganchar, y añadir un test de regresión con stub de device UAC1.

### 3.3 Clock source negotiation UAC 2.0 — ausente **[C]**

**Observación.** `UsbDescriptorParser` sí parsea los descriptores `UAC2_AC_CLOCK_SOURCE`, `UAC2_AC_CLOCK_SELECTOR` y `UAC2_AC_CLOCK_MULTIPLIER` (`UsbDescriptorParser.cpp:298`), y los almacena en vectores dentro de `UsbAudioDevice`. También extrae `syncedToSof` del campo `bmAttributes`. Sin embargo, esta información **no se usa en ningún otro lugar**:

- No hay selección de clock source activo antes de `start()`.
- Si hay un `Clock Selector` (UAC2 5.2.5.1.3), nadie envía el `CUR` request para elegir entre fuentes.
- No se lee `RANGE` para saber qué sample rates acepta cada clock source.
- El TODO en `UsbDescriptorParser.cpp:894` (`queryClockSourceSampleRates`) es un stub que retorna false.

**Consecuencia.** Devices UAC 2.0 con más de un clock source (ejemplo: Scarlett 2i2 con internal + ADAT word clock, Motu M2, Focusrite Scarlett 18i20) no son configurables correctamente. Algunos seleccionan automáticamente el primero; otros esperan el CUR y fallan.

**Fix.** Etapa 3 — completar el grafo de clock topology, implementar `getClockSources()` y `selectClockSource(id)`, y llamar el SET_CUR correspondiente antes de `start()`.

### 3.4 Descubrimiento de altsettings/formatos — reductivo **[M]**

**Observación.** En `UsbAudioTypes.h`, `UsbAudioDevice` contiene `std::vector<UsbStreamingInterface> playbackInterfaces` y `captureInterfaces` — un entry por altsetting. Cada entry lleva **un solo `UsbAudioFormat`**. El parser, al encontrar Type I Format Descriptor, sobreescribe el format previo (si había varios formatos en el mismo altsetting, el último gana). Además, `selectBestInterfaces()` (`LibusbBackend.cpp:224-310`) hace un lineal del primer match:

```cpp
for (const auto& iface : mUsbDevice->playbackInterfaces) {
    if (iface.format.supportsSampleRate(mRequestedSampleRate)) {
        mSelectedPlayback = iface;
        break;
    }
}
```

No hay criterio para preferir: mayor bit depth, async sobre adaptive, mayor número de canales, sync type específico, o endpoint con feedback EP presente. Si el primer altsetting matching es S16 mono adaptive y el tercero es S24 estéreo async, el código elige el peor.

**Consecuencia.** Interfaces multi-format pierden capacidades, el usuario no puede forzar una selección específica (ej: "solo S24"), y no hay forma de degradar elegantemente cuando un altsetting óptimo falla al `SET_INTERFACE`.

**Fix.** Etapa 2 — cambiar el modelo de datos a `std::vector<UsbAudioFormat>` por altsetting, introducir un `StreamPreference` struct con pesos, y exponer la selección en la API.

### 3.5 DSP thread — busy-wait con sleep de 200 µs **[M]**

**Observación.** El thread DSP de `LibusbBackend` (`LibusbBackend.cpp:671-899`) corre con prioridad RT y pineado a un core grande (big.LITTLE) — ambos correctos. Sin embargo, la sincronización con el ring buffer es **polling con sleep fijo**:

```cpp
bool outputReady = !mSelectedPlayback ||
    (mTransferManager->getOutputBufferAvailable() >= outputSamples);
bool inputReady  = !mSelectedCapture ||
    (mTransferManager->getInputBufferAvailable() >= inputSamples);

if (!outputReady || !inputReady) {
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    continue;
}
```

200 µs ≈ 9.6 muestras a 48 kHz estéreo. Bajo carga, ese sleep introduce jitter determinista en el callback DSP. Además, si el kernel schedulea el wake con granularidad ≥ 1 ms (caso común en Android), el sleep real puede ser mucho mayor. Un diseño event-driven con condition variable, semáforo o `eventfd` tendría latencia de wake cercana a cero.

**Consecuencia.** Jitter piso de ~200 µs–1 ms que no depende del device USB sino del DSP loop. Incompatible con el objetivo declarado de **< 10 ms round-trip**.

**Fix.** Etapa 1 — reemplazar el poll por un `std::counting_semaphore` (C++20) o, en Android, `eventfd`/futex wake desde las callbacks de transfer USB.

### 3.6 Ring buffer resize — race condition potencial **[M]**

**Observación.** `UsbTransferManager::reconfigureBufferSize()` (`UsbTransferManager.cpp:951-1000`) llama `mOutputRingBuffer->resize(newOutputSize)` mientras el stream está corriendo. El comentario explícitamente dice "the ring buffer implementation should handle concurrent access safely" pero:

- Un ring buffer lock-free SPSC típico basado en índices atómicos **no es seguro** bajo `resize()` concurrente — cambiar la capacidad cambia las máscaras y la aritmética modular.
- El DSP thread llama `writeOutput()` en paralelo con `handleOutputComplete()` que llama `fillOutputTransfer()` → `mOutputRingBuffer->read()`. Si el resize corre entre esos dos, hay corrupción.
- La operación `resize()` podría implicar `realloc`, violando la regla RT-safe (sin allocations en el hot path).

**Consecuencia.** Bajo buffer adaptativo activo, un glitch al resize puede convertirse en crash o audio corrupto. En la práctica la función puede no haberse ejercido mucho porque el buffer adaptativo reciente rara vez cambia tamaño.

**Fix.** Etapa 4 — esquema de double-buffer con atomic swap, o stop→reconfigure→start controlado por el DSP thread durante un gap forzado.

### 3.7 Descubrimiento Kotlin→C++ de capacidades — incompleto **[M]**

**Observación.** `UsbAudioManagerImpl.parseBasicCapabilities()` (`UsbAudioManagerImpl.kt:443-493`) infiere capacidades desde las interfaces USB visibles vía Android `UsbInterface`, pero:

- Devuelve hardcoded `[44100, 48000, 96000]` y `[16, 24]` independientemente de lo que el device soporte realmente (línea 485–492).
- Hay un TODO en la línea 452 indicando que la integración con el parser nativo está pendiente.
- `nativeBridge.parseUsbDescriptors()` retorna un `FloatArray` cuyo layout es opaco.

En consecuencia, el Kotlin nunca sabe:
- Cuántos altsettings hay.
- Qué formatos/bit depths soporta cada uno.
- Cuál es el sync type (async/adaptive/sync).
- Si hay feedback endpoint presente.
- Qué clock sources hay (UAC2).

**Fix.** Etapa 5 — diseñar un protocolo estable (JSON vía JNI string, o ByteArray con esquema versionado) para exponer el `UsbAudioDevice` parseado al Kotlin, y usarlo desde `getDeviceCapabilities()`.

### 3.8 Combinaciones de streaming — limitadas **[M]**

**Observación.** El objetivo del audit incluye explícitamente "combinar input por Oboe (mic del dispositivo) con output por USB". La implementación actual:

- `BackendManager` tiene dos backends pero **solo uno activo** a la vez (`mActiveBackend`).
- `IUsbAudioManager` modela un único `selectedDevice`.
- No hay API para pedir "mic de Oboe + output de USB".
- El `IAudioCallback` recibe `inputData` y `outputData` del mismo backend.

**Consecuencia.** Cualquier escenario profesional realista — cantante/instrumentista usando USB DAC + mic del teléfono; loopback en caliente; bus de efectos externo — requiere mezcla de backends que no existe hoy.

**Fix.** Etapa 4 — introducir un `SplitBackend` / `DualBackendRouter` que componga dos `IAudioBackend` con clock reconciliation (el que tenga feedback impone el drift, el otro resamplea).

### 3.9 Formato, channels, mono↔estéreo, per-channel volume **[m]**

**Observación.** El DSP thread hace mono→estéreo con -3 dB hardcoded (`LibusbBackend.cpp:787-793`) usando `0.707f`. Está bien para un mic mono. Sin embargo:

- `AudioFormatConverter` solo soporta interleaved, no planar.
- No hay SIMD vectorization de las conversiones (ARM NEON ausente).
- `UsbVolumeControl` solo expone master volume (canal 0). Devices multicanal no pueden controlarse per-channel.
- No hay mixer de canales (downmix 4→2, selector de canal input).

**Fix.** Etapa 2 (selección) + etapa 6 (SIMD + per-channel) + etapa 4 (routing).

### 3.10 Compatibility allowlist — chica y estática **[m]**

**Observación.** `UsbDeviceCompatibility.kt:55-157` tiene 10 devices hardcoded, gateado por `BuildConfig.DEBUG`. Para una librería "profesional" es insuficiente. Además:

- Los campos `knownIssues` y `minUacVersion` están declarados pero no se usan en `checkCompatibility()`.
- No hay forma de actualizar la lista sin recompilar.
- No hay per-serial filtering ni listas remotas.

**Fix.** Etapa 5 — repositorio externo actualizable (resource bundle, remote config opcional), con campos efectivos.

### 3.11 Test runner — cobertura aparente vs real **[m]**

**Observación.** `UsbAudioTestRunner.kt` tiene buena interfaz (progress flow, presets, reportes), pero:

- El playback test asume que hay tono pre-generado sonando (línea 51 del .kt). No genera su propio tono.
- El capture test tiene input levels hardcoded placeholder (−20 a −12 dB, línea 346).
- El loopback test **no mide round-trip latency real** — solo delega al playback con full-duplex. No detecta impulse ni hace correlation.
- No hay tone sweep, FFT, THD, SINAD.

**Fix.** Etapa 6 — test harness con impulse detection nativo, tone sweep y FFT en C++.

### 3.12 Android 14 bit-perfect / AAudio MMAP — oportunidad **[oportunidad]**

**Observación.** Android 14 introdujo `AAudio` con `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY` + `AAUDIO_SHARING_MODE_EXCLUSIVE` y las [preferred mixer attributes](https://source.android.com/docs/core/audio/preferred-mixer-attr) para USB, lo que permite bit-perfect sin pasar por libusb en devices compatibles. El backend actual ignora este camino y siempre va por libusb cuando hay USB.

**Consecuencia.** En devices con Android 14+ y kernel USB audio moderno, la ruta libusb es más lenta y más inestable que la nativa. En devices Pixel 8+, el bit-perfect MMAP directo a través de AAudio alcanza < 5 ms round-trip sin esfuerzo.

**Fix.** Etapa 7 — detección de Android 14+ y política "USB via Oboe/AAudio cuando es suficiente, libusb cuando se necesita control fino" (sync async, clock source selection, hot-plug sin interrupción, devices no-MMAP).

---

## 4. Comparación con el mercado (2026)

| Aspecto | `watermelon-audio` hoy | USB Audio Player PRO (UAPP) | Neutron Music Player | JUCE/PortAudio | Linux `snd-usb-audio` | AAudio MMAP (Android 14+) |
|---|---|---|---|---|---|---|
| Sample rate negotiation | ❌ ausente | ✅ class-specific per UAC | ✅ | ✅ (backend nativo) | ✅ | ✅ (kernel) |
| Clock source UAC2 | ❌ parseado, no seleccionado | ✅ | ✅ | N/A (delega OS) | ✅ | parcial (via mixer attrs) |
| Async feedback EP | ⚠️ parcial (UAC2 only) | ✅ UAC1+UAC2 | ✅ | delega OS | ✅ | delega kernel |
| Bit depths | S16/S24_3LE/S32 | S16/S24/S32/DSD | S16/S24/S32/DSD | float/int planar + interleaved | todo | preferred mixer |
| Per-channel volume | ❌ master only | ✅ | ✅ | ✅ | ✅ | vía mixer attrs |
| Multi-backend routing | ❌ uno o el otro | ❌ single | ❌ single | ✅ (dev matrix) | ✅ (pipewire) | ⚠️ limitado |
| DSP thread sync | ⚠️ poll 200 µs | event-driven | event-driven | event-driven | event-driven | event-driven |
| Allowlist de devices | ⚠️ 10 static | ✅ cientos | ✅ cientos | open | open | open |
| Latencia RT mínima | ~20 ms reportada | 11 ms UAPP | 15 ms | 3–5 ms escritorio | 1–5 ms | 4–8 ms |
| Kotlin-first API | ✅ único | ❌ Java/C | ❌ | N/A | N/A | ⚠️ Java |

**Conclusión.** `watermelon-audio` tiene el mejor KMP surface del mercado y una base sólida de descubrimiento parcial, pero le faltan los cimientos: sample rate negotiation, clock source selection y feedback UAC1. Sin eso, la latencia < 10 ms es alcanzable "por suerte" en devices específicos, no por diseño. El rediseño propuesto en §5 cierra la brecha con UAPP/Neutron manteniendo la ventaja KMP.

**Referencias y fuentes consultadas:**
- [Android USB digital audio (AOSP)](https://source.android.com/docs/core/audio/usb)
- [Android NDK audio latency guide](https://developer.android.com/ndk/guides/audio/audio-latency)
- [Android 14 preferred mixer attributes](https://source.android.com/docs/core/audio/preferred-mixer-attr)
- [Superpowered: How Android Mutes The Next Billion With USB Audio](https://superpowered.com/android-usb-audio-android-midi)
- [USB Audio Class 2.0 spec, Sección 5.2.5.1 (Clock Source unit)](https://www.usb.org/sites/default/files/audio10.pdf)
- [eXtream USB Audio Driver — arquitectura de referencia Android](https://www.extreamsd.com/index.php/technology/usb-audio-driver)

---

## 5. Propuesta arquitectónica

### 5.1 Objetivos del rediseño

1. **Correctitud antes que features.** Implementar primero lo que falta para que el path feliz funcione realmente: sample rate negotiation, clock source selection, feedback UAC1, señalización event-driven del DSP thread.
2. **Exponer el descubrimiento.** Kotlin debe poder enumerar altsettings, formatos, clock sources y sync types sin atravesar una API opaca.
3. **Componibilidad de backends.** Un `IAudioEngine` debe poder componerse como `SplitBackend(inputFrom = oboe, outputTo = libusb)` sin refactor masivo.
4. **Observabilidad profesional.** Eventos de threshold (drift > N PPM, underrun sostenido, jitter p99 > T), no solo polling de stats.
5. **No regresar lo que ya funciona.** El profiler, el adaptive buffer, el hot-plug con watchdog y el fallback a Oboe son assets; se mantienen y se integran al nuevo modelo.

### 5.2 Nueva vista del subsistema USB

```
┌──────────────────────────────────────────────────────────────────────┐
│                         IAudioEngine                                 │
│  (orquesta backends; DSP callback recibe in/out sin saber el origen) │
└──────────────────────────────────────────────────────────────────────┘
                                │
            ┌───────────────────┼───────────────────┐
            ▼                   ▼                   ▼
  ┌──────────────────┐ ┌──────────────────┐ ┌────────────────────────┐
  │   OboeBackend    │ │  LibusbBackend   │ │  SplitBackend (nuevo)  │
  │                  │ │   (rediseñado)   │ │  routing matrix        │
  └──────────────────┘ └──────────────────┘ └────────────────────────┘
                                │
        ┌───────────────────────┼───────────────────────┐
        ▼                       ▼                       ▼
┌──────────────┐      ┌─────────────────┐      ┌─────────────────┐
│ UsbDescriptor│─────▶│  UsbTopology    │◀─────│ UsbClockGraph   │
│ Parser       │      │ (altsettings,   │      │ (UAC2 sources + │
│ (completo)   │      │  formats,       │      │  selectors)     │
└──────────────┘      │  feedback EPs)  │      └─────────────────┘
                      └─────────────────┘
                                │
                                ▼
                    ┌──────────────────────┐
                    │ UsbStreamNegotiator  │
                    │ (SET_CUR rate,       │
                    │  SET_INTERFACE alt,  │
                    │  clock src select)   │
                    └──────────────────────┘
                                │
                                ▼
                    ┌──────────────────────┐
                    │  UsbTransferManager  │
                    │  (event-driven DSP   │
                    │  signal, UAC1/UAC2   │
                    │  feedback, safe      │
                    │  reconfigure)        │
                    └──────────────────────┘
```

### 5.3 Nuevas abstracciones

- **`UsbTopology`** (reemplaza `UsbAudioDevice`): mantiene `vector<UsbAudioFormat>` por altsetting, el grafo de terminales↔feature units↔selector units↔clock graph, y ofrece queries tipo `findBestAltsetting(StreamPreference)`.
- **`UsbStreamNegotiator`**: ejecuta la secuencia estándar USB Audio — `CLAIM → SET_ALT(0) → SET_CUR clock src → SET_CUR sample rate → SET_ALT(n)`. Reemplaza al código disperso de `start()`.
- **`StreamPreference`**: struct con pesos que guía la selección (`preferAsync`, `preferBitDepth`, `requireFeedback`, `maxChannels`, `targetLatencyMs`).
- **`SplitBackend`**: implementa `IAudioBackend` componiendo dos fuentes. Hace clock reconciliation mediante un resampler en el lado que no manda el drift, con un ring buffer de borrachera acotada.
- **`UsbEventBus`**: publica `UsbAudioEvent` tipados (drift, jitter, underrun, disconnect, clock switch). Reemplaza el patrón de callback directo a `mErrorCallback`.
- **`UsbCapabilitySnapshot`** (Kotlin): proyección completa de `UsbTopology` vía un serializador estable (protobuf o JSON), consumible desde `commonMain`.

### 5.4 Surface pública extendida (Kotlin)

```kotlin
interface IUsbAudioManager {
    // ... (surface actual)

    // Nuevos — etapa 2 y 5
    suspend fun getCapabilitySnapshot(deviceId: String): Result<UsbCapabilitySnapshot>
    suspend fun selectAltsetting(deviceId: String, preference: StreamPreference): Result<SelectedAltsetting>
    suspend fun setClockSource(deviceId: String, clockSourceId: Int): Result<Unit>
    suspend fun setSampleRate(deviceId: String, hz: Int): Result<Int>  // returns actual rate

    // Etapa 4
    suspend fun startSplitStream(
        inputBackend: BackendChoice,
        outputBackend: BackendChoice,
        config: UsbStreamConfig,
    ): Result<StreamHandle>

    // Observabilidad — etapa 5
    val healthEvents: Flow<UsbHealthEvent>  // drift, jitter, underrun thresholds
    val capabilityChanges: Flow<UsbCapabilitySnapshot>
}
```

### 5.5 Invariantes de diseño (no negociables)

1. **No allocations en el hot path.** Todo lo que corra dentro de `handleOutputComplete` o del DSP loop debe usar buffers pre-allocados. `MemoryUtils::prepareVectorForRealtime` debe invocarse en toda nueva variable.
2. **Event-driven sync.** El DSP loop solo despierta por señal de "data available" o "space available", nunca por sleep periódico.
3. **Threading single-owner.** Cada recurso USB (`libusb_transfer`, ring buffer) tiene un solo thread propietario durante su vida activa. Los intercambios se hacen con atomics release/acquire.
4. **Versión UAC explícita.** Ningún código fuera del parser infiere la versión por tamaños — se pasa como parámetro de arranque.
5. **KMP commonMain puro.** Los nuevos tipos (`UsbCapabilitySnapshot`, `StreamPreference`, `UsbHealthEvent`) viven en `commonMain` sin imports android/java.

---

## 6. Plan incremental por etapas

Cada etapa es un PR (o conjunto pequeño de PRs) mergeable y verificable de manera independiente. El orden es prescriptivo — etapas posteriores asumen que las anteriores están mergeadas.

| # | Etapa | Objetivo | Duración estimada | Bloqueante para |
|---|---|---|---|---|
| 1 | [Fundamentos críticos](stage_01_foundations.md) | Sample rate SET_CUR, feedback UAC1, DSP sync event-driven | 3–5 días | Todas |
| 2 | [Descubrimiento completo](stage_02_discovery.md) | Multi-format por altsetting, `UsbTopology`, `StreamPreference` | 4–6 días | 3,4,5 |
| 3 | [Clock sync profesional](stage_03_clock_sync.md) | Clock source selection, feedback UAC1/UAC2 correcto end-to-end | 3–4 días | 4 |
| 4 | [Routing y split backends](stage_04_mixing_routing.md) | `SplitBackend`, Oboe+USB mixing, resize seguro, per-channel | 5–7 días | 5 |
| 5 | [API Kotlin y observabilidad](stage_05_kotlin_api.md) | `UsbCapabilitySnapshot`, health events, audio focus | 3–5 días | 6 |
| 6 | [Test harness profesional](stage_06_test_harness.md) | Loopback impulse, tone sweep, FFT, device matrix | 4–6 días | — |
| 7 | [Android 14 bit-perfect](stage_07_android14_bitperfect.md) | Fast path AAudio MMAP cuando aplica | 3–4 días | — |

Total estimado: **25–37 días de trabajo concentrado**, con entregas intermedias cada 3–7 días.

---

## 7. Métricas de éxito

Al finalizar la etapa 5 (MVP profesional):

- **Latencia round-trip medida con impulse** en Scarlett Solo 3rd Gen (referencia): **≤ 10 ms** @ 48 kHz / 24 bit en Pixel 6+.
- **Drift sostenido** tras 1 hora continua: **< 50 PPM** con feedback endpoint activo; **< 5 PPM** con clock source selection en UAC2.
- **Underruns** en stress test 30 min: **0** con buffer 64 frames; **< 5** con buffer 32 frames.
- **DSP callback jitter p99**: **< 500 µs** (hoy es indeterminado por el sleep de 200 µs).
- **Device matrix** pasando: **≥ 25 devices** (expansión desde 10).
- **Sample rate switching en caliente**: funciona sin reconexión para al menos 5 devices de clase.

Al finalizar la etapa 7:

- **Latencia round-trip en Pixel 8+**: **≤ 5 ms** bit-perfect por AAudio MMAP.
- **CPU en DSP loop** durante stream 48 kHz/24 bit stereo: **< 2% single big core**.

---

## 8. Riesgos y mitigaciones

| Riesgo | Prob | Impacto | Mitigación |
|---|---|---|---|
| Algunos devices UAC2 no aceptan SET_CUR en el clock source durante `SET_INTERFACE(0)` | Media | Alto | Implementar secuencia alternativa: SET_CUR primero, luego SET_INTERFACE; logs exhaustivos para alimentar la allowlist |
| Ring buffer double-buffer swap introduce gap audible | Baja | Medio | El swap debe ocurrir en silencio controlado (fade out → swap → fade in) gestionado por el DSP thread |
| AAudio MMAP no disponible en todos los kernels Android 14 | Media | Bajo | Feature detection y fallback automático a libusb — son caminos paralelos, no excluyentes |
| Cambios en `UsbAudioDevice` rompen ABI con NoisyPad | Alta | Medio | Mantener `UsbAudioDevice` deprecated como typealias sobre `UsbTopology` durante una versión; deprecation notices claras |
| PID tuning del ClockController degrada con los nuevos parámetros | Media | Medio | Suite de tests sintéticos que simule feedback patterns conocidos; golden numbers validados contra UAPP |

---

## 9. Resumen ejecutivo (para revisión rápida)

**¿Qué hay hoy?** Una base sólida con buena arquitectura KMP, profiler lock-free ejemplar, adaptive buffer y hot-plug con watchdog. El parser cubre UAC1/UAC2 en grandes rasgos.

**¿Qué falta?** Los tres cimientos: negociación de sample rate (SET_CUR), feedback endpoint correcto para UAC1, y señalización event-driven del DSP thread. Sin ellos, cualquier afirmación de "latencia baja" depende del device y la suerte.

**¿Qué se propone?** Siete etapas incrementales, 25–37 días, que empiezan corrigiendo los cimientos y terminan con bit-perfect AAudio en Android 14+. El diseño mantiene lo que funciona (profiler, buffer adaptativo, fallback a Oboe, KMP surface) y añade las abstracciones faltantes (`UsbTopology`, `StreamNegotiator`, `SplitBackend`, `UsbCapabilitySnapshot`).

**¿Cuál es el criterio de "listo"?** ≤ 10 ms round-trip medido en Scarlett Solo + Pixel 6+ tras la etapa 5, y ≤ 5 ms en Pixel 8+ tras la etapa 7. Device matrix ≥ 25 devices. 0 underruns en stress 30 min con buffer 64.

**Siguiente paso.** Stage 1 está mergeada. Ver §10 para los glitches pendientes y la etapa óptima donde cada uno aterriza.

---

## 10. Glitches pendientes post-stage-1 y asignación a etapas

Stage 1 resolvió los defectos bloqueantes y estableció streaming funcional en los tres devices de prueba. Sin embargo, dos clases de glitches persisten y deben ser abordados en etapas futuras. Este análisis los clasifica y asigna al stage con mejor contexto técnico para resolverlos.

### 10.1 Glitches esporádicos en playback UAC1 (GHW USB AUDIO, C-Media)

**Síntoma.** Clicks o micro-roturas intermitentes durante playback continuo, particularmente audibles en tonos sostenidos. No son constantes — pueden ocurrir cada 10–60 segundos, o en bursts durante cambios de carga del scheduler de Android.

**Hipótesis candidatas (por probabilidad):**

| # | Hipótesis | Evidencia | Etapa óptima |
|---|---|---|---|
| A | **PID del ClockController mal tuneado para UAC1.** El GHW es un endpoint asíncrono (Attr=0x05) sin feedback endpoint explícito (el parser de stage 1 no detectó feedback EP en la captura). Sin feedback, el `ClockController` no ajusta frame count → drift constante acumulado que produce underrun/overrun periódico. | El log muestra `fb=no` en ambos altsettings de playback. Sin feedback, `getAdjustedFrameCount` siempre retorna nominal. | **Stage 3** (Clock sync profesional) — ahí se implementa la detección de implicit feedback en adaptive endpoints, tuning del PID, y métricas de drift observables. |
| B | **El output endpoint es Adaptive (Attr=0x09), no Async.** Esto cambia la semántica: en Adaptive, el device adapta su reloj al ritmo de host. Si el host envía exactamente 48 frames/ms y el device tiene un clock de 48.002 kHz, no hay mecanismo de corrección → el DAC acumula 0.002 samples/ms de error → cada ~500ms rebasa su buffer interno → click. | El log muestra `Attr=0x09 = Isochronous/Adaptive` para los endpoints de playback. Adaptive output no usa feedback endpoint sino que sincroniza via la tasa de packets recibidos del host. | **Stage 3** — la correcta implementación de Adaptive mode (host pacing exacto sin feedback, potencialmente con timestamps de SOF) es la pieza faltante. El PID del ClockController está diseñado para Async, no para Adaptive. |
| C | **Jitter del DSP thread wakeup.** El `try_acquire_for(5ms)` del semáforo tiene un timeout de seguridad. Bajo carga de scheduler Android (doze kicks, GC pressure, thermal throttling), el wakeup puede llegar tarde → un callback entero se delay → underrun transitorio en el ring buffer de output. | Plausible pero menos probable: `underruns=0` en los stats de las corridas recientes. El profiler `dspCallback.p99LatencyUs` debería mostrar si hay outliers significativos. | **Stage 1 ya lo mejoró** con el semáforo. Para investigar más: habilitar el profiling (`nativeSetUsbProfilingEnabled(true)`) y correr una sesión de 5 minutos, analizando `p99LatencyUs`. Si p99 > 2ms, hay jitter problem. Si p99 < 500µs, el problema es clock drift, no DSP scheduling. |

**Veredicto:** La causa más probable es **B (Adaptive mode)**: el host manda exactamente 48 frames/ms pero el device espera un rate ligeramente distinto. Sin mecanismo de corrección (ni feedback endpoint, ni rate-matching del host), drift acumula. **Stage 3 es el lugar correcto** porque ahí se rediseña el clock sync incluyendo:
- Diferenciación de sync type (Async vs Adaptive vs Synchronous) en la selección de comportamiento del ClockController.
- Para Adaptive output: el host debería variar ligeramente el frame count basado en SOF timestamps, no feedback endpoint.
- Métricas de drift observables (`healthEvents: Flow<UsbHealthEvent>`) que permiten al consumer (NoisyPad) decidir si muestra un warning al usuario.

**Acción inmediata (sin esperar stage 3):** habilitar profiling en el próximo test run y capturar `dspCallback.p99LatencyUs` + `driftPpm` durante 5 minutos. Si `driftPpm` crece monótonamente, confirma la hipótesis B y la priority de stage 3 sube.

### 10.2 Glitches del UGREEN CM720 (UAC2) con auriculares+micrófono

**Síntoma.** Al conectar auriculares con micrófono integrado al CM720, el playback (tono del chaos pad) tiene clicks frecuentes. Sin auriculares+mic (solo el DAC puro), el playback suena limpio.

**Hipótesis candidatas:**

| # | Hipótesis | Evidencia | Etapa óptima |
|---|---|---|---|
| A | **El device cambia de altsetting/mode cuando detecta mic.** Muchos adaptadores USB-C DAC "headset" re-enumeran los endpoints cuando un auricular con mic se conecta: el device puede pasar de "stereo playback only" a "stereo playback + mono capture" internamente, modificando el wMaxPacketSize o el bInterval de los endpoints activos sin que el host lo sepa (no hay re-negotiation automática en UAC2 sin el host chequeando). | Sin logs todavía — necesitamos ver el descriptor parsing cuando el auricular está conectado vs desconectado. | **Stage 2** (Discovery completo) — la re-enumeración de endpoints bajo hot-plug de accesorios es exactamente lo que `UsbTopology` + `UsbCapabilitySnapshot` están diseñados para manejar. |
| B | **bInterval > 1 en el modo headset.** Si el endpoint del auricular declara `bInterval=2` (polling cada 250µs en lugar de 125µs en high-speed), nuestro código que asume `bInterval=1` podría estar mandando packets al doble de la tasa esperada → el device bufferiza la mitad de lo que esperábamos → clicks. | Plausible pero necesita confirmación. El log actual muestra `bInterval=1` pero eso fue capturado sin auriculares+mic. | **Stage 2** — el parser ya lee `bInterval` pero nadie lo usa en los cálculos de timing. Stage 2 introduce el `AltsettingDescriptor` que incluye `bInterval` como campo first-class y lo expone en la snapshot. |
| C | **El CM720 en modo headset activa full-duplex internamente y compite por el bandwidth del bus.** UAC2 high-speed con playback + capture en microframes simultáneos puede saturar el bus USB si el host no space los transfers correctamente. | Menos probable en USB 2.0 que tiene headroom de bandwidth para audio stereo+mono. | **Stage 4** (Routing) — la composición de input/output backends con clock reconciliation ayudaría, pero este caso es un solo device haciendo full-duplex en su propio hub. No es un caso de multi-backend. |

**Veredicto:** Necesitamos los logs del CM720 **con auriculares+mic conectados** para discriminar entre A (re-enumeración) y B (bInterval). La captura del descriptor parsing con el accesorio puesto va a mostrar:
- Si los altsettings/endpoints cambian respecto al caso sin accesorio.
- Si `bInterval` o `wMaxPacketSize` difieren.
- Si aparecen interfaces de capture nuevas.

**Etapa óptima: Stage 2** para la detection y Stage 3 para la corrección de clock behavior con bInterval variado. Si los endpoints no cambian con el accesorio, la causa es más sutil (posiblemente C) y stage 4 la abordaría.

**Acción inmediata:** capturar los logs del CM720 con auriculares+mic enchufado, desde el connect hasta playback, y comparar con los logs sin accesorio ya capturados.

### 10.3 Resumen de asignación

| Glitch | Causa más probable | Etapa óptima | Prioridad |
|---|---|---|---|
| UAC1 clicks esporádicos en playback | Adaptive mode sin rate correction | **Stage 3** (Clock sync) | Alta — afecta a usuarios con DACs baratos que son la mayoría |
| CM720 clicks con auriculares+mic | Re-enumeración de endpoints o bInterval | **Stage 2** (Discovery) → **Stage 3** (Clock) | Media — afecta solo un device/accesorios específicos |
| Stage 6 test gaps revelados | No hay smoke tests paramétricos para iso layout/format divergentes | **Stage 6** (Test harness) | Alta para prevenir regresiones futuras |

### 10.4 Prioridad de etapas actualizada post-stage-1

```
Etapa 2 (Discovery)  ─┬─ Preparación para Stage 3 + fix de CM720 headset
                       │
Etapa 3 (Clock sync) ─┘─ Fix de glitches UAC1 adaptive + CM720 bInterval
                         ─ Prerequisito para stages 4 y 5

Etapa 6 (Test harness) ─── Puede ejecutarse en paralelo con 2/3 para
                            cerrar los gaps de validación que produjeron
                            las 3 regresiones de stage 1
```

La recomendación es ejecutar **Stage 2 → Stage 3 en secuencia** como próximo bloque de trabajo, intercalando Stage 6 si hay tiempo. Stages 4, 5 y 7 quedan para después de que el clock sync esté estable.
