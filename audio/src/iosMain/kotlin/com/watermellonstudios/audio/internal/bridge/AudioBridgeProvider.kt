package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.IAudioNativeBridge

/**
 * iOS actual — NOT YET IMPLEMENTED.
 *
 * The real implementation is `IosAudioBridge` (WA-3.2): an [IAudioNativeBridge]
 * over Kotlin/Native cinterop against the `wma_*` C API. It is blocked on the
 * C++ side of the requirement:
 *
 * - WA-2.1 — CMake iOS build producing the static slices
 * - WA-2.4 — `CoreAudioBackend`
 * - WA-2.5 — C API coverage parity with the JNI
 * - WA-3.1 — the cinterop def file
 *
 * Until then the iOS targets exist so that `commonMain` is compiled and
 * published as KMP metadata (gate G1: NoisyPad can convert its `core-domain`
 * against the shared `domain/` types). Anything that actually reaches the audio
 * engine fails loudly rather than silently producing no sound.
 */
actual fun getAudioBridge(): IAudioNativeBridge =
    throw NotImplementedError(
        "watermelon-audio: the iOS audio bridge is not implemented yet (WA-3.2). " +
            "iOS targets currently publish shared Kotlin only — no audio engine."
    )
