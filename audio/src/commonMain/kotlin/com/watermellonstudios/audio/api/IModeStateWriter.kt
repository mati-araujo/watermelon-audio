package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.mode.AudioMode

/**
 * Interface for writing mode state changes to the native audio engine.
 *
 * This interface abstracts the JNI layer, allowing the ModeTransitionManager
 * to work without direct dependency on the native bridge.
 *
 * All operations return Result to handle potential errors gracefully.
 */
interface IModeStateWriter {

    /**
     * Sets the audio mode in the native engine.
     *
     * The native code handles:
     * - Starting input stream if needed (for INPUT_FX and MIX modes)
     * - Sample rate synchronization between input and output
     * - Engine restart if sample rate changes
     * - Enabling monitoring
     *
     * @param mode The target audio mode
     * @return Result.success if mode was set, or failure with exception
     */
    suspend fun setAudioMode(mode: AudioMode): Result<Unit>

    /**
     * Gets the current audio mode from the native engine.
     *
     * @return The current AudioMode ID as set in native
     */
    fun getAudioMode(): Int

    /**
     * Pauses the audio engine with fade out.
     *
     * @param fadeTimeMs Duration of fade out in milliseconds
     * @return Result.success if paused, or failure with exception
     */
    suspend fun pauseWithFade(fadeTimeMs: Int): Result<Unit>

    /**
     * Resumes the audio engine with fade in.
     *
     * @param fadeTimeMs Duration of fade in in milliseconds
     * @return Result.success if resumed, or failure with exception
     */
    suspend fun resumeWithFade(fadeTimeMs: Int): Result<Unit>

    /**
     * Sets the crossfade position for MIX mode.
     *
     * @param position 0.0 = full oscillator, 1.0 = full input
     * @return Result.success if set, or failure with exception
     */
    suspend fun setCrossfadePosition(position: Float): Result<Unit>

    /**
     * Checks if the engine is currently running.
     *
     * @return true if engine is running
     */
    fun isEngineRunning(): Boolean
}
