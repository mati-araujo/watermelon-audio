package com.watermellonstudios.audio.api.callbacks

/**
 * Interface for crash reporting integration.
 *
 * This abstraction allows the audio module to log breadcrumbs and set
 * custom keys without depending on Firebase or any specific crash
 * reporting implementation.
 *
 * Implementations should be provided by the app module via dependency injection.
 *
 * @see NoOpCrashReporter for a default no-op implementation
 */
interface ICrashReporter {

    /**
     * Log a breadcrumb message for crash reports.
     *
     * Breadcrumbs help understand the sequence of events leading to a crash.
     *
     * @param message The message to log
     */
    fun log(message: String)

    /**
     * Set a custom key-value pair for crash context.
     *
     * Custom keys help categorize and filter crash reports.
     *
     * @param key The key name
     * @param value The value (will be converted to string if needed)
     */
    fun setCustomKey(key: String, value: String)

    /**
     * Set a custom key with an integer value.
     */
    fun setCustomKey(key: String, value: Int)

    /**
     * Set a custom key with a boolean value.
     */
    fun setCustomKey(key: String, value: Boolean)

    /**
     * Set a custom key with a float value.
     */
    fun setCustomKey(key: String, value: Float)

    /**
     * Set a custom key with a double value.
     */
    fun setCustomKey(key: String, value: Double)

    /**
     * Set a custom key with a long value.
     */
    fun setCustomKey(key: String, value: Long)
}

/**
 * No-op implementation of ICrashReporter.
 *
 * Use this when crash reporting is not needed or not available.
 */
object NoOpCrashReporter : ICrashReporter {
    override fun log(message: String) = Unit
    override fun setCustomKey(key: String, value: String) = Unit
    override fun setCustomKey(key: String, value: Int) = Unit
    override fun setCustomKey(key: String, value: Boolean) = Unit
    override fun setCustomKey(key: String, value: Float) = Unit
    override fun setCustomKey(key: String, value: Double) = Unit
    override fun setCustomKey(key: String, value: Long) = Unit
}
