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

    testOptions {
        unitTests {
            isReturnDefaultValues = true
        }
    }
}

// Host C++ unit tests (dsp + effects + looper + usb googletest suites).
// Delegates to the platform wrapper, which locates a host toolchain
// (MinGW g++ + Ninja on Windows, system g++ elsewhere). Wired into `check`
// so `./gradlew :audio:check` runs Kotlin AND C++ tests. Requires a host
// C++ compiler — see scripts/run-cpp-tests.{ps1,sh}.
val cppTest by tasks.registering(Exec::class) {
    group = "verification"
    description = "Build and run the host C++ (googletest) test suite."
    workingDir = rootProject.projectDir
    commandLine = if (System.getProperty("os.name").startsWith("Windows", ignoreCase = true)) {
        listOf(
            "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", "scripts/run-cpp-tests.ps1",
        )
    } else {
        listOf("bash", "scripts/run-cpp-tests.sh")
    }
}

tasks.named("check") { dependsOn(cppTest) }

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
