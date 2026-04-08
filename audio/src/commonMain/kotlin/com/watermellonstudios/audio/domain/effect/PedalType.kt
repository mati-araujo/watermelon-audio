package com.watermellonstudios.audio.domain.effect

/**
 * Professional distortion pedal types for the DistortionEffect.
 *
 * Each pedal type corresponds to a famous distortion pedal emulation
 * implemented in the C++ DistortionEffect class.
 *
 * IDs match C++ DistortionVariants namespace in EffectTypes.h
 *
 * @property id Native ID used by the C++ engine
 * @property displayName Human-readable name for UI
 * @property description Short description of the pedal character
 * @property category High-level category (Overdrive, Distortion, Fuzz, Special)
 */
enum class PedalType(
    val id: Int,
    val displayName: String,
    val description: String,
    val category: PedalCategory
) {
    // ========== OVERDRIVE PEDALS ==========

    /** Ibanez TS-808/TS9 style - iconic mid-hump overdrive */
    TUBE_SCREAMER(
        id = 0,
        displayName = "Tube Screamer",
        description = "Mid-hump overdrive, warm and smooth",
        category = PedalCategory.OVERDRIVE
    ),

    /** Boss OD-1/SD-1 style - bright, articulate overdrive */
    BOSS_OVERDRIVE(
        id = 1,
        displayName = "Boss Overdrive",
        description = "Bright, articulate overdrive",
        category = PedalCategory.OVERDRIVE
    ),

    /** Klon Centaur style - transparent, dynamic overdrive */
    KLON(
        id = 2,
        displayName = "Klon",
        description = "Transparent, touch-sensitive overdrive",
        category = PedalCategory.OVERDRIVE
    ),

    /** Fulltone OCD style - amp-like crunch, versatile */
    OCD(
        id = 3,
        displayName = "OCD",
        description = "Amp-like crunch, dynamic response",
        category = PedalCategory.OVERDRIVE
    ),

    // ========== DISTORTION PEDALS ==========

    /** Boss DS-1 style - aggressive, cutting distortion */
    BOSS_DS1(
        id = 4,
        displayName = "Boss DS-1",
        description = "Aggressive, cutting distortion",
        category = PedalCategory.DISTORTION
    ),

    /** ProCo RAT style - gritty, saturated distortion */
    RAT(
        id = 5,
        displayName = "RAT",
        description = "Gritty, saturated distortion with filter",
        category = PedalCategory.DISTORTION
    ),

    /** MXR Distortion+ style - classic mid-focused */
    DIST_PLUS(
        id = 6,
        displayName = "Distortion+",
        description = "Classic mid-focused distortion",
        category = PedalCategory.DISTORTION
    ),

    /** Boss MT-2 Metal Zone style - high-gain, scooped */
    METAL_ZONE(
        id = 7,
        displayName = "Metal Zone",
        description = "High-gain with parametric EQ",
        category = PedalCategory.DISTORTION
    ),

    // ========== FUZZ PEDALS ==========

    /** EHX Big Muff Pi style - massive sustained fuzz */
    BIG_MUFF(
        id = 8,
        displayName = "Big Muff",
        description = "Massive sustained fuzz, creamy",
        category = PedalCategory.FUZZ
    ),

    /** Fuzz Face Germanium - warm, organic fuzz */
    FUZZ_FACE_GERM(
        id = 9,
        displayName = "Fuzz Face Ge",
        description = "Warm organic fuzz, cleans up with volume",
        category = PedalCategory.FUZZ
    ),

    /** Fuzz Face Silicon - bright, aggressive fuzz */
    FUZZ_FACE_SI(
        id = 10,
        displayName = "Fuzz Face Si",
        description = "Bright aggressive fuzz, tight",
        category = PedalCategory.FUZZ
    ),

    /** Octavia style - ring-mod octave-up fuzz */
    OCTAVE_FUZZ(
        id = 11,
        displayName = "Octave Fuzz",
        description = "Ring-mod style octave-up fuzz",
        category = PedalCategory.FUZZ
    ),

    // ========== SPECIAL/EXTREME ==========

    /** Boss HM-2 style - Swedish death metal chainsaw */
    HM2_CHAINSAW(
        id = 12,
        displayName = "HM-2 Chainsaw",
        description = "Swedish death metal chainsaw tone",
        category = PedalCategory.SPECIAL
    ),

    /** Sunn/Acapulco Gold style - crushing doom fuzz */
    DOOM_FUZZ(
        id = 13,
        displayName = "Doom Fuzz",
        description = "Crushing doom fuzz, endless sustain",
        category = PedalCategory.SPECIAL
    ),

    // ========== LEGACY (backward compatibility) ==========

    /** Original soft clip algorithm */
    LEGACY_SOFT_CLIP(
        id = 100,
        displayName = "Soft Clip",
        description = "Legacy soft clip algorithm",
        category = PedalCategory.LEGACY
    ),

    /** Original hard clip algorithm */
    LEGACY_HARD_CLIP(
        id = 101,
        displayName = "Hard Clip",
        description = "Legacy hard clip algorithm",
        category = PedalCategory.LEGACY
    ),

    /** Original tube sim algorithm */
    LEGACY_TUBE_SIM(
        id = 102,
        displayName = "Tube Sim",
        description = "Legacy tube simulation algorithm",
        category = PedalCategory.LEGACY
    ),

    /** Original foldback algorithm */
    LEGACY_FOLDBACK(
        id = 103,
        displayName = "Foldback",
        description = "Legacy foldback algorithm",
        category = PedalCategory.LEGACY
    ),

    /** Original bitcrush algorithm */
    LEGACY_BITCRUSH(
        id = 104,
        displayName = "Bitcrush",
        description = "Legacy bitcrush algorithm",
        category = PedalCategory.LEGACY
    );

    companion object {
        /** Number of professional pedal types (excluding legacy) */
        const val PEDAL_COUNT = 14

        /** Total count including legacy algorithms */
        const val TOTAL_COUNT = 19

        /**
         * Find a PedalType by its native ID.
         * @param id Native ID from C++
         * @return PedalType or null if not found
         */
        fun fromId(id: Int): PedalType? = entries.find { it.id == id }

        /**
         * Get all pedals in a specific category.
         * @param category The category to filter by
         * @return List of pedals in that category
         */
        fun forCategory(category: PedalCategory): List<PedalType> =
            entries.filter { it.category == category }

        /**
         * Get all professional pedals (excluding legacy).
         */
        val professional: List<PedalType>
            get() = entries.filter { it.category != PedalCategory.LEGACY }

        /**
         * Get all overdrive pedals.
         */
        val overdrives: List<PedalType>
            get() = forCategory(PedalCategory.OVERDRIVE)

        /**
         * Get all distortion pedals.
         */
        val distortions: List<PedalType>
            get() = forCategory(PedalCategory.DISTORTION)

        /**
         * Get all fuzz pedals.
         */
        val fuzzes: List<PedalType>
            get() = forCategory(PedalCategory.FUZZ)

        /**
         * Get all special/extreme pedals.
         */
        val specials: List<PedalType>
            get() = forCategory(PedalCategory.SPECIAL)

        /**
         * Get all legacy algorithms.
         */
        val legacy: List<PedalType>
            get() = forCategory(PedalCategory.LEGACY)
    }
}

/**
 * High-level categorization of distortion pedal types.
 */
enum class PedalCategory(val displayName: String) {
    OVERDRIVE("Overdrive"),
    DISTORTION("Distortion"),
    FUZZ("Fuzz"),
    SPECIAL("Special"),
    LEGACY("Legacy")
}

/**
 * Pedal-specific parameter labels for UI.
 *
 * Each pedal type uses PARAM_A, PARAM_B, PARAM_C differently.
 * This data class provides the appropriate labels for each pedal.
 */
data class PedalParameterLabels(
    val paramALabel: String?,
    val paramADescription: String?,
    val paramBLabel: String?,
    val paramBDescription: String?,
    val paramCLabel: String?,
    val paramCDescription: String?
) {
    companion object {
        /**
         * Get parameter labels for a specific pedal type.
         */
        fun forPedal(pedalType: PedalType): PedalParameterLabels {
            return when (pedalType) {
                PedalType.TUBE_SCREAMER -> PedalParameterLabels(
                    paramALabel = "Mid Freq",
                    paramADescription = "Mid-hump frequency (520-920 Hz)",
                    paramBLabel = "Mid Width",
                    paramBDescription = "Mid-hump Q/width",
                    paramCLabel = null,
                    paramCDescription = null
                )

                PedalType.BOSS_OVERDRIVE -> PedalParameterLabels(
                    paramALabel = null,
                    paramADescription = null,
                    paramBLabel = null,
                    paramBDescription = null,
                    paramCLabel = null,
                    paramCDescription = null
                )

                PedalType.KLON -> PedalParameterLabels(
                    paramALabel = "Treble",
                    paramADescription = "Treble boost amount",
                    paramBLabel = "Clean Blend",
                    paramBDescription = "Mix clean signal with distortion",
                    paramCLabel = null,
                    paramCDescription = null
                )

                PedalType.OCD -> PedalParameterLabels(
                    paramALabel = null,
                    paramADescription = null,
                    paramBLabel = null,
                    paramBDescription = null,
                    paramCLabel = null,
                    paramCDescription = null
                )

                PedalType.BOSS_DS1 -> PedalParameterLabels(
                    paramALabel = null,
                    paramADescription = null,
                    paramBLabel = null,
                    paramBDescription = null,
                    paramCLabel = null,
                    paramCDescription = null
                )

                PedalType.RAT -> PedalParameterLabels(
                    paramALabel = "Filter",
                    paramADescription = "LPF cutoff (dark to bright)",
                    paramBLabel = "Turbo",
                    paramBDescription = "LED clipping for tighter response",
                    paramCLabel = null,
                    paramCDescription = null
                )

                PedalType.DIST_PLUS -> PedalParameterLabels(
                    paramALabel = null,
                    paramADescription = null,
                    paramBLabel = null,
                    paramBDescription = null,
                    paramCLabel = null,
                    paramCDescription = null
                )

                PedalType.METAL_ZONE -> PedalParameterLabels(
                    paramALabel = "Low EQ",
                    paramADescription = "Low frequency boost/cut",
                    paramBLabel = "High EQ",
                    paramBDescription = "High frequency boost/cut",
                    paramCLabel = "Mid Freq",
                    paramCDescription = "Parametric mid frequency (200-3000 Hz)"
                )

                PedalType.BIG_MUFF -> PedalParameterLabels(
                    paramALabel = "Sustain",
                    paramADescription = "Amount of sustain/compression",
                    paramBLabel = "Mids",
                    paramBDescription = "Mid scoop depth (less = more scoop)",
                    paramCLabel = null,
                    paramCDescription = null
                )

                PedalType.FUZZ_FACE_GERM, PedalType.FUZZ_FACE_SI -> PedalParameterLabels(
                    paramALabel = "Bias",
                    paramADescription = "Transistor bias point",
                    paramBLabel = "Cleanup",
                    paramBDescription = "Volume knob cleanup response",
                    paramCLabel = null,
                    paramCDescription = null
                )

                PedalType.OCTAVE_FUZZ -> PedalParameterLabels(
                    paramALabel = null,
                    paramADescription = null,
                    paramBLabel = null,
                    paramBDescription = null,
                    paramCLabel = null,
                    paramCDescription = null
                )

                PedalType.HM2_CHAINSAW -> PedalParameterLabels(
                    paramALabel = "Low Boost",
                    paramADescription = "Low frequency boost (0-12 dB)",
                    paramBLabel = "High Boost",
                    paramBDescription = "High frequency boost (0-12 dB)",
                    paramCLabel = null,
                    paramCDescription = null
                )

                PedalType.DOOM_FUZZ -> PedalParameterLabels(
                    paramALabel = null,
                    paramADescription = null,
                    paramBLabel = null,
                    paramBDescription = null,
                    paramCLabel = null,
                    paramCDescription = null
                )

                // Legacy algorithms don't use pedal-specific params
                PedalType.LEGACY_SOFT_CLIP,
                PedalType.LEGACY_HARD_CLIP,
                PedalType.LEGACY_TUBE_SIM,
                PedalType.LEGACY_FOLDBACK,
                PedalType.LEGACY_BITCRUSH -> PedalParameterLabels(
                    paramALabel = null,
                    paramADescription = null,
                    paramBLabel = null,
                    paramBDescription = null,
                    paramCLabel = null,
                    paramCDescription = null
                )
            }
        }
    }
}
