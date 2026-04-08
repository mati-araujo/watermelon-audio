package com.watermellonstudios.audio.internal.mode

import android.util.Log
import com.watermellonstudios.audio.api.IModeStateWriter
import com.watermellonstudios.audio.domain.mode.AudioMode
import com.watermellonstudios.audio.internal.bridge.AudioNativeBridge
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Implementation of [IModeStateWriter] using [AudioNativeBridge].
 *
 * This bridges the mode transition manager to the native audio engine.
 */
internal class NativeModeStateWriter : IModeStateWriter {

    companion object {
        private const val TAG = "NativeModeStateWriter"
    }

    private val bridge = AudioNativeBridge.getInstance()

    override suspend fun setAudioMode(mode: AudioMode): Result<Unit> {
        Log.d(TAG, "setAudioMode: ENTER mode=${mode.id} (${mode.displayName})")
        val beforeMode = bridge.getAudioMode()
        Log.d(TAG, "setAudioMode: beforeMode=$beforeMode")

        return bridge.setAudioMode(mode.id).also { result ->
            result.onSuccess {
                val afterMode = bridge.getAudioMode()
                Log.d(TAG, "setAudioMode: afterMode=$afterMode, expected=${mode.id}")
                if (afterMode != mode.id) {
                    Log.e(TAG, "setAudioMode: MODE DID NOT CHANGE! beforeMode=$beforeMode, afterMode=$afterMode, expected=${mode.id}")
                }
            }.onFailure { e ->
                Log.e(TAG, "setAudioMode: EXCEPTION", e)
            }
        }
    }

    override fun getAudioMode(): Int {
        val mode = bridge.getAudioMode()
        Log.d(TAG, "getAudioMode: mode=$mode")
        return mode
    }

    override suspend fun pauseWithFade(fadeTimeMs: Int): Result<Unit> {
        Log.d(TAG, "pauseWithFade: fadeTimeMs=$fadeTimeMs")
        return bridge.pauseEngineWithFade(fadeTimeMs).also { result ->
            result.onFailure { e ->
                Log.e(TAG, "pauseWithFade: EXCEPTION", e)
            }
        }
    }

    override suspend fun resumeWithFade(fadeTimeMs: Int): Result<Unit> {
        Log.d(TAG, "resumeWithFade: fadeTimeMs=$fadeTimeMs")
        return bridge.resumeEngineWithFade(fadeTimeMs).also { result ->
            result.onFailure { e ->
                Log.e(TAG, "resumeWithFade: EXCEPTION", e)
            }
        }
    }

    override suspend fun setCrossfadePosition(position: Float): Result<Unit> = withContext(Dispatchers.IO) {
        Log.d(TAG, "setCrossfadePosition: position=$position")
        // Note: Crossfade is handled at the Kotlin level for now.
        // Native implementation would be added when mixer routing is complete.
        Result.success(Unit)
    }

    override fun isEngineRunning(): Boolean {
        // Engine state: 0=Stopped, 1=Starting, 2=Running, 3=Stopping
        val state = bridge.getEngineState()
        val running = state == 2
        Log.d(TAG, "isEngineRunning: state=$state, running=$running")
        return running
    }
}
