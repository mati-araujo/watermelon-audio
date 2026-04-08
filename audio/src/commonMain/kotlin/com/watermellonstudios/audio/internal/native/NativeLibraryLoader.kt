package com.watermellonstudios.audio.internal.native

/**
 * Platform-specific native library loader.
 *
 * - Android: System.loadLibrary("noisypad")
 * - iOS (future): no-op (framework linked at build time)
 */
internal expect object NativeLibraryLoader {
    fun ensureLoaded(): Boolean
    fun isLibraryLoaded(): Boolean
}
