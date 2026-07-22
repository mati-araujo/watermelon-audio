package com.watermellonstudios.audio.internal.util

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith

/**
 * Covers the multiplatform replacements for `String.format` (WA-0.2).
 *
 * These render user-visible strings (USB test reports, JNI stats), so the
 * expectations below mirror what `String.format("%.2f", x)` produced on
 * Android before the port.
 */
class FormatTest {

    @Test
    fun `formats fixed decimals`() {
        assertEquals("3.14", 3.14159.fmt(2))
        assertEquals("3.1", 3.14159.fmt(1))
        assertEquals("3", 3.14159.fmt(0))
        assertEquals("0.00", 0.0.fmt(2))
    }

    @Test
    fun `pads fractional digits to the requested width`() {
        assertEquals("1.50", 1.5.fmt(2))
        assertEquals("1.05", 1.05.fmt(2))
        assertEquals("2.000", 2.0.fmt(3))
    }

    @Test
    fun `rounds half away from zero`() {
        assertEquals("1.24", 1.235.fmt(2))
        assertEquals("2.0", 1.95.fmt(1))
        assertEquals("-1.24", (-1.235).fmt(2))
    }

    @Test
    fun `keeps the sign only when the rounded value is non-zero`() {
        assertEquals("-1.50", (-1.5).fmt(2))
        // -0.001 rounds to zero at 2 decimals: "-0.00" would be misleading.
        assertEquals("0.00", (-0.001).fmt(2))
    }

    @Test
    fun `renders non-finite values like String format did`() {
        assertEquals("NaN", Double.NaN.fmt(2))
        assertEquals("Infinity", Double.POSITIVE_INFINITY.fmt(2))
        assertEquals("-Infinity", Double.NEGATIVE_INFINITY.fmt(1))
    }

    @Test
    fun `rejects an unsupported decimal count`() {
        assertFailsWith<IllegalArgumentException> { 1.0.fmt(-1) }
        assertFailsWith<IllegalArgumentException> { 1.0.fmt(20) }
    }

    @Test
    fun `formats floats via the double path`() {
        assertEquals("48.00", 48.0f.fmt(2))
        assertEquals("-12.3", (-12.34f).fmt(1))
    }

    @Test
    fun `renders four-digit uppercase hex for vid pid`() {
        assertEquals("0000", 0.toHex4())
        assertEquals("00FF", 255.toHex4())
        assertEquals("1235", 0x1235.toHex4())
        assertEquals("FFFF", 0xFFFF.toHex4())
    }

    @Test
    fun `masks hex input to sixteen bits`() {
        // Keeps the "%04X" contract on values that overflow a USB VID/PID.
        assertEquals("0001", 0x10001.toHex4())
        assertEquals("FFFF", (-1).toHex4())
    }
}
