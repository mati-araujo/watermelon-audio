# Consumer ProGuard Rules for Audio Module
# These rules are applied to consumers of this library

# Keep JNI methods
-keepclasseswithmembernames class * {
    native <methods>;
}

# Keep the native bridge (JNI method signatures required at runtime)
-keepclasseswithmembernames class com.watermellonstudios.audio.internal.bridge.AudioNativeBridge {
    native <methods>;
}

# Keep public API classes
-keep class com.watermellonstudios.audio.api.** { *; }
-keep class com.watermellonstudios.audio.domain.** { *; }
