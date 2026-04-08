package com.watermellonstudios.audio.domain.effect

/**
 * Professional pedal presets for the DistortionEffect.
 *
 * Each preset represents a carefully tuned configuration that emulates
 * the sound character of famous distortion pedals.
 *
 * Presets are organized by category (Overdrive, Distortion, Fuzz, Special)
 * and include both factory presets and "signature" variations.
 */
object PedalPresets {

    // ========== OVERDRIVE PRESETS ==========

    /** TS-808 Classic - The iconic mid-hump overdrive sound */
    val TUBE_SCREAMER_CLASSIC = PedalPreset(
        id = "ts808-classic",
        name = "TS-808 Classic",
        displayName = "Tube Screamer Classic",
        pedalType = PedalType.TUBE_SCREAMER,
        description = "The iconic mid-hump overdrive. Warm, smooth breakup with enhanced mids.",
        tags = listOf("blues", "rock", "warm", "mid-boost"),
        parameters = PedalParameters(
            drive = 0.5f,
            tone = 0.5f,
            level = 0.6f,
            mix = 1.0f,
            paramA = 0.5f,      // Mid Frequency: 720Hz
            paramB = 0.5f,      // Mid Q: moderate
            oversample = 2,
            preLowCut = 80f,
            postHighCut = 10000f
        )
    )

    /** TS-808 Hot - Cranked Tube Screamer for singing leads */
    val TUBE_SCREAMER_HOT = PedalPreset(
        id = "ts808-hot",
        name = "TS-808 Hot",
        displayName = "Tube Screamer Cranked",
        pedalType = PedalType.TUBE_SCREAMER,
        description = "Tube Screamer with drive maxed. Fat, compressed, singing sustain.",
        tags = listOf("rock", "lead", "sustain", "compressed"),
        parameters = PedalParameters(
            drive = 0.85f,
            tone = 0.6f,
            level = 0.5f,
            mix = 1.0f,
            paramA = 0.6f,      // Mid Frequency: slightly higher
            paramB = 0.4f,      // Mid Q: wider
            oversample = 2
        )
    )

    /** Klon - Transparent overdrive with clean blend */
    val KLON_TRANSPARENT = PedalPreset(
        id = "klon-transparent",
        name = "Klon Transparent",
        displayName = "Transparent Overdrive",
        pedalType = PedalType.KLON,
        description = "Clean boost with touch-sensitive breakup. Preserves your tone.",
        tags = listOf("transparent", "boost", "dynamic", "clean"),
        parameters = PedalParameters(
            drive = 0.3f,
            tone = 0.55f,
            level = 0.7f,
            mix = 1.0f,
            paramA = 0.6f,      // Treble
            paramB = 0.4f,      // Clean blend
            oversample = 1
        )
    )

    /** OCD Crunch - Amp-like dynamic response */
    val OCD_CRUNCH = PedalPreset(
        id = "ocd-crunch",
        name = "OCD Crunch",
        displayName = "Amp-Like Crunch",
        pedalType = PedalType.OCD,
        description = "Dynamic, amp-like breakup. Responds to pick attack.",
        tags = listOf("amp-like", "dynamic", "crunch", "responsive"),
        parameters = PedalParameters(
            drive = 0.55f,
            tone = 0.5f,
            level = 0.6f,
            mix = 1.0f,
            oversample = 2
        )
    )

    // ========== DISTORTION PRESETS ==========

    /** DS-1 Classic - The orange pedal */
    val DS1_CLASSIC = PedalPreset(
        id = "ds1-classic",
        name = "DS-1 Classic",
        displayName = "Orange Distortion",
        pedalType = PedalType.BOSS_DS1,
        description = "The classic orange pedal. Bright, cutting distortion.",
        tags = listOf("rock", "punk", "bright", "cutting"),
        parameters = PedalParameters(
            drive = 0.6f,
            tone = 0.5f,
            level = 0.55f,
            mix = 1.0f,
            oversample = 2,
            postHighCut = 8000f
        )
    )

    /** RAT Crunch - Gritty, saturated with normal diodes */
    val RAT_CRUNCH = PedalPreset(
        id = "rat-crunch",
        name = "RAT Crunch",
        displayName = "Rodent Crunch",
        pedalType = PedalType.RAT,
        description = "Gritty, saturated distortion. Filter control shapes the bite.",
        tags = listOf("grunge", "alternative", "gritty", "saturated"),
        parameters = PedalParameters(
            drive = 0.65f,
            tone = 0.5f,
            level = 0.5f,
            mix = 1.0f,
            paramA = 0.4f,      // Filter: moderate brightness
            paramB = 0.0f,      // Turbo: off (silicon diodes)
            oversample = 2
        )
    )

    /** RAT Turbo - LED clipping for tighter response */
    val RAT_TURBO = PedalPreset(
        id = "rat-turbo",
        name = "RAT Turbo",
        displayName = "Turbo Rodent",
        pedalType = PedalType.RAT,
        description = "RAT with LED clipping. More headroom, tighter low end.",
        tags = listOf("modern", "tight", "metal", "high-gain"),
        parameters = PedalParameters(
            drive = 0.7f,
            tone = 0.55f,
            level = 0.5f,
            mix = 1.0f,
            paramA = 0.5f,      // Filter
            paramB = 1.0f,      // Turbo: on (LED diodes)
            oversample = 2
        )
    )

    /** Metal Zone - Scooped high-gain for metal */
    val METAL_ZONE_SCOOP = PedalPreset(
        id = "mt2-scoop",
        name = "Metal Zone Scoop",
        displayName = "Scooped Metal",
        pedalType = PedalType.METAL_ZONE,
        description = "High-gain with scooped mids. The '90s metal sound.",
        tags = listOf("metal", "high-gain", "scooped", "90s"),
        parameters = PedalParameters(
            drive = 0.75f,
            tone = 0.5f,
            level = 0.5f,
            mix = 1.0f,
            paramA = 0.7f,      // Low EQ boost
            paramB = 0.7f,      // High EQ boost
            paramC = 0.3f,      // Mid freq (scooped)
            oversample = 2,
            preLowCut = 60f
        )
    )

    // ========== FUZZ PRESETS ==========

    /** Big Muff Classic - NYC sustained fuzz */
    val BIG_MUFF_CLASSIC = PedalPreset(
        id = "muff-classic",
        name = "Big Muff Classic",
        displayName = "NYC Muff",
        pedalType = PedalType.BIG_MUFF,
        description = "Massive, sustained fuzz. Creamy and smooth with the mids scooped.",
        tags = listOf("fuzz", "sustain", "smooth", "grunge"),
        parameters = PedalParameters(
            drive = 0.7f,       // Sustain
            tone = 0.5f,
            level = 0.5f,
            mix = 1.0f,
            paramA = 0.6f,      // Sustain amount
            paramB = 0.7f,      // Mid scoop depth
            oversample = 2,
            preLowCut = 50f
        )
    )

    /** Big Muff Russian - Darker, woolier variant */
    val BIG_MUFF_RUSSIAN = PedalPreset(
        id = "muff-russian",
        name = "Big Muff Russian",
        displayName = "Russian Muff",
        pedalType = PedalType.BIG_MUFF,
        description = "Darker, woolier Big Muff variant. Less mids scoop.",
        tags = listOf("fuzz", "dark", "woolly", "vintage"),
        parameters = PedalParameters(
            drive = 0.65f,
            tone = 0.35f,       // Darker
            level = 0.55f,
            mix = 1.0f,
            paramA = 0.5f,
            paramB = 0.4f,      // Less mid scoop
            oversample = 2,
            postHighCut = 8000f
        )
    )

    /** Fuzz Face Germanium - Hendrix-style warm fuzz */
    val FUZZ_FACE_HENDRIX = PedalPreset(
        id = "fuzz-face-germ",
        name = "Fuzz Face Germanium",
        displayName = "Vintage Fuzz",
        pedalType = PedalType.FUZZ_FACE_GERM,
        description = "Classic germanium fuzz. Warm, organic, cleans up with volume.",
        tags = listOf("vintage", "warm", "organic", "60s"),
        parameters = PedalParameters(
            drive = 0.75f,      // Fuzz
            tone = 0.5f,
            level = 0.5f,
            mix = 1.0f,
            paramA = 0.5f,      // Bias
            paramB = 0.6f,      // Cleanup response
            oversample = 2,
            sag = 0.3f,         // Voltage sag simulation
            bias = 0.45f
        )
    )

    /** Fuzz Face Silicon - Bright, aggressive fuzz */
    val FUZZ_FACE_SILICON = PedalPreset(
        id = "fuzz-face-si",
        name = "Fuzz Face Silicon",
        displayName = "Modern Fuzz",
        pedalType = PedalType.FUZZ_FACE_SI,
        description = "Brighter, more aggressive fuzz. Tighter and more stable.",
        tags = listOf("modern", "bright", "aggressive", "tight"),
        parameters = PedalParameters(
            drive = 0.7f,
            tone = 0.6f,
            level = 0.5f,
            mix = 1.0f,
            paramA = 0.5f,
            paramB = 0.4f,
            oversample = 2
        )
    )

    /** Octave Fuzz - Octavia-style ring-mod fuzz */
    val OCTAVE_FUZZ_CLASSIC = PedalPreset(
        id = "octavia",
        name = "Octave Fuzz",
        displayName = "Octavia",
        pedalType = PedalType.OCTAVE_FUZZ,
        description = "Ring-mod style octave-up fuzz. Bell-like tones on high frets.",
        tags = listOf("octave", "ring-mod", "psychedelic", "unique"),
        parameters = PedalParameters(
            drive = 0.6f,
            tone = 0.55f,
            level = 0.5f,
            mix = 1.0f,
            oversample = 2
        )
    )

    // ========== SPECIAL PRESETS ==========

    /** HM-2 Swedish Death - The chainsaw tone */
    val HM2_SWEDISH = PedalPreset(
        id = "hm2-chainsaw",
        name = "HM-2 Chainsaw",
        displayName = "Swedish Death",
        pedalType = PedalType.HM2_CHAINSAW,
        description = "The Swedish death metal tone. Chainsaw-like distortion.",
        tags = listOf("death-metal", "chainsaw", "extreme", "swedish"),
        parameters = PedalParameters(
            drive = 1.0f,
            tone = 0.5f,
            level = 0.5f,
            mix = 1.0f,
            paramA = 1.0f,      // Low boost maxed
            paramB = 1.0f,      // High boost maxed
            oversample = 2,
            preLowCut = 40f
        )
    )

    /** Doom Fuzz - Crushing Acapulco Gold style */
    val DOOM_ACAPULCO = PedalPreset(
        id = "doom-acapulco",
        name = "Doom Fuzz",
        displayName = "Acapulco Gold",
        pedalType = PedalType.DOOM_FUZZ,
        description = "Massive, crushing doom fuzz. One-knob simplicity, endless sustain.",
        tags = listOf("doom", "stoner", "massive", "sustain"),
        parameters = PedalParameters(
            drive = 0.8f,       // El unico control real
            tone = 0.45f,
            level = 0.5f,
            mix = 1.0f,
            oversample = 2,
            preLowCut = 40f,
            postHighCut = 6000f
        )
    )

    // ========== ALL PRESETS ==========

    /** All professional pedal presets */
    val ALL_PRESETS: List<PedalPreset> = listOf(
        // Overdrive
        TUBE_SCREAMER_CLASSIC,
        TUBE_SCREAMER_HOT,
        KLON_TRANSPARENT,
        OCD_CRUNCH,
        // Distortion
        DS1_CLASSIC,
        RAT_CRUNCH,
        RAT_TURBO,
        METAL_ZONE_SCOOP,
        // Fuzz
        BIG_MUFF_CLASSIC,
        BIG_MUFF_RUSSIAN,
        FUZZ_FACE_HENDRIX,
        FUZZ_FACE_SILICON,
        OCTAVE_FUZZ_CLASSIC,
        // Special
        HM2_SWEDISH,
        DOOM_ACAPULCO
    )

    /** Default preset (Tube Screamer Classic) */
    val DEFAULT: PedalPreset = TUBE_SCREAMER_CLASSIC

    /**
     * Get all presets for a specific category.
     */
    fun forCategory(category: PedalCategory): List<PedalPreset> =
        ALL_PRESETS.filter { it.pedalType.category == category }

    /**
     * Get all presets for a specific pedal type.
     */
    fun forPedalType(type: PedalType): List<PedalPreset> =
        ALL_PRESETS.filter { it.pedalType == type }

    /**
     * Find a preset by its ID.
     */
    fun findById(id: String): PedalPreset? =
        ALL_PRESETS.find { it.id == id }

    /**
     * Find presets by tag.
     */
    fun findByTag(tag: String): List<PedalPreset> =
        ALL_PRESETS.filter { tag.lowercase() in it.tags.map { t -> t.lowercase() } }

    /**
     * Get all overdrive presets.
     */
    val overdrives: List<PedalPreset>
        get() = forCategory(PedalCategory.OVERDRIVE)

    /**
     * Get all distortion presets.
     */
    val distortions: List<PedalPreset>
        get() = forCategory(PedalCategory.DISTORTION)

    /**
     * Get all fuzz presets.
     */
    val fuzzes: List<PedalPreset>
        get() = forCategory(PedalCategory.FUZZ)

    /**
     * Get all special presets.
     */
    val specials: List<PedalPreset>
        get() = forCategory(PedalCategory.SPECIAL)
}

/**
 * A professional pedal preset configuration.
 *
 * @property id Unique identifier for the preset
 * @property name Short name for display
 * @property displayName Full display name
 * @property pedalType The pedal type this preset is based on
 * @property description Description of the sound character
 * @property tags Searchable tags for this preset
 * @property parameters The actual parameter values
 * @property isFactory True if this is a factory preset (not user-created)
 */
data class PedalPreset(
    val id: String,
    val name: String,
    val displayName: String,
    val pedalType: PedalType,
    val description: String,
    val tags: List<String> = emptyList(),
    val parameters: PedalParameters,
    val isFactory: Boolean = true
)

/**
 * Parameter values for a pedal preset.
 *
 * All parameters use normalized 0-1 ranges except where noted.
 */
data class PedalParameters(
    // Universal parameters
    val drive: Float = 0.5f,
    val tone: Float = 0.5f,
    val level: Float = 0.7f,
    val mix: Float = 1.0f,

    // Pedal-specific parameters
    val paramA: Float = 0.5f,
    val paramB: Float = 0.5f,
    val paramC: Float = 0.5f,

    // Advanced parameters
    val oversample: Int = 1,        // 0=off, 1=2x, 2=4x
    val preLowCut: Float = 80f,     // Hz
    val postHighCut: Float = 12000f, // Hz
    val sag: Float = 0f,            // 0-1
    val bias: Float = 0.5f,         // 0-1
    val gateThreshold: Float = 0f   // 0-1, 0=off
) {
    /**
     * Convert to a map of parameter ID to value for native bridge.
     */
    fun toParameterMap(): Map<Int, Float> = mapOf(
        0 to drive,         // DRIVE
        1 to tone,          // TONE
        2 to level,         // LEVEL
        3 to mix,           // MIX
        // Note: ALGORITHM (4) is set separately via pedalType.id
        5 to paramA,        // PARAM_A
        6 to paramB,        // PARAM_B
        7 to paramC,        // PARAM_C
        8 to oversample.toFloat(),  // OVERSAMPLE
        9 to preLowCut,     // PRE_LOW_CUT
        10 to postHighCut,  // POST_HIGH_CUT
        11 to sag,          // SAG
        12 to bias,         // BIAS
        13 to gateThreshold // GATE_THRESHOLD
    )
}
