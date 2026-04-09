package com.watermellonstudios.audio.internal.native

import android.util.Log

/**
 * Singleton responsable de cargar la librería nativa.
 *
 * Garantiza que la librería se carga una sola vez, incluso si
 * múltiples bridges intentan cargarla.
 *
 * ## Usage
 *
 * ```kotlin
 * // Ensure library is loaded before JNI calls
 * NativeLibraryLoader.ensureLoaded()
 *
 * // Check if loaded
 * if (NativeLibraryLoader.isLibraryLoaded()) {
 *     // Safe to make JNI calls
 * }
 * ```
 *
 * ## Thread Safety
 *
 * All operations are thread-safe via double-checked locking pattern.
 */
internal actual object NativeLibraryLoader {
    private const val TAG = "NativeLibraryLoader"
    private const val LIBRARY_NAME = "watermelon_audio"

    @Volatile
    private var isLoaded = false

    /**
     * Carga la librería nativa si no está cargada.
     * Thread-safe via double-checked locking.
     *
     * @return true si la librería está disponible
     */
    actual fun ensureLoaded(): Boolean {
        if (isLoaded) return true

        synchronized(this) {
            if (isLoaded) return true

            return try {
                System.loadLibrary(LIBRARY_NAME)
                isLoaded = true
                Log.d(TAG, "Native library '$LIBRARY_NAME' loaded successfully")
                true
            } catch (e: UnsatisfiedLinkError) {
                Log.e(TAG, "Failed to load native library '$LIBRARY_NAME'", e)
                false
            }
        }
    }

    /**
     * Verifica si la librería está cargada.
     */
    actual fun isLibraryLoaded(): Boolean = isLoaded
}
