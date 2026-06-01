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

# 2. Merge the Release Please PR on master
# It updates CHANGELOG.md, .release-please-manifest.json, and gradle.properties.

# 3. Let the tag-triggered Publish workflow deploy to GitHub Packages.

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
