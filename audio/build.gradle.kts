plugins {
    id("watermelon.kmp.native")
    `maven-publish`
}

group = "com.watermellonstudios"
version = providers.gradleProperty("version").get()

android {
    namespace = "com.watermellonstudios.audio"

    defaultConfig {
        externalNativeBuild {
            cmake {
                arguments += "-DWMA_PROJECT_VERSION=${project.version}"
            }
        }
    }
}

// KMP automatically creates publications for each target.
// We only configure the repository here.
publishing {
    repositories {
        maven {
            name = "GitHubPackages"
            url = uri("https://maven.pkg.github.com/mati-araujo/watermelon-audio")
            credentials {
                username = project.findProperty("gpr.user")?.toString() ?: System.getenv("GITHUB_ACTOR")
                password = project.findProperty("gpr.key")?.toString() ?: System.getenv("GITHUB_TOKEN")
            }
        }
    }
}
