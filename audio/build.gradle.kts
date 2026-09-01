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
val jniCoverageDir = layout.buildDirectory.dir("jni-coverage")

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

// ============================================================================
// REQ-016 AC-016.3 / REQ-018 — el conteo del arnés, sumado ENTRE PROCESOS.
//
// Cada clase del arnés corre en su propia JVM (`forkEvery = 1`, arriba), porque
// el motor nativo es un singleton de proceso y los tests de ausencia necesitan
// uno virgen. Así que el total no lo puede calcular ninguna clase: lo arma esto,
// leyendo lo que cada una dejó en build/jni-coverage/.
//
// 🔴 "No pude sumar" NO es un pase. Si no hay archivos, o si el total da cero,
// esto FALLA — un arnés que no ejerció nada se ve idéntico a uno que ejerció
// todo, y ésa es la forma en que mueren los gates de este repo.
// ============================================================================
val jniHarnessCoverage by tasks.registering {
    group = "verification"
    description = "Suma e imprime cuántas JNIEXPORT ejecutó el arnés (AC-016.3)."

    // Mismos locales por la misma razón que arriba: nada de referencias al script
    // adentro de la acción, o la caché de configuración se cae.
    val dir = jniCoverageDir.get().asFile
    val jniSources = rootProject.file("audio/src/main/cpp/jni")
    doLast {
        val files = dir.listFiles { f -> f.isFile && f.name.endsWith(".txt") }.orEmpty()
        if (files.isEmpty()) {
            throw GradleException(
                "el arnés JNI no dejó ninguna cobertura en $dir. No es 'cero funciones': " +
                    "es un conteo que no se pudo medir, y eso nunca es un pase."
            )
        }
        val executed = files.flatMap { it.readLines() }.filter { it.isNotBlank() }.toSortedSet()
        val total = jniSources.listFiles { f -> f.name.endsWith(".cpp") }.orEmpty()
            .sumOf { f -> Regex("""^JNIEXPORT\b""", RegexOption.MULTILINE).findAll(f.readText()).count() }
        if (total == 0) throw GradleException("no encontré una sola JNIEXPORT en $jniSources: el conteo se rompió.")
        if (executed.isEmpty()) throw GradleException("el arnés corrió y no ejerció UNA sola función JNIEXPORT.")

        logger.lifecycle(
            """
            |[REQ-016] arnés JNI — TOTAL: ${executed.size} de $total funciones JNIEXPORT ejecutadas
            |          contra un JNIEnv real · hueco: ${total - executed.size}
            |          sumado sobre ${files.size} clases: ${files.map { it.nameWithoutExtension }.sorted().joinToString()}
            |          🔴 backend FALSO adentro: valida la frontera JNI/Kotlin, NO audio en dispositivo
            """.trimMargin()
        )
    }
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

    // 🔴 UN PROCESO POR CLASE, y no es higiene decorativa.
    //
    // El motor nativo es un SINGLETON DE PROCESO: `g_wmaEngine` lo crea
    // `nativeStartTuner` —el único de los 310 que lo hace— y **no se destruye
    // nunca**. O sea que el estado "todavía no hay motor" existe una sola vez por
    // JVM, y es justo el que vuelve no-triviales a las respuestas de después: sin
    // él, "devuelve true" no se distingue de "devuelve true siempre".
    //
    // Con una sola JVM eso se lo lleva la primera clase que arranque el motor, y
    // qué clase es depende del orden en que Gradle las corra. Medido: al entrar
    // `InputJniTest` (REQ-018), que arranca el motor en su `@Before` y ordena antes
    // que `TunerEngineJniTest`, el test de ausencia de REQ-016 se puso rojo.
    //
    // El arreglo NO es bajar la exigencia de ese test —lo dice su propio comentario—
    // sino darle a cada clase el proceso virgen que su premisa necesita. Costo
    // medido: ver las notas de REQ-018 S1.
    forkEvery = 1

    // Dónde deja cada clase su cobertura, para que `jniHarnessCoverage` la sume.
    // Absoluto porque cada worker de test tiene su propio directorio de trabajo.
    //
    // El `File` se captura en un local ANTES del `doFirst`: meter ahí la propiedad
    // del script hace que la caché de configuración falle con "cannot serialize
    // Gradle script object references" (medido).
    val coverageDirFile = jniCoverageDir.get().asFile
    systemProperty("wma.jniCoverageDir", coverageDirFile.absolutePath)
    doFirst { coverageDirFile.deleteRecursively() }

    // 🔴 Y el directorio va declarado como SALIDA, o el finalizador se queda sin
    // nada que sumar. Es la lección de `dependsOn` SOLO ORDENA, del lado de las
    // salidas: Gradle no sabe que esta task produce `build/jni-coverage/`, así que
    // si el directorio no está —lo borró alguien, o vino de otro checkout— la task
    // igual se declara UP-TO-DATE, no lo repuebla, y `jniHarnessCoverage` falla con
    // "no pude sumar". Medido: `rm -rf audio/build/jni-coverage && ./gradlew
    // :audio:testDebugUnitTest` daba rojo con la task en UP-TO-DATE, y lo mismo
    // pasaba en `gate.sh` cuando la task venía FROM-CACHE.
    //
    // Declarándolo, borrar el directorio vuelve la task out-of-date (se recorre) y
    // un acierto de caché lo restaura junto con el resto. De paso cierra el modo de
    // falla espejado y peor: un directorio VIEJO que sobrevive a un cambio de tests
    // y deja pasar un conteo stale.
    outputs.dir(coverageDirFile)

    // El total se imprime DONDE SEA que corran los tests: `gate.sh` corre
    // `:audio:testDebugUnitTest` y el job `build` del CI también, y ninguno de los
    // dos corre `check`. Con un finalizador, AC-016.3 se cumple sin tocar
    // `scripts/gate.sh` ni `ci.yml`. Y si el finalizador falla, falla el build:
    // "no pude sumar" no es un pase.
    //
    // Va acá adentro y no con `tasks.named("testDebugUnitTest")` al final del
    // script porque esa task la registra AGP más tarde y el lookup eager revienta
    // con "Task with name 'testDebugUnitTest' not found" (medido).
    finalizedBy(jniHarnessCoverage)
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

