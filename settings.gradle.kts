pluginManagement {
    includeBuild("build-logic")
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    @Suppress("UnstableApiUsage")
    repositoriesMode = RepositoriesMode.FAIL_ON_PROJECT_REPOS
    @Suppress("UnstableApiUsage")
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "watermelon-audio"
include(":audio")

// WA-5.5 — harness de UI. NO se publica: no aplica `maven-publish`, asi que no
// produce publicaciones, y los dos workflows publican con `:audio:publishAll...`
// (path-qualified), que no lo alcanza ni queriendo.
// `scripts/check-no-ui-in-library.sh` afirma las dos cosas.
include(":harness")

// REQ-055 S3 — espiga Compose Desktop (gate de continuidad). Rama de espiga, NO se
// mergea a master. Tampoco se publica: no aplica `maven-publish` (lo afirma
// `scripts/check-no-ui-in-library.sh`) y depende de :audio en una sola direccion.
include(":studio-spike")
