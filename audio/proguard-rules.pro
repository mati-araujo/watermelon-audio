# Watermelon Audio — ProGuard Rules

# Keep JNI methods
-keepclasseswithmembernames class * {
    native <methods>;
}

# Keep the native bridge (unified JNI bindings)
-keep class com.watermellonstudios.audio.internal.bridge.AudioNativeBridge { *; }

# Keep public API classes
-keep class com.watermellonstudios.audio.api.** { *; }

# Keep domain models (used by consumers for serialization, reflection, etc.)
-keep class com.watermellonstudios.audio.domain.** { *; }

# Keep callback interfaces (dependency inversion — implemented by consumers)
-keep class com.watermellonstudios.audio.callback.** { *; }
