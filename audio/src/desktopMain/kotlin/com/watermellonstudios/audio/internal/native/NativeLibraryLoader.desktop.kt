package com.watermellonstudios.audio.internal.native

import com.watermellonstudios.audio.callback.platformDefaultAudioLogger

/**
 * Desktop (REQ-055 S1 + S3): si la app fija [LIBRARY_PATH_PROPERTY] con la ruta ABSOLUTA
 * del dylib dentro de su bundle firmado, se carga con `System.load` desde ahí (ADR-0009
 * §4 de NoisyPad: hardened runtime y library validation sólo aceptan código del bundle).
 * Sin la propiedad, `System.loadLibrary` desde `java.library.path`, como en el arnés JNI
 * de host. Extraer la librería de recursos a un temporal está prohibido.
 */
internal actual object NativeLibraryLoader {
    private const val TAG = "NativeLibraryLoader"
    private const val LIBRARY_NAME = "watermelon_audio"

    /** Ruta absoluta del dylib; la fija la app antes de tocar el bridge. */
    const val LIBRARY_PATH_PROPERTY = "watermelon.audio.library"

    @Volatile
    private var isLoaded = false

    actual fun ensureLoaded(): Boolean {
        if (isLoaded) return true
        synchronized(this) {
            if (isLoaded) return true
            return try {
                val path = System.getProperty(LIBRARY_PATH_PROPERTY)
                if (path.isNullOrEmpty()) System.loadLibrary(LIBRARY_NAME) else System.load(path)
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
