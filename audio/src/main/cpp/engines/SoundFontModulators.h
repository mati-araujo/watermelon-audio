#pragma once

/**
 * SoundFontModulators.h — la evaluación de un modulador SF2 (REQ-039 S2).
 *
 * ## Qué es esto, y qué NO es
 *
 * Sólo la **transferencia**: dado el valor de un controlador y cómo está declarado
 * el modulador, cuánto aporta a su generador destino. Es una función **pura** — sin
 * estado, sin asignaciones, sin dependencias— y por eso se puede probar contra la
 * fórmula del spec sin motor, sin audio y sin threads.
 *
 * Lo que NO hace: leer el archivo, decidir qué moduladores aplican a qué región, ni
 * componerlos. Eso viene después y vive aparte.
 *
 * ## 🔴 Por qué NO vive en `thirdparty/`
 *
 * `scripts/cpp_callgraph.py:33` excluye ese árbol entero, así que `check-rt-safety`
 * y `check-mechanism-callers` son **ciegos** a todo lo que caiga ahí — y se quedan
 * en verde revisando menos, que es la misma clase de falso verde que
 * `rt-coverage-baseline` existe para atajar. `tsf_impl.cpp` es sólo el punto de
 * entrada de datos; la lógica va acá, donde el lint la camina.
 *
 * ## La fórmula, y de dónde salió
 *
 * El spec SF2 §8.10 define cuatro curvas de transferencia. La cóncava es la que
 * importa para el defecto que REQ-039 persigue (el modulador por defecto #1 es
 * velocity → `initialAttenuation`, cóncava, unipolar decreciente, 960 cB):
 *
 *     concave_decreasing(x) = -(20/96) · log10(x²) = -(40/96) · log10(x)
 *
 * 🔑 **No se escribió de memoria: se validó contra cuatro oráculos externos**, y
 * ninguno sale de este motor.
 *
 * | oráculo | esperado | la fórmula |
 * |---|---|---|
 * | README del `SoundFont-Spec-Test`: v127→v111 con 96 dB | 2,34 dB | **2,34** |
 * | FluidSynth 2.6.0, prueba #13 A (96 dB cóncava, v127→v15) | 37,11 dB | **37,11** |
 * | FluidSynth 2.6.0, prueba #13 B (144 dB) | 55,60 dB | 55,66 |
 * | FluidSynth 2.6.0, prueba #13 C (48 dB) | 18,50 dB | 18,55 |
 *
 * Los residuos de ~0,05 dB crecen con la atenuación (—0,00 a 37 dB, +0,06 a 55 dB,
 * y +1,23 a 84 dB en la variante lineal): es el método de medición —el RANGO de un
 * render con envolvente, no la atenuación teórica— acercándose al piso, no un
 * desacuerdo de fórmula.
 *
 * 🔑 **Y explica una casualidad que S1 había medido sin poder explicar**: la prueba
 * `#13 C` coincidía con la referencia mientras las otras nueve no. Es porque la
 * curva de velocity **cableada** en `tsf.h:1619` da 18,5 dB de rango y una cóncava
 * de 48 dB da 18,55. Se parecen por accidente, y por eso el defecto no salta en un
 * promedio global.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace wma {
namespace sfmod {

/** Las cuatro curvas de transferencia del spec SF2 §8.10. */
enum class Curve : std::uint8_t {
    Linear = 0,
    Concave = 1,
    Convex = 2,
    Switch = 3,
};

/**
 * Cómo está declarado un modulador, sin el "de dónde salió".
 *
 * Son los cinco campos de `pmod`/`imod` ya desempaquetados. `sourcePolarity` y
 * `sourceDirection` son del `modSrcOper`; los del `modAmtSrcOper` van aparte
 * porque el spec los trata igual pero por separado.
 */
struct Transform {
    Curve curve = Curve::Linear;
    bool bipolar = false;
    bool decreasing = false;
};

/** Desempaqueta la mitad de transferencia de un `modSrcOper` (spec §8.2). */
inline Transform transformOf(std::uint16_t oper) {
    Transform t;
    t.decreasing = (oper & 0x0100) != 0;
    t.bipolar = (oper & 0x0200) != 0;
    t.curve = static_cast<Curve>((oper >> 10) & 0x3F);
    return t;
}

/** El índice de controlador de un `modSrcOper`, y si es un CC. */
inline int sourceIndexOf(std::uint16_t oper) { return oper & 0x7F; }
inline bool sourceIsMidiCC(std::uint16_t oper) { return (oper & 0x0080) != 0; }

namespace detail {

/**
 * La cóncava **creciente unipolar**, base de las otras tres variantes.
 *
 * Vale 0 en x=0 y 1 en x=1. Se acota arriba porque `log10(0)` diverge: el spec la
 * define saturada, no infinita.
 */
inline float concaveIncreasing(float x) {
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    const float y = -(40.0f / 96.0f) * std::log10(1.0f - x);
    return std::min(1.0f, y);
}

/** La convexa es la cóncava reflejada en los dos ejes: `1 - concave(1 - x)`. */
inline float convexIncreasing(float x) {
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return 1.0f - concaveIncreasing(1.0f - x);
}

/** La curva unipolar creciente que corresponda, sobre [0,1]. */
inline float unipolarIncreasing(float x, Curve curve) {
    switch (curve) {
        case Curve::Linear:
            return x;
        case Curve::Concave:
            return concaveIncreasing(x);
        case Curve::Convex:
            return convexIncreasing(x);
        case Curve::Switch:
            return (x >= 0.5f) ? 1.0f : 0.0f;
    }
    return x;
}

}  // namespace detail

/**
 * Aplica la transferencia a un controlador ya normalizado a [0,1].
 *
 * Devuelve [0,1] si es unipolar y [-1,1] si es bipolar, que es lo que el spec
 * define como salida de la sección de transferencia.
 *
 * La **dirección** invierte la entrada (`1 - x`), no la salida: así una cóncava
 * decreciente sigue siendo cóncava —empinada cerca de su extremo alto— en vez de
 * volverse una convexa dada vuelta. Es lo que hace que el modulador por defecto #1
 * reproduzca los 2,34 dB del README entre las velocities 127 y 111.
 */
inline float applyTransform(float normalized, const Transform& t) {
    const float x = std::clamp(normalized, 0.0f, 1.0f);
    const float dirApplied = t.decreasing ? (1.0f - x) : x;

    if (!t.bipolar) return detail::unipolarIncreasing(dirApplied, t.curve);

    // Bipolar: el spec espeja la curva alrededor del centro, así que cada mitad usa
    // la misma forma unipolar sobre su propio tramo y la de abajo sale negada.
    if (dirApplied >= 0.5f) {
        return detail::unipolarIncreasing((dirApplied - 0.5f) * 2.0f, t.curve);
    }
    return -detail::unipolarIncreasing((0.5f - dirApplied) * 2.0f, t.curve);
}

/**
 * Normaliza un valor de controlador MIDI de 7 bits a [0,1].
 *
 * El divisor es **127 y no 128**: el spec quiere que el máximo del controlador dé
 * exactamente 1, y de eso depende que el default #1 no atenúe nada con velocity
 * 127. Con 128 quedaría un resto de atenuación en la nota más fuerte.
 */
inline float normalize7bit(int value) {
    return std::clamp(static_cast<float>(value) / 127.0f, 0.0f, 1.0f);
}

/**
 * Lo que un modulador aporta a su generador destino, en las unidades de ese
 * generador (centibeles para `initialAttenuation`, cents para `initialFilterFc`…).
 *
 * `primaryNormalized` es la fuente principal ya en [0,1]; `amountNormalized` es la
 * secundaria (`modAmtSrcOper`), que **escala** el aporte. Sin fuente secundaria se
 * pasa 1,0 — que es el caso de los 1377 moduladores del font bundleado que no la
 * usan, y de los diez por defecto.
 *
 * 🔴 La secundaria **no es opcional en la práctica**: 1435 de los 2812 moduladores
 * de `GeneralUser_GS.sf3` la declaran (medido, `scripts/read-sf2-modulators.py`).
 */
inline float contribution(float amount, float primaryNormalized, const Transform& primary,
                          float amountNormalized, const Transform& secondary) {
    return amount * applyTransform(primaryNormalized, primary) *
           applyTransform(amountNormalized, secondary);
}

/**
 * Un modulador tal como vive en `pmod`/`imod`: los cinco campos del spec §8.2.
 *
 * `amount` queda aparte de los otros cuatro a propósito: la **identidad** de un
 * modulador —lo que decide si dos son "el mismo" para las reglas de precedencia—
 * son `srcOper`, `destOper`, `amtSrcOper` y `transOper`, y NO el amount. De eso
 * depende que declarar uno con amount 0 **anule** su equivalente en vez de
 * agregar un cero al montón.
 */
struct Modulator {
    std::uint16_t srcOper = 0;
    std::uint16_t destOper = 0;
    std::int16_t amount = 0;
    std::uint16_t amtSrcOper = 0;
    std::uint16_t transOper = 0;

    /** Los CUATRO campos que definen identidad. El amount no entra. */
    bool sameIdentity(const Modulator& other) const {
        return srcOper == other.srcOper && destOper == other.destOper &&
               amtSrcOper == other.amtSrcOper && transOper == other.transOper;
    }
};

/**
 * Las listas de moduladores de los cuatro ámbitos de SF2 §9.5, tal como salen del
 * archivo y antes de componerse.
 */
struct Scopes {
    std::vector<Modulator> instrumentGlobal;
    std::vector<Modulator> instrumentZone;
    std::vector<Modulator> presetGlobal;
    std::vector<Modulator> presetZone;
};

namespace detail {

/**
 * Agrega `incoming` sobre `base` **reemplazando** por identidad.
 *
 * Es la regla de un nivel contra sí mismo: dentro de una lista gana el último, y
 * una lista más específica pisa a la más general.
 */
inline void mergeReplacing(std::vector<Modulator>& base, const std::vector<Modulator>& incoming) {
    for (const Modulator& m : incoming) {
        bool replaced = false;
        for (Modulator& existing : base) {
            if (existing.sameIdentity(m)) {
                existing = m;
                replaced = true;
                // Sin `break`: si la base trajera duplicados, todos quedan en el
                // mismo valor y el resultado no depende de cuál se encontró primero.
            }
        }
        if (!replaced) base.push_back(m);
    }
}

/** Agrega `incoming` sobre `base` **sumando** los amounts por identidad. */
inline void mergeAdding(std::vector<Modulator>& base, const std::vector<Modulator>& incoming) {
    for (const Modulator& m : incoming) {
        bool summed = false;
        for (Modulator& existing : base) {
            if (existing.sameIdentity(m)) {
                existing.amount = static_cast<std::int16_t>(existing.amount + m.amount);
                summed = true;
            }
        }
        if (!summed) base.push_back(m);
    }
}

}  // namespace detail

/**
 * Compone los cuatro ámbitos y los diez por defecto en la lista que se evalúa al
 * disparar la nota.
 *
 * ## 🔑 Las reglas NO salen del spec leído de memoria: se MIDIERON
 *
 * Se generó un font por regla —con moduladores idénticos en los cuatro campos y
 * amounts distintos— y se renderizó con **FluidSynth 2.6.0**, midiendo el rango de
 * nivel entre velocity 127 y 15. El amount efectivo se lee del rango:
 *
 * | font | rango medido | qué prueba |
 * |---|---|---|
 * | sólo el default (960 cB) | 37,12 dB | la línea de base |
 * | global 960 **vs** zona 480 | **18,60** (= 480 solo) | el local **reemplaza** al global |
 * | instrumento 960 **vs** preset 480 | **55,51** (= 1440) | el preset **suma** sobre el instrumento |
 * | uno igual al default con amount 0 | **0,00** | el declarado **reemplaza** al default |
 * | dos en el mismo ámbito (1440, después 480) | **18,60** | gana el **último** |
 *
 * 🔴 **La asimetría es el punto y es fácil de escribir al revés**: entre ámbitos de
 * instrumento se REEMPLAZA, pero el preset SUMA sobre lo que quedó. El spec lo
 * justifica —los generadores de preset son *offsets*— pero la justificación no es
 * la prueba; el render sí.
 *
 * Y cierra el hueco que S1 dejó declarado: `AC-039.4` hablaba de dos niveles
 * (preset e instrumento) sin decir qué pasa entre el **global** y el **local** del
 * mismo nivel. Pasa que el local gana.
 */
inline std::vector<Modulator> resolve(const std::vector<Modulator>& defaults,
                                      const Scopes& scopes) {
    // Nivel de instrumento: los defaults son la base y cada ámbito más específico
    // pisa por identidad. Por eso declarar uno igual al default con amount 0 lo anula.
    std::vector<Modulator> instrument = defaults;
    detail::mergeReplacing(instrument, scopes.instrumentGlobal);
    detail::mergeReplacing(instrument, scopes.instrumentZone);

    // Nivel de preset: se compone aparte, con la misma regla de reemplazo.
    std::vector<Modulator> preset;
    detail::mergeReplacing(preset, scopes.presetGlobal);
    detail::mergeReplacing(preset, scopes.presetZone);

    // Y recién acá SUMA sobre el instrumento — medido, no supuesto.
    detail::mergeAdding(instrument, preset);
    return instrument;
}

}  // namespace sfmod
}  // namespace wma
