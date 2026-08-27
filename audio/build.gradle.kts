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

// ============================================================================
// REQ-016 — el arnés JNI. La librería nativa DE HOST que `testDebugUnitTest`
// carga para EJECUTAR funciones `JNIEXPORT` reales contra un `JNIEnv` real.
//
// 310 funciones `JNIEXPORT` y ningún test las ejecutaba: el camino estaba
// cubierto por compilación, por el gate de símbolos de MINI-001 —que compara
// sólo NOMBRES— y por el smoke manual en emulador. Un desajuste de FIRMA pasa
// los tres.
//
// 🔴 CERO CAMBIOS EN PRODUCCIÓN, y es la razón por la que esto entra acá y no en
// `androidMain`: `System.loadLibrary` busca en `java.library.path`, así que la
// carga se resuelve desde la task de test. Si alguna vez pareciera que hace
// falta un hook en producción, eso es un hallazgo, no una línea que se agrega.
// ============================================================================
val hostJniLibDir = layout.buildDirectory.dir("hostjni")

val buildHostJniLib by tasks.registering(Exec::class) {
    group = "build"
    description = "Compila libwatermelon_audio.{so,dylib} para el host (arnés JNI, REQ-016)."
    workingDir = rootProject.projectDir
    commandLine("bash", "scripts/build-host-jni.sh")

    // El MISMO JDK que va a correr los tests. Headers de una versión y runtime de
    // otra es exactamente la clase de desajuste que este arnés existe para no
    // tener, y `find_package(JNI)` ni siquiera lo mira.
    environment("WMA_JAVA_HOME", System.getProperty("java.home"))

    inputs.files(
        fileTree(file("src/main/cpp")) {
            exclude("ios/build/**", "**/.deps/**", "**/build/**", "**/build-san/**")
        }
    )
    // Y el script que la task EJECUTA. Sin esta línea, cambiarle las banderas sin
    // tocar C++ deja la task UP-TO-DATE y sobrevive el binario viejo — medido en
    // `buildIosNativeLib` (ver KmpNativeConventionPlugin.kt).
    inputs.file(rootProject.file("scripts/build-host-jni.sh"))
    outputs.dir(hostJniLibDir)
}

tasks.withType<Test>().configureEach {
    // Hacen falta LAS DOS lineas, y esto NO es ceremonia: lo destapo el control
    // positivo de esta etapa. Con solo `dependsOn` —que unicamente ORDENA— se
    // renombro el simbolo del lado C++, la libreria se reconstruyo SIN el, y
    // `testDebugUnitTest` quedo UP-TO-DATE: verde, sin ejecutar un solo test.
    // O sea el modo de falla exacto que REQ-016 existe para borrar, dentro del
    // propio arnes. Declarando el .so como INPUT, cambiarlo re-corre los tests.
    //
    // Es la misma trampa que ya tenia documentada `cinteropWatermelonAudio` en
    // KmpNativeConventionPlugin.kt, donde costo que la app de iOS corriera
    // codigo viejo con el gate en OK.
    dependsOn(buildHostJniLib)
    inputs.files(buildHostJniLib.map { it.outputs.files })
        .withPropertyName("hostJniHarnessLibrary")
        .withPathSensitivity(PathSensitivity.NONE)
    // D4: así es como la librería aparece sin tocar `androidMain`. Se APENDEA al
    // valor que traiga el runtime en vez de reemplazarlo: pisarlo deja a la JVM
    // sin sus propias librerías nativas.
    val existing = System.getProperty("java.library.path").orEmpty()
    val harnessDir = hostJniLibDir.get().asFile.absolutePath
    systemProperty(
        "java.library.path",
        if (existing.isEmpty()) harnessDir else "$harnessDir${File.pathSeparator}$existing",
    )
    // Lo que el arnés imprime —el conteo de AC-016.3 y la causa de un fallo de
    // carga— tiene que llegar al log del gate, no morir en el worker de test.
    testLogging { showStandardStreams = true }
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
