package com.watermellonstudios.audio.domain.mode

/**
 * Thrown when attempting to start a transition while one is already in progress.
 */
class TransitionInProgressException : Exception("A mode transition is already in progress")

/**
 * Thrown when a mode transition fails at the native layer.
 *
 * @param message Description of the failure
 */
class ModeTransitionFailedException(message: String) : Exception(message)

/**
 * Thrown when a required permission (e.g., RECORD_AUDIO) is not granted.
 *
 * @param permission The permission that was denied
 */
class PermissionDeniedException(
    val permission: String = "RECORD_AUDIO"
) : Exception("Required permission not granted: $permission")

/**
 * Thrown when waiting for native mode change confirmation times out.
 *
 * @param timeoutMs The timeout duration that was exceeded
 * @param targetMode The mode that was being transitioned to
 */
class ModeChangeTimeoutException(
    val timeoutMs: Long,
    val targetMode: AudioMode
) : Exception("Mode change to ${targetMode.displayName} timed out after ${timeoutMs}ms")
