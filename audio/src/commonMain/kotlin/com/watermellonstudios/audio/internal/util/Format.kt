package com.watermellonstudios.audio.internal.util

import kotlin.math.abs
import kotlin.math.round

/**
 * Multiplatform number formatting (WA-0.2).
 *
 * `String.format` is JVM-only, so it cannot be used from commonMain. These
 * helpers cover the only cases the library needs — fixed-decimal display
 * strings and 16-bit hex — without pulling in a formatting dependency.
 *
 * Display/logging use only: not RT-safe (allocates).
 */

private val POW10 = longArrayOf(
    1L, 10L, 100L, 1_000L, 10_000L, 100_000L,
    1_000_000L, 10_000_000L, 100_000_000L, 1_000_000_000L
)

/**
 * Formats with a fixed number of decimals, half-up rounding — the common cases
 * of `String.format("%.2f", x)`.
 *
 * NaN and infinities render as their [toString] form, matching `String.format`.
 */
internal fun Double.fmt(decimals: Int): String {
    require(decimals in POW10.indices) { "decimals must be in 0..${POW10.size - 1}, was $decimals" }
    if (isNaN() || isInfinite()) return toString()

    val factor = POW10[decimals]
    val scaled = round(abs(this) * factor)
    // Beyond Long range the digits are meaningless anyway — fall back to toString.
    if (scaled > Long.MAX_VALUE.toDouble()) return toString()

    val units = scaled.toLong()
    val whole = units / factor
    val frac = units % factor

    return buildString {
        // Only emit the sign if the rounded value is actually non-zero, so
        // -0.001 at 2 decimals renders "0.00" rather than "-0.00".
        if (this@fmt < 0.0 && units != 0L) append('-')
        append(whole)
        if (decimals > 0) {
            append('.')
            val fracDigits = frac.toString()
            repeat(decimals - fracDigits.length) { append('0') }
            append(fracDigits)
        }
    }
}

/** @see fmt */
internal fun Float.fmt(decimals: Int): String = toDouble().fmt(decimals)

/** Uppercase 4-digit hex of the low 16 bits — the equivalent of `"%04X"`. */
internal fun Int.toHex4(): String =
    (this and 0xFFFF).toString(16).uppercase().padStart(4, '0')
