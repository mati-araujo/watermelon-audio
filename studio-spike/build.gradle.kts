import org.jetbrains.compose.desktop.application.dsl.TargetFormat
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

/**
 * :studio-spike — REQ-055 S3 (gate de continuidad). App Compose Desktop MINIMA que toca
 * el motor desde la JVM en la Mac: un pad x/y, un selector de engine y un modo de
 * medicion (`--measure`). Rama de espiga: NO se mergea a master.
 *
 * NO SE PUBLICA, igual que :harness: no aplica `maven-publish` (lo afirma
 * scripts/check-no-ui-in-library.sh) y depende de :audio en una sola direccion.
 *
 * Nativo: `buildSpikeNative` corre CMake sobre src/main/native, que arma el dylib de
 * PRODUCCION del desktop (audio/src/main/cpp/desktop: CoreAudio, sin fakes ni USB) y el
 * shim JNI de la espiga. Los dos van a `appResourcesRootDir/macos-arm64`, o sea a
 * `Contents/app/resources/` del .app: adentro del bundle firmado, nunca extraidos a un
 * temporal (ADR-0009 §4). La app los carga por ruta absoluta con `System.load`.
 */
plugins {
    // Sin version: KGP ya esta en el classpath via includeBuild("build-logic").
    id("org.jetbrains.kotlin.jvm")
    alias(libs.plugins.compose.multiplatform)
    alias(libs.plugins.compose.compiler)
}

kotlin {
    compilerOptions { jvmTarget.set(JvmTarget.JVM_17) }
}
java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}

dependencies {
    implementation(project(":audio"))
    implementation(compose.desktop.currentOs)
    implementation(libs.kotlinx.coroutines.core)
}

val nativeBuildDir = layout.buildDirectory.dir("native")
val spikeResourcesDir = layout.buildDirectory.dir("spike-resources")

val buildSpikeNative = tasks.register<Exec>("buildSpikeNative") {
    group = "build"
    description = "Dylib de produccion del desktop + shim JNI de la espiga (CMake, Release)."
    val src = layout.projectDirectory.dir("src/main/native").asFile
    val out = nativeBuildDir.get().asFile
    val javaHome = providers.systemProperty("java.home").get()
    inputs.dir(src)
    inputs.dir(rootProject.file("audio/src/main/cpp")).withPropertyName("engineSources")
    outputs.dir(out)
    environment("JAVA_HOME", javaHome)
    commandLine(
        "bash", "-c",
        "cmake -S '${src}' -B '${out}' -G Ninja -DCMAKE_BUILD_TYPE=Release " +
            "-DCMAKE_OSX_ARCHITECTURES=arm64 -DWMA_JAVA_HOME='${javaHome}' && " +
            "cmake --build '${out}'",
    )
}

val stageSpikeNative = tasks.register<Sync>("stageSpikeNative") {
    dependsOn(buildSpikeNative)
    from(nativeBuildDir.map { it.dir("lib") }) { include("*.dylib") }
    into(spikeResourcesDir.map { it.dir("macos-arm64") })
}

compose.desktop {
    application {
        mainClass = "com.watermellonstudios.studiospike.MainKt"
        jvmArgs += listOf("-Xss2m")
        nativeDistributions {
            targetFormats(TargetFormat.Dmg)
            packageName = "NoisyPadStudioSpike"
            packageVersion = "1.0.0"
            description = "REQ-055 S3: espiga del motor desde la JVM"
            vendor = "Watermelon Studios"
            appResourcesRootDir.set(spikeResourcesDir)
            includeAllModules = true
            macOS {
                bundleID = "com.watermellonstudios.studiospike"
                // La firma Developer ID + notarizacion la hace packaging/sign-notarize.sh
                // sobre el .app (con la identidad y el perfil de notarytool como argumentos);
                // aca queda la firma por defecto de jpackage.
            }
        }
    }
}

tasks.matching { it.name == "prepareAppResources" }.configureEach { dependsOn(stageSpikeNative) }
tasks.named("compileKotlin") { dependsOn(stageSpikeNative) }
