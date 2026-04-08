# Audio Module ProGuard Rules

# Keep JNI methods
-keepclasseswithmembernames class * {
    native <methods>;
}

# Keep the native bridge class
-keep class com.watermellonstudios.audio.bridge.NativeAudioBridge { *; }

# Keep all public API classes
-keep class com.watermellonstudios.audio.api.** { *; }
-keep class com.watermellonstudios.audio.state.** { *; }
-keep class com.watermellonstudios.audio.effects.** { *; }
-keep class com.watermellonstudios.audio.oscillators.** { *; }
-keep class com.watermellonstudios.audio.modulators.** { *; }
-keep class com.watermellonstudios.audio.scale.** { *; }
