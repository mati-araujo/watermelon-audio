# Phase 2A — KMP Migration Runbook

**Guia ejecutable paso a paso para migrar el modulo audio a Kotlin Multiplatform.**
*Pre-requisitos ya completados en sesion anterior. Este runbook es para ejecutar en una sesion fresca.*

---

## Pre-requisitos (YA COMPLETADOS)

- [x] File map: `phase2_kmp_file_map.md` — 51 commonMain / 14 androidMain
- [x] Version catalog: `kotlin-multiplatform` plugin + `kotlinx-coroutines-core` library agregados
- [x] Convention plugin: `watermelon.kmp.native` (`KmpNativeConventionPlugin.kt`) creado y compilando
- [x] Build green con plugin registrado (no afecta build actual)

---

## Paso 1: Switch build.gradle.kts (5 min)

```kotlin
// audio/build.gradle.kts — CAMBIAR DE:
plugins {
    id("watermelon.kmp.native")
}
android {
    namespace = "com.watermellonstudios.audio"
}

// A:
plugins {
    id("watermelon.kmp.native")
}
android {
    namespace = "com.watermellonstudios.audio"
}
```

**Verificar:** `./gradlew :build-logic:convention:compileKotlin` (plugin compila)

---

## Paso 2: Crear estructura de directorios (2 min)

```bash
cd audio/src
mkdir -p commonMain/kotlin/com/watermellonstudios/audio
mkdir -p androidMain/kotlin/com/watermellonstudios/audio
# Nota: el C++ queda en main/cpp/ — CMake path no cambia
```

---

## Paso 3: Mover archivos commonMain (10 min)

### domain/ (23 files)
```bash
cd audio/src
# Crear estructura
mkdir -p commonMain/kotlin/com/watermellonstudios/audio/domain/{effect,error,mode,modulator,oscillator,scale,state,usb}

# Mover
mv main/kotlin/com/watermellonstudios/audio/domain/effect/*.kt commonMain/kotlin/com/watermellonstudios/audio/domain/effect/
mv main/kotlin/com/watermellonstudios/audio/domain/error/*.kt commonMain/kotlin/com/watermellonstudios/audio/domain/error/
mv main/kotlin/com/watermellonstudios/audio/domain/mode/*.kt commonMain/kotlin/com/watermellonstudios/audio/domain/mode/
mv main/kotlin/com/watermellonstudios/audio/domain/modulator/*.kt commonMain/kotlin/com/watermellonstudios/audio/domain/modulator/
mv main/kotlin/com/watermellonstudios/audio/domain/oscillator/*.kt commonMain/kotlin/com/watermellonstudios/audio/domain/oscillator/
mv main/kotlin/com/watermellonstudios/audio/domain/scale/*.kt commonMain/kotlin/com/watermellonstudios/audio/domain/scale/
mv main/kotlin/com/watermellonstudios/audio/domain/state/*.kt commonMain/kotlin/com/watermellonstudios/audio/domain/state/
mv main/kotlin/com/watermellonstudios/audio/domain/usb/*.kt commonMain/kotlin/com/watermellonstudios/audio/domain/usb/
```

**Nota:** `UsbDeviceCompatibility.kt` tiene `android.os.Build` — dejarlo en androidMain o abstraer.

### callback/ (2 files)
```bash
mkdir -p commonMain/kotlin/com/watermellonstudios/audio/callback
mv main/kotlin/com/watermellonstudios/audio/callback/*.kt commonMain/kotlin/com/watermellonstudios/audio/callback/
```

### api/ (15 files — todo excepto UsbAudioManagerFactory y latency/)
```bash
mkdir -p commonMain/kotlin/com/watermellonstudios/audio/api/{callbacks,config}
mv main/kotlin/com/watermellonstudios/audio/api/AudioEngine.kt commonMain/kotlin/com/watermellonstudios/audio/api/
mv main/kotlin/com/watermellonstudios/audio/api/AudioEngineFactory.kt commonMain/kotlin/com/watermellonstudios/audio/api/
mv main/kotlin/com/watermellonstudios/audio/api/EffectManagerFactory.kt commonMain/kotlin/com/watermellonstudios/audio/api/
mv main/kotlin/com/watermellonstudios/audio/api/IEffectManager.kt commonMain/kotlin/com/watermellonstudios/audio/api/
mv main/kotlin/com/watermellonstudios/audio/api/IEffectStateProvider.kt commonMain/kotlin/com/watermellonstudios/audio/api/
mv main/kotlin/com/watermellonstudios/audio/api/IEffectStateWriter.kt commonMain/kotlin/com/watermellonstudios/audio/api/
mv main/kotlin/com/watermellonstudios/audio/api/IModeStateWriter.kt commonMain/kotlin/com/watermellonstudios/audio/api/
mv main/kotlin/com/watermellonstudios/audio/api/IModeTransitionHandler.kt commonMain/kotlin/com/watermellonstudios/audio/api/
mv main/kotlin/com/watermellonstudios/audio/api/IUsbAudioManager.kt commonMain/kotlin/com/watermellonstudios/audio/api/
mv main/kotlin/com/watermellonstudios/audio/api/ModeTransitionFactory.kt commonMain/kotlin/com/watermellonstudios/audio/api/
mv main/kotlin/com/watermellonstudios/audio/api/StateSynchronizerFactory.kt commonMain/kotlin/com/watermellonstudios/audio/api/
mv main/kotlin/com/watermellonstudios/audio/api/SyncApi.kt commonMain/kotlin/com/watermellonstudios/audio/api/
mv main/kotlin/com/watermellonstudios/audio/api/callbacks/*.kt commonMain/kotlin/com/watermellonstudios/audio/api/callbacks/
mv main/kotlin/com/watermellonstudios/audio/api/config/*.kt commonMain/kotlin/com/watermellonstudios/audio/api/config/
```

### internal/ pure utilities (11 files)
```bash
mkdir -p commonMain/kotlin/com/watermellonstudios/audio/internal/{engine,effect,sync,optimization,util}
mv main/kotlin/com/watermellonstudios/audio/internal/engine/AudioEngineImpl.kt commonMain/kotlin/com/watermellonstudios/audio/internal/engine/
mv main/kotlin/com/watermellonstudios/audio/internal/sync/StateDivergence.kt commonMain/kotlin/com/watermellonstudios/audio/internal/sync/
mv main/kotlin/com/watermellonstudios/audio/internal/sync/SyncConfig.kt commonMain/kotlin/com/watermellonstudios/audio/internal/sync/
mv main/kotlin/com/watermellonstudios/audio/internal/sync/SyncEvent.kt commonMain/kotlin/com/watermellonstudios/audio/internal/sync/
mv main/kotlin/com/watermellonstudios/audio/internal/sync/SyncedAudioState.kt commonMain/kotlin/com/watermellonstudios/audio/internal/sync/
mv main/kotlin/com/watermellonstudios/audio/internal/optimization/Phase4Types.kt commonMain/kotlin/com/watermellonstudios/audio/internal/optimization/
mv main/kotlin/com/watermellonstudios/audio/internal/util/ChordGenerator.kt commonMain/kotlin/com/watermellonstudios/audio/internal/util/
mv main/kotlin/com/watermellonstudios/audio/internal/util/ScaleQuantizer.kt commonMain/kotlin/com/watermellonstudios/audio/internal/util/
```

---

## Paso 4: Mover archivos androidMain (5 min)

```bash
cd audio/src
# Todo lo que queda en main/kotlin/ va a androidMain/kotlin/
# (14 files con deps de Android)
mv main/kotlin/com/watermellonstudios/audio/* androidMain/kotlin/com/watermellonstudios/audio/

# El directorio main/kotlin/ deberia quedar vacio
rmdir main/kotlin/com/watermellonstudios/audio 2>/dev/null
```

---

## Paso 5: Compilar y fix (15-30 min)

```bash
./gradlew :audio:assembleDebug
```

**Errores esperados y soluciones:**

1. **`Unresolved reference: Log`** en commonMain
   - Archivo: `EffectManagerImpl.kt`, `StateSynchronizer.kt`
   - Fix: Reemplazar `android.util.Log` con callback `AudioLogger`
   - O: Mover esos archivos a androidMain temporalmente

2. **`Unresolved reference: BuildConfig`** en commonMain
   - Archivo: `UsbDeviceCompatibility.kt`
   - Fix: Mover a androidMain

3. **`Unresolved reference: Context`** en commonMain
   - Archivo: `UsbAudioManagerFactory.kt`
   - Fix: Ya deberia estar en androidMain

4. **kotlinx-coroutines import issues**
   - commonMain usa `kotlinx-coroutines-core` (no `-android`)
   - `Dispatchers.Main` no disponible en commonMain
   - Fix: Verificar que no hay `Dispatchers.Main` en commonMain files

5. **`NativeEffectSnapshot`** class referenced from commonMain
   - Puede estar en un package que se movio parcialmente
   - Fix: Mover la clase al source set correcto

---

## Paso 6: Verificacion (5 min)

```bash
# Build completo
./gradlew assembleDebug

# Verificar estructura
find audio/src/commonMain/kotlin -name "*.kt" | wc -l  # Target: ~51
find audio/src/androidMain/kotlin -name "*.kt" | wc -l  # Target: ~14

# No Android imports en commonMain
grep -rl "import android\.\|import androidx\." audio/src/commonMain/ && echo "FAIL" || echo "PASS"

# App funciona
# (manual test)
```

---

## Paso 7: Cleanup (5 min)

```bash
# Eliminar directorio main/kotlin/ vacio
rm -rf audio/src/main/kotlin

# Verificar que main/ solo tiene cpp/ y AndroidManifest.xml
ls audio/src/main/
# Esperado: AndroidManifest.xml  cpp/
```

---

## Troubleshooting

### "No matching variant" error
Si Gradle no encuentra variantes, verificar que `android.namespace` esta seteado y que el plugin KMP configura `androidTarget()` correctamente.

### CMake path issues
El CMakeLists.txt sigue en `src/main/cpp/` — el KMP convention plugin ya lo referencia ahi. No mover.

### Kotlin source set conflicts
Si hay archivos duplicados entre commonMain y androidMain, Gradle falla. Verificar que ningun package tiene archivos en ambos source sets (excepto expect/actual).

### hiltViewModel dependency
Si algun consumer del modulo audio usa `hiltViewModel()`, verificar que la dependencia de Hilt esta en el consumer, no en el modulo audio.

---

## Resultado esperado

```
audio/src/
  commonMain/
    kotlin/com/watermellonstudios/audio/
      api/         (15 files — interfaces)
      callback/    (2 files — interfaces)
      domain/      (23 files — pure models)
      internal/    (11 files — pure implementations)
  androidMain/
    kotlin/com/watermellonstudios/audio/
      api/latency/ (2 files)
      api/UsbAudioManagerFactory.kt
      internal/bridge/AudioNativeBridge.kt
      internal/mode/ (2 files)
      internal/native/NativeLibraryLoader.kt
      internal/optimization/JniMetrics.kt
      internal/sync/StateSynchronizer.kt
      internal/usb/ (4 files)
      internal/util/DeviceCapabilities.kt
      internal/effect/EffectManagerImpl.kt
  main/
    AndroidManifest.xml
    cpp/           (C++ code — unchanged)
```
