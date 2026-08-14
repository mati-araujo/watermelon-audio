#ifndef WMA_EFFECT_CATALOG_H
#define WMA_EFFECT_CATALOG_H

/**
 * Nombres de los `EffectType`, para tests que barren el catalogo entero.
 *
 * Vive aparte de GoldenHarness.h porque es otra cosa: el harness MIDE, esto
 * solo nombra. Y vive en un header compartido porque la tabla ya existia
 * duplicada en `test_effect_latency.cpp`, y dos copias de un switch sobre un
 * enum que crece es exactamente la clase de deriva que este programa persigue.
 */

#include "../EffectTypes.h"

namespace wma::catalog {

inline const char* nameOf(EffectType t) {
    switch (t) {
        case FILTER: return "FILTER";
        case REVERB: return "REVERB";
        case DELAY: return "DELAY";
        case VOCODER: return "VOCODER";
        case DISTORTION: return "DISTORTION";
        case COMPRESSOR: return "COMPRESSOR";
        case CHORUS: return "CHORUS";
        case PHASER: return "PHASER";
        case AMP_SIM: return "AMP_SIM";
        case CABINET: return "CABINET";
        case DECIMATOR: return "DECIMATOR";
        case DECI_HPF: return "DECI_HPF";
        case AUTO_PAN: return "AUTO_PAN";
        case COMPLEX_TREM: return "COMPLEX_TREM";
        case RANDOM_RESO: return "RANDOM_RESO";
        case HPF_DELAY: return "HPF_DELAY";
        case TAPE_ECHO: return "TAPE_ECHO";
        case HALL_REVERB: return "HALL_REVERB";
        case RISER_REVERB: return "RISER_REVERB";
        case BEAT_GRAIN: return "BEAT_GRAIN";
        case SPRING_REVERB: return "SPRING_REVERB";
        case PLATE_REVERB: return "PLATE_REVERB";
        case SHIMMER_REVERB: return "SHIMMER_REVERB";
        default: return "?";
    }
}

}  // namespace wma::catalog

#endif  // WMA_EFFECT_CATALOG_H
