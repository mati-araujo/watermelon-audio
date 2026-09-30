import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import java.io.ByteArrayOutputStream
import java.security.MessageDigest
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
// (`Res.readBytes("files/wma-fixture.sf2")`).
//
// D10 (humano): el .sf2 es Python puro y su receta falla el build si falla. El .sf3 necesita un
// encoder Vorbis (ffmpeg), que los runners del CI NO traen: sin encoder la receta sale con exit 3 y
// su mensaje, el build lo muestra como WARNING y empaqueta SOLO el .sf2. No se esconde: la app dice
// "fixture .sf3 no empaquetado" y emite `panel=sf3 step=fixture ok=false`, que smoke-device.sh da
// como FAIL. Cualquier OTRO fallo de la receta del .sf3 (ffmpeg que no codifica, salida rota) sigue
// rompiendo el build. No se versiona un binario para esquivar nada de esto.
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
        val script = recipe.get().asFile.absolutePath
        execOps.exec {
            commandLine("python3", script, "--out", files.absolutePath, "--only", "sf2")
        }
        val stderr = ByteArrayOutputStream()
        val sf3 = execOps.exec {
            commandLine("python3", script, "--out", files.absolutePath, "--only", "sf3")
            errorOutput = stderr
            isIgnoreExitValue = true
        }
        when (sf3.exitValue) {
            0 -> Unit
            NO_VORBIS_ENCODER -> logger.warn(
                "WARNING: fixture .sf3 NO empaquetado (MINI-038, D10) — el harness sale solo con el " +
                    ".sf2 y su panel sf3 va a dar FAIL.\n" + stderr.toString().trimEnd(),
            )
            else -> throw GradleException(
                "gen-harness-soundfonts.py --only sf3 fallo (exit ${sf3.exitValue}):\n" + stderr.toString().trimEnd(),
            )
        }

        // El MANIFIESTO de lo que este build empaqueto, y lo que la app consulta antes de usar un
        // fixture. Existe por algo medido: la copia de recursos de Compose a los assets de Android
        // NO borra un archivo que desaparecio de la entrada, asi que un build incremental sin
        // encoder seguia empaquetando el .sf3 de un build anterior. El manifiesto se reescribe
        // siempre, y un archivo que cambia si se propaga: la app lo lee y un .sf3 que no figura es
        // "no empaquetado" aunque sus bytes hayan quedado en el APK.
        val manifest = files.listFiles().orEmpty()
            .filter { it.isFile && it.name != MANIFEST }
            .sortedBy { it.name }
            .joinToString("") { f ->
                val sha = MessageDigest.getInstance("SHA-256").digest(f.readBytes())
                    .joinToString("") { b -> "%02x".format(b) }
                "${f.name} ${f.length()} $sha\n"
            }
        files.resolve(MANIFEST).writeText(manifest)
    }

    private companion object {
        /** El exit de la receta cuando no hay encoder Vorbis. Cualquier otro fallo rompe el build. */
        const val NO_VORBIS_ENCODER = 3
        const val MANIFEST = "fixtures-manifest.txt"
    }
}

val generateHarnessSoundFonts = tasks.register<GenerateHarnessSoundFonts>("generateHarnessSoundFonts") {
    recipe.set(rootProject.layout.projectDirectory.file("scripts/gen-harness-soundfonts.py"))
    resourcesDir.set(layout.buildDirectory.dir("generated/harness-soundfonts"))
    // La salida del .sf3 depende del encoder Vorbis del ENTORNO (ffmpeg, libvorbis o el nativo),
    // que no es un input declarable. Sin esto la task quedaba UP-TO-DATE aunque se desinstalara el
    // encoder, y "sin encoder falla" sólo valía en un build limpio. Correrla siempre cuesta ~1 s y
    // re-imprime el sha; las tasks de abajo comparan CONTENIDO, así que bytes iguales no las rehacen.
    doNotTrackState("la salida depende del encoder Vorbis del entorno, que no es un input declarable")
}

compose.resources {
    packageOfResClass = "com.watermellonstudios.audio.harness.resources"
    customDirectory(
        sourceSetName = "commonMain",
        directoryProvider = generateHarnessSoundFonts.flatMap { it.resourcesDir },
    )
}
