package com.watermellonstudios.audio.domain.scale

/**
 * Musical scale modes for frequency quantization.
 *
 * @property label Display name
 * @property intervals Semitone intervals from root (empty for FREE mode)
 */
enum class ScaleMode(
    val label: String,
    val intervals: List<Int> = emptyList()
) {
    /** No quantization - continuous frequency */
    FREE("Free"),

    /** Chromatic scale - all 12 semitones */
    CHROMATIC("Chromatic", listOf(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11)),

    /** Major scale (Ionian) */
    MAJOR("Major", listOf(0, 2, 4, 5, 7, 9, 11)),

    /** Natural minor scale (Aeolian) */
    MINOR("Minor", listOf(0, 2, 3, 5, 7, 8, 10)),

    /** Major pentatonic */
    PENTATONIC_MAJOR("Pentatonic Major", listOf(0, 2, 4, 7, 9)),

    /** Minor pentatonic */
    PENTATONIC_MINOR("Pentatonic Minor", listOf(0, 3, 5, 7, 10)),

    /** Blues scale */
    BLUES("Blues", listOf(0, 3, 5, 6, 7, 10)),

    /** Dorian mode */
    DORIAN("Dorian", listOf(0, 2, 3, 5, 7, 9, 10)),

    /** Mixolydian mode */
    MIXOLYDIAN("Mixolydian", listOf(0, 2, 4, 5, 7, 9, 10)),

    /** Harmonic minor */
    HARMONIC_MINOR("Harmonic Minor", listOf(0, 2, 3, 5, 7, 8, 11)),

    /** Whole tone scale */
    WHOLE_TONE("Whole Tone", listOf(0, 2, 4, 6, 8, 10));

    val isQuantized: Boolean get() = this != FREE
}
