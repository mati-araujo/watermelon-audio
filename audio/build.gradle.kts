plugins {
    id("watermelon.kmp.native")
    `maven-publish`
}

android {
    namespace = "com.watermellonstudios.audio"
}

group = "com.watermellonstudios"
version = "1.0.0-SNAPSHOT"

// KMP automatically creates publications for each target.
// We only configure the repository here.
publishing {
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
