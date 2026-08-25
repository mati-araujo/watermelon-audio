# Contributing to Watermelon Audio

## Development Setup

```bash
# Clone
git clone https://github.com/mati-araujo/watermelon-audio.git
cd watermelon-audio

# Create local.properties with Android SDK path
echo "sdk.dir=C:\\Users\\<user>\\AppData\\Local\\Android\\Sdk" > local.properties

# Build
./gradlew :audio:assembleDebug
```

## Workflow: Audio changes that affect NoisyPad

Since NoisyPad consumes this library as an external dependency, changes require
a coordinated workflow across two repos.

### Option A: Local development (fast iteration)

Best for active development where you're making frequent changes.

**1. Enable composite build in NoisyPad:**

```kotlin
// NoisyPad/settings.gradle.kts — add before include(":app"):
includeBuild("../watermelon-audio") {
    dependencySubstitution {
        substitute(module("com.watermellonstudios:audio-android"))
            .using(project(":audio"))
    }
}
```

**2. Make changes in watermelon-audio/ — NoisyPad sees them immediately** (no publish needed).

**3. When done, remove the `includeBuild` block and publish:**
```bash
cd ~/android/watermelon-audio
./gradlew :audio:publishToMavenLocal
```

**4. Verify NoisyPad builds against the published artifact** (not composite).

### Option B: SNAPSHOT workflow (CI-friendly)

Best for isolated changes or when working from different machines.

```bash
# 1. Make changes in watermelon-audio
cd ~/android/watermelon-audio
# ... edit code ...

# 2. Publish locally
./gradlew :audio:publishToMavenLocal

# 3. Build NoisyPad (auto-resolves new SNAPSHOT)
cd ~/android/NoisyPad
./gradlew assembleDebug
```

SNAPSHOT versions in local Maven are resolved fresh each build — no version bump needed.

### Option C: Release workflow (production)

For shipping to production:

```bash
# 1. Use Conventional Commit prefixes on the changes that should drive the release
# feat:, fix:, perf:, build:, docs:, chore:

# 2. When you decide to cut a version, trigger the `Release Please` workflow BY HAND
# (workflow_dispatch). Since 2026-08-25 it no longer runs on every push to master:
# a one-line fix: used to open a release PR nobody had asked for. Merging its PR
# updates CHANGELOG.md, .release-please-manifest.json and gradle.properties, and
# creates the tag — but it does NOT publish.

# 3. Trigger `Release Please` AGAIN. That second dispatch is what creates the release and
# the vX.Y.Z tag. It is easy to miss: while this ran on push, merging the release PR was
# itself the push that tagged it. With a manual trigger nothing observes that merge, so
# you end up with the version bumped and no tag. It is idempotent — just run it again.

# 4. Publish as a third, explicit gesture: trigger the `Publish` workflow on that tag.
# It waits for the CI of the commit to be green before publishing (fail-closed); the
# `saltear_espera_ci` input skips that wait, and is only for a rescue. See docs/release.md.

# 4. Update version in NoisyPad
# All build.gradle.kts: "com.watermellonstudios:audio-android:<released-version>"

# 5. Build and test NoisyPad
cd ~/android/NoisyPad
./gradlew assembleRelease
```

See `docs/release.md` for the full release and emergency publish workflow.

## Adding a new effect

This is the most common cross-repo change. The workflow:

```
watermelon-audio                          NoisyPad
─────────────────                         ────────
1. C++: effects/NewEffect.h/.cpp
2. Register in EffectRegistry
3. Add to CMakeLists.txt
4. Add to EffectTypes.h (C++)
5. Kotlin: EffectType enum
6. Kotlin: EffectParameter constants
7. Kotlin: EffectConstants defaults
8. Publish (local or GitHub)
                                          9. Presets in core-domain/preset/
                                          10. UI in feature-effects/
                                          11. Scene support if needed
```

## Adding a new synth engine

```
watermelon-audio                          NoisyPad
─────────────────                         ────────
1. C++: engines/NewEngine.h
2. Register in AudioEngine + OscillatorNode
3. Publish
                                          4. EngineType.kt already in audio
                                          5. UI in feature-chaospad if needed
```

## Commands

```bash
./gradlew :audio:assembleDebug                                    # Debug build (4 ABIs)
./gradlew :audio:assembleRelease                                  # Release build
./gradlew :audio:publishToMavenLocal                              # Publish to ~/.m2/
./gradlew :audio:publishAllPublicationsToGitHubPackagesRepository  # Publish to GitHub
```

## Credentials

GitHub Packages auth goes in `~/.gradle/gradle.properties` (NOT in the project):

```properties
gpr.user=<github-username>
gpr.key=<personal-access-token-with-write:packages>
```

Un `local.properties` gitignoreado del consumidor también sirve, y es donde está
hoy en la máquina de desarrollo.

**Ese PAT es el único que puede consultar el registro.** El token de `gh auth
login` es un OAuth propio de la CLI y no trae scope de packages, así que
`gh api user/packages` devuelve 403 por más que estés logueado. Para verificar un
release contra el registro —que no es lo mismo que ver el publish en verde— ver
`docs/release.md`.
