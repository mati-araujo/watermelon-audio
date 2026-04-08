package com.watermellonstudios.audio.domain.state

/**
 * Lifecycle states of the audio engine.
 */
enum class EngineLifecycle {
    /** Engine is stopped, no audio stream active */
    STOPPED,

    /** Engine is starting, initializing audio stream */
    STARTING,

    /** Engine is running, processing audio */
    RUNNING,

    /** Engine is stopping, closing audio stream */
    STOPPING;

    val isActive: Boolean get() = this == RUNNING || this == STARTING
    val isStopped: Boolean get() = this == STOPPED

    companion object {
        fun fromNativeCode(code: Int): EngineLifecycle = when (code) {
            0 -> STOPPED
            1 -> STARTING
            2 -> RUNNING
            3 -> STOPPING
            else -> STOPPED
        }
    }
}
