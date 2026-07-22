package com.watermellonstudios.audio.internal.native

/**
 * iOS actual: no-op.
 *
 * The C++ engine is linked statically into the binary (see WA-2.1/WA-4.1), so
 * there is no dynamic library to load at runtime — unlike Android, where
 * `System.loadLibrary` is required.
 */
internal actual object NativeLibraryLoader {
    actual fun ensureLoaded(): Boolean = true
    actual fun isLibraryLoaded(): Boolean = true
}
