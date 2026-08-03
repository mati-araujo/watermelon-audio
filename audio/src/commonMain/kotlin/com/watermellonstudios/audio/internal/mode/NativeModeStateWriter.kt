package com.watermellonstudios.audio.internal.mode

import com.watermellonstudios.audio.api.IAudioNativeBridge
import com.watermellonstudios.audio.api.IModeStateWriter
import com.watermellonstudios.audio.api.InternalWatermelonApi
import com.watermellonstudios.audio.callback.AudioLogger
import com.watermellonstudios.audio.callback.platformDefaultAudioLogger
import com.watermellonstudios.audio.domain.mode.AudioMode
import com.watermellonstudios.audio.internal.bridge.getAudioBridge

/**
 * [IModeStateWriter] sobre el puente nativo.
 *
 * Ata el manager de transiciones al motor. Vivía en `androidMain` y lo único que lo
 * ataba ahí eran tres cosas, ninguna de fondo: `android.util.Log`, el
 * `AudioNativeBridge` concreto de Android y un `Dispatchers.IO` que envolvía un no-op.
 *
 * El puente ahora entra por [getAudioBridge], que es `expect`/`actual` y tiene una
 * implementación real de iOS sobre cinterop. El [logger] entra por constructor con el
 * default de la plataforma, así que en Android estas líneas siguen saliendo por logcat
 * exactamente como antes.
 */
internal class NativeModeStateWriter @OptIn(InternalWatermelonApi::class) constructor(
    private val bridge: IAudioNativeBridge = getAudioBridge(),
    private val logger: AudioLogger = platformDefaultAudioLogger,
) : IModeStateWriter {

    private companion object {
        const val TAG = "NativeModeStateWriter"
    }

    override suspend fun setAudioMode(mode: AudioMode): Result<Unit> {
        logger.debug(TAG, "setAudioMode: ENTER mode=${mode.id} (${mode.displayName})")
        val beforeMode = bridge.getAudioMode()
        logger.debug(TAG, "setAudioMode: beforeMode=$beforeMode")

        return bridge.setAudioMode(mode.id).also { result ->
            result.onSuccess {
                val afterMode = bridge.getAudioMode()
                logger.debug(TAG, "setAudioMode: afterMode=$afterMode, expected=${mode.id}")
                if (afterMode != mode.id) {
                    logger.error(TAG, "setAudioMode: MODE DID NOT CHANGE! beforeMode=$beforeMode, afterMode=$afterMode, expected=${mode.id}")
                }
            }.onFailure { e ->
                logger.error(TAG, "setAudioMode: EXCEPTION", e)
            }
        }
    }

    override fun getAudioMode(): Int {
        val mode = bridge.getAudioMode()
        logger.debug(TAG, "getAudioMode: mode=$mode")
        return mode
    }

    override suspend fun pauseWithFade(fadeTimeMs: Int): Result<Unit> {
        logger.debug(TAG, "pauseWithFade: fadeTimeMs=$fadeTimeMs")
        return bridge.pauseEngineWithFade(fadeTimeMs).also { result ->
            result.onFailure { e ->
                logger.error(TAG, "pauseWithFade: EXCEPTION", e)
            }
        }
    }

    override suspend fun resumeWithFade(fadeTimeMs: Int): Result<Unit> {
        logger.debug(TAG, "resumeWithFade: fadeTimeMs=$fadeTimeMs")
        return bridge.resumeEngineWithFade(fadeTimeMs).also { result ->
            result.onFailure { e ->
                logger.error(TAG, "resumeWithFade: EXCEPTION", e)
            }
        }
    }

    override fun isEngineRunning(): Boolean {
        // Engine state: 0=Stopped, 1=Starting, 2=Running, 3=Stopping
        val state = bridge.getEngineState()
        val running = state == 2
        logger.debug(TAG, "isEngineRunning: state=$state, running=$running")
        return running
    }
}
