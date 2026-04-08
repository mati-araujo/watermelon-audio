package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.IAudioNativeBridge

/**
 * Platform-specific factory for obtaining the [IAudioNativeBridge] singleton.
 *
 * - Android: returns [AudioNativeBridge.getInstance()]
 * - iOS (future): returns cinterop-based bridge
 */
expect fun getAudioBridge(): IAudioNativeBridge
