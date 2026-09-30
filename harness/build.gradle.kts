import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import javax.inject.Inject

/**
 * :harness — WA-5.5. App de prueba multiplataforma que corre la libreria en
 * Android e iOS.
 *
 * NO SE PUBLICA, y eso es estructural, no una convencion:
 *
 *   1. No aplica `maven-publish`. Sin ese plugin no hay publicaciones que
 *      publicar, ni siquiera con un publish de raiz.
 *   2. Los dos workflows publican con `./gradlew :audio:publishAll...`,
 *      path-qualified — no alcanzan a este modulo ni queriendo.
 *   3. La dependencia va en una sola direccion: :harness -> :audio.
 *   4. `scripts/check-no-ui-in-library.sh` (noveno comando del gate) afirma lo
 *      anterior y, sobre todo, que el classpath resuelto de :audio no tiene una
 *      sola coordenada de Compose. Ese es el check que importa; la direccion de
 *      la arista hoy la sostiene el grafo de tareas y no el script — esta
 *      explicado ahi.
 *   5. Compose se aplica ACA y solo aca. `KmpNativeConventionPlugin` —de donde
 *      :audio toma su configuracion— no lo nombra, asi que :audio no puede
 *      heredarlo.
 *
 * La capa 4 es la que agarra el modo de falla realista. Nadie va a publicar el
 * harness por accidente; lo que pasa de verdad es que alguien le agrega una
 * dependencia de Compose a :audio "para un helper de preview", y las capas 1-3
 * no ven eso.
 */
plugins {
    // Sin version: AGP y KGP ya estan en el classpath del build via
    // `includeBuild("build-logic")`, y Gradle rechaza que se les vuelva a
    // declarar una ("already on the classpath with an unknown version").
    // El catalogo igual los declara, para que la version viva en un solo lugar.
    id("com.android.application")
    id("org.jetbrains.kotlin.multiplatform")

    // Estos dos SI llevan version: no estan en el classpath de build-logic, que
    // es justamente lo que mantiene a Compose fuera del alcance de :audio.
    alias(libs.plugins.compose.multiplatform)
    alias(libs.plugins.compose.compiler)

    // Deliberadamente SIN `maven-publish`. Ver el bloque de arriba.
}

kotlin {
    androidTarget {
        compilerOptions {
            jvmTarget.set(JvmTarget.JVM_11)
        }
    }

    // Un framework por target de iOS. Ojo: este NO es el XCFramework de WA-4.1.
    //
    // Las dos vias de consumo de la libreria son alternativas, no
    // complementarias — usar las dos en la misma app duplicaria el motor (ver
    // el comentario en KmpNativeConventionPlugin). El harness consume :audio
    // como dependencia KMP (klib), y el klib ya trae libwatermelon_audio.a
    // adentro por `staticLibraries` del .def. Asi que lo que Xcode embebe es
    // ESTE framework, no el XCFramework, que sigue siendo la salida para un
    // consumidor Swift que no es KMP.
    listOf(
        iosArm64(),
        iosSimulatorArm64(),
    ).forEach { iosTarget ->
        iosTarget.binaries.framework {
            baseName = "HarnessKit"

            // Estatico por el mismo motivo que el framework de :audio: el motor
            // C++ ya viaja como archivo estatico adentro del klib, asi que uno
            // dinamico agregaria un dylib para embeber y firmar, mas un salto de
            // dyld en el arranque, sin ganar nada.
            isStatic = true
        }
    }

    sourceSets {
        commonMain.dependencies {
            // La direccion de la dependencia, y la unica que existe.
            implementation(project(":audio"))

            implementation(compose.runtime)
            implementation(compose.foundation)
            implementation(compose.material3)
            // MINI-038: los fixtures SoundFont viajan como recursos de Compose (assets en
            // Android, bundle en iOS) desde un solo directorio generado. Ver abajo.
            implementation(compose.components.resources)
            implementation(libs.kotlinx.coroutines.core)
        }

        // MINI-038: el formato HARNESS-SMOKE, el plan y los veredictos del SoundFont. Corren en
        // la JVM (`:harness:testDebugUnitTest`, que build-harness.sh ejecuta) sin tocar el motor.
        commonTest.dependencies {
            implementation(kotlin("test"))
            implementation(libs.kotlinx.coroutines.test)
        }

        androidMain.dependencies {
            implementation(libs.androidx.activity.compose)
            implementation(libs.kotlinx.coroutines.android)
        }
    }
}

android {
    namespace = "com.watermellonstudios.audio.harness"

    // Propio, y mas alto que el de :audio a proposito — ver la nota en el
    // catalogo. El harness no arrastra la config de lo que se publica.
    compileSdk = libs.versions.harnessCompileSdk.get().toInt()

    defaultConfig {
        applicationId = "com.watermellonstudios.audio.harness"
        minSdk = libs.versions.minSdk.get().toInt()
        targetSdk = libs.versions.harnessCompileSdk.get().toInt()
        versionCode = 1
        versionName = "1.0"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
        }
    }
}

// ---------------------------------------------------------------------------
// MINI-038 — los fixtures .sf2/.sf3 se GENERAN en el build, no se versionan.
//
// `scripts/gen-harness-soundfonts.py` es la receta (y su cabecera explica la forma de cada
// archivo). Su salida entra como directorio de recursos de Compose de commonMain, asi que el MISMO
// par de archivos llega a los assets del APK y al bundle de la app de iOS
// (`Res.readBytes("files/wma-fixture.sf2")`). Sin encoder Vorbis la receta falla con su mensaje y
// el build con ella: no se versiona un binario para esquivarlo.
// ---------------------------------------------------------------------------
abstract class GenerateHarnessSoundFonts @Inject constructor(
    private val execOps: ExecOperations,
) : DefaultTask() {

    @get:InputFile
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val recipe: RegularFileProperty

    /** Raiz de recursos de Compose: la receta escribe en `<raiz>/files/`. */
    @get:OutputDirectory
    abstract val resourcesDir: DirectoryProperty

    @TaskAction
    fun generate() {
        val files = resourcesDir.get().dir("files").asFile
        files.deleteRecursively()
        files.mkdirs()
        execOps.exec {
            commandLine("python3", recipe.get().asFile.absolutePath, "--out", files.absolutePath)
        }
    }
}

val generateHarnessSoundFonts = tasks.register<GenerateHarnessSoundFonts>("generateHarnessSoundFonts") {
    recipe.set(rootProject.layout.projectDirectory.file("scripts/gen-harness-soundfonts.py"))
    resourcesDir.set(layout.buildDirectory.dir("generated/harness-soundfonts"))
}

compose.resources {
    packageOfResClass = "com.watermellonstudios.audio.harness.resources"
    customDirectory(
        sourceSetName = "commonMain",
        directoryProvider = generateHarnessSoundFonts.flatMap { it.resourcesDir },
    )
}
