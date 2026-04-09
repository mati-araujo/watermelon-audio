import com.android.build.api.dsl.LibraryExtension
import com.watermellonstudios.audio.libs
import com.watermellonstudios.audio.version
import com.watermellonstudios.audio.library
import org.gradle.api.JavaVersion
import org.gradle.api.Plugin
import org.gradle.api.Project
import org.gradle.kotlin.dsl.configure
import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import org.jetbrains.kotlin.gradle.dsl.KotlinMultiplatformExtension

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

            extensions.configure<KotlinMultiplatformExtension> {
                androidTarget {
                    compilerOptions {
                        jvmTarget.set(JvmTarget.JVM_11)
                    }
                    publishLibraryVariants("release")
                }

                sourceSets.apply {
                    commonMain.dependencies {
                        implementation(libs.library("kotlinx-coroutines-core"))
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
