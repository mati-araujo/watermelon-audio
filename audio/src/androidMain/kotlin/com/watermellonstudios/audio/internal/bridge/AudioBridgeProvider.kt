package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.IAudioNativeBridge

actual fun getAudioBridge(): IAudioNativeBridge = AudioNativeBridge.getInstance()
