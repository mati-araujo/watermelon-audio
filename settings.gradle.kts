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
