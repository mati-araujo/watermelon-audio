package com.watermellonstudios.audio.internal.util

import kotlin.time.Clock
import kotlin.time.TimeSource

/**
 * Multiplatform time sources (WA-0.2).
 *
 * `System.currentTimeMillis()` / `System.nanoTime()` are JVM-only. These are
 * the single point of change if the library ever needs a different clock
 * (injected time for tests, kotlinx-datetime, etc.).
 *
 * Not RT-safe — never call from the audio thread.
 */

/** Wall-clock milliseconds since the Unix epoch — replaces `System.currentTimeMillis()`. */
internal fun epochMillis(): Long = Clock.System.now().toEpochMilliseconds()

private val processStart = TimeSource.Monotonic.markNow()

/**
 * Monotonic nanoseconds since library load — replaces `System.nanoTime()`.
 *
 * Like `nanoTime()`, the absolute value is meaningless; only differences are.
 * Unaffected by wall-clock adjustments, so it is the correct source for
 * elapsed-time and change-detection timestamps.
 */
internal fun monotonicNanos(): Long = processStart.elapsedNow().inWholeNanoseconds

/**
 * Renders epoch milliseconds as `yyyy-MM-dd HH:mm:ss UTC`.
 *
 * Replaces `java.text.SimpleDateFormat`, which is JVM-only. Note this is **UTC**,
 * whereas SimpleDateFormat used the device's local zone — the suffix makes the
 * difference explicit in the rendered report. Proleptic Gregorian, valid for any
 * epoch value (including pre-1970 negatives).
 */
internal fun formatEpochMillisUtc(millis: Long): String {
    val totalSeconds = millis.floorDiv(1000L)
    val secondOfDay = totalSeconds.mod(86_400L)
    val epochDay = totalSeconds.floorDiv(86_400L)

    // civil_from_days (Howard Hinnant): shift the era to start 0000-03-01 so
    // the leap day lands at the end of the year and month lengths stay regular.
    val z = epochDay + 719_468L
    val era = z.floorDiv(146_097L)
    val dayOfEra = z - era * 146_097L                                        // [0, 146096]
    val yearOfEra = (dayOfEra - dayOfEra / 1_460 + dayOfEra / 36_524 - dayOfEra / 146_096) / 365
    val dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100)
    val monthShifted = (5 * dayOfYear + 2) / 153                             // [0, 11], 0 = March
    val day = dayOfYear - (153 * monthShifted + 2) / 5 + 1
    val month = if (monthShifted < 10) monthShifted + 3 else monthShifted - 9
    val year = yearOfEra + era * 400 + if (month <= 2) 1 else 0

    val hour = secondOfDay / 3_600
    val minute = (secondOfDay / 60) % 60
    val second = secondOfDay % 60

    return "$year-${month.pad2()}-${day.pad2()} ${hour.pad2()}:${minute.pad2()}:${second.pad2()} UTC"
}

private fun Long.pad2(): String = toString().padStart(2, '0')
