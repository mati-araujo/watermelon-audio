# Phase 3 — Repositorio y Distribucion

**Extraer a repositorio independiente y publicar via GitHub Packages**

*Prerequisito: Phase 2 completada (KMP structure funcional)*

---

## Tabla de Contenidos

1. [Objetivo](#1-objetivo)
2. [Sub-fase 3A: Crear Repositorio](#2-sub-fase-3a-crear-repositorio)
3. [Sub-fase 3B: Build System Standalone](#3-sub-fase-3b-build-system-standalone)
4. [Sub-fase 3C: GitHub Packages Publishing](#4-sub-fase-3c-github-packages-publishing)
5. [Sub-fase 3D: CI/CD Pipeline](#5-sub-fase-3d-cicd-pipeline)
6. [Sub-fase 3E: Migrar NoisyPad](#6-sub-fase-3e-migrar-noisypad)
7. [Sub-fase 3F: Documentacion](#7-sub-fase-3f-documentacion)

---

## 1. Objetivo

Mover el modulo audio a su propio repositorio GitHub con build system
independiente, publicacion automatizada via GitHub Packages, y CI/CD.
NoisyPad pasa a consumir la libreria como dependencia Maven.

### Artifacts publicados

| Artifact ID | Contenido | Plataforma |
|-------------|-----------|-----------|
| `audio-kotlin` | KMP module (commonMain + androidMain) | Android + common |
| `audio-android` | Android integration (umbrella que incluye audio-kotlin + native) | Android |

**Group ID:** `com.watermellonstudios`

---

## 2. Sub-fase 3A: Crear Repositorio

### Pre-condiciones
- Phase 2 completada

### Tareas

#### 3A.1 — Crear repositorio en GitHub

- Nombre: `watermelon-audio` (o `wma-audio`)
- Organizacion: `watermellonstudios` (o personal)
- Privado inicialmente (public cuando este listo)
- License: Apache 2.0

#### 3A.2 — Estructura inicial del repositorio

```
watermelon-audio/
├── audio-core/                    C++ source (from audio/src/main/cpp/)
│   ├── include/watermelon/audio/
│   │   └── watermelon_audio.h
│   ├── src/
│   │   ├── api/
│   │   ├── platform/
│   │   ├── dsp/
│   │   ├── effects/
│   │   ├── engines/
│   │   ├── voice/
│   │   ├── graph/
│   │   ├── core/
│   │   ├── nodes/
│   │   ├── looper/
│   │   ├── sequencer/
│   │   ├── modulators/
│   │   ├── oscillators/
│   │   ├── analysis/
│   │   ├── backends/
│   │   ├── usb/
│   │   └── jni/
│   ├── tests/
│   └── CMakeLists.txt
│
├── audio-kotlin/                  KMP Kotlin module
│   ├── src/
│   │   ├── commonMain/kotlin/
│   │   └── androidMain/kotlin/
│   └── build.gradle.kts
│
├── audio-android/                 Android-specific module (optional umbrella)
│   ├── src/androidMain/
│   └── build.gradle.kts
│
├── sample/                        Sample app (minimal consumer)
│   └── build.gradle.kts
│
├── build-logic/
│   └── convention/                Convention plugins propios
│
├── gradle/
│   └── libs.versions.toml
├── settings.gradle.kts
├── build.gradle.kts
├── gradle.properties
├── LICENSE
├── CLAUDE.md
└── README.md
```

#### 3A.3 — Copiar codigo (no mover aun)

Copiar todo el contenido del modulo audio de NoisyPad al nuevo repo.
NoisyPad sigue usando su modulo local durante esta fase.

**Archivos a copiar:**
- `audio/src/main/cpp/` → `audio-core/src/`
- `audio/src/commonMain/kotlin/` → `audio-kotlin/src/commonMain/kotlin/`
- `audio/src/androidMain/kotlin/` → `audio-kotlin/src/androidMain/kotlin/`

### Verificacion

```bash
# V-3A.1: Repo exists locally
test -d watermelon-audio/.git && echo "PASS" || echo "FAIL"

# V-3A.2: Core structure present
test -f watermelon-audio/audio-core/CMakeLists.txt && \
test -f watermelon-audio/audio-kotlin/build.gradle.kts && \
test -f watermelon-audio/settings.gradle.kts && \
echo "PASS" || echo "FAIL"
```

---

## 3. Sub-fase 3B: Build System Standalone

### Pre-condiciones
- 3A completada

### Contexto

El modulo actual depende de `build-logic/convention/` de NoisyPad
(plugin `watermelon.kmp.native`). Necesitamos un build system propio.

### Tareas

#### 3B.1 — Crear version catalog propio

```toml
# gradle/libs.versions.toml
[versions]
compileSdk = "36"
minSdk = "29"
targetSdk = "36"

agp = "9.1.0"
kotlin = "2.3.20"
cmake = "3.22.1"

oboe = "1.10.0"
kotlinxCoroutines = "1.10.2"
datastore = "1.2.1"
lifecycleRuntime = "2.10.0"
coreKtx = "1.18.0"

# Testing
junit = "4.13.2"
androidxTestJunit = "1.3.0"

[libraries]
oboe = { module = "com.google.oboe:oboe", version.ref = "oboe" }
kotlinx-coroutines-core = { module = "org.jetbrains.kotlinx:kotlinx-coroutines-core", version.ref = "kotlinxCoroutines" }
kotlinx-coroutines-android = { module = "org.jetbrains.kotlinx:kotlinx-coroutines-android", version.ref = "kotlinxCoroutines" }
datastore = { module = "androidx.datastore:datastore-preferences", version.ref = "datastore" }
lifecycle-runtime = { module = "androidx.lifecycle:lifecycle-runtime-ktx", version.ref = "lifecycleRuntime" }
core-ktx = { module = "androidx.core:core-ktx", version.ref = "coreKtx" }
junit = { module = "junit:junit", version.ref = "junit" }
androidx-test-junit = { module = "androidx.test.ext:junit", version.ref = "androidxTestJunit" }

[plugins]
android-library = { id = "com.android.library", version.ref = "agp" }
kotlin-multiplatform = { id = "org.jetbrains.kotlin.multiplatform", version.ref = "kotlin" }
maven-publish = { id = "maven-publish" }
```

#### 3B.2 — Crear convention plugins

```kotlin
// build-logic/convention/src/main/kotlin/WmaAndroidNativePlugin.kt
class WmaAndroidNativePlugin : Plugin<Project> {
    override fun apply(target: Project) {
        with(target) {
            pluginManager.apply("com.android.library")
            pluginManager.apply("org.jetbrains.kotlin.multiplatform")

            extensions.configure<LibraryExtension> {
                compileSdk = libs.version("compileSdk").toInt()
                defaultConfig {
                    minSdk = libs.version("minSdk").toInt()
                    ndk {
                        abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64", "x86")
                    }
                    externalNativeBuild {
                        cmake {
                            cppFlags += "-std=c++20"
                            arguments += listOf(
                                "-DANDROID_STL=c++_shared",
                                "-DANDROID_PLATFORM=android-${libs.version("minSdk")}"
                            )
                        }
                    }
                }
                externalNativeBuild {
                    cmake {
                        path = file("src/main/cpp/CMakeLists.txt")
                        version = libs.version("cmake")
                    }
                }
                buildFeatures {
                    prefab = true
                    buildConfig = true
                }
            }
        }
    }
}
```

#### 3B.3 — Configurar settings.gradle.kts

```kotlin
// settings.gradle.kts
pluginManagement {
    includeBuild("build-logic")
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "watermelon-audio"
include(":audio-kotlin")
include(":audio-android")
include(":sample")
```

#### 3B.4 — Configurar CMakeLists.txt para standalone build

Actualizar paths relativos. El CMakeLists.txt principal ya funciona,
solo necesita ajustar paths de include ahora que la estructura es diferente.

#### 3B.5 — Crear sample app

App minima que:
1. Crea un `AudioEngine` via factory
2. Inicia audio
3. Setea XY y escucha sonido
4. Agrega un effect

Sirve como smoke test y ejemplo para consumidores.

#### 3B.6 — Verificar build completo

```bash
cd watermelon-audio
./gradlew assembleDebug          # Build all modules
./gradlew :audio-kotlin:assembleDebug  # Kotlin module alone
./gradlew :sample:assembleDebug   # Sample app
```

### Verificacion

```bash
# V-3B.1: Independent build works
cd watermelon-audio && ./gradlew assembleDebug

# V-3B.2: No references to NoisyPad build-logic
grep -rn "noisypad" watermelon-audio/build-logic/ && echo "FAIL" || echo "PASS"

# V-3B.3: Sample app builds
cd watermelon-audio && ./gradlew :sample:assembleDebug

# V-3B.4: CMake build works (native library produced)
find watermelon-audio -name "libwatermelon_audio.so" | head -1 | \
  xargs test -f && echo "PASS" || echo "FAIL"
```

---

## 4. Sub-fase 3C: GitHub Packages Publishing

### Pre-condiciones
- 3B completada

### Tareas

#### 3C.1 — Configurar maven-publish en audio-kotlin

```kotlin
// audio-kotlin/build.gradle.kts
plugins {
    id("wma.android.native")
    id("maven-publish")
}

group = "com.watermellonstudios"
version = "1.0.0-alpha01"

publishing {
    publications {
        // KMP automatically creates publications for each target
    }
    repositories {
        maven {
            name = "GitHubPackages"
            url = uri("https://maven.pkg.github.com/watermellonstudios/watermelon-audio")
            credentials {
                username = project.findProperty("gpr.user")?.toString() ?: System.getenv("GITHUB_ACTOR")
                password = project.findProperty("gpr.key")?.toString() ?: System.getenv("GITHUB_TOKEN")
            }
        }
    }
}
```

#### 3C.2 — Configurar publishing para audio-android (umbrella)

```kotlin
// audio-android/build.gradle.kts
plugins {
    id("com.android.library")
    id("maven-publish")
}

dependencies {
    api(project(":audio-kotlin"))
}

afterEvaluate {
    publishing {
        publications {
            create<MavenPublication>("release") {
                from(components["release"])
                groupId = "com.watermellonstudios"
                artifactId = "audio-android"
                version = rootProject.version.toString()
            }
        }
    }
}
```

#### 3C.3 — Versionado

Usar propiedad centralizada en `gradle.properties`:
```properties
VERSION_NAME=1.0.0-alpha01
VERSION_CODE=1
GROUP=com.watermellonstudios
```

Versionado semantico:
- `alpha` — pre-release, API puede cambiar
- `beta` — API estable, bugs posibles
- `rc` — release candidate
- Release — production ready

#### 3C.4 — Publish test local

```bash
./gradlew publishToMavenLocal
# Verify:
ls ~/.m2/repository/com/watermellonstudios/audio-android/
```

#### 3C.5 — Publish a GitHub Packages

```bash
# With GITHUB_TOKEN set:
./gradlew publish
```

### Verificacion

```bash
# V-3C.1: Local publish works
cd watermelon-audio && ./gradlew publishToMavenLocal
ls ~/.m2/repository/com/watermellonstudios/audio-android/1.0.0-alpha01/ && echo "PASS" || echo "FAIL"

# V-3C.2: POM file correct
cat ~/.m2/repository/com/watermellonstudios/audio-android/1.0.0-alpha01/*.pom | \
  grep -q "watermellonstudios" && echo "PASS" || echo "FAIL"

# V-3C.3: AAR produced
find ~/.m2/repository/com/watermellonstudios/ -name "*.aar" | head -1 | \
  xargs test -f && echo "PASS" || echo "FAIL"
```

---

## 5. Sub-fase 3D: CI/CD Pipeline

### Pre-condiciones
- 3C completada (publishing configurado)

### Tareas

#### 3D.1 — GitHub Actions: Build + Test on PR

```yaml
# .github/workflows/build.yml
name: Build & Test

on:
  pull_request:
    branches: [main]
  push:
    branches: [main]

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Set up JDK 17
        uses: actions/setup-java@v4
        with:
          java-version: '17'
          distribution: 'temurin'

      - name: Setup Android SDK
        uses: android-actions/setup-android@v3

      - name: Setup NDK
        run: sdkmanager "ndk;27.0.12077973"

      - name: Build
        run: ./gradlew assembleDebug

      - name: C++ Unit Tests
        run: |
          cd audio-core
          mkdir -p build && cd build
          cmake -DBUILD_TESTS=ON ..
          make -j$(nproc)
          ctest --output-on-failure

      - name: Kotlin Tests
        run: ./gradlew test

      - name: Lint
        run: ./gradlew lint
```

#### 3D.2 — GitHub Actions: Publish on Tag

```yaml
# .github/workflows/publish.yml
name: Publish

on:
  push:
    tags:
      - 'v*'

jobs:
  publish:
    runs-on: ubuntu-latest
    permissions:
      packages: write
    steps:
      - uses: actions/checkout@v4

      - name: Set up JDK 17
        uses: actions/setup-java@v4
        with:
          java-version: '17'
          distribution: 'temurin'

      - name: Setup Android SDK
        uses: android-actions/setup-android@v3

      - name: Extract version from tag
        run: echo "VERSION=${GITHUB_REF#refs/tags/v}" >> $GITHUB_ENV

      - name: Build & Publish
        env:
          GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        run: ./gradlew publish -PVERSION_NAME=${{ env.VERSION }}
```

#### 3D.3 — Build matrix (opcional)

Testear en multiples configuraciones:
- `ubuntu-latest` + NDK (primary)
- `macos-latest` + NDK (secondary, para iOS future)

#### 3D.4 — Badge en README

```markdown
![Build](https://github.com/watermellonstudios/watermelon-audio/actions/workflows/build.yml/badge.svg)
```

### Verificacion

```bash
# V-3D.1: Workflow files exist
test -f .github/workflows/build.yml && \
test -f .github/workflows/publish.yml && \
echo "PASS" || echo "FAIL"

# V-3D.2: Push to repo and verify CI runs
git push origin main
gh run list --limit 1
```

---

## 6. Sub-fase 3E: Migrar NoisyPad

### Pre-condiciones
- 3C completada (package publicado)
- 3D completada (CI green)

### Contexto

Momento de la verdad: NoisyPad deja de usar su modulo `:audio` local
y pasa a consumir el package de GitHub Packages.

### Tareas

#### 3E.1 — Agregar GitHub Packages repository a NoisyPad

```kotlin
// NoisyPad/settings.gradle.kts
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
        maven {
            url = uri("https://maven.pkg.github.com/watermellonstudios/watermelon-audio")
            credentials {
                username = project.findProperty("gpr.user")?.toString() ?: System.getenv("GITHUB_ACTOR")
                password = project.findProperty("gpr.key")?.toString() ?: System.getenv("GITHUB_TOKEN")
            }
        }
    }
}
```

#### 3E.2 — Reemplazar include(:audio) con dependencia Maven

```kotlin
// NoisyPad/settings.gradle.kts
// REMOVE: include(":audio")

// NoisyPad/app/build.gradle.kts (y otros modulos que dependan de audio)
dependencies {
    // ANTES: implementation(projects.audio)
    // DESPUES:
    implementation("com.watermellonstudios:audio-android:1.0.0-alpha01")
}
```

#### 3E.3 — Actualizar imports si hubo cambios de package

Si los packages cambiaron (improbable si se mantuvo `com.watermellonstudios.audio`),
actualizar imports en NoisyPad.

#### 3E.4 — Eliminar directorio audio/ de NoisyPad

Una vez que la dependencia Maven funciona, eliminar el modulo local:
```bash
rm -rf audio/
```

Y actualizar `settings.gradle.kts` para no incluirlo.

#### 3E.5 — Actualizar core-domain interfaces

Si `core-domain` depende de types de `audio` (EffectType, EngineType),
esos types ahora vienen del package Maven. Actualizar imports.

**Alternativa:** Si hay dependencia circular, mover los shared types a un
artifact separado `audio-api` que solo contiene interfaces.

#### 3E.6 — Full regression test

Ejecutar la misma matrix de tests que Phase 0E.2 y Phase 2E.4.

### Verificacion

```bash
# V-3E.1: No :audio module in NoisyPad
grep ":audio" NoisyPad/settings.gradle.kts | grep 'include' && echo "FAIL" || echo "PASS"

# V-3E.2: audio/ directory removed
test -d NoisyPad/audio && echo "FAIL: audio dir still exists" || echo "PASS"

# V-3E.3: NoisyPad builds with Maven dependency
cd NoisyPad && ./gradlew assembleDebug

# V-3E.4: App works correctly (manual test)
```

---

## 7. Sub-fase 3F: Documentacion

### Pre-condiciones
- 3E completada

### Tareas

#### 3F.1 — README.md del repositorio

Contenido:
- Quick start (3 pasos: add repo, add dependency, create engine)
- API overview con ejemplos
- Architecture diagram
- Building from source
- Contributing guide
- License

#### 3F.2 — CLAUDE.md del repositorio

Instrucciones para Claude Code:
- Estructura del proyecto
- Reglas de desarrollo (RT-safety, lock-free, etc.)
- Como agregar effects/engines
- Como agregar un nuevo backend
- Comandos de build y test

#### 3F.3 — CHANGELOG.md

```markdown
# Changelog

## 1.0.0-alpha01 (2026-04-XX)

### Initial Release
- 20 DSP effects with 6 routing modes
- 7 synth engines (Classic, FM, KarplusStrong, Wavetable, Granular, Supersaw, SoundFont)
- 16-voice polyphony with voice stealing
- 8-track audio looper
- Arpeggiator with 10 patterns
- C API (watermelon_audio.h) for cross-platform integration
- Dual backend: Oboe (Android) and libusb (USB Audio Class)
- Lock-free, RT-safe audio processing
```

#### 3F.4 — API reference

Generar documentacion de la C API con Doxygen o similar.
La Kotlin API se documenta con KDoc.

### Verificacion

```bash
# V-3F.1: Key docs exist
test -f watermelon-audio/README.md && \
test -f watermelon-audio/CLAUDE.md && \
test -f watermelon-audio/CHANGELOG.md && \
echo "PASS" || echo "FAIL"
```

### Post-condiciones (Phase 3 completa)

- [ ] Repositorio `watermelon-audio` en GitHub con CI green
- [ ] Package publicado en GitHub Packages (`audio-android:1.0.0-alpha01`)
- [ ] NoisyPad consume el package (no modulo local)
- [ ] `audio/` eliminado de NoisyPad
- [ ] Sample app funcional
- [ ] README con quickstart
- [ ] CI: build + test on PR, publish on tag
