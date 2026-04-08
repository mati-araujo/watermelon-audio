package com.watermellonstudios.audio.callback

/**
 * Interface for external logging integration.
 *
 * Implement this interface in the app module to connect
 * to your logging framework (Crashlytics, Timber, etc.)
 *
 * Example implementation:
 * ```kotlin
 * class CrashlyticsAudioLogger : AudioLogger {
 *     override fun debug(tag: String, message: String, params: Map<String, Any>) {
 *         FirebaseCrashlytics.getInstance().log("[$tag] $message")
 *     }
 *     // ... other methods
 * }
 * ```
 */
interface AudioLogger {
    fun debug(tag: String, message: String, params: Map<String, Any> = emptyMap())
    fun info(tag: String, message: String, params: Map<String, Any> = emptyMap())
    fun warn(tag: String, message: String, params: Map<String, Any> = emptyMap())
    fun error(tag: String, message: String, throwable: Throwable? = null, params: Map<String, Any> = emptyMap())
}

/**
 * Default no-op logger implementation.
 * Used when no logger is provided.
 */
object NoOpAudioLogger : AudioLogger {
    override fun debug(tag: String, message: String, params: Map<String, Any>) {}
    override fun info(tag: String, message: String, params: Map<String, Any>) {}
    override fun warn(tag: String, message: String, params: Map<String, Any>) {}
    override fun error(tag: String, message: String, throwable: Throwable?, params: Map<String, Any>) {}
}
