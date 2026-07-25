import com.android.build.api.dsl.LibraryExtension
import com.watermellonstudios.audio.libs
import com.watermellonstudios.audio.version
import com.watermellonstudios.audio.library
import org.gradle.api.JavaVersion
import org.gradle.api.Plugin
import org.gradle.api.Project
import org.gradle.api.tasks.Exec
import org.gradle.kotlin.dsl.configure
import org.gradle.kotlin.dsl.register
import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import org.jetbrains.kotlin.gradle.dsl.KotlinMultiplatformExtension
import org.jetbrains.kotlin.gradle.plugin.mpp.apple.XCFramework

/**
 * Convention plugin for KMP library modules with native C++ code (CMake).
 *
 * Watermelon Audio — standalone library build.
 */
class KmpNativeConventionPlugin : Plugin<Project> {
    override fun apply(target: Project) {
        with(target) {
            pluginManager.apply("org.jetbrains.kotlin.multiplatform")
            pluginManager.apply("com.android.library")

            extensions.configure<LibraryExtension> {
                compileSdk = libs.version("compileSdk").toInt()
                ndkVersion = libs.version("ndk")

                defaultConfig {
                    minSdk = libs.version("minSdk").toInt()
                    consumerProguardFiles("consumer-rules.pro")

                    ndk {
                        abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64", "x86")
                    }

                    externalNativeBuild {
                        cmake {
                            cppFlags += "-std=c++20"
                            arguments += listOf(
                                "-DANDROID_STL=c++_shared",
                                "-DANDROID_PLATFORM=android-${libs.version("minSdk")}"
                            )
                        }
                    }
                }

                externalNativeBuild {
                    cmake {
                        path = file("src/main/cpp/CMakeLists.txt")
                        version = libs.version("cmake")
                    }
                }

                buildFeatures {
                    prefab = true
                    buildConfig = true
                }

                compileOptions {
                    sourceCompatibility = JavaVersion.VERSION_11
                    targetCompatibility = JavaVersion.VERSION_11
                }
            }

            // WA-3.1 — el .a contra el que linkea cinterop. El build C++ de iOS vive
            // en su propio CMakeLists (src/main/cpp/ios/), fuera del que maneja AGP,
            // asi que Gradle lo invoca via el script en vez de con externalNativeBuild.
            //
            // Solo corre en macOS: necesita el SDK de iOS y Xcode. En Linux la task
            // se saltea con un mensaje claro en vez de fallar con un error de xcrun.
            val isMac = System.getProperty("os.name").startsWith("Mac", ignoreCase = true)

            val buildIosNativeLib = tasks.register<Exec>("buildIosNativeLib") {
                group = "build"
                description =
                    "Compila libwatermelon_audio.a para device y simulador (scripts/build-ios.sh)."
                workingDir = rootProject.projectDir
                commandLine("bash", "scripts/build-ios.sh")

                // Sin inputs/outputs declarados la task correria en cada build; el .a
                // tarda minutos. Se excluyen los arboles de build para que la salida
                // no sea tambien entrada.
                inputs.files(
                    fileTree(file("src/main/cpp")) {
                        exclude("ios/build/**", "**/.deps/**", "**/build/**", "**/build-san/**")
                    }
                )
                outputs.files(
                    file("src/main/cpp/ios/build/iphoneos/libwatermelon_audio.a"),
                    file("src/main/cpp/ios/build/iphonesimulator/libwatermelon_audio.a"),
                )

                onlyIf {
                    if (!isMac) logger.lifecycle("buildIosNativeLib: se saltea (requiere macOS)")
                    isMac
                }
            }

            // cinterop no puede correr sin el .a. El nombre de la task lo genera KGP
            // a partir del nombre del cinterop y del target
            // (cinteropWatermelonAudioIosArm64, ...Simulator...), de ahi el matching.
            tasks.matching { it.name.startsWith("cinteropWatermelonAudio") }.configureEach {
                dependsOn(buildIosNativeLib)
            }

            extensions.configure<KotlinMultiplatformExtension> {
                compilerOptions {
                    freeCompilerArgs.add("-Xexpect-actual-classes")
                }

                androidTarget {
                    compilerOptions {
                        jvmTarget.set(JvmTarget.JVM_11)
                    }
                    publishLibraryVariants("release")
                }

                // WA-0.2 — targets iOS. WA-3.1 les enchufa el motor C++ via cinterop.
                //
                // El .a se construye fuera de Gradle (scripts/build-ios.sh, ver
                // buildIosNativeLib abajo): el build iOS vive en su propio
                // CMakeLists, separado del que maneja AGP, que es Android-specific
                // de punta a punta.
                //
                // Un slice por target, y no son intercambiables: el .a de iphoneos
                // no linkea contra el sysroot del simulador.
                val iosSlices = mapOf(
                    iosArm64() to "iphoneos",
                    iosSimulatorArm64() to "iphonesimulator",
                )

                // WA-4.1 — XCFramework. El nombre define la task: KGP genera
                // `assembleWatermelonXCFramework` (mas las variantes Debug/Release)
                // a partir de XCFramework("Watermelon").
                //
                // Para que sirve, si D5 dice que NoisyPad consume el klib: es la via
                // de salida para un consumidor iOS que NO es KMP — un proyecto Xcode
                // que quiere `import WatermelonAudio` desde Swift. Las dos formas de
                // consumo son alternativas, no complementarias: usar las dos en la
                // misma app duplicaria el motor.
                val xcf = XCFramework("Watermelon")

                iosSlices.forEach { (iosTarget, sdk) ->
                    iosTarget.compilations.getByName("main").cinterops.create("watermelonAudio") {
                        definitionFile.set(file("src/nativeInterop/cinterop/watermelon_audio.def"))
                        includeDirs(file("src/main/cpp/api"))
                        // -libraryPath va acá y no en el .def porque es lo unico que
                        // cambia entre slices.
                        extraOpts(
                            "-libraryPath",
                            file("src/main/cpp/ios/build/$sdk").absolutePath,
                        )
                    }

                    iosTarget.binaries.framework {
                        // Tiene que coincidir con el nombre del XCFramework: KGP no
                        // soporta renombrar el framework interno y avisa que el
                        // resultado puede no ser consumible. Es tambien el nombre del
                        // modulo Swift: `import Watermelon`.
                        baseName = "Watermelon"

                        // Estatico a proposito. El motor C++ ya viaja como archivo
                        // estatico dentro del klib (staticLibraries en el .def), asi
                        // que un framework dinamico agregaria un dylib que la app
                        // tiene que embeber y firmar, mas un salto de dyld en el
                        // arranque, sin ganar nada a cambio.
                        isStatic = true

                        xcf.add(this)
                    }
                }

                // Linkear un framework de iOS necesita Xcode. En Linux la task no
                // puede correr, y saltearla con un mensaje es mejor que un error de
                // linker a mitad del pipeline. Las tasks de link son las que hay que
                // guardar: declarar el binario es inocuo, ejecutarlo no.
                tasks.matching {
                    it.name.startsWith("link") && it.name.contains("Framework")
                }.configureEach {
                    onlyIf {
                        if (!isMac) logger.lifecycle("${name}: se saltea (requiere macOS)")
                        isMac
                    }
                }

                sourceSets.apply {
                    commonMain.dependencies {
                        implementation(libs.library("kotlinx-coroutines-core"))
                    }
                    commonTest.dependencies {
                        implementation(kotlin("test"))
                        implementation(libs.library("kotlinx-coroutines-test"))
                    }
                    androidMain.dependencies {
                        implementation(libs.library("oboe"))
                        implementation(libs.library("kotlinx-coroutines-android"))
                        implementation(libs.library("androidx-datastore-preferences"))
                        implementation(libs.library("androidx-lifecycle-runtime-ktx"))
                        implementation(libs.library("androidx-core-ktx"))
                    }
                }
            }
        }
    }
}
