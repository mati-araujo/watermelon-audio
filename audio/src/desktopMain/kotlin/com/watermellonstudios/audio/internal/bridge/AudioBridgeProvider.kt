package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.IAudioNativeBridge
import com.watermellonstudios.audio.api.InternalWatermelonApi

/** Desktop: el mismo [AudioNativeBridge] que Android (jvmShared, REQ-055 S1). */
@InternalWatermelonApi
actual fun getAudioBridge(): IAudioNativeBridge = AudioNativeBridge.getInstance()
