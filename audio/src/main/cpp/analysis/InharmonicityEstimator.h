/**
 * InharmonicityEstimator.h — REQ-001 S7.
 *
 * Estima el coeficiente de inarmonicidad **B** de la cuerda que suena y COMPUTA
 * desde ahi el objetivo de afinacion perceptual. **No hay tabla de offsets en
 * cents** en ningun lado de este archivo, y eso es el punto de la etapa.
 *
 * DE DONDE SALE B, SIN VOLVER A ANALIZAR LA SEÑAL
 * ------------------------------------------------
 * Una cuerda con rigidez tiene sus parciales estirados:
 *
 *     f_n = n·f₀·√(1 + B·n²)
 *
 * S6 ya rastrea cuatro fases, y publica cuanto se desvia cada parcial de su
 * objetivo `n·objetivo`. Esa desviacion vale
 *
 *     cents_n = C + 600·log2(1 + B·n²)
 *
 * donde C es la desafinacion COMUN de la cuerda (la misma para los cuatro). O
 * sea que el desacuerdo ENTRE armonicos es exactamente la inarmonicidad, y
 * separarlo de C es un ajuste lineal de `cents_n` contra `600·log2(1+B·n²)`.
 * Para los B de una cuerda real —10⁻⁵ a 10⁻³— eso es `865,617·B·n²` con error
 * despreciable, asi que la recta se ajusta contra **n²** y B sale de la
 * pendiente. Un solo ajuste de cuatro puntos: cero analisis nuevo de la señal.
 *
 * EL OBJETIVO PERCEPTUAL SALE DE ALINEAR PARCIALES, QUE ES LO QUE DICE EL AC
 * --------------------------------------------------------------------------
 * AC-001.10: *"el offset perceptual no es propiedad de una cuerda sola: sale de
 * alinear los parciales entre cuerdas"*. Eso no es una metafora, es la cuenta.
 *
 * Dos cuerdas separadas por un intervalo de razon justa `p:q` se afinan —a
 * oido, y desde siempre— igualando el parcial `p` de la grave con el parcial `q`
 * de la aguda. Con cuerdas ideales eso da exactamente el intervalo teorico. Con
 * cuerdas rigidas, cada parcial esta estirado por su propio B, y al igualarlos
 * queda un desajuste:
 *
 *     Δcents = 600·log2(1 + p²·B_grave) − 600·log2(1 + q²·B_aguda)
 *
 * Eso es TODO el modelo. El "indice de cuerda" del AC entra solo, y no como
 * parametro suelto: la correccion de la cuerda i es la SUMA de los desajustes
 * de todos los intervalos que la separan de la cuerda de referencia. Por eso el
 * offset es propiedad del conjunto y no de una cuerda.
 *
 * Con B = 0 en todas, `log2(1) = 0` y la correccion es **exactamente** cero: la
 * correccion no puede ensuciar el caso limpio, que es lo que exige 7.3.
 *
 * RT: nada de esto corre en el thread de audio. Lo llama el thread de analisis
 * despues de que el strobe publico, y no asigna.
 */
#pragma once

#include "StrobeTracker.h"

#include <cmath>

namespace wma::analysis {

/**
 * Los parametros fisicos de una cuerda, para el respaldo de AC-001.11.
 *
 * 🔴 EL CALIBRE ES EL DEL NUCLEO, NO EL TOTAL. En una cuerda entorchada el
 * bobinado agrega masa pero **no rigidez**: usar el diametro total da un B dos
 * ordenes de magnitud alto (medido: 2,7·10⁻² contra los ~10⁻⁴ reales de un bajo).
 */
struct StringPhysics {
    double youngModulusPa;   ///< E — acero de cuerda ≈ 200 GPa; nylon ≈ 5 GPa
    double coreDiameterM;    ///< d — diametro del NUCLEO
    double tensionN;         ///< T
    double scaleLengthM;     ///< L
};

class InharmonicityEstimator {
public:
    /// Cuantos parciales entrega S6.
    static constexpr int kPartials = StrobeTracker::kPartials;

    /**
     * Techo de la correccion perceptual, en cents.
     *
     * 35 es **menos de medio semitono** (50), y esa es toda la justificacion que
     * necesita: por definicion, una correccion saturada NO PUEDE desplazar el
     * objetivo hasta la nota vecina. Es la promesa de 7.6, y es una propiedad del
     * numero, no una esperanza sobre la señal.
     */
    static constexpr double kMaxCorrectionCents = 35.0;

    /// `cents_n ≈ K·n²` con `K = 1200/ln2 · B / 2`. Sale de linealizar
    /// `600·log2(1+Bn²)`; con B ≤ 10⁻³ el error relativo es < 0,1 %.
    static constexpr double kCentsPerBPerNSquared = 600.0 / 0.6931471805599453;

    /// Minimo de parciales con medicion para intentar el ajuste. Con dos puntos
    /// una recta pasa exacto y la pendiente no significa nada.
    static constexpr int kMinPartialsForFit = 3;

    void reset() noexcept {
        mB = 0.0;
        mMeasured = false;
    }

    /**
     * Ajusta B con lo que S6 ya midio. No toca la señal.
     * @return true si B quedo MEDIDO; false si no convergio (y entonces el
     *         consumidor tiene que caer al respaldo de `physicsB`).
     */
    bool estimateFrom(const StrobeTracker& strobe) noexcept;

    /// El B estimado. Sin medicion vale 0.
    double b() const noexcept { return mB; }

    /// Si `b()` se midio de verdad o el consumidor tiene que usar el respaldo.
    bool measured() const noexcept { return mMeasured; }

    /**
     * El respaldo de AC-001.11: `B = π³·E·d⁴ / (64·T·L²)`.
     *
     * Es la formula de la rigidez de una barra, no un ajuste empirico. Verificada
     * contra el rango publicado: da 1,28·10⁻⁵ para la prima de una guitarra y
     * 1,05·10⁻⁴ para su bordona, y lo publicado es ~10⁻⁵ a ~5·10⁻⁴.
     */
    static double physicsB(const StringPhysics& p) noexcept;

    /**
     * El desajuste, en cents, de igualar el parcial `pLower` de la cuerda grave
     * con el parcial `qUpper` de la aguda.
     *
     * Positivo = la aguda tiene que subir respecto del intervalo teorico.
     */
    static double alignmentCents(double bLower, double bUpper,
                                 int pLower, int qUpper) noexcept;

    /**
     * Los dos parciales que se igualan para un intervalo, en semitonos.
     *
     * Sale de la razon JUSTA mas cercana al intervalo temperado, que es la que el
     * oido usa porque es la que hace desaparecer el batido: octava 2:1, quinta
     * 3:2, cuarta 4:3, tercera mayor 5:4, tercera menor 6:5.
     */
    static void matchingPartials(int semitones, int& pLower, int& qUpper) noexcept;

    /**
     * La correccion perceptual de cada cuerda, acumulada desde la de referencia.
     *
     * `freqHz[i]` en orden ascendente y `bs[i]` su inarmonicidad. La cuerda
     * `reference` queda en 0 por definicion —es la que el musico afina contra el
     * diapason— y el resto acumula el desajuste de cada intervalo del camino.
     * Cada salida sale saturada a ±`kMaxCorrectionCents`.
     */
    static void perceptualCorrections(const double* freqHz, const double* bs, int count,
                                      int reference, double* outCents) noexcept;

private:
    double mB{0.0};
    bool mMeasured{false};
};

}  // namespace wma::analysis
