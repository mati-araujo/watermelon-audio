package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.IAudioNativeBridge
import com.watermellonstudios.audio.api.InternalWatermelonApi

@InternalWatermelonApi
actual fun getAudioBridge(): IAudioNativeBridge = AudioNativeBridge.getInstance()
