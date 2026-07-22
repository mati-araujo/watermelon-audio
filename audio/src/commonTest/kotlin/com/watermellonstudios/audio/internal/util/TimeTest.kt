package com.watermellonstudios.audio.internal.util

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/**
 * Covers the multiplatform replacements for `System.currentTimeMillis()`,
 * `System.nanoTime()` and `SimpleDateFormat` (WA-0.2).
 */
class TimeTest {

    @Test
    fun `formats the epoch itself`() {
        assertEquals("1970-01-01 00:00:00 UTC", formatEpochMillisUtc(0L))
    }

    @Test
    fun `formats a known instant`() {
        // 2026-07-21T15:30:45Z
        assertEquals("2026-07-21 15:30:45 UTC", formatEpochMillisUtc(1_784_647_845_000L))
    }

    @Test
    fun `handles leap days`() {
        // 2024-02-29T12:00:00Z — a leap year divisible by 4.
        assertEquals("2024-02-29 12:00:00 UTC", formatEpochMillisUtc(1_709_208_000_000L))
        // 2000-02-29T00:00:00Z — the divisible-by-400 case the century rule exempts.
        assertEquals("2000-02-29 00:00:00 UTC", formatEpochMillisUtc(951_782_400_000L))
    }

    @Test
    fun `handles the day before an epoch year boundary`() {
        assertEquals("1969-12-31 23:59:59 UTC", formatEpochMillisUtc(-1_000L))
        assertEquals("1999-12-31 23:59:59 UTC", formatEpochMillisUtc(946_684_799_000L))
    }

    @Test
    fun `floors sub-second values rather than truncating toward zero`() {
        // -1 ms is still inside 1969-12-31, not 1970-01-01.
        assertEquals("1969-12-31 23:59:59 UTC", formatEpochMillisUtc(-1L))
        assertEquals("1970-01-01 00:00:00 UTC", formatEpochMillisUtc(999L))
    }

    @Test
    fun `zero-pads every fixed-width field`() {
        // 2001-01-02T03:04:05Z — single digits in month, day, hour, minute, second.
        assertEquals("2001-01-02 03:04:05 UTC", formatEpochMillisUtc(978_404_645_000L))
    }

    @Test
    fun `epochMillis returns a plausible wall clock`() {
        // Sanity bound rather than an exact value: after 2020, before 2100.
        val now = epochMillis()
        assertTrue(now > 1_577_836_800_000L, "epochMillis() returned $now, before 2020")
        assertTrue(now < 4_102_444_800_000L, "epochMillis() returned $now, after 2100")
    }

    @Test
    fun `monotonicNanos never goes backwards`() {
        val first = monotonicNanos()
        val second = monotonicNanos()
        assertTrue(second >= first, "monotonicNanos() went backwards: $first then $second")
    }
}
