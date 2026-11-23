# Mapa del repo y stack

Movido del `CLAUDE.md` raiz para que el nucleo entre en el presupuesto de contexto de arranque
(8 KB). Texto sin cambios; «Conteos medidos» es la tabla del `CLAUDE.md` raiz.

### Mapa

```
audio/src/
  commonMain/kotlin/   Kotlin puro, cero deps de Android: api/ (AudioEngine, IAudioNativeBridge,
                       IEffectManager, IInputBridge, factories), domain/, callback/, internal/
  jvmSharedMain/kotlin/ JNI bridge (AudioNativeBridge) compartido Android + JVM, sin android.*;
                       logger y AAC inyectados por expect/actual (REQ-055 S1)
  androidMain/kotlin/  USB, mode, logcat, MediaCodec (MediaCodecAacEncoder)
  desktopMain/kotlin/  target jvm("desktop"): actual minimos (stderr, loadLibrary, sin AAC)
  iosMain/kotlin/      IosAudioBridge sobre cinterop, AudioSessionManager, NativeLibraryLoader no-op
  commonTest/ iosTest/
  androidUnitTest/     el ARNES JNI: ejecuta JNIEXPORT reales en la JVM del host
  main/cpp/            motor C++20
    api/               C API pura (watermelon_audio.h)
    dsp/ effects/ engines/ voice/ looper/ analysis/ (afinador)   sub-librerias
    core/              fachada AudioEngine + subsistemas
    backends/          IAudioBackend, Oboe/Libusb (Android), CoreAudio (iOS), PlatformBackends.cpp
    jni/               capa JNI   ·   platform/  Logger, Platform, PlatformIsa.inc
    ios/               CMakeLists del build iOS (separado del de AGP)
    tests/hostjni/     la libreria JNI para el HOST, con FakeAudioBackend
harness/               app de prueba multiplataforma (Compose MP). NO se publica
```

Detalle por archivo y su historia: `docs/dev/arquitectura.md`.

### Stack

| Componente | Version |
|------------|---------|
| Kotlin | ver «Conteos medidos» |
| AGP | ver «Conteos medidos» |
| Oboe | 1.11.0  |
| C++ | C++20   |
| CMake | 3.22.1  |
| Min SDK | 29      |
| Compile SDK | 37      |
| kotlinx-coroutines | 1.11.0  |
| TinySoundFont | 0.9, **fork** (ver `thirdparty/VENDORED.md`) |
| iOS deployment target | 15.0    |
| googletest (suite de host) | ver `audio/src/main/cpp/thirdparty/googletest.cmake` |
| Compose Multiplatform (sólo `:harness`) | ver `gradle/libs.versions.toml` |
| Xcode del CI | ver la clave `xcode` de `.github/toolchain-pins.json` |
| Windows (desktop) | MinGW-w64 UCRT64 (GCC), ver `_windows_cross` en `.github/toolchain-pins.json` |

Targets KMP: `androidTarget`, `iosArm64`, `iosSimulatorArm64`. Las filas "ver" apuntan a la
**unica fuente** del pin: `scripts/check-dep-pins.py` falla si un pin aparece en un segundo
lugar. Lo vendorizado y como buscar sus CVE: `audio/src/main/cpp/thirdparty/VENDORED.md`.
