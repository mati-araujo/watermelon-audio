plugins {
    id("watermelon.kmp.native")
    `maven-publish`
}

group = "com.watermellonstudios"
version = findProperty("version")?.toString()?.takeIf { it != "unspecified" } ?: "1.0.0-SNAPSHOT"

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
