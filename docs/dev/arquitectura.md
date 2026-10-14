# Arquitectura — mapa por archivo

Movido del `CLAUDE.md` raiz por MINI-044 (2026-10-09). **Los conteos de este mapa son historicos y driftean**: los vigentes y vigilados estan en la tabla «Conteos medidos» del `CLAUDE.md` raiz.

```
audio/src/
  commonMain/kotlin/    94 files — pure Kotlin, zero Android deps
    api/                AudioEngine interface, IAudioNativeBridge, IEffectManager,
                        IInputBridge + AudioInput (camino de entrada, WA-5.5),
                        factories (AudioEngine, EffectManager, AudioInput,
                        StateSynchronizer)
    domain/             Effect types, oscillators, scales, modes, USB types, errors
    callback/           AudioLogger, AudioAnalyticsListener (dependency inversion)
    internal/           AudioEngineImpl, EffectManagerImpl, StateSynchronizer,
                        ScaleQuantizer, ChordGenerator, BridgeConcurrency,
                        util/Format, util/Time
                        expect: AudioBridgeProvider, NativeLibraryLoader,
                                currentDeviceCapabilities
    domain/device/      DeviceCapabilities (interfaz de hechos) + Snapshot
    domain/input/       InputSource + InputMetering (snapshot de 7 valores)
  androidMain/kotlin/   21 files — JNI bridge, USB, platform-specific
    internal/bridge/    AudioNativeBridge (3357 LOC, 308 external funs)
    internal/usb/       USB audio driver (DataStore, BroadcastReceiver)
    internal/mode/      ModeTransitionManagerImpl, NativeModeStateWriter
  iosMain/kotlin/       6 files — IosAudioBridge (sobre cinterop), AudioBridgeProvider,
                        AudioSessionManager (AVAudioSession como Flow),
                        NativeLibraryLoader (no-op, link estatico),
                        DeviceCapabilitiesProvider (NSProcessInfo)
  commonTest/kotlin/    8 suites  ·  iosTest/kotlin/ 4 suites
  androidUnitTest/      6 files — el ARNES JNI (REQ-016 + REQ-018). Corre en la JVM
                        del host y EJECUTA funciones JNIEXPORT reales contra un
                        JNIEnv real, entrando por AudioNativeBridge. UNA JVM POR
                        CLASE (forkEvery=1): el motor nativo es un singleton de
                        proceso y los tests de ausencia necesitan uno virgen
  main/cpp/             C++20 engine
    api/                C API — watermelon_audio.h (274 declaraciones WMA_API, pure C)
    dsp/                watermelon-dsp sub-library (30 files, zero deps)
    effects/            watermelon-effects sub-library (59 files, 23 efectos + EffectRegistry)
    engines/            watermelon-engines sub-library (SynthEngine + 6 engines
                        header-only, SoundFontManager)
    voice/              watermelon-voice sub-library (10 files, VoiceManager, VoicePool)
    looper/             watermelon-looper sub-library (16 files, header-only salvo
                        LooperExporter.cpp)
    analysis/           watermelon-analysis (17 files) — el afinador de REQ-001:
                        ring lock-free + thread de analisis + snapshot atomico,
                        PhaseSlopeEstimator (S2), StrobeTracker (S6),
                        InharmonicityEstimator (S7), FastModeTracker (S5),
                        IntonationMode (S9). REQ-014 le sumo la compuerta de
                        ausencia de señal, el arbitraje por signo y el contador
                        acumulado de discontinuidades (snapshot: 17 valores)
    core/               AudioEngine facade + subsistemas (22 files)
    backends/           IAudioBackend, BackendManager, SplitBackend, DriftResampler,
                        OboeBackend + LibusbBackend (Android),
                        CoreAudioBackend.mm (iOS, output + captura full-duplex),
                        PlatformBackends.cpp (unico punto que nombra backends concretos)
    jni/                5 files — jni_audio_bridge.cpp (295 JNIEXPORT), jni_engine,
                        jni_usb, jni_benchmark, jni_common.h
    platform/           Logger.h/.cpp (logcat / os_log / stderr), Platform.h,
                        PlatformAndroid.cpp, PlatformApple.cpp, PlatformIsa.inc (ISA comun)
    ios/                CMakeLists.txt del build iOS (separado del que maneja AGP)
    tests/hostjni/      REQ-016: libwatermelon_audio.{so,dylib} PARA EL HOST — el
                        motor + la capa JNI compilados con el jni.h del JDK, para
                        que un test de JVM pueda cargarlos. Lleva FakeAudioBackend
                        adentro: valida la frontera JNI/Kotlin, NO audio en device

harness/src/            :harness — app de prueba multiplataforma (WA-5.5). NO se publica
  commonMain/kotlin/    HarnessApp — la UI entera (Compose Multiplatform)
  androidMain/kotlin/   MainActivity (shell) + AndroidManifest (RECORD_AUDIO)
  iosMain/kotlin/       MainViewController (shell)
harness/iosApp/         Proyecto de Xcode. Embebe el framework de :harness, NO el
                        XCFramework de WA-4.1 (usar los dos duplica el motor).
                        Info.plist: NSMicrophoneUsageDescription +
                        CADisableMinimumFrameDurationOnPhone (sin esta ultima
                        Compose aborta al arrancar)
```

## Los duenos de la captura (REQ-054)

**Problema.** `ITuner.stop()` no soltaba la captura que abrio `start()`: el microfono quedaba
abierto hasta un `AudioInput.stop()` (medido por Tunio en un g42, 2026-09-28). Cerrar siempre
rompe al consumidor con su propio `AudioInput`; no cerrar nunca es el bug.

**La regla.** La captura propia del `InputNode` tiene dos duenos, bits de `CaptureOwner`:
`Consumer` (`wma_input_start/stop` y los modos INPUT_FX / MIX, sin bit propio: `wma_input_stop`
siempre cerro lo que abrio el modo) y `Tuner` (`wma_tuner_start/stop`). Cada uno suelta sólo lo
suyo y **cierra el ultimo**. Es simetrica: `AudioInput.stop()` con el afinador corriendo no lo deja
sordo. USB e iOS entregan la entrada por callback y el afinador no les abre stream: una segunda
mascara, `mOwnStreamOwners`, guarda quien pidio stream **propio** y se cierra cuando no queda
ninguno. Si corre lo dice sólo `mInputStreamRunning` (el atomico del callback; el hilo RT no ve
mutex ni mascara). Invariante: corre => `mOwnStreamOwners != vacio`.

**Quien abre y cierra.** `startInputStream/stopInputStream` son privados: todo pasa por
`acquireCapture / releaseCapture / shutdownCapture / closeOwnStreamForExternalDelivery` bajo
`mCaptureMutex`. `acquireCapture` devuelve `CaptureAcquire` porque re-preparar el motor tira el
audio en vuelo y lo decide el llamador, despues de soltar el mutex. `captureLifecycleMutex` (en
`WmaEngine`) abarca tuner start/stop, input start/stop/set_source/release y `set_audio_mode`.
Orden, todo en hilos de control: `engineMutex` -> `captureLifecycleMutex` ->
(`analysisMutex` | `inputNodeMutex`) -> `mCaptureMutex`.

**El seam.** `backends/INodeCaptureStream.h` esconde el stream de Oboe (`createNodeCaptureStream()`
en `PlatformBackends.cpp`; iOS: nullptr). El host no puede ver el stream de Oboe: sin el seam,
abrir y cerrar no son observables. La suite de host usa un `FakeCaptureStream` (apagado por
defecto) que cuenta aperturas, cierres y violaciones.

**Descartado.**
- *Conteo de referencias*: los llamadores no estan balanceados (el modo re-adquiere en cada
  `set_audio_mode`; `AudioInput.stop()` sin `start()`); con bits dos acquire valen uno.
- *El afinador recuerda si abrio*: si arranca primero, su `stop` cierra la captura del consumidor.
- *Registro en `BackendManager`*: gobierna la captura del backend, no el stream propio del nodo.

**Deuda.** Volver de INPUT_FX a otro modo no suelta `Consumer`; `onInputStreamConfigChanged` no
pasa por `captureLifecycleMutex`.
