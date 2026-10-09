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
