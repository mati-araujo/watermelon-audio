package com.watermellonstudios.audio.internal.native

import com.watermellonstudios.audio.callback.platformDefaultAudioLogger

/**
 * Desktop, mínimo (REQ-055 S1): `System.loadLibrary` desde `java.library.path`, como en
 * el arnés JNI de host. Cargar el dylib por ruta absoluta dentro del bundle firmado
 * (hardened runtime) es de la etapa de la espiga; extraer la librería de recursos a un
 * temporal está prohibido (ADR-0009 §4 de NoisyPad).
 */
internal actual object NativeLibraryLoader {
    private const val TAG = "NativeLibraryLoader"
    private const val LIBRARY_NAME = "watermelon_audio"

    @Volatile
    private var isLoaded = false

    actual fun ensureLoaded(): Boolean {
        if (isLoaded) return true
        synchronized(this) {
            if (isLoaded) return true
            return try {
                System.loadLibrary(LIBRARY_NAME)
                isLoaded = true
                true
            } catch (e: UnsatisfiedLinkError) {
                platformDefaultAudioLogger.error(TAG, "Failed to load native library '$LIBRARY_NAME'", e)
                false
            }
        }
    }

    actual fun isLibraryLoaded(): Boolean = isLoaded
}
