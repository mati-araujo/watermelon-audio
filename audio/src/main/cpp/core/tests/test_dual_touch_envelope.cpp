/**
 * test_dual_touch_envelope.cpp — REQ-052 S1 (envolvente por slot del dual touch).
 *
 * ## Que se prueba
 *
 * El modo dual (`setDualTouchMode(true)` + `updateDualTouch`) tenia dos defectos:
 *
 *   1. SUM y AVERAGE sumaban a (b1+b2)*0,5: la voz que ya sonaba BAJABA 6 dB al entrar la
 *      otra y SUBIA 6 dB cuando la otra salia.
 *   2. Un slot con amp <= 0,001 dejaba de procesarse en seco: el dedo que se iba se cortaba
 *      sin release (click), y volver a tocar arrancaba sin rampa.
 *
 * El comportamiento nuevo (spec aprobada, decisiones cerradas) que estos tests EXIGEN:
 *
 *   - SUM/AVERAGE: out = voz1 + voz2, ganancia 1 por voz (igual que un dedo solo).
 *   - Cada slot lleva una envolvente: ataque ~5 ms al entrar (amp pasa de <= 0,001 a > 0,001);
 *     release exponencial ~-60 dB en ~80 ms al salir, durante el cual el engine del slot
 *     SIGUE procesandose con su ULTIMA frecuencia y amplitud; al terminar, el engine deja de
 *     procesarse. Si el slot vuelve durante su release, la ganancia sube desde donde estaba.
 *     El ultimo dedo (los dos slots en 0 con el modo dual puesto) tambien hace release.
 *   - MAX/CROSSFADE/RING/AMPLITUDE_BALANCED ganan la misma envolvente y conservan su ley.
 *     Interpretacion que fija esta etapa: con UN solo dedo en modo dual la salida es ese dedo
 *     a ganancia 1 (en los cuatro modos); con dos dedos rige la ley de hoy; las transiciones
 *     entre esos estados son continuas.
 *
 * ## Como se mide (skill `medir-dsp`)
 *
 * Puerta: `AudioEngine::startOffline` + `renderBlock` (la misma cadena que el callback real:
 * DC blocker, efectos —vacios por defecto, y se verifica—, fade, master, limitador, soft-clip,
 * dither). Instancia fresca por render.
 *
 * Observable de NIVEL: energia de la BANDA de la fundamental de cada voz (Goertzel con ventana
 * de Hann sobre 20 ms), no RMS de la mezcla. Observable de CLICK: |x[n]-x[n-1]| de la salida
 * con Classic seno. Observable de PROCESAMIENTO: la sonda `dualTouchSlotBlocksRendered`.
 *
 * ## Los instrumentos que sostienen las mediciones
 *
 *   - GEMELO: para la voz V, la referencia es el MISMO gesto con la otra voz siempre en amp 0
 *     (o con V sostenida, para el release). Un engine que no es estacionario (Karplus-Strong
 *     re-excita, Granular crece, Supersaw bate) sigue siendo comparable consigo mismo.
 *   - DIFERENCIA muestra a muestra (`dual - gemelo de la otra voz`): aisla a la voz que sale
 *     si la cadena es lineal en ese regimen. El piso de fuga de Hann a 20 ms (~-50 dB) no deja
 *     ver -60 dB en la banda de la voz que SIGUE sonando; la diferencia lo cancela. Su
 *     linealidad la verifica un CONTROL (`TheChainIsLinearEnoughForTheDifferenceMethod`).
 *   - RESTAR EL SILENCIO: el dither (TPDF +-1 LSB de 16 bit, determinista) es un piso de ~1e-6
 *     de amplitud de banda; para ver -60 dB de una voz de ~1e-3 se resta un render en
 *     silencio, que contiene SOLO el dither.
 *   - AMPLITUD BAJA a proposito: el soft-clip es `tanh(0,666 x)*1,5` = 0,999 x - 0,1475 x^3. La
 *     compresion cruzada entre dos voces es ~0,22 a^2: con a = 0,05 son ~-65 dB, con a = 1 son
 *     -13 dB. Para que la diferencia valga, las voces suenan a ~0,05 de pico (tabla abajo).
 *
 * ## La limitacion declarada
 *
 * Estos tests se escribieron ANTES de la implementacion. Los umbrales salen de los AC y de las
 * cuentas escritas junto a cada constante. Los controles sinteticos (`TheMeter...`) ejercen los
 * VEREDICTOS sobre senales fabricadas a partir de renders del gemelo, para probar que el
 * instrumento puede ponerse rojo en el assert correcto sin tocar codigo de produccion.
 */

#include "analysis/tests/support/PartialOracle.h"
#include "core/AudioEngine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using wma_test::oracle::goertzel;

// ===========================================================================
// Constantes con su origen
// ===========================================================================

constexpr int kSampleRate = 48000;
constexpr int kMaxBlock = 256;          // startOffline(48000, 256), como pide el brief
constexpr int kFramesPerMs = 48;        // 48000 / 1000

/// Paso de gesto GRUESO: 5 ms = 240 muestras = el crossfade de engine (ENGINE_CROSSFADE_SAMPLES)
/// = kParamSmoothingMs = el ataque. Un gesto por bloque.
constexpr int kCoarseStepFrames = 240;

/// Paso de gesto FINO: 0,125 ms = un octavo de periodo a 1 kHz. Es lo que permite BARRER la fase
/// con que cae el evento (ver los tests de click). Los osciladores Classic arrancan en fase 0, asi
/// que a 1 kHz todo evento en un milisegundo ENTERO cae en un cruce por cero y un corte en seco no
/// produce ningun escalon (medido: Quarter0 y Quarter2 del primer borrador daban verde con el
/// defecto). Un desfase de 0,125 ms pone el evento a 1/8 de ciclo: |sin| = 0,707 en las cuatro
/// posiciones del barrido.
constexpr int kFineStepFrames = 6;
/// Desfase que saca al evento del cruce por cero (ver arriba).
constexpr double kOffZeroCrossingMs = 0.125;

/// Ventana de medicion: 20 ms (AC-052.3). A 48 kHz resuelve 50 Hz/bin y con Hann su lobulo
/// principal es +-100 Hz.
constexpr int kWindowFrames = 960;

/// Salto entre ventanas consecutivas de la serie: un bloque grueso.
constexpr int kHopFrames = kCoarseStepFrames;

/// El limitador (`LookaheadLimiter::LOOKAHEAD_MS` = 5 ms) es SIEMPRE una linea de retardo: lo
/// que entra en n sale en n + 240. Todo evento aparece en la salida 240 muestras despues.
constexpr int kOutputLatencyFrames = 240;

/// Calentamiento: ataque 5 ms + smoother de params 5 ms + crossfade de engine 5 ms = 15 ms,
/// duplicado de margen. La ventana de medicion arranca DESPUES.
constexpr double kSettleMs = 40.0;

/// Umbral del limitador (-0,5 dBFS). Precondicion de AC-052.1: bajo esto el limitador no actua.
constexpr double kLimiterThreshold = 0.944;

/// "Sono" es > 0,005; el piso de silencio del motor es ~1e-5. Nunca `> 0`.
constexpr double kAudible = 0.005;
constexpr double kSilenceFloor = 1e-5;

constexpr int kOscSine = 0;
constexpr int kClassic = 0;
constexpr int kFm = 2;
constexpr int kWavetable = 3;

/// Fundamentales de las dos voces de los tests por engine: no armonicas entre si (cada una queda
/// a >= 220 Hz de todo armonico de la otra) y a 280 Hz entre si (5,6 bins de Hann).
constexpr float kFreqSlot0 = 500.0f;
constexpr float kFreqSlot1 = 780.0f;

// --- AC-052.1 / .2: nivel de la voz que queda ---
/// AC-052.1/.2: |delta| <= 0,5 dB.
constexpr double kLevelToleranceDb = 0.5;

// --- AC-052.3 / .6: release ---
/// Release -60 dB en ~80 ms => ~-15 dB a 20 ms. Un corte en seco, o un release <= ~25 ms, deja
/// la ventana [20,40) en el piso (<< -40). Mata al mutante "sin release".
constexpr double kReleaseNotCutMinDb = -40.0;
/// D4 pide release EXPONENCIAL. Una rampa LINEAL de 80 ms pasa "no cortado" y "profundo" y es un
/// mutante distinto: exp -60 dB/80 ms esta en -15 dB a 20 ms y en ~-22 dB de media en [20,40)
/// (medido con la implementacion: -21,6 a -27,5); la rampa lineal de 80 ms esta en -2,5 dB a
/// 20 ms y ~-4 dB de media. -10 dB deja 12 dB de margen al exponencial y 6 dB a la lineal.
constexpr double kReleaseDecayMaxDb = -10.0;
/// AC-052.3: "cae >= 60 dB respecto del regimen en <= 100 ms". La ventana [80,100) (centro de
/// Hann a 90 ms) ve ~-67 dB con el release nominal de 80 ms.
constexpr double kReleaseDeepMaxDb = -60.0;
/// "no crece entre ventanas consecutivas": tolerancia de la ESTIMACION (cociente de bandas sobre
/// una senal con envolvente propia), no una holgura del AC.
constexpr double kMonotoneSlackDb = 0.5;
/// Bajo este nivel (re la voz sostenida) la serie ya es el residuo de la resta `dual - gemelo`
/// (medido: un corte en seco deja -127 dB), no la voz: 27 dB por encima de ese piso y 40 dB
/// por debajo del -60 del AC. No se exige monotonia ahi.
constexpr double kMonotoneFloorDb = -100.0;
/// Cuantas ventanas de 20 ms mide el release: [0,20) .. [80,100).
constexpr int kReleaseWindows = 5;
/// La sonda crece mientras dura el release: 60 ms / 5 ms = 12 bloques, menos 2 de holgura.
constexpr double kProbeGrowthWindowMs = 60.0;
constexpr uint64_t kProbeGrowthMinBlocks = 10;
/// "Despues de ~120 ms queda congelada": 80 ms de release + 50 % de margen.
constexpr double kEngineDoneMs = 120.0;
/// Despues del ultimo release la salida (menos el dither) es silencio. -60 dB en 80 ms => a 150 ms
/// el residuo es < -110 dB de ~0,1 = 3e-7, bajo el piso de 1e-5.
constexpr double kSilenceAfterMs = 150.0;
/// "Silencio" = lo AUDIBLE de la salida. MEDIDO: tras el ultimo soltar, Karplus-Strong y Granular
/// dejan una cola monotona de ~1,9e-5 que decae con tau ~54 ms y cuya media es ~0,85 de su pico: es un
/// OFFSET DE CC (esos engines no son de media cero) que el DC blocker de OutputStage —de esquina
/// ~3 Hz— tarda en drenar. Classic no la tiene (cero a los 560 ms). Es sub-audible, asi que el pico
/// crudo mide otra cosa. Se mide el pico de la salida pasada por un pasa-altos de un polo a 20 Hz (el
/// limite inferior de lo audible; deja ~0,15 de una exponencial de tau 54 ms: 2,8e-6 < 1e-5) y la
/// banda de la fundamental contra el regimen.
constexpr double kSilenceHighPassHz = 20.0;
/// La fundamental, 150 ms despues del soltar, a >= 80 dB bajo el regimen: los 60 dB del release
/// (AC-052.3) mas 20 dB de margen. Con el release del spec queda a ~-100 dB.
constexpr double kSilenceBandMaxDb = -80.0;

// --- AC-052.4 / .5 / .8: click ---
/// Tolerancia del AC-052.4: "x 1,10".
constexpr double kClickTolerance = 1.10;
/// Ataque ~5 ms lineal => a 1 ms (48 muestras) la ganancia es ~0,2. Se afirma <= 0,5.
constexpr int kAttackProbeFrames = 48;
constexpr double kAttackFirstMsMaxFraction = 0.5;
/// Con 5 ms de ataque, a los 15 ms (3x) ya esta abierto: >= 0,9 del pico de regimen.
constexpr double kAttackOpenMs = 15.0;
constexpr double kAttackOpenMinFraction = 0.9;
/// Umbral de "empezo a sonar" para ubicar el onset: 40x el piso, bajo el 1 % del pico de regimen.
constexpr double kOnsetLevel = 0.002;
/// Margen alrededor de un evento al abrir la ventana de click (cubre la imprecision de ubicar
/// el evento en la salida).
constexpr double kEdgeGuardMs = 2.0;
constexpr double kAttackClickWindowMs = 25.0;     // ataque 5 ms x 5
constexpr double kReleaseClickWindowMs = 130.0;   // release 80 ms + 50 ms
constexpr double kRetouchClickWindowMs = 30.0;
/// Un seno limpio tiene |dx|max = pico * 2*pi*f/fs. Se afirma dentro de +-10 % (control de que
/// lo medido ES un seno y no otra cosa).
constexpr double kSineStepModelTolerance = 0.10;
/// Fraccion de la gananacia al re-tocar: el pico en el primer ciclo tras el re-toque no puede
/// ser menor que el del release sin re-toque (la ganancia SUBE desde donde estaba).
constexpr double kRetouchNotBelowReleaseFraction = 0.9;

constexpr float kFineFreq = 1000.0f;  // periodo = 48 muestras = kAttackProbeFrames
constexpr float kFineAmp = 0.25f;

// --- AC-052.8: leyes en regimen ---
/// Tolerancia de las leyes de mezcla en dB (la cadena comprime ~0,05 dB a estas amplitudes).
constexpr double kLawToleranceDb = 0.5;
/// MAX por muestra: DC blocker (R = 0,995, fc ~ 38 Hz) adelanta ~2,2 grados a 1 kHz => hasta 4 %
/// del pico; se da 8 % y se tolera 1 % de las muestras (cruces de signo).
constexpr double kMaxLawSampleTolerance = 0.08;
constexpr double kMaxLawViolatingFraction = 0.01;
/// Bajo este nivel (re el pico) el signo de b1+b2 es ruido de redondeo y no se compara.
constexpr double kMaxLawSignGuard = 0.05;
constexpr double kPassthroughToleranceDb = 0.3;

// ===========================================================================
// Engines y modos
// ===========================================================================

struct EngineInfo {
    int type;
    const char* name;
    /// Amplitud de CADA voz en los tests de nivel. Origen: pico de una voz a amp 1 medido en
    /// el probe (Classic 0,846 / KS 0,551 / FM 0,855 / WT 0,858 / Granular 0,870 / Supersaw
    /// 0,874). El soft-clip comprime una voz por otra a ~0,22 a^2: el presupuesto de linealidad
    /// del metodo de la diferencia es -60 dB, y `TheSoftClipCrossModulation...` lo MIDE por
    /// engine. Con 0,07 daba Classic -59,3 / FM -56,7 / Wavetable -59,3 dB (rojo); con 0,05
    /// da ~6 dB menos en todos. Karplus-Strong (0,10) ya cumplia: su energia esta en el pico de
    /// la rafaga, no en regimen.
        float amp;
};

constexpr EngineInfo kEngines[6] = {
    {0, "Classic", 0.05f},       {1, "KarplusStrong", 0.10f}, {2, "FM", 0.05f},
    {3, "Wavetable", 0.05f},     {4, "Granular", 0.05f},      {5, "Supersaw", 0.05f},
};

const char* modeName(DualTouchMixMode m) {
    switch (m) {
        case DualTouchMixMode::SUM: return "Sum";
        case DualTouchMixMode::AVERAGE: return "Average";
        case DualTouchMixMode::MAX: return "Max";
        case DualTouchMixMode::CROSSFADE: return "Crossfade";
        case DualTouchMixMode::RING: return "Ring";
        case DualTouchMixMode::AMPLITUDE_BALANCED: return "AmplitudeBalanced";
    }
    return "?";
}

float freqOfSlot(int slot) { return slot == 0 ? kFreqSlot0 : kFreqSlot1; }

inline int ms(double t) { return static_cast<int>(std::lround(t * kFramesPerMs)); }

// ===========================================================================
// Gestos
// ===========================================================================

struct Finger {
    int slot;
    int fromFrame;
    int toFrame;
    float freq;
    float amp;
};

/**
 * Un gesto dual por escalones de `stepFrames`. Un slot FUERA de cualquiera de sus dedos manda
 * `freq = 0, amp = 0` (levantar el dedo): es el caso hostil para "release con la ULTIMA
 * frecuencia" —una implementacion que use la frecuencia vigente en el release oye 0 Hz— y es
 * lo que un cliente real puede mandar al soltar.
 */
class Script {
public:
    Script(int stepFrames, double totalMs) : mStep(stepFrames), mTotal(ms(totalMs)) {}

    Script& touch(int slot, double fromMs, double toMs, float freq, float amp) {
        mFingers.push_back({slot, ms(fromMs), ms(toMs), freq, amp});
        return *this;
    }
    Script& withDistance(float d) {
        mDistance = d;
        return *this;
    }

    /// El mismo gesto con SOLO los dedos de `slot`: el gemelo de "la otra voz siempre en 0".
    Script onlySlot(int slot) const {
        Script s(mStep, 0);
        s.mTotal = mTotal;
        s.mDistance = mDistance;
        for (const auto& f : mFingers) {
            if (f.slot == slot) s.mFingers.push_back(f);
        }
        return s;
    }
    /// Solo `slot`, y su ULTIMO dedo no se levanta nunca: la voz sostenida, referencia del release.
    Script sustained(int slot) const {
        Script s = onlySlot(slot);
        if (!s.mFingers.empty()) {
            auto last = std::max_element(
                s.mFingers.begin(), s.mFingers.end(),
                [](const Finger& a, const Finger& b) { return a.toFrame < b.toFrame; });
            last->toFrame = s.mTotal;
        }
        return s;
    }
    /// Sin ningun dedo: el render que contiene SOLO el dither.
    Script silent() const {
        Script s(mStep, 0);
        s.mTotal = mTotal;
        s.mDistance = mDistance;
        return s;
    }

    int stepFrames() const { return mStep; }
    int totalFrames() const { return mTotal; }
    int steps() const { return mTotal / mStep; }
    float distance() const { return mDistance; }
    const std::vector<Finger>& fingers() const { return mFingers; }

    void command(int slot, int frame, float* freq, float* amp) const {
        *freq = 0.0f;
        *amp = 0.0f;
        for (const auto& f : mFingers) {
            if (f.slot == slot && frame >= f.fromFrame && frame < f.toFrame) {
                *freq = f.freq;
                *amp = f.amp;
            }
        }
    }

private:
    int mStep;
    int mTotal;
    float mDistance = 0.5f;
    std::vector<Finger> mFingers;
};

struct Rendered {
    std::vector<float> mono;  // (L + R) / 2
    double peak = 0.0;        // max |L|, |R|
    bool allFinite = true;
    int stepFrames = kCoarseStepFrames;
    /// probe[slot][k] = bloques procesados por el slot ANTES del escalon k (k = 0 .. steps).
    std::vector<uint64_t> probe[2];

    uint64_t probeAtMs(int slot, double tMs) const {
        const auto k = static_cast<size_t>(std::lround(tMs * kFramesPerMs / stepFrames));
        const auto& p = probe[slot];
        return p.empty() ? 0 : p[std::min(k, p.size() - 1)];
    }
};

/// Renderiza un gesto dual por la puerta de afuera. Instancia fresca. SIN limpiar el dither.
Rendered renderRaw(int engineType, DualTouchMixMode mode, const Script& script) {
    Rendered r;
    r.stepFrames = script.stepFrames();
    for (const auto& f : script.fingers()) {
        EXPECT_EQ(f.fromFrame % script.stepFrames(), 0) << "el gesto no cae en el paso";
        EXPECT_EQ(f.toFrame % script.stepFrames(), 0) << "el gesto no cae en el paso";
    }

    AudioEngine engine;
    EXPECT_TRUE(engine.startOffline(kSampleRate, kMaxBlock));
    // Un reverb por defecto arruinaria el release: la cadena tiene que estar vacia.
    EXPECT_EQ(engine.getNumEffects(), 0u) << "la cadena de efectos del render (mEffectChain) no esta vacia";
    engine.setOscillatorEnabled(true);
    engine.setEngineType(engineType);
    if (engineType == kClassic) engine.setOscillatorType(kOscSine);
    engine.setDualTouchMixMode(mode);
    engine.setDualTouchMode(true);

    const int steps = script.steps();
    const int step = script.stepFrames();
    r.mono.reserve(static_cast<size_t>(steps) * static_cast<size_t>(step));
    r.probe[0].push_back(engine.dualTouchSlotBlocksRendered(0));
    r.probe[1].push_back(engine.dualTouchSlotBlocksRendered(1));

    std::vector<float> block(static_cast<size_t>(step) * 2, 0.0f);
    for (int k = 0; k < steps; ++k) {
        float f0, a0, f1, a1;
        script.command(0, k * step, &f0, &a0);
        script.command(1, k * step, &f1, &a1);
        engine.updateDualTouch(0.5f, 0.5f, f0, a0, 0.5f,   //
                               0.5f, 0.5f, f1, a1, 0.5f,   //
                               script.distance(), 0.0f);
        std::fill(block.begin(), block.end(), 0.0f);
        EXPECT_TRUE(engine.renderBlock(block.data(), nullptr, step));
        for (int n = 0; n < step; ++n) {
            const float l = block[static_cast<size_t>(n) * 2];
            const float rr = block[static_cast<size_t>(n) * 2 + 1];
            if (!std::isfinite(l) || !std::isfinite(rr)) r.allFinite = false;
            r.peak = std::max({r.peak, static_cast<double>(std::fabs(l)),
                               static_cast<double>(std::fabs(rr))});
            r.mono.push_back(0.5f * (l + rr));
        }
        r.probe[0].push_back(engine.dualTouchSlotBlocksRendered(0));
        r.probe[1].push_back(engine.dualTouchSlotBlocksRendered(1));
    }
    engine.stop();
    return r;
}

/**
 * El piso de dither de un render de `stepFrames` x `totalFrames`: el render en SILENCIO. El
 * dither (TPDF +-1 LSB de 16 bit, semilla fija) se suma DESPUES del soft-clip, asi que es aditivo y
 * depende solo del numero de muestras; no depende del engine (lo verifica
 * `TheDitherFloorIsTheSameForEveryEngineAndIsAdditive`).
 *
 * POR QUE SE RESTA EN TODOS LOS RENDERS. Es el paso 5 de medir-dsp: "antes de restar dos niveles,
 * confirma que tienen el MISMO piso". `dual - gemelo(otra)` cancela el dither pero el render de la
 * voz sola lo conserva; `dual - a - b` deja -1 vez el dither. En Karplus-Strong (la voz decae a 3e-4
 * entre re-excitaciones) y en Granular (huecos de 6e-5 entre granos) el dither pesa -20 dB o mas en
 * la banda y fabricaba deltas de hasta 1,6 dB que NO eran de la implementacion. Con todos los
 * renders limpios, cualquier resta o cociente compara voces contra voces.
 */
const std::vector<float>& ditherFloor(const Script& like) {
    static std::map<std::pair<int, int>, std::vector<float>> cache;
    const auto key = std::make_pair(like.stepFrames(), like.totalFrames());
    auto it = cache.find(key);
    if (it == cache.end()) {
        it = cache.emplace(key, renderRaw(kClassic, DualTouchMixMode::AVERAGE, like.silent()).mono).first;
    }
    return it->second;
}

/// Un render CON EL DITHER RESTADO (`peak` y `allFinite` son los de la salida real).
Rendered render(int engineType, DualTouchMixMode mode, const Script& script) {
    Rendered r = renderRaw(engineType, mode, script);
    const std::vector<float>& floorSignal = ditherFloor(script);
    for (size_t i = 0; i < r.mono.size() && i < floorSignal.size(); ++i) r.mono[i] -= floorSignal[i];
    return r;
}

/// Un dedo en modo NO dual (single touch): la referencia independiente de "ganancia 1".
std::vector<float> renderSingleTouch(float freq, float amp, int frames) {
    AudioEngine engine;
    EXPECT_TRUE(engine.startOffline(kSampleRate, kMaxBlock));
    engine.setOscillatorEnabled(true);
    engine.setEngineType(kClassic);
    engine.setOscillatorType(kOscSine);
    engine.setFrequencyAndAmplitude(freq, amp);
    std::vector<float> mono;
    std::vector<float> block(static_cast<size_t>(kCoarseStepFrames) * 2, 0.0f);
    for (int done = 0; done < frames; done += kCoarseStepFrames) {
        std::fill(block.begin(), block.end(), 0.0f);
        EXPECT_TRUE(engine.renderBlock(block.data(), nullptr, kCoarseStepFrames));
        for (int n = 0; n < kCoarseStepFrames; ++n) {
            mono.push_back(0.5f * (block[static_cast<size_t>(n) * 2] +
                                   block[static_cast<size_t>(n) * 2 + 1]));
        }
    }
    engine.stop();
    return mono;
}

// ===========================================================================
// Observables
// ===========================================================================

const std::vector<double>& hann() {
    static const std::vector<double> w = [] {
        std::vector<double> v(kWindowFrames);
        for (int i = 0; i < kWindowFrames; ++i) {
            v[static_cast<size_t>(i)] = 0.5 * (1.0 - std::cos(2.0 * M_PI * (i + 0.5) / kWindowFrames));
        }
        return v;
    }();
    return w;
}

/// Magnitud de Goertzel de la ventana de 20 ms que arranca en `start`, ventaneada con Hann.
double bandMagnitude(const std::vector<float>& x, long start, double hz) {
    if (start < 0 || start + kWindowFrames > static_cast<long>(x.size())) {
        ADD_FAILURE() << "ventana fuera de la senal: start=" << start << " size=" << x.size();
        return 1e-12;
    }
    std::vector<double> w(kWindowFrames);
    for (int i = 0; i < kWindowFrames; ++i) {
        w[static_cast<size_t>(i)] = x[static_cast<size_t>(start + i)] * hann()[static_cast<size_t>(i)];
    }
    return std::max(goertzel(w, kSampleRate, hz), 1e-12);
}

double bandDb(const std::vector<float>& x, long start, double hz) {
    return 20.0 * std::log10(bandMagnitude(x, start, hz));
}

/// Amplitud (pico) de una senoidal pura en `hz`: 2 * magnitud / suma de Hann.
double bandAmplitude(const std::vector<float>& x, long start, double hz) {
    double sumHann = 0.0;
    for (double v : hann()) sumHann += v;
    return 2.0 * bandMagnitude(x, start, hz) / sumHann;
}

std::vector<float> subtract(const std::vector<float>& a, const std::vector<float>& b) {
    std::vector<float> d(std::min(a.size(), b.size()));
    for (size_t i = 0; i < d.size(); ++i) d[i] = a[i] - b[i];
    return d;
}
std::vector<float> add(const std::vector<float>& a, const std::vector<float>& b) {
    std::vector<float> d(std::min(a.size(), b.size()));
    for (size_t i = 0; i < d.size(); ++i) d[i] = a[i] + b[i];
    return d;
}

struct Worst {
    double abs = 0.0;
    double signedValue = 0.0;
    double atMs = 0.0;
};

/// |dB_banda(a) - dB_banda(b)| en ventanas de 20 ms que arrancan cada `kHopFrames` desde
/// `firstStart` hasta `lastStart` (en el tiempo de SALIDA). Devuelve la peor.
Worst worstDeltaDb(const std::vector<float>& a, const std::vector<float>& b, double hz,
                   long firstStart, long lastStart) {
    Worst w;
    for (long s = firstStart; s <= lastStart; s += kHopFrames) {
        const double d = bandDb(a, s, hz) - bandDb(b, s, hz);
        if (std::fabs(d) > w.abs) {
            w.abs = std::fabs(d);
            w.signedValue = d;
            w.atMs = static_cast<double>(s) / kFramesPerMs;
        }
    }
    return w;
}

/// Evento de entrada en `tMs` => primer cuadro en que aparece en la SALIDA.
long outFrame(double tMs) { return ms(tMs) + kOutputLatencyFrames; }

// --- release ---------------------------------------------------------------

/// dB de la banda de `lifted` respecto de `sustained` en las ventanas [0,20) .. [80,100) ms
/// contadas desde que el soltar aparece en la salida.
std::vector<double> releaseSeries(const std::vector<float>& lifted,
                                  const std::vector<float>& sustained, double hz, long liftOut) {
    std::vector<double> s;
    for (int k = 0; k < kReleaseWindows; ++k) {
        const long start = liftOut + static_cast<long>(k) * kWindowFrames;
        s.push_back(bandDb(lifted, start, hz) - bandDb(sustained, start, hz));
    }
    return s;
}

struct ReleaseVerdict {
    bool notCut = false;      // [20,40) >= regimen - 40 dB
    bool decaying = false;    // [20,40) <= regimen - 10 dB (exponencial, no lineal)
    bool deepEnough = false;  // [80,100) <= regimen - 60 dB
    bool monotone = false;    // ninguna ventana crece respecto de la anterior
    bool ok() const { return notCut && decaying && deepEnough && monotone; }
};

ReleaseVerdict judgeRelease(const std::vector<double>& s) {
    ReleaseVerdict v;
    v.notCut = s[1] >= kReleaseNotCutMinDb;
    v.decaying = s[1] <= kReleaseDecayMaxDb;
    v.deepEnough = s[kReleaseWindows - 1] <= kReleaseDeepMaxDb;
    v.monotone = true;
    for (size_t k = 1; k < s.size(); ++k) {
        // Por debajo de kMonotoneFloorDb se esta midiendo el residuo de la resta, no la voz.
        if (s[k - 1] > kMonotoneFloorDb && s[k] > s[k - 1] + kMonotoneSlackDb) v.monotone = false;
    }
    return v;
}

std::string describeSeries(const std::vector<double>& s) {
    std::string out = "[dB re sostenido, ventanas de 20 ms desde el soltar:";
    for (double d : s) out += " " + std::to_string(d);
    return out + "]";
}

struct SilenceReading {
    double audiblePeak = 0.0;      // pico de la salida por encima de kSilenceHighPassHz
    double bandDbReRegime = 0.0;   // banda de la fundamental, re el mayor nivel de regimen
};

/// `lifted` y `sustained` ya sin dither. Regimen = el mayor nivel de banda de `sustained` en los
/// 100 ms previos al soltar (un cociente contra la MISMA ventana compararia dos pisos cuando la
/// voz sostenida tiene huecos —Granular—).
SilenceReading readSilence(const std::vector<float>& lifted, const std::vector<float>& sustained,
                           double hz, long liftOut) {
    SilenceReading r;
    const double a = std::exp(-2.0 * M_PI * kSilenceHighPassHz / kSampleRate);
    double y = 0.0, xPrev = 0.0;
    const long from = liftOut + static_cast<long>(kSilenceAfterMs * kFramesPerMs);
    for (long n = 0; n < static_cast<long>(lifted.size()); ++n) {
        const double x = lifted[static_cast<size_t>(n)];
        y = a * (y + x - xPrev);
        xPrev = x;
        if (n >= from) r.audiblePeak = std::max(r.audiblePeak, std::fabs(y));
    }
    double regime = -300.0;
    for (long s = liftOut - static_cast<long>(100.0 * kFramesPerMs); s + kWindowFrames <= liftOut; s += kHopFrames) {
        regime = std::max(regime, bandDb(sustained, s, hz));
    }
    r.bandDbReRegime = bandDb(lifted, from, hz) - regime;
    return r;
}

/// Aplica a `x` una ganancia `g(tMs)` desde el cuadro `fromFrame` (cuadros previos intactos).
std::vector<float> withGain(const std::vector<float>& x, long fromFrame,
                            const std::function<double(double)>& g) {
    std::vector<float> y = x;
    for (long n = fromFrame; n < static_cast<long>(y.size()); ++n) {
        y[static_cast<size_t>(n)] *= static_cast<float>(g(static_cast<double>(n - fromFrame) / kFramesPerMs));
    }
    return y;
}

// --- click -------------------------------------------------------------------

double maxStep(const std::vector<float>& x, long from, long to, bool useAbs = false) {
    double m = 0.0;
    const long lo = std::max<long>(from, 1);
    const long hi = std::min<long>(to, static_cast<long>(x.size()));
    for (long n = lo; n < hi; ++n) {
        const double a = useAbs ? std::fabs(x[static_cast<size_t>(n)]) : x[static_cast<size_t>(n)];
        const double b = useAbs ? std::fabs(x[static_cast<size_t>(n - 1)]) : x[static_cast<size_t>(n - 1)];
        m = std::max(m, std::fabs(a - b));
    }
    return m;
}

double maxAbs(const std::vector<float>& x, long from, long to) {
    double m = 0.0;
    const long lo = std::max<long>(from, 0);
    const long hi = std::min<long>(to, static_cast<long>(x.size()));
    for (long n = lo; n < hi; ++n) m = std::max(m, static_cast<double>(std::fabs(x[static_cast<size_t>(n)])));
    return m;
}

/// Pico en los `span` cuadros siguientes al primer cuadro de [from,to) con |x| > onsetLevel.
/// -1 si no hay onset.
double peakAfterOnset(const std::vector<float>& x, long from, long to, double onsetLevel, int span) {
    const long hi = std::min<long>(to, static_cast<long>(x.size()));
    for (long n = std::max<long>(from, 0); n < hi; ++n) {
        if (std::fabs(x[static_cast<size_t>(n)]) > onsetLevel) {
            return maxAbs(x, n, n + span);
        }
    }
    return -1.0;
}

}  // namespace

// ===========================================================================
// Parametros
// ===========================================================================

struct EngineCase {
    int engineIndex;
};
void PrintTo(const EngineCase& c, std::ostream* os) { *os << kEngines[c.engineIndex].name; }

struct ModeCase {
    int engineIndex;
    DualTouchMixMode mode;
    int order;  // ver cada test
};
void PrintTo(const ModeCase& c, std::ostream* os) {
    *os << kEngines[c.engineIndex].name << "/" << modeName(c.mode) << "/" << c.order;
}

namespace {

// ---------------------------------------------------------------------------
// El gesto estandar 1 -> 2 -> 1 -> 0 de los tests por engine
// ---------------------------------------------------------------------------
constexpr double kLeadMs = 20.0;           // silencio inicial: el crossfade del engine termina antes del 1er dedo
constexpr double kFirstDownMs = 20.0;
constexpr double kSecondDownMs = 160.0;    // 140 ms con una sola voz (calentamiento 40 + ventanas)
constexpr double kExitFirstMs = 320.0;     // 160 ms con las dos
constexpr double kExitLastMs = 480.0;      // 160 ms con la que queda (> 120 de kEngineDoneMs)
constexpr double kGestureTotalMs = 730.0;  // 250 ms de cola: release (80) + silencio (> kSilenceAfterMs)
constexpr double kPostEntryMs = 100.0;     // AC-052.1: cuanto se mide despues de que cae el 2.o dedo
constexpr double kBeforeExitMs = 60.0;     // AC-052.2: cuanto se mide ANTES de que salga el primero
constexpr double kAfterExitMs = 100.0;     // AC-052.2: y despues

static_assert(kLeadMs <= kFirstDownMs, "el lead precede al primer dedo");

Script standardGesture(const EngineInfo& e, int firstSlot, int exitFirstSlot) {
    const int secondSlot = 1 - firstSlot;
    Script s(kCoarseStepFrames, kGestureTotalMs);
    s.touch(firstSlot, kFirstDownMs, firstSlot == exitFirstSlot ? kExitFirstMs : kExitLastMs,
            freqOfSlot(firstSlot), e.amp);
    s.touch(secondSlot, kSecondDownMs, secondSlot == exitFirstSlot ? kExitFirstMs : kExitLastMs,
            freqOfSlot(secondSlot), e.amp);
    return s;
}

}  // namespace

// ===========================================================================
// Fixtures
// ===========================================================================

class DualTouchByEngine : public ::testing::TestWithParam<EngineCase> {};
class DualTouchByMode : public ::testing::TestWithParam<ModeCase> {};
/// Igual que DualTouchByMode pero sin eje de orden (controles que no dependen de el).
class DualTouchByEngineAndMode : public ::testing::TestWithParam<ModeCase> {};

// ===========================================================================
// 0. Determinismo, antes que nada
// ===========================================================================

TEST_P(DualTouchByEngine, TheDualGestureRendersIdenticallyTwiceOnFreshInstances) {
    // PRECONDICION de todo lo demas: si esto se cae, los gemelos y las diferencias de abajo
    // miden ruido. Cubre los seis engines (Supersaw "distribucion al azar", Granular con RNG).
    const EngineInfo& e = kEngines[GetParam().engineIndex];
    const Script g = standardGesture(e, 0, 0);

    const Rendered a = render(e.type, DualTouchMixMode::AVERAGE, g);
    const Rendered b = render(e.type, DualTouchMixMode::AVERAGE, g);

    ASSERT_EQ(a.mono.size(), b.mono.size());
    ASSERT_GT(maxAbs(a.mono, 0, static_cast<long>(a.mono.size())), kAudible)
        << e.name << ": el motor no esta sonando, no hay nada que medir";
    for (size_t i = 0; i < a.mono.size(); ++i) {
        ASSERT_FLOAT_EQ(a.mono[i], b.mono[i]) << e.name << ": no determinista, cuadro " << i;
    }
}

TEST_P(DualTouchByEngine, TheTwinIsExactlyTheDualRenderBeforeTheSecondFingerLands) {
    // Validez del gemelo (AC-052.1/.2): mientras el 2.o slot no entro, "dual" y "solo el 1.o"
    // tienen que ser la MISMA senal. Si un RNG o un estado compartido entre las dos instancias de
    // engine (primaria y de dual touch) los separara, el gemelo seria otra senal.
    // Tolerancia 1e-6: multiplicar por 1 / sumar 0 en float no cambia nada; es el margen de
    // redondeo de una implementacion que sume los dos buffers.
    constexpr double kTwinExactnessAbs = 1e-6;
    const EngineInfo& e = kEngines[GetParam().engineIndex];
    const Script g = standardGesture(e, 0, 0);
    const Rendered dual = render(e.type, DualTouchMixMode::AVERAGE, g);
    const Rendered solo = render(e.type, DualTouchMixMode::AVERAGE, g.onlySlot(0));

    const long end = outFrame(kSecondDownMs);
    double worst = 0.0;
    for (long n = 0; n < end; ++n) {
        worst = std::max(worst, static_cast<double>(std::fabs(dual.mono[static_cast<size_t>(n)] -
                                                              solo.mono[static_cast<size_t>(n)])));
    }
    EXPECT_LE(worst, kTwinExactnessAbs) << e.name << ": el gemelo no reproduce al dual con un solo dedo";
    EXPECT_GT(maxAbs(solo.mono, 0, end), kAudible) << e.name << ": el solo no suena";
}

// ===========================================================================
// Controles del instrumento (paso 6 de medir-dsp)
// ===========================================================================

TEST_P(DualTouchByEngine, TheDitherFloorIsTheSameForEveryEngineAndIsAdditive) {
    // Control de `ditherFloor`: (1) el render en silencio de cada engine es BIT A BIT el que se
    // resta (si dependiera del engine, restarlo fabricaria una senal); (2) es aditivo: un render con
    // voz menos el piso no deja el piso (el dither de la voz sola esta en el piso a +-1 LSB).
    const EngineInfo& e = kEngines[GetParam().engineIndex];
    const Script g = standardGesture(e, 0, 0);
    const Rendered silent = renderRaw(e.type, DualTouchMixMode::AVERAGE, g.silent());
    const std::vector<float>& floorSignal = ditherFloor(g);
    ASSERT_EQ(silent.mono.size(), floorSignal.size());
    for (size_t i = 0; i < silent.mono.size(); ++i) {
        ASSERT_EQ(silent.mono[i], floorSignal[i]) << e.name << ": el piso depende del engine, cuadro " << i;
    }
    // El piso es dither de verdad (no cero): pico ~1 LSB de 16 bit = 3,05e-5.
    EXPECT_GT(maxAbs(floorSignal, 0, static_cast<long>(floorSignal.size())), 1e-5);
    // Aditivo: la cola de un render con una voz (cuando ya no hay voz) menos el piso es << el piso.
    const Rendered voiced = render(e.type, DualTouchMixMode::AVERAGE, g.onlySlot(0));
    const long tail = outFrame(kGestureTotalMs - 80.0);
    EXPECT_LT(maxAbs(voiced.mono, tail, static_cast<long>(voiced.mono.size()) - 1), 1e-5) << e.name;
}

TEST_P(DualTouchByEngine, TheDifferenceMethodIsolatesAVoiceAndSeesTheOldHalvingLaw) {
    // Control de ANULACION del metodo de la DIFERENCIA (paso 6 de medir-dsp). El brief recomienda
    // medir la banda de V directamente en el dual y controlar que la otra voz quede >= 30 dB abajo.
    // MEDIDO: no se cumple. Karplus-Strong re-excita con rafagas de RUIDO BLANCO (a ~365 ms la
    // rafaga de una voz mete la banda de la otra a -3,5 dB de su nivel; el cross-talk medido es de
    // 26 dB para 500 Hz y 17 dB para 780 Hz), y la entrada de una voz salpica el espectro de la otra.
    // Por eso AC-052.1/.2 miden sobre `dual - gemelo de la OTRA voz`, que la cancela entera.
    //
    // Este control verifica las dos puntas con senales fabricadas desde los gemelos:
    //   - la suma ideal onlyF + onlyS, aislada restando onlyS, reproduce onlyF (|delta| ~ 0): el
    //     metodo no inventa un delta;
    //   - la ley VIEJA (b1+b2)*0,5 en la zona de dos dedos, aislada igual, se ve a >= 3 dB de
    //     onlyF: el metodo puede ponerse rojo.
    constexpr double kIsolationExactDb = 0.05;   // redondeo de float de una suma y una resta
    constexpr double kOldLawVisibleMinDb = 3.0;  // la ley vieja son -6 dB; con la fuga de la otra voz >= 3
    const EngineInfo& e = kEngines[GetParam().engineIndex];
    const Script g = standardGesture(e, 0, 0);
    const Rendered only0 = render(e.type, DualTouchMixMode::AVERAGE, g.onlySlot(0));
    const Rendered only1 = render(e.type, DualTouchMixMode::AVERAGE, g.onlySlot(1));

    const std::vector<float> ideal = add(only0.mono, only1.mono);
    const long first = outFrame(kFirstDownMs + kSettleMs);
    const long last = outFrame(kExitFirstMs) - kWindowFrames;
    const Worst idealWorst = worstDeltaDb(subtract(ideal, only1.mono), only0.mono, kFreqSlot0, first, last);
    EXPECT_LE(idealWorst.abs, kIsolationExactDb)
        << e.name << ": la suma ideal, aislada, no reproduce la voz (delta " << idealWorst.signedValue << " dB)";

    std::vector<float> old = ideal;
    const long bothFrom = outFrame(kSecondDownMs);
    const long bothTo = outFrame(kExitFirstMs);
    for (long n = bothFrom; n < bothTo; ++n) old[static_cast<size_t>(n)] *= 0.5f;
    const Worst oldWorst = worstDeltaDb(subtract(old, only1.mono), only0.mono, kFreqSlot0,
                                        bothFrom + 4 * kHopFrames, bothTo - kWindowFrames);
    EXPECT_GE(oldWorst.abs, kOldLawVisibleMinDb)
        << e.name << ": el metodo no ve la ley vieja (delta " << oldWorst.signedValue << " dB)";
}

TEST_P(DualTouchByEngine, TheMeterSeesAHalvedVoiceAsMinusSixDb) {
    // Control: el medidor de banda detecta un cambio de amplitud conocido. Renderiza la voz a
    // amp y a amp/2 y compara la banda; verifica de paso que cada engine es LINEAL en amp (si no
    // lo fuera, comparar a otra amp no valdria). `render` ya resta el dither: Karplus-Strong baja a
    // -56 dB y ahi el dither mueve la lectura 0,6 dB.
    const EngineInfo& e = kEngines[GetParam().engineIndex];
    Script full(kCoarseStepFrames, kGestureTotalMs);
    full.touch(0, kFirstDownMs, kExitFirstMs, kFreqSlot0, e.amp);
    Script half(kCoarseStepFrames, kGestureTotalMs);
    half.touch(0, kFirstDownMs, kExitFirstMs, kFreqSlot0, e.amp * 0.5f);

    const std::vector<float> a = render(e.type, DualTouchMixMode::AVERAGE, full).mono;
    const std::vector<float> b = render(e.type, DualTouchMixMode::AVERAGE, half).mono;
    const long first = outFrame(kFirstDownMs + kSettleMs);
    const long last = outFrame(kExitFirstMs) - kWindowFrames;
    const Worst w = worstDeltaDb(b, a, kFreqSlot0, first, last);
    EXPECT_NEAR(w.signedValue, -6.02, 0.3) << e.name << " no es lineal en amp o el medidor no ve -6 dB";
}

TEST_P(DualTouchByEngine, TheSoftClipCrossModulationAtTheChosenAmplitudeStaysUnderTheLinearityBudget) {
    // Control PREDICTIVO del de abajo, que se puede correr ANTES de que exista la implementacion:
    // reconstruye la suma ideal ANTES de la cadena invirtiendo el soft-clip de cada voz sola
    // (r = 1,5 tanh(0,666 x) => x = atanh(r/1,5)/0,666; `render` ya resta el dither), las suma, la vuelve a pasar por el soft-clip y mide el residuo
    //    soft-clip(x0 + x1) - r0 - r1
    // en la banda de cada voz. Es lo que el control de linealidad va a ver con la ley nueva
    // (out = voz1 + voz2): si esto supera -60 dB a la amplitud elegida, hay que bajar la amplitud
    // del engine, no aflojar el umbral. Depende de las constantes de `SoftClipper` (TANH).
    constexpr double kLinearityBudgetDb = -60.0;  // brief
    const EngineInfo& e = kEngines[GetParam().engineIndex];
    const Script g = standardGesture(e, 0, 0);
    const std::vector<float> r0 = render(e.type, DualTouchMixMode::AVERAGE, g.onlySlot(0)).mono;
    const std::vector<float> r1 = render(e.type, DualTouchMixMode::AVERAGE, g.onlySlot(1)).mono;

    std::vector<float> residual(r0.size());
    for (size_t n = 0; n < r0.size(); ++n) {
        const auto pre = [](double r) { return std::atanh(std::clamp(r / 1.5, -0.999, 0.999)) / 0.666; };
        const double sum = pre(r0[n]) + pre(r1[n]);
        residual[n] = static_cast<float>(1.5 * std::tanh(0.666 * sum) - r0[n] - r1[n]);
    }
    const long first = outFrame(kSecondDownMs + kSettleMs);
    const long last = outFrame(kExitFirstMs) - kWindowFrames;
    for (int v = 0; v < 2; ++v) {
        const std::vector<float>& own = v == 0 ? r0 : r1;
        double worstDb = -300.0;
        for (long s = first; s <= last; s += kHopFrames) {
            worstDb = std::max(worstDb, bandDb(residual, s, freqOfSlot(v)) - bandDb(own, s, freqOfSlot(v)));
        }
        EXPECT_LE(worstDb, kLinearityBudgetDb)
            << e.name << ": la compresion cruzada del soft-clip en la banda de " << freqOfSlot(v)
            << " Hz es " << worstDb << " dB re la voz: bajar la amplitud del engine";
    }
}

TEST_P(DualTouchByEngineAndMode, TheChainIsLinearEnoughForTheDifferenceMethod) {
    // Control del metodo de la DIFERENCIA (AC-052.3): con las dos voces estables,
    //    dual - onlyA - onlyB
    // tiene que ser << la voz (<= -60 dB en la banda de cada una). Si la cadena fuera no lineal
    // —el soft-clip compresa una voz por otra a ~0,22 a^2— la diferencia no aislaria a la voz que
    // sale. CON EL CODIGO ACTUAL ESTE CONTROL DA ROJO A PROPOSITO: la ley vieja (x0,5) no es
    // la suma; es lo que el cambio arregla. Queda verde si y solo si out = voz1 + voz2.
    constexpr double kLinearityMaxDb = -60.0;  // brief
    const ModeCase c = GetParam();
    const EngineInfo& e = kEngines[c.engineIndex];
    const Script g = standardGesture(e, 0, 0);
    const Rendered dual = render(e.type, c.mode, g);
    const Rendered only0 = render(e.type, c.mode, g.onlySlot(0));
    const Rendered only1 = render(e.type, c.mode, g.onlySlot(1));

    const std::vector<float> residual = subtract(subtract(dual.mono, only0.mono), only1.mono);
    const long first = outFrame(kSecondDownMs + kSettleMs);
    const long last = outFrame(kExitFirstMs) - kWindowFrames;
    for (int v = 0; v < 2; ++v) {
        const Rendered& own = v == 0 ? only0 : only1;
        double worstDb = -300.0;
        for (long s = first; s <= last; s += kHopFrames) {
            worstDb = std::max(worstDb, bandDb(residual, s, freqOfSlot(v)) - bandDb(own.mono, s, freqOfSlot(v)));
        }
        EXPECT_LE(worstDb, kLinearityMaxDb)
            << e.name << "/" << modeName(c.mode) << ": dual - voz0 - voz1, banda de " << freqOfSlot(v)
            << " Hz, peor ventana " << worstDb << " dB re la voz";
    }
}

TEST(DualTouchEnvelopeInstrument, TheReleaseVerdictAcceptsTheSpecEnvelopeAndRejectsEachMutant) {
    // Los veredictos de release (AC-052.3/.6) ejercidos sobre una voz SOSTENIDA real (Classic
    // seno) multiplicada por envolventes fabricadas. Es la tabla mutante -> assert de los
    // criterios de release SIN tocar produccion:
    //    spec (-60 dB en 80 ms) ........ pasa los tres
    //    corte en seco ................. notCut rojo
    //    release de 25 ms .............. notCut rojo
    //    release de 160 ms ............. deepEnough rojo
    //    rampa lineal de 100 ms ........ deepEnough rojo
    //    release con un bache que sube . monotone rojo
    const EngineInfo& e = kEngines[kClassic];
    Script g(kCoarseStepFrames, kGestureTotalMs);
    g.touch(0, kFirstDownMs, kGestureTotalMs, kFreqSlot0, e.amp);
    const Rendered sustained = render(e.type, DualTouchMixMode::AVERAGE, g);
    const long liftOut = outFrame(kExitFirstMs);
    const auto exp60 = [](double t80) {
        return [t80](double tMs) { return std::pow(10.0, -3.0 * tMs / t80); };
    };
    const auto seriesFor = [&](const std::function<double(double)>& gain) {
        return releaseSeries(withGain(sustained.mono, liftOut, gain), sustained.mono, kFreqSlot0, liftOut);
    };

    const auto spec = seriesFor(exp60(80.0));
    EXPECT_TRUE(judgeRelease(spec).ok()) << "la envolvente del spec tiene que pasar: " << describeSeries(spec);

    const auto cut = seriesFor([](double) { return 0.0; });
    EXPECT_FALSE(judgeRelease(cut).notCut) << describeSeries(cut);

    const auto fast = seriesFor(exp60(25.0));
    EXPECT_FALSE(judgeRelease(fast).notCut) << describeSeries(fast);

    const auto slow = seriesFor(exp60(160.0));
    EXPECT_TRUE(judgeRelease(slow).notCut);
    EXPECT_FALSE(judgeRelease(slow).deepEnough) << describeSeries(slow);

    // D4: el release es EXPONENCIAL. La rampa lineal de 80 ms cumple "no cortado" y "profundo"
    // (llega a 0 en 80 ms) y solo la cota superior de [20,40) la distingue.
    const auto linear80 = seriesFor([](double t) { return std::max(0.0, 1.0 - t / 80.0); });
    EXPECT_TRUE(judgeRelease(linear80).notCut);
    EXPECT_TRUE(judgeRelease(linear80).deepEnough);
    EXPECT_FALSE(judgeRelease(linear80).decaying) << describeSeries(linear80);
    EXPECT_FALSE(judgeRelease(linear80).ok());

    const auto linear = seriesFor([](double t) { return std::max(0.0, 1.0 - t / 100.0); });
    EXPECT_FALSE(judgeRelease(linear).deepEnough) << describeSeries(linear);

    // Veredicto de SILENCIO (AC-052.6): una cola de CC sub-audible (como la del DC blocker con
    // Karplus-Strong/Granular) NO es ruido; un zumbido audible de la misma amplitud SI; una voz
    // que cae solo a -60 dB y se queda ahi tampoco es silencio.
    {
        const long lift = liftOut;
        const auto tail = [&](const std::function<double(double)>& dc, const std::function<double(double)>& gain) {
            std::vector<float> y = withGain(sustained.mono, lift, gain);
            for (long n = lift; n < static_cast<long>(y.size()); ++n) {
                const double t = static_cast<double>(n - lift) / kFramesPerMs;
                y[static_cast<size_t>(n)] += static_cast<float>(dc(t));
            }
            return readSilence(y, sustained.mono, kFreqSlot0, lift);
        };
        const auto none = [](double) { return 0.0; };
        const auto stopAt80 = [](double t) { return t < 80.0 ? std::pow(10.0, -3.0 * t / 80.0) : 0.0; };
        const SilenceReading clean = tail(none, stopAt80);
        EXPECT_LE(clean.audiblePeak, kSilenceFloor);
        EXPECT_LE(clean.bandDbReRegime, kSilenceBandMaxDb);
        // CC de 2,5e-5 con tau 54 ms (mas que lo medido: 1,9e-5): pasa.
        const SilenceReading dcTail = tail([](double t) { return 2.5e-5 * std::exp(-(t - 0.0) / 54.0) * std::exp(150.0 / 54.0); }, stopAt80);
        EXPECT_LE(dcTail.audiblePeak, kSilenceFloor) << "la cola de CC sub-audible no debe contar como ruido";
        // Zumbido audible de 3e-5 a 1 kHz (la fundamental): se ve en las dos.
        const SilenceReading hum = tail([](double t) { return 3e-5 * std::sin(2.0 * M_PI * 1000.0 * t / 1000.0); }, stopAt80);
        EXPECT_GT(hum.audiblePeak, kSilenceFloor);
        // La voz que se queda en -60 dB: la banda lo ve.
        const SilenceReading held = tail(none, [](double t) { return std::max(1e-3, std::pow(10.0, -3.0 * t / 80.0)); });
        EXPECT_GT(held.bandDbReRegime, kSilenceBandMaxDb) << "una voz clavada a -60 dB no es silencio";
    }

    const auto bump = seriesFor([](double t) {
        return std::pow(10.0, -3.0 * t / 80.0) * ((t > 45.0 && t < 65.0) ? 50.0 : 1.0);
    });
    EXPECT_FALSE(judgeRelease(bump).monotone) << describeSeries(bump);
}

TEST(DualTouchEnvelopeInstrument, TheClickMetersSeeACutAndARamplessEntryOnAPureSine) {
    // Control de AC-052.4/.5: sobre un seno puro, el medidor de |dx| tiene que dejar pasar la
    // envolvente del spec (ataque lineal 5 ms, release -60 dB/80 ms) y marcar el corte en seco
    // y la entrada sin rampa, en CUALQUIER fase de caida del evento (se barren los cuatro cuartos
    // de ciclo a 1 kHz).
    Script g(kFineStepFrames, 400.0);
    g.touch(0, 20.0, 400.0, kFineFreq, kFineAmp);
    const Rendered sustained = render(kClassic, DualTouchMixMode::AVERAGE, g);
    const long regimeFrom = outFrame(60.0);
    const long regimeTo = outFrame(120.0);
    const double regime = maxStep(sustained.mono, regimeFrom, regimeTo);
    const double regimePeak = maxAbs(sustained.mono, regimeFrom, regimeTo);
    ASSERT_NEAR(regime, regimePeak * 2.0 * M_PI * kFineFreq / kSampleRate,
                regimePeak * 2.0 * M_PI * kFineFreq / kSampleRate * kSineStepModelTolerance)
        << "lo medido no es un seno limpio";

    for (int quarter = 0; quarter < 4; ++quarter) {
        const long at = outFrame(200.0 + kOffZeroCrossingMs + 0.25 * quarter);
        // Conforme al spec: release exponencial, y luego entrada con rampa de 5 ms.
        const auto specRelease = withGain(sustained.mono, at, [](double t) { return std::pow(10.0, -3.0 * t / 80.0); });
        EXPECT_LE(maxStep(specRelease, at - ms(kEdgeGuardMs), at + ms(kReleaseClickWindowMs)),
                  regime * kClickTolerance)
            << "el release del spec no debe verse como click (cuarto " << quarter << ")";
        std::vector<float> specAttack(sustained.mono.size(), 0.0f);  // silencio y luego la rampa de 5 ms
        for (long n = at; n < static_cast<long>(specAttack.size()); ++n) {
            const double t = static_cast<double>(n - at) / kFramesPerMs;
            specAttack[static_cast<size_t>(n)] = sustained.mono[static_cast<size_t>(n)] * static_cast<float>(std::min(1.0, t / 5.0));
        }
        EXPECT_LE(maxStep(specAttack, at - ms(kEdgeGuardMs), at + ms(kAttackClickWindowMs)),
                  regime * kClickTolerance)
            << "el ataque del spec no debe verse como click (cuarto " << quarter << ")";
    }

    // Mutantes: corte en seco (release 0) y entrada sin rampa (silencio y luego la senal a
    // ganancia 1). El escalon vale |sin(fase)| * pico = 0,707 pico en las cuatro posiciones.
    int cutsSeen = 0;
    int ramplessSeen = 0;
    for (int quarter = 0; quarter < 4; ++quarter) {
        const long at = outFrame(200.0 + kOffZeroCrossingMs + 0.25 * quarter);
        const auto cut = withGain(sustained.mono, at, [](double) { return 0.0; });
        if (maxStep(cut, at - ms(kEdgeGuardMs), at + ms(kReleaseClickWindowMs)) > regime * kClickTolerance) ++cutsSeen;
        std::vector<float> rampless(sustained.mono.size(), 0.0f);
        std::copy(sustained.mono.begin() + at, sustained.mono.end(), rampless.begin() + at);
        if (maxStep(rampless, at - ms(kEdgeGuardMs), at + ms(kAttackClickWindowMs)) > regime * kClickTolerance) ++ramplessSeen;
    }
    EXPECT_EQ(cutsSeen, 4) << "el medidor no ve el corte en seco en las cuatro fases";
    EXPECT_EQ(ramplessSeen, 4) << "el medidor no ve la entrada sin rampa en las cuatro fases";
}

// ===========================================================================
// AC-052.1 — al apoyar el 2.o dedo, la voz que ya sonaba mantiene su nivel
// ===========================================================================

TEST_P(DualTouchByMode, Ac0521_TheVoiceThatWasSoundingKeepsItsLevelWhenTheSecondFingerLands) {
    // order = que slot cae PRIMERO (0: el principal; 1: el otro).
    //
    // Observable: dB de la banda de la voz F en el dual MENOS dB de la banda de F en su render
    // gemelo (el mismo gesto con la otra voz siempre en 0), ventanas de 20 ms con salto de 5 ms
    // desde que F esta calentada hasta 100 ms despues de que cae S. Para que S no contamine la
    // banda de F, el dual se AISLA restandole el gemelo de S (ver el control del metodo). Bug que
    // atrapa: la ley (b1+b2)*0,5 baja a F -6 dB al entrar S.
    const ModeCase c = GetParam();
    const EngineInfo& e = kEngines[c.engineIndex];
    const int first = c.order;
    const int second = 1 - first;
    const Script g = standardGesture(e, first, first);

    const Rendered dual = render(e.type, c.mode, g);
    const Rendered onlyFirst = render(e.type, c.mode, g.onlySlot(first));
    const Rendered onlySecond = render(e.type, c.mode, g.onlySlot(second));

    // Precondicion: suma bajo el umbral del limitador, o el limitador actua y el test mide otra cosa.
    ASSERT_LT(dual.peak, kLimiterThreshold) << e.name << ": la suma supera el umbral del limitador";
    ASSERT_GT(maxAbs(onlyFirst.mono, 0, static_cast<long>(onlyFirst.mono.size())), kAudible)
        << e.name << ": la voz de referencia no suena";

    // Cada voz se aisla restando el gemelo de la OTRA (ver el control de arriba: medir la banda
    // directa deja pasar las rafagas de ruido de Karplus-Strong y el transitorio de entrada).
    const std::vector<float> firstIsolated = subtract(dual.mono, onlySecond.mono);
    const std::vector<float> secondIsolated = subtract(dual.mono, onlyFirst.mono);

    const Worst firstVoice = worstDeltaDb(
        firstIsolated, onlyFirst.mono, freqOfSlot(first), outFrame(kFirstDownMs + kSettleMs),
        outFrame(kSecondDownMs + kPostEntryMs));
    EXPECT_LE(firstVoice.abs, kLevelToleranceDb)
        << "AC-052.1 " << e.name << "/" << modeName(c.mode) << ": la voz que ya sonaba (slot " << first
        << ") cambio " << firstVoice.signedValue << " dB respecto de su gemelo, en la ventana de t="
        << firstVoice.atMs << " ms (el 2.o dedo cae en t=" << kSecondDownMs << " ms)";

    // La voz que entra suena a ganancia 1 (SUM/AVERAGE: out = voz1 + voz2) mientras estan las dos.
    const Worst secondVoice = worstDeltaDb(
        secondIsolated, onlySecond.mono, freqOfSlot(second), outFrame(kSecondDownMs + kSettleMs),
        outFrame(kExitFirstMs) - kWindowFrames);
    EXPECT_LE(secondVoice.abs, kLevelToleranceDb)
        << "AC-052.1 " << e.name << "/" << modeName(c.mode) << ": la voz que entra (slot " << second
        << ") no suena a ganancia 1: " << secondVoice.signedValue << " dB en t=" << secondVoice.atMs << " ms";
}

// ===========================================================================
// AC-052.2 — al levantar cualquiera de las dos, la que queda mantiene su nivel
// ===========================================================================

TEST_P(DualTouchByMode, Ac0522_TheVoiceThatRemainsKeepsItsLevelWhenTheOtherFingerLifts) {
    // order = que slot SALE primero (0: el principal, 1: el otro). El slot 0 siempre cae primero.
    // Observable: igual que AC-052.1, sobre la banda de la voz que queda, de 60 ms antes del
    // soltar a 100 ms despues. Bug que atrapa: la voz que queda SUBE 6 dB cuando la otra sale.
    const ModeCase c = GetParam();
    const EngineInfo& e = kEngines[c.engineIndex];
    const int leaving = c.order;
    const int remaining = 1 - leaving;
    const Script g = standardGesture(e, 0, leaving);

    const Rendered dual = render(e.type, c.mode, g);
    const Rendered onlyRemaining = render(e.type, c.mode, g.onlySlot(remaining));
    const Rendered onlyLeaving = render(e.type, c.mode, g.onlySlot(leaving));
    ASSERT_LT(dual.peak, kLimiterThreshold) << e.name << ": la suma supera el umbral del limitador";

    // Aislada la voz que queda: dual - gemelo de la que sale (cancela su release y sus rafagas).
    const Worst w = worstDeltaDb(subtract(dual.mono, onlyLeaving.mono), onlyRemaining.mono, freqOfSlot(remaining),
                                 outFrame(kExitFirstMs - kBeforeExitMs),
                                 outFrame(kExitFirstMs + kAfterExitMs));
    EXPECT_LE(w.abs, kLevelToleranceDb)
        << "AC-052.2 " << e.name << "/" << modeName(c.mode) << ": al levantar el slot " << leaving
        << " la voz del slot " << remaining << " cambio " << w.signedValue
        << " dB respecto de su gemelo, en la ventana de t=" << w.atMs << " ms (soltar en t="
        << kExitFirstMs << " ms)";
}

// ===========================================================================
// AC-052.3 — el slot que sale hace release y su engine deja de procesarse
// ===========================================================================

TEST_P(DualTouchByMode, Ac0523_TheLeavingSlotReleasesWhileTheOtherVoiceKeepsSounding) {
    // order = que slot sale primero. Observable: la banda de la voz que sale, aislada por la
    // DIFERENCIA muestra a muestra `dual - gemelo de la otra voz`, respecto de la misma voz
    // SOSTENIDA (para descontar la evolucion propia del engine), en ventanas de 20 ms desde el
    // soltar. Mas la sonda: crece durante el release y se congela despues.
    // Bugs que atrapa: sin release (corte en seco), release de ~25 ms, release que nunca termina,
    // engine que se deja de procesar al instante, engine que sigue procesandose para siempre.
    const ModeCase c = GetParam();
    const EngineInfo& e = kEngines[c.engineIndex];
    const int leaving = c.order;
    const int remaining = 1 - leaving;
    const Script g = standardGesture(e, 0, leaving);

    const Rendered dual = render(e.type, c.mode, g);
    const Rendered onlyRemaining = render(e.type, c.mode, g.onlySlot(remaining));
    const Rendered sustained = render(e.type, c.mode, g.sustained(leaving));

    const std::vector<float> isolated = subtract(dual.mono, onlyRemaining.mono);
    const long liftOut = outFrame(kExitFirstMs);
    const std::vector<double> series = releaseSeries(isolated, sustained.mono, freqOfSlot(leaving), liftOut);
    const ReleaseVerdict verdict = judgeRelease(series);

    EXPECT_TRUE(verdict.notCut)
        << "AC-052.3 " << e.name << "/" << modeName(c.mode) << ": la ventana [20,40) ms tras soltar el slot "
        << leaving << " esta a " << series[1] << " dB del regimen (>= " << kReleaseNotCutMinDb
        << " si hay release; un corte la deja en el piso) " << describeSeries(series);
    EXPECT_TRUE(verdict.decaying)
        << "AC-052.3 " << e.name << "/" << modeName(c.mode) << ": la ventana [20,40) ms esta a " << series[1]
        << " dB del regimen (debe ser <= " << kReleaseDecayMaxDb << ": el release es exponencial, no lineal) "
        << describeSeries(series);
    EXPECT_TRUE(verdict.deepEnough)
        << "AC-052.3 " << e.name << "/" << modeName(c.mode) << ": la ventana [80,100) ms esta a "
        << series[kReleaseWindows - 1] << " dB del regimen (debe ser <= " << kReleaseDeepMaxDb << ") "
        << describeSeries(series);
    EXPECT_TRUE(verdict.monotone)
        << "AC-052.3 " << e.name << "/" << modeName(c.mode) << ": la banda CRECE entre ventanas "
        << describeSeries(series);

    // La sonda: el engine del slot sigue procesandose durante el release y se detiene despues.
    const uint64_t during = dual.probeAtMs(leaving, kExitFirstMs + kProbeGrowthWindowMs) -
                            dual.probeAtMs(leaving, kExitFirstMs);
    EXPECT_GE(during, kProbeGrowthMinBlocks)
        << "AC-052.3 " << e.name << ": el engine del slot " << leaving << " proceso solo " << during
        << " bloques en los " << kProbeGrowthWindowMs << " ms del release (sin release no se procesa)";
    const uint64_t atDone = dual.probeAtMs(leaving, kExitFirstMs + kEngineDoneMs);
    const uint64_t atEnd = dual.probeAtMs(leaving, kGestureTotalMs);
    EXPECT_EQ(atDone, atEnd)
        << "AC-052.3 " << e.name << ": el engine del slot " << leaving << " sigue procesandose "
        << (atEnd - atDone) << " bloques despues de " << kEngineDoneMs << " ms de release";
    // La otra voz no se paro: la sonda del slot que queda sigue creciendo hasta que sale.
    EXPECT_GT(dual.probeAtMs(remaining, kExitLastMs), dual.probeAtMs(remaining, kExitFirstMs + kEngineDoneMs));
}

// ===========================================================================
// AC-052.6 — el ultimo dedo tambien hace release; despues, silencio
// ===========================================================================

TEST_P(DualTouchByMode, Ac0526_TheLastFingerReleasesThenSilenceAndNoEngineIsProcessed) {
    // order = que slot sale primero; el ULTIMO en salir es el otro. Observable: igual que
    // AC-052.3 pero sin la otra voz: el dual (sin dither) contra la misma voz sostenida; despues
    // "silencio" (ver `judgeSilence`) y sondas de los dos slots congeladas.
    const ModeCase c = GetParam();
    const EngineInfo& e = kEngines[c.engineIndex];
    const int firstOut = c.order;
    const int last = 1 - firstOut;
    const Script g = standardGesture(e, 0, firstOut);

    const Rendered dual = render(e.type, c.mode, g);
    const Rendered sustained = render(e.type, c.mode, g.sustained(last));

    const long liftOut = outFrame(kExitLastMs);
    const std::vector<double> series = releaseSeries(dual.mono, sustained.mono, freqOfSlot(last), liftOut);
    const ReleaseVerdict verdict = judgeRelease(series);

    EXPECT_TRUE(verdict.notCut)
        << "AC-052.6 " << e.name << "/" << modeName(c.mode) << ": el ultimo dedo (slot " << last
        << ") se corta en seco: ventana [20,40) a " << series[1] << " dB (>= " << kReleaseNotCutMinDb
        << " con release) " << describeSeries(series);
    EXPECT_TRUE(verdict.decaying)
        << "AC-052.6 " << e.name << "/" << modeName(c.mode) << ": la ventana [20,40) ms esta a " << series[1]
        << " dB del regimen (debe ser <= " << kReleaseDecayMaxDb << ": el release es exponencial, no lineal) "
        << describeSeries(series);
    EXPECT_TRUE(verdict.deepEnough)
        << "AC-052.6 " << e.name << "/" << modeName(c.mode) << ": ventana [80,100) a "
        << series[kReleaseWindows - 1] << " dB (<= " << kReleaseDeepMaxDb << ") " << describeSeries(series);
    EXPECT_TRUE(verdict.monotone) << "AC-052.6 " << e.name << ": la banda crece " << describeSeries(series);

    // Despues del release: silencio y ningun engine procesado.
    const SilenceReading silence = readSilence(dual.mono, sustained.mono, freqOfSlot(last), liftOut);
    EXPECT_LE(silence.audiblePeak, kSilenceFloor)
        << "AC-052.6 " << e.name << ": quedan " << silence.audiblePeak << " de pico sobre " << kSilenceHighPassHz
        << " Hz " << kSilenceAfterMs << " ms despues del ultimo soltar";
    EXPECT_LE(silence.bandDbReRegime, kSilenceBandMaxDb)
        << "AC-052.6 " << e.name << ": la fundamental esta a " << silence.bandDbReRegime
        << " dB del regimen " << kSilenceAfterMs << " ms despues del ultimo soltar";

    const uint64_t during = dual.probeAtMs(last, kExitLastMs + kProbeGrowthWindowMs) -
                            dual.probeAtMs(last, kExitLastMs);
    EXPECT_GE(during, kProbeGrowthMinBlocks)
        << "AC-052.6 " << e.name << ": el engine del ultimo slot proceso solo " << during
        << " bloques durante el release";
    for (int slot = 0; slot < 2; ++slot) {
        const uint64_t atDone = dual.probeAtMs(slot, kExitLastMs + kEngineDoneMs);
        const uint64_t atEnd = dual.probeAtMs(slot, kGestureTotalMs);
        EXPECT_EQ(atDone, atEnd) << "AC-052.6 " << e.name << ": el engine del slot " << slot
                                 << " sigue procesandose tras el ultimo soltar ("
                                 << (atEnd - atDone) << " bloques despues de " << kEngineDoneMs << " ms)";
    }
    EXPECT_GT(maxAbs(sustained.mono, liftOut - ms(40.0), liftOut), kAudible * 0.01)
        << "control: la voz sostenida no suena antes del soltar";
}

// ===========================================================================
// AC-052.7 — dos voces a amplitud 1: finito y acotado
// ===========================================================================

TEST_P(DualTouchByMode, Ac0527_TwoVoicesAtFullAmplitudeStayFiniteAndWithinFullScale) {
    // Bug que atrapa: sumar a ganancia 1 sin proteccion desborda (hasta 2,0) o produce NaN/inf.
    // order = que slot sale primero (recorre ataque y release a amplitud plena).
    const ModeCase c = GetParam();
    EngineInfo loud = kEngines[c.engineIndex];
    loud.amp = 1.0f;  // AC-052.7: "dos voces a amplitud 1"
    const Script g = standardGesture(loud, 0, c.order);
    const Rendered dual = render(loud.type, c.mode, g);

    EXPECT_TRUE(dual.allFinite) << "AC-052.7 " << loud.name << "/" << modeName(c.mode) << ": hay no-finitos";
    EXPECT_LE(dual.peak, 1.0) << "AC-052.7 " << loud.name << "/" << modeName(c.mode) << ": |muestra| > 1,0";
    // Control: el render es fuerte de verdad (el limitador y el soft-clip estan trabajando).
    EXPECT_GT(dual.peak, 0.3) << "control: el render a amp 1 es mas debil de lo esperable";
}

// ===========================================================================
// Click — Classic seno, paso fino, barrido de fase
// ===========================================================================

class DualTouchPhaseSweep : public ::testing::TestWithParam<int> {};

TEST_P(DualTouchPhaseSweep, Ac0524_AttackAndReleaseOfASingleFingerHaveNoClick) {
    // Un dedo (slot 0), Classic seno a 1 kHz: toca, suelta, espera a que el engine termine, toca de
    // nuevo (RE-ENTRADA: el oscilador ya no suaviza su amplitud desde 0, asi que el ataque tiene
    // que salir de la envolvente del slot) y suelta. El instante del soltar se barre en cuartos de
    // ciclo (GetParam() = 0..3, mas `kOffZeroCrossingMs`) porque el escalon de un corte vale
    // |sin(fase)| * pico y en una sola fase puede ser ~0.
    // Observable: max |x[n]-x[n-1]| de la salida durante cada transicion <= 1,10 x el maximo en
    // regimen. Mas: pico de los primeros 48 cuadros tras el onset del re-ataque <= 0,5 del pico de
    // regimen (a 1 ms una rampa de 5 ms esta en ~0,2).
    const double quarter = kOffZeroCrossingMs + 0.25 * GetParam();
    const double firstDown = 20.0;
    const double firstUp = 120.0 + quarter;
    const double secondDown = firstUp + 200.0;
    const double secondUp = secondDown + 100.0;
    Script g(kFineStepFrames, secondUp + 250.0);
    g.touch(0, firstDown, firstUp, kFineFreq, kFineAmp).touch(0, secondDown, secondUp, kFineFreq, kFineAmp);
    const Rendered r = render(kClassic, DualTouchMixMode::AVERAGE, g);

    const long regimeFrom = outFrame(firstDown + kSettleMs);
    const long regimeTo = outFrame(firstUp);
    const double regimeStep = maxStep(r.mono, regimeFrom, regimeTo);
    const double regimePeak = maxAbs(r.mono, regimeFrom, regimeTo);
    const double sineStep = regimePeak * 2.0 * M_PI * kFineFreq / kSampleRate;
    ASSERT_NEAR(regimeStep, sineStep, sineStep * kSineStepModelTolerance)
        << "lo medido en regimen no es un seno limpio";

    const double limit = regimeStep * kClickTolerance;
    const long guard = ms(kEdgeGuardMs);
    EXPECT_LE(maxStep(r.mono, outFrame(firstDown) - guard, outFrame(firstDown) + ms(kAttackClickWindowMs)), limit)
        << "AC-052.4: click en el primer ataque";
    EXPECT_LE(maxStep(r.mono, outFrame(firstUp) - guard, outFrame(firstUp) + ms(kReleaseClickWindowMs)), limit)
        << "AC-052.4: click al soltar (limite " << limit << ", regimen " << regimeStep << ")";
    EXPECT_LE(maxStep(r.mono, outFrame(secondDown) - guard, outFrame(secondDown) + ms(kAttackClickWindowMs)), limit)
        << "AC-052.4: click en la re-entrada (limite " << limit << ", regimen " << regimeStep << ")";
    EXPECT_LE(maxStep(r.mono, outFrame(secondUp) - guard, outFrame(secondUp) + ms(kReleaseClickWindowMs)), limit)
        << "AC-052.4: click en el segundo soltar";

    const double firstCycle = peakAfterOnset(r.mono, outFrame(secondDown) - guard,
                                             outFrame(secondDown) + ms(kAttackClickWindowMs),
                                             kOnsetLevel, kAttackProbeFrames);
    ASSERT_GE(firstCycle, 0.0) << "la re-entrada no suena";
    EXPECT_LE(firstCycle, regimePeak * kAttackFirstMsMaxFraction)
        << "AC-052.4: el primer ms de la re-entrada ya esta a " << firstCycle / regimePeak
        << " del pico (con ataque de ~5 ms deberia estar en ~0,2)";

    // El ataque no es eterno: a los 15 ms esta abierto.
    const double opened = maxAbs(r.mono, outFrame(secondDown + kAttackOpenMs),
                                 outFrame(secondDown + kAttackOpenMs) + kAttackProbeFrames);
    EXPECT_GE(opened, regimePeak * kAttackOpenMinFraction)
        << "AC-052.4: a los " << kAttackOpenMs << " ms del re-toque la voz sigue en " << opened / regimePeak << " del pico";
}

INSTANTIATE_TEST_SUITE_P(Quarters, DualTouchPhaseSweep, ::testing::Range(0, 4),
                         [](const ::testing::TestParamInfo<int>& i) {
                             return "Quarter" + std::to_string(i.param);
                         });

class DualTouchRetouch : public ::testing::TestWithParam<int> {};

TEST_P(DualTouchRetouch, Ac0525_ARetouchDuringTheReleaseRisesFromWhereItWasWithoutAStep) {
    // Un dedo suelta y vuelve d ms despues, DENTRO del release. Dos barridos de fase, porque el
    // escalon de un defecto vale (ganancia) * |sin(fase)| * pico y en una sola fase puede ser ~0:
    //   - el instante del soltar se corre en cuartos de ciclo a 1 kHz (idx % 4) mas 1/8 de ciclo: un corte en seco
    //     o una caida brusca de la ganancia salta |sin(fase del corte)|;
    //   - d crece 1,25 ms por caso (= 1,25 ciclos, o sea un cuarto de ciclo de fase) de 6 a 14,75 ms:
    //     un re-ataque desde 0 salta g(d) * |sin(fase del re-toque)|.
    // Observable:
    //   - |dx| de la salida alrededor del soltar y del re-toque <= 1,10 x regimen (AC-052.5).
    //   - "sube desde donde estaba": el pico del primer ciclo tras el re-toque NO es menor que el
    //     del release SIN re-toque en el mismo instante (fase-independiente: abarca un periodo).
    const double d = 6.0 + 1.25 * GetParam();
    const double firstDown = 20.0;
    const double firstUp = 120.0 + kOffZeroCrossingMs + 0.25 * (GetParam() % 4);
    const double secondDown = firstUp + d;
    const double secondUp = secondDown + 100.0;
    Script g(kFineStepFrames, secondUp + 150.0);
    g.touch(0, firstDown, firstUp, kFineFreq, kFineAmp).touch(0, secondDown, secondUp, kFineFreq, kFineAmp);
    Script releaseOnly(kFineStepFrames, secondUp + 150.0);
    releaseOnly.touch(0, firstDown, firstUp, kFineFreq, kFineAmp);

    const Rendered r = render(kClassic, DualTouchMixMode::AVERAGE, g);
    const Rendered twin = render(kClassic, DualTouchMixMode::AVERAGE, releaseOnly);

    const long regimeFrom = outFrame(firstDown + kSettleMs);
    const long regimeTo = outFrame(firstUp);
    const double regimeStep = maxStep(r.mono, regimeFrom, regimeTo);
    const double regimePeak = maxAbs(r.mono, regimeFrom, regimeTo);
    ASSERT_GT(regimePeak, kAudible);

    const double limit = regimeStep * kClickTolerance;
    EXPECT_LE(maxStep(r.mono, outFrame(firstUp) - ms(kEdgeGuardMs), outFrame(secondDown) + ms(kRetouchClickWindowMs)), limit)
        << "AC-052.5: escalon alrededor del re-toque a d=" << d << " ms (limite " << limit
        << ", regimen " << regimeStep << ")";

    const double atRetouch = maxAbs(r.mono, outFrame(secondDown), outFrame(secondDown) + kAttackProbeFrames);
    const double releaseOnlyThere = maxAbs(twin.mono, outFrame(secondDown), outFrame(secondDown) + kAttackProbeFrames);
    EXPECT_GT(releaseOnlyThere, kAudible * 0.1)
        << "AC-052.5: el release sin re-toque ya esta apagado a d=" << d
        << " ms (sin release no hay una ganancia 'desde donde estaba')";
    EXPECT_GE(atRetouch, releaseOnlyThere * kRetouchNotBelowReleaseFraction)
        << "AC-052.5: tras el re-toque a d=" << d << " ms el pico del primer ciclo es " << atRetouch
        << " contra " << releaseOnlyThere << " del release sin re-toque: la ganancia bajo (re-ataque desde 0)";
}

INSTANTIATE_TEST_SUITE_P(Delays, DualTouchRetouch, ::testing::Range(0, 8),
                         [](const ::testing::TestParamInfo<int>& i) {
                             return "Step" + std::to_string(i.param);
                         });

class DualTouchEngineAttack : public ::testing::TestWithParam<int> {};

TEST_P(DualTouchEngineAttack, Ac0524_TheReentryOfAnEngineVoiceCarriesAnAttackRamp) {
    // Un engine que recibe la amplitud cruda por bloque (FM) y uno que la suaviza (Wavetable): la
    // RE-ENTRADA tras un release completo. Supersaw queda afuera: sus siete sierras desafinadas
    // baten, asi que "el pico de regimen" no es estacionario y la razon primer-ciclo/regimen
    // discrimina poco (medido: 0,54 con el codigo actual contra un umbral de 0,5). Karplus-Strong
    // y Granular tampoco: re-excitan o tienen granos al azar. El mecanismo de la rampa es el mismo
    // para todos (lo cubren AC-052.1/.2/.3 por banda en los seis). Con el codigo viejo el engine esta
    // congelado a su amp de regimen y vuelve entero al primer bloque. Se mide el pico del primer
    // ciclo (48 cuadros = 1 periodo a 1 kHz, asi que contiene al pico de la forma de onda con
    // cualquier fase).
    const EngineInfo& e = kEngines[GetParam()];
    const double firstDown = 20.0;
    const double firstUp = 220.0;
    const double secondDown = firstUp + 200.0;
    Script g(kCoarseStepFrames, secondDown + 300.0);
    g.touch(0, firstDown, firstUp, kFineFreq, 0.5f).touch(0, secondDown, secondDown + 200.0, kFineFreq, 0.5f);
    const Rendered r = render(e.type, DualTouchMixMode::AVERAGE, g);

    const long regimeFrom = outFrame(firstDown + kSettleMs);
    const long regimeTo = outFrame(firstUp);
    const double regimePeak = maxAbs(r.mono, regimeFrom, regimeTo);
    ASSERT_GT(regimePeak, kAudible) << e.name << " no suena";

    const double firstCycle = peakAfterOnset(r.mono, outFrame(secondDown) - ms(kEdgeGuardMs),
                                             outFrame(secondDown) + ms(kAttackClickWindowMs),
                                             kOnsetLevel, kAttackProbeFrames);
    ASSERT_GE(firstCycle, 0.0) << e.name << ": la re-entrada no suena";
    EXPECT_LE(firstCycle, regimePeak * kAttackFirstMsMaxFraction)
        << "AC-052.4 " << e.name << ": el primer ciclo de la re-entrada esta a " << firstCycle / regimePeak
        << " del pico de regimen (con ataque de ~5 ms ~0,2)";

    const double opened = maxAbs(r.mono, outFrame(secondDown + kAttackOpenMs),
                                 outFrame(secondDown + kAttackOpenMs) + 2 * kAttackProbeFrames);
    EXPECT_GE(opened, regimePeak * kAttackOpenMinFraction)
        << "AC-052.4 " << e.name << ": a los " << kAttackOpenMs << " ms la voz sigue en "
        << opened / regimePeak << " del pico";
}

INSTANTIATE_TEST_SUITE_P(Engines, DualTouchEngineAttack, ::testing::Values(kFm, kWavetable),
                         [](const ::testing::TestParamInfo<int>& i) {
                             return std::string(kEngines[i.param].name);
                         });

// ===========================================================================
// AC-052.8 — los otros cuatro modos: misma envolvente
// ===========================================================================

struct ModeVariant {
    DualTouchMixMode mode;
    int variant;
};
void PrintTo(const ModeVariant& c, std::ostream* os) { *os << modeName(c.mode) << "/" << c.variant; }

class DualTouchModeEnvelope : public ::testing::TestWithParam<ModeVariant> {};

TEST_P(DualTouchModeEnvelope, Ac0528_EveryMixModeGetsTheSameEnvelopeWithoutAStep) {
    // Classic seno; slot 0 a 1 kHz y slot 1 a 250 Hz. Secuencia: 0 entra; 1 entra; 1 suelta;
    // 1 vuelve DENTRO de su release; 1 suelta; 0 suelta (el ultimo). Cubre ataque, release,
    // re-toque, 2 -> 1 y 1 -> 0. `variant` desplaza en cuartos de ciclo (del slot 1) el soltar y el
    // re-toque (barrido de fase). Observable: ninguna |x[n]-x[n-1]| de la salida supera el maximo de los
    // tres regimenes estables (0 solo, los dos, 0 solo) x 1,10. En MAX se mide sobre |x|: la ley
    // max(|b1|,|b2|)*signo(b1+b2) tiene saltos propios cuando la suma cruza cero, pero |x| es continua.
    // SUM/AVERAGE tambien pasan por aca: son dos dedos con la ley nueva.
    const ModeVariant c = GetParam();
    // El slot 1 suena a 250 Hz (periodo 4 ms) y el 0 a 1 kHz (1 ms). El soltar del slot 1 cae a
    // +0,625 ms: 5/8 de ciclo del slot 0 (|sin| = 0,707, el escalon de pasar de la ley de dos dedos
    // —en RING, un producto chico— a la voz sola) y 0,16 + 0,25 v de ciclo del slot 1 (|sin| entre
    // 0,56 y 0,84). Cada variante corre el soltar y el re-toque 1 ms: la fase del slot 0 no cambia
    // y la del slot 1 avanza un cuarto de ciclo.
    const double slot1Up = 300.625 + 1.0 * c.variant;
    const double slot1Retouch = slot1Up + 8.0 + 1.0 * c.variant;
    const double slot1Up2 = 440.0;
    const double slot0Up = 620.0;  // > slot1Up2 + kReleaseClickWindowMs: hay un tramo de slot 0 solo al final
    Script g(kFineStepFrames, slot0Up + 250.0);
    g.touch(0, 20.0, slot0Up, kFineFreq, kFineAmp)
        .touch(1, 140.0, slot1Up, 250.0f, kFineAmp)
        .touch(1, slot1Retouch, slot1Up2, 250.0f, kFineAmp);
    const Rendered r = render(kClassic, c.mode, g);
    const bool useAbs = c.mode == DualTouchMixMode::MAX;

    const double regimeA = maxStep(r.mono, outFrame(20.0 + kSettleMs), outFrame(140.0), useAbs);
    const double regimeB = maxStep(r.mono, outFrame(140.0 + kSettleMs), outFrame(slot1Up), useAbs);
    const double regimeC = maxStep(r.mono, outFrame(slot1Up2 + kReleaseClickWindowMs), outFrame(slot0Up), useAbs);
    const double regime = std::max({regimeA, regimeB, regimeC});
    ASSERT_GT(regime, 0.0);
    ASSERT_GT(maxAbs(r.mono, outFrame(20.0 + kSettleMs), outFrame(140.0)), kAudible);

    const double worst = maxStep(r.mono, outFrame(20.0) - ms(kEdgeGuardMs),
                                 outFrame(slot0Up) + ms(kReleaseClickWindowMs) + ms(kEdgeGuardMs), useAbs);
    EXPECT_LE(worst, regime * kClickTolerance)
        << "AC-052.8 " << modeName(c.mode) << " variante " << c.variant << ": hay un escalon de " << worst
        << " contra " << regime << " de regimen (x" << kClickTolerance << ")";
}

std::vector<ModeVariant> allModeVariants() {
    std::vector<ModeVariant> v;
    for (auto m : {DualTouchMixMode::SUM, DualTouchMixMode::AVERAGE, DualTouchMixMode::MAX,
                   DualTouchMixMode::CROSSFADE, DualTouchMixMode::RING,
                   DualTouchMixMode::AMPLITUDE_BALANCED}) {
        for (int i = 0; i < 4; ++i) v.push_back({m, i});
    }
    return v;
}

INSTANTIATE_TEST_SUITE_P(Modes, DualTouchModeEnvelope, ::testing::ValuesIn(allModeVariants()),
                         [](const ::testing::TestParamInfo<ModeVariant>& i) {
                             return std::string(modeName(i.param.mode)) + "_V" + std::to_string(i.param.variant);
                         });

class DualTouchOtherModes : public ::testing::TestWithParam<DualTouchMixMode> {};

TEST_P(DualTouchOtherModes, Ac0528_TheLastFingerReleasesInEveryMode) {
    // Dos dedos, el 1 suelta, luego el 0 (el ultimo): el release del ultimo dedo tiene la
    // envolvente del spec en MAX/CROSSFADE/RING/AMPLITUDE_BALANCED. La voz sostenida de
    // referencia es el slot 0 solo (un dedo = ganancia 1 en los cuatro modos).
    const DualTouchMixMode mode = GetParam();
    const EngineInfo& e = kEngines[kClassic];
    Script g(kCoarseStepFrames, kGestureTotalMs);
    g.touch(0, kFirstDownMs, kExitLastMs, kFreqSlot0, e.amp).touch(1, kSecondDownMs, kExitFirstMs, kFreqSlot1, e.amp);

    const Rendered dual = render(e.type, mode, g);
    const Rendered sustained = render(e.type, mode, g.sustained(0));
    const std::vector<double> series = releaseSeries(dual.mono, sustained.mono, kFreqSlot0, outFrame(kExitLastMs));
    const ReleaseVerdict v = judgeRelease(series);
    EXPECT_TRUE(v.ok()) << "AC-052.8 " << modeName(mode) << ": el release del ultimo dedo no tiene la envolvente del spec "
                        << describeSeries(series);
    const SilenceReading silence = readSilence(dual.mono, sustained.mono, kFreqSlot0, outFrame(kExitLastMs));
    EXPECT_LE(silence.audiblePeak, kSilenceFloor) << "AC-052.8 " << modeName(mode) << ": no queda en silencio";
    EXPECT_LE(silence.bandDbReRegime, kSilenceBandMaxDb) << "AC-052.8 " << modeName(mode) << ": la fundamental no cae";
    EXPECT_EQ(dual.probeAtMs(0, kExitLastMs + kEngineDoneMs), dual.probeAtMs(0, kGestureTotalMs))
        << "AC-052.8 " << modeName(mode) << ": el engine sigue procesandose tras el release";
}

INSTANTIATE_TEST_SUITE_P(Modes, DualTouchOtherModes,
                         ::testing::Values(DualTouchMixMode::MAX, DualTouchMixMode::CROSSFADE,
                                           DualTouchMixMode::RING, DualTouchMixMode::AMPLITUDE_BALANCED),
                         [](const ::testing::TestParamInfo<DualTouchMixMode>& i) {
                             return std::string(modeName(i.param));
                         });

// ---------------------------------------------------------------------------
// Las leyes en regimen (AC-052.8) y la interpretacion de "un dedo = ganancia 1"
// ---------------------------------------------------------------------------

namespace {

constexpr double kLawF1 = 1000.0;
constexpr double kLawF2 = 1560.0;  // no armonica de 1000; |f2-f1| = 560 y f1+f2 = 2560 no caen en ninguna de las dos
constexpr double kLawFromMs = 200.0;
constexpr double kLawToMs = 320.0;

struct LawRenders {
    Rendered dual, only0, only1;
};

LawRenders renderLaw(DualTouchMixMode mode, float amp0, float amp1, float distance) {
    Script g(kCoarseStepFrames, kLawToMs + 20.0);
    g.touch(0, 20.0, kLawToMs, static_cast<float>(kLawF1), amp0)
        .touch(1, 20.0, kLawToMs, static_cast<float>(kLawF2), amp1)
        .withDistance(distance);
    LawRenders r;
    r.dual = render(kClassic, mode, g);
    r.only0 = render(kClassic, mode, g.onlySlot(0));
    r.only1 = render(kClassic, mode, g.onlySlot(1));
    return r;
}

double db(double ratio) { return 20.0 * std::log10(ratio); }

}  // namespace

TEST(DualTouchEnvelopeLaws, Ac0528_CrossfadeKeepsItsLawInRegime) {
    // CROSSFADE: (1-d)*b1 + d*b2 con d = clamp(distance, 0, 1). Con dos dedos sostenidos, la banda de
    // cada voz es (1-d) y d veces la de su voz sola. Bug que atrapa: una implementacion que reemplace
    // el crossfade por una suma, o que invierta los pesos.
    for (const float d : {0.25f, 0.75f}) {
        SCOPED_TRACE("distance=" + std::to_string(d));
        const LawRenders r = renderLaw(DualTouchMixMode::CROSSFADE, 0.2f, 0.2f, d);
        const long at = outFrame(kLawFromMs);
        EXPECT_NEAR(db(bandAmplitude(r.dual.mono, at, kLawF1) / bandAmplitude(r.only0.mono, at, kLawF1)),
                    db(1.0 - d), kLawToleranceDb);
        EXPECT_NEAR(db(bandAmplitude(r.dual.mono, at, kLawF2) / bandAmplitude(r.only1.mono, at, kLawF2)),
                    db(d), kLawToleranceDb);
    }
}

TEST(DualTouchEnvelopeLaws, Ac0528_AmplitudeBalancedKeepsItsLawInRegime) {
    // AMPLITUDE_BALANCED: pesos amp_i / (amp1 + amp2). Con 0,30 y 0,15 los pesos son 2/3 y 1/3.
    const LawRenders r = renderLaw(DualTouchMixMode::AMPLITUDE_BALANCED, 0.30f, 0.15f, 0.5f);
    const long at = outFrame(kLawFromMs);
    EXPECT_NEAR(db(bandAmplitude(r.dual.mono, at, kLawF1) / bandAmplitude(r.only0.mono, at, kLawF1)),
                db(2.0 / 3.0), kLawToleranceDb);
    EXPECT_NEAR(db(bandAmplitude(r.dual.mono, at, kLawF2) / bandAmplitude(r.only1.mono, at, kLawF2)),
                db(1.0 / 3.0), kLawToleranceDb);
}

TEST(DualTouchEnvelopeLaws, Ac0528_RingKeepsItsLawInRegime) {
    // RING: b1*b2*0,5. Dos senos de amplitud a1 y a2 dan componentes en |f2-f1| y f1+f2 de
    // amplitud a1*a2/4 cada una, y NINGUNA en f1 ni f2. Se compara contra las voces solas
    // (A1*A2/4 con A_i medidos), no contra amp nominal, para no depender del soft-clip.
    const LawRenders r = renderLaw(DualTouchMixMode::RING, 0.2f, 0.2f, 0.5f);
    const long at = outFrame(kLawFromMs);
    const double a1 = bandAmplitude(r.only0.mono, at, kLawF1);
    const double a2 = bandAmplitude(r.only1.mono, at, kLawF2);
    EXPECT_NEAR(db(bandAmplitude(r.dual.mono, at, kLawF2 - kLawF1) / (a1 * a2 / 4.0)), 0.0, kLawToleranceDb)
        << "componente de diferencia de RING";
    EXPECT_NEAR(db(bandAmplitude(r.dual.mono, at, kLawF2 + kLawF1) / (a1 * a2 / 4.0)), 0.0, kLawToleranceDb)
        << "componente de suma de RING";
    EXPECT_LE(db(bandAmplitude(r.dual.mono, at, kLawF1) / a1), -30.0) << "RING no deja fundamental: no es una suma";
}

TEST(DualTouchEnvelopeLaws, Ac0528_MaxKeepsItsLawInRegime) {
    // MAX: max(|b1|,|b2|)*signo(b1+b2). Muestra a muestra contra las voces solas. El soft-clip es
    // impar y monotono, asi que conmuta con el max; la unica diferencia es el DC blocker.
    const LawRenders r = renderLaw(DualTouchMixMode::MAX, 0.3f, 0.2f, 0.5f);
    const long from = outFrame(kLawFromMs);
    const long to = outFrame(kLawToMs) - ms(10.0);
    const double peak = maxAbs(r.only0.mono, from, to);
    ASSERT_GT(peak, kAudible);
    long violations = 0, signChecked = 0, signWrong = 0;
    for (long n = from; n < to; ++n) {
        const double x1 = r.only0.mono[static_cast<size_t>(n)];
        const double x2 = r.only1.mono[static_cast<size_t>(n)];
        const double expectedAbs = std::max(std::fabs(x1), std::fabs(x2));
        const double y = r.dual.mono[static_cast<size_t>(n)];
        if (std::fabs(std::fabs(y) - expectedAbs) > kMaxLawSampleTolerance * peak) ++violations;
        if (std::fabs(x1 + x2) > kMaxLawSignGuard * peak) {
            ++signChecked;
            if ((y >= 0.0) != (x1 + x2 >= 0.0)) ++signWrong;
        }
    }
    const double total = static_cast<double>(to - from);
    EXPECT_LE(violations / total, kMaxLawViolatingFraction) << "|salida| no es max(|b1|,|b2|)";
    EXPECT_LE(signWrong / static_cast<double>(std::max<long>(signChecked, 1)), kMaxLawViolatingFraction)
        << "el signo no es el de b1+b2";
}

TEST(DualTouchEnvelopeLaws, Ac0521_SumAndAverageAddEachVoiceAtUnityGain) {
    // La ley de AC-052.1 con sines puros: SUM y AVERAGE dan b1 + b2 (cada voz a ganancia 1), sin
    // el x0,5 de la ley vieja. La banda de cada voz en el dual coincide con la de su voz sola.
    for (const auto mode : {DualTouchMixMode::SUM, DualTouchMixMode::AVERAGE}) {
        SCOPED_TRACE(modeName(mode));
        const LawRenders r = renderLaw(mode, 0.2f, 0.2f, 0.5f);
        const long at = outFrame(kLawFromMs);
        EXPECT_NEAR(db(bandAmplitude(r.dual.mono, at, kLawF1) / bandAmplitude(r.only0.mono, at, kLawF1)), 0.0,
                    kLevelToleranceDb);
        EXPECT_NEAR(db(bandAmplitude(r.dual.mono, at, kLawF2) / bandAmplitude(r.only1.mono, at, kLawF2)), 0.0,
                    kLevelToleranceDb);
    }
}

class DualTouchPassthrough : public ::testing::TestWithParam<ModeCase> {};

TEST_P(DualTouchPassthrough, Ac0528_AloneFingerInDualModeIsTheVoiceAtUnityGainInEveryMode) {
    // Interpretacion que fija la etapa: en regimen con UN solo dedo en modo dual la salida es ese
    // dedo a ganancia 1 (como hoy), en los seis modos y en los dos slots (order = slot). La
    // referencia es el modo NO dual (single touch) a la misma frecuencia y amplitud: una ruta
    // independiente del dual touch. Bug que atrapa: RING con un dedo da producto por cero
    // (silencio), CROSSFADE con d != 0/1 baja la voz, AMPLITUDE_BALANCED con peso 0.
    const ModeCase c = GetParam();
    constexpr float kAmp = 0.2f;
    Script g(kCoarseStepFrames, kLawToMs + 20.0);
    g.touch(c.order, 20.0, kLawToMs, static_cast<float>(kLawF1), kAmp).withDistance(0.3f);
    const Rendered dual = render(kClassic, c.mode, g);
    const std::vector<float> single = renderSingleTouch(static_cast<float>(kLawF1), kAmp, ms(kLawToMs + 20.0));

    const long at = ms(kLawFromMs);
    const double single_ = bandAmplitude(single, at + kOutputLatencyFrames, kLawF1);
    ASSERT_GT(single_, kAudible * 0.1) << "control: la referencia single touch no suena";
    EXPECT_NEAR(db(bandAmplitude(dual.mono, at + kOutputLatencyFrames, kLawF1) / single_), 0.0,
                kPassthroughToleranceDb)
        << "AC-052.8 " << modeName(c.mode) << ": un dedo solo (slot " << c.order << ") no sale a ganancia 1";
}

// ===========================================================================
// Instancias
// ===========================================================================

namespace {

std::vector<EngineCase> allEngines() {
    std::vector<EngineCase> v;
    for (int i = 0; i < 6; ++i) v.push_back({i});
    return v;
}

/// AVERAGE en los seis engines y SUM en Classic: comparten la ley (b1 + b2) y el costo de cada
/// caso es de 3 renders; Classic es el engine de referencia de los osciladores.
std::vector<ModeCase> sumAndAverageByEngineAndOrder() {
    std::vector<ModeCase> v;
    for (int i = 0; i < 6; ++i) {
        for (int order = 0; order < 2; ++order) v.push_back({i, DualTouchMixMode::AVERAGE, order});
    }
    for (int order = 0; order < 2; ++order) v.push_back({kClassic, DualTouchMixMode::SUM, order});
    return v;
}

std::vector<ModeCase> sumAndAverageByEngine() {
    std::vector<ModeCase> v;
    for (int i = 0; i < 6; ++i) v.push_back({i, DualTouchMixMode::AVERAGE, 0});
    v.push_back({kClassic, DualTouchMixMode::SUM, 0});
    return v;
}

std::vector<ModeCase> allModesBySlot() {
    std::vector<ModeCase> v;
    for (auto m : {DualTouchMixMode::SUM, DualTouchMixMode::AVERAGE, DualTouchMixMode::MAX,
                   DualTouchMixMode::CROSSFADE, DualTouchMixMode::RING, DualTouchMixMode::AMPLITUDE_BALANCED}) {
        for (int slot = 0; slot < 2; ++slot) v.push_back({kClassic, m, slot});
    }
    return v;
}

}  // namespace

INSTANTIATE_TEST_SUITE_P(Engines, DualTouchByEngine, ::testing::ValuesIn(allEngines()),
                         [](const ::testing::TestParamInfo<EngineCase>& i) {
                             return std::string(kEngines[i.param.engineIndex].name);
                         });

INSTANTIATE_TEST_SUITE_P(EnginesAndOrders, DualTouchByMode, ::testing::ValuesIn(sumAndAverageByEngineAndOrder()),
                         [](const ::testing::TestParamInfo<ModeCase>& i) {
                             // "First" = el slot que se mueve primero (cae primero en AC-052.1, sale
                             // primero en AC-052.2/.3/.6/.7).
                             const char* order = i.param.order == 0 ? "Slot0First" : "Slot1First";
                             return std::string(kEngines[i.param.engineIndex].name) + "_" + modeName(i.param.mode) +
                                    "_" + order;
                         });

INSTANTIATE_TEST_SUITE_P(EnginesAndModes, DualTouchByEngineAndMode, ::testing::ValuesIn(sumAndAverageByEngine()),
                         [](const ::testing::TestParamInfo<ModeCase>& i) {
                             return std::string(kEngines[i.param.engineIndex].name) + "_" + modeName(i.param.mode);
                         });

INSTANTIATE_TEST_SUITE_P(Slots, DualTouchPassthrough, ::testing::ValuesIn(allModesBySlot()),
                         [](const ::testing::TestParamInfo<ModeCase>& i) {
                             return std::string(modeName(i.param.mode)) + "_Slot" + std::to_string(i.param.order);
                         });






// ===========================================================================
// AC-052.3 / .6 / .4 — una envolvente congelada no reaparece
// ===========================================================================
//
// `planSlotBlock` corre solo desde `renderDualTouch`. Si el callback sale del camino dual con un
// dedo apoyado (oscilador apagado, modo dual apagado, stop), el estado de la envolvente queda en
// "held, ganancia 1"; al volver al camino dual SIN dedos tiene que sonar silencio, no un release
// fantasma de 80 ms de la nota vieja.
//
// Estos tests manejan UNA instancia de motor a mano (`Rig`) en vez de un `Script`, porque el
// escenario es justamente que la instancia vive entre un camino y otro.

namespace {

/// Una instancia de motor dual, avanzada a mano de a `stepFrames`.
class Rig {
public:
    Rig(int engineType, int stepFrames) : mStep(stepFrames), mBlock(static_cast<size_t>(stepFrames) * 2, 0.0f) {
        EXPECT_TRUE(engine.startOffline(kSampleRate, kMaxBlock));
        EXPECT_EQ(engine.getNumEffects(), 0u) << "la cadena de efectos del render (mEffectChain) no esta vacia";
        engine.setOscillatorEnabled(true);
        engine.setEngineType(engineType);
        if (engineType == kClassic) engine.setOscillatorType(kOscSine);
        engine.setDualTouchMixMode(DualTouchMixMode::AVERAGE);
        engine.setDualTouchMode(true);
    }

    /// `ms` milisegundos con el slot 0 en (f0, a0) y el 1 en (f1, a1).
    void run(double ms, float f0, float a0, float f1 = 0.0f, float a1 = 0.0f) {
        const int steps = static_cast<int>(std::lround(ms * kFramesPerMs / mStep));
        for (int k = 0; k < steps; ++k) {
            engine.updateDualTouch(0.5f, 0.5f, f0, a0, 0.5f, 0.5f, 0.5f, f1, a1, 0.5f, 0.5f, 0.0f);
            std::fill(mBlock.begin(), mBlock.end(), 0.0f);
            EXPECT_TRUE(engine.renderBlock(mBlock.data(), nullptr, mStep));
            for (int n = 0; n < mStep; ++n) {
                mono.push_back(0.5f * (mBlock[static_cast<size_t>(n) * 2] + mBlock[static_cast<size_t>(n) * 2 + 1]));
            }
        }
    }
    long frames() const { return static_cast<long>(mono.size()); }

    AudioEngine engine;
    std::vector<float> mono;

private:
    int mStep;
    std::vector<float> mBlock;
};

constexpr double kGhostLeadMs = 20.0;
constexpr double kGhostRegimeEndMs = 200.0;   // el dedo esta en regimen hasta aca
constexpr double kGhostHeldWhileAwayMs = 20.0;  // sigue apoyado un rato con el motor fuera del camino dual
constexpr double kGhostLiftedWhileAwayMs = 40.0;  // y se levanta SIN que el camino dual lo vea
constexpr double kGhostAfterReturnMs = 400.0;
/// Ventanas [0,20) .. [60,80) ms desde que la salida del primer bloque de vuelta aparece. Un release
/// fantasma de 80 ms vive en esas cuatro (-7 / -22 / -37 / -52 dB re regimen).
constexpr int kGhostWindows = 4;
/// El piso "sin voz" de la banda: -50 dB re regimen. Origen: el fantasma da -7 dB en [0,20) y
/// >= -52 dB hasta [60,80); lo que SI queda en la banda sin fantasma es la fuga de Hann de la cola
/// de continua del DC blocker (que NO avanza con el oscilador apagado y retoma al volver: el
/// bloque `else` de `renderDualTouch` no pasa por `applyEffectsAndLooper`), de ~-77 dB (cuenta:
/// continua de ~-10 dB re regimen con tau 54 ms, a 10 bins de 50 Hz, Hann ~-67 dB).
/// NO se usa el pico por pasa-altos de AC-052.6 en estos escenarios, porque esa cola de continua lo
/// ensucia (2e-3 a la vuelta) con independencia de la envolvente.
constexpr double kGhostBandMaxDb = -50.0;

struct GhostReading {
    double worstBandDb = -300.0;
    long worstWindow = 0;
};

/// El peor nivel de la banda de `hz` en las cuatro ventanas siguientes a `returnFrame`, re el regimen
/// de `reference` (voz sostenida) en los 100 ms previos a `regimeEndMs`.
GhostReading readGhost(const std::vector<float>& x, long returnFrame, const std::vector<float>& reference,
                       double hz) {
    GhostReading g;
    double regime = -300.0;
    for (long s = outFrame(kGhostRegimeEndMs - 100.0); s + kWindowFrames <= outFrame(kGhostRegimeEndMs); s += kHopFrames) {
        regime = std::max(regime, bandDb(reference, s, hz));
    }
    for (int k = 0; k < kGhostWindows; ++k) {
        const long start = returnFrame + kOutputLatencyFrames + static_cast<long>(k) * kWindowFrames;
        const double rel = bandDb(x, start, hz) - regime;
        if (rel > g.worstBandDb) {
            g.worstBandDb = rel;
            g.worstWindow = k;
        }
    }
    return g;
}

Script ghostReference(double totalMs, float freq, float amp) {
    Script ref(kCoarseStepFrames, totalMs);
    ref.touch(0, kGhostLeadMs, totalMs, freq, amp);
    return ref;
}

}  // namespace

class DualTouchGhost : public ::testing::TestWithParam<int> {};

TEST_P(DualTouchGhost, Ac0523_TheOscillatorDisabledPathLeavesNoGhostReleaseWhenTheFingerLiftedMeanwhile) {
    // a) dual on, dedo en regimen -> setOscillatorEnabled(false) -> el dedo se levanta mientras el
    // camino dual no corre -> setOscillatorEnabled(true) SIN dedos. Observable: la banda de 500 Hz
    // en las cuatro ventanas de 20 ms tras la vuelta, y la sonda del slot 0.
    // Bug que atrapa: la envolvente quedo en "held, ganancia 1" y suena un release de 80 ms de la
    // nota vieja (-7 dB re regimen en la primera ventana).
    const EngineInfo& e = kEngines[GetParam()];
    const float f = kFreqSlot0;
    Rig r(e.type, kCoarseStepFrames);
    r.run(kGhostLeadMs, 0, 0);
    r.run(kGhostRegimeEndMs - kGhostLeadMs, f, e.amp);
    r.engine.setOscillatorEnabled(false);
    r.run(kGhostHeldWhileAwayMs, f, e.amp);
    r.run(kGhostLiftedWhileAwayMs, 0, 0);
    r.engine.setOscillatorEnabled(true);
    const long returnFrame = r.frames();
    const uint64_t probeAtReturn = r.engine.dualTouchSlotBlocksRendered(0);
    r.run(kGhostAfterReturnMs, 0, 0);

    const double totalMs = static_cast<double>(r.frames()) / kFramesPerMs;
    const Rendered ref = render(e.type, DualTouchMixMode::AVERAGE, ghostReference(totalMs, f, e.amp));
    const GhostReading g = readGhost(r.mono, returnFrame, ref.mono, f);
    EXPECT_LE(g.worstBandDb, kGhostBandMaxDb)
        << "AC-052.3 " << e.name << ": tras volver al camino dual sin dedos, la banda de " << f
        << " Hz esta a " << g.worstBandDb << " dB del regimen en la ventana " << g.worstWindow
        << " (release fantasma de la nota vieja; debe ser <= " << kGhostBandMaxDb << ")";
    EXPECT_EQ(r.engine.dualTouchSlotBlocksRendered(0), probeAtReturn)
        << "AC-052.3 " << e.name << ": el engine del slot 0 se proceso "
        << (r.engine.dualTouchSlotBlocksRendered(0) - probeAtReturn) << " bloques tras volver sin dedos";
    // Control positivo del instrumento: con el dedo en regimen la banda SI esta cerca de 0 dB.
    EXPECT_GE(bandDb(r.mono, outFrame(kGhostRegimeEndMs - 60.0), f) -
                  bandDb(ref.mono, outFrame(kGhostRegimeEndMs - 60.0), f), -0.5)
        << "control: el regimen previo no suena igual que la referencia";
}

TEST_P(DualTouchGhost, Ac0523_AnotherRenderPathLeavesNoGhostReleaseWhenTheFingerLiftedMeanwhile) {
    // a') igual que a), pero el callback sale del camino dual por OTRA RAMA de onAudioReady
    // (engine SoundFont, sin font cargado: la rama de SoundFont va antes que la dual) y vuelve
    // con setEngineType(engine original) sin dedos. A diferencia de a), aca renderDualTouch NO
    // corre mientras tanto, y ninguna epoca se mueve (ni setDualTouchMode ni prepare): lo unico
    // que suelta la envolvente es el bloque que pasa por otra rama.
    // Bug que atrapa: sin eso la envolvente queda en "held, ganancia 1" y al volver suena el
    // release de 80 ms de la nota vieja (mismo observable y umbral que a).
    constexpr int kEngineTypeSoundFont = 6;
    const EngineInfo& e = kEngines[GetParam()];
    const float f = kFreqSlot0;
    Rig r(e.type, kCoarseStepFrames);
    r.run(kGhostLeadMs, 0, 0);
    r.run(kGhostRegimeEndMs - kGhostLeadMs, f, e.amp);
    r.engine.setEngineType(kEngineTypeSoundFont);
    r.run(kGhostHeldWhileAwayMs, f, e.amp);
    r.run(kGhostLiftedWhileAwayMs, 0, 0);
    r.engine.setEngineType(e.type);
    const long returnFrame = r.frames();
    const uint64_t probeAtReturn = r.engine.dualTouchSlotBlocksRendered(0);
    r.run(kGhostAfterReturnMs, 0, 0);

    const double totalMs = static_cast<double>(r.frames()) / kFramesPerMs;
    const Rendered ref = render(e.type, DualTouchMixMode::AVERAGE, ghostReference(totalMs, f, e.amp));
    const GhostReading g = readGhost(r.mono, returnFrame, ref.mono, f);
    EXPECT_LE(g.worstBandDb, kGhostBandMaxDb)
        << "AC-052.3 " << e.name << ": tras volver de la rama SoundFont sin dedos, la banda de " << f
        << " Hz esta a " << g.worstBandDb << " dB del regimen en la ventana " << g.worstWindow
        << " (release fantasma de la nota vieja; debe ser <= " << kGhostBandMaxDb << ")";
    EXPECT_EQ(r.engine.dualTouchSlotBlocksRendered(0), probeAtReturn)
        << "AC-052.3 " << e.name << ": el engine del slot 0 se proceso "
        << (r.engine.dualTouchSlotBlocksRendered(0) - probeAtReturn) << " bloques tras volver sin dedos";
}

TEST_P(DualTouchGhost, Ac0523_TheDualModeSwitchedOffLeavesNoGhostReleaseWhenTheFingerLiftedMeanwhile) {
    // b) igual, saliendo con setDualTouchMode(false) (el motor pasa al camino single touch) y
    // volviendo con setDualTouchMode(true) sin dedos.
    const EngineInfo& e = kEngines[GetParam()];
    const float f = kFreqSlot0;
    Rig r(e.type, kCoarseStepFrames);
    r.run(kGhostLeadMs, 0, 0);
    r.run(kGhostRegimeEndMs - kGhostLeadMs, f, e.amp);
    r.engine.setDualTouchMode(false);
    r.run(kGhostHeldWhileAwayMs, f, e.amp);
    r.run(kGhostLiftedWhileAwayMs, 0, 0);
    r.engine.setDualTouchMode(true);
    const long returnFrame = r.frames();
    const uint64_t probeAtReturn = r.engine.dualTouchSlotBlocksRendered(0);
    r.run(kGhostAfterReturnMs, 0, 0);

    const double totalMs = static_cast<double>(r.frames()) / kFramesPerMs;
    const Rendered ref = render(e.type, DualTouchMixMode::AVERAGE, ghostReference(totalMs, f, e.amp));
    const GhostReading g = readGhost(r.mono, returnFrame, ref.mono, f);
    EXPECT_LE(g.worstBandDb, kGhostBandMaxDb)
        << "AC-052.3 " << e.name << ": tras volver al modo dual sin dedos, la banda de " << f << " Hz esta a "
        << g.worstBandDb << " dB del regimen en la ventana " << g.worstWindow
        << " (release fantasma de la nota vieja; debe ser <= " << kGhostBandMaxDb << ")";
    EXPECT_EQ(r.engine.dualTouchSlotBlocksRendered(0), probeAtReturn)
        << "AC-052.3 " << e.name << ": el engine del slot 0 se proceso "
        << (r.engine.dualTouchSlotBlocksRendered(0) - probeAtReturn) << " bloques tras volver sin dedos";
}

INSTANTIATE_TEST_SUITE_P(Engines, DualTouchGhost, ::testing::Values(kClassic, kFm),
                         [](const ::testing::TestParamInfo<int>& i) { return std::string(kEngines[i.param].name); });

TEST(DualTouchGhostStop, Ac0526_AStoppedAndRestartedEngineLeavesNoGhostReleaseWhenTheFingerLiftedMeanwhile) {
    // c) offline stop() con un dedo apoyado y startOffline de nuevo en la MISMA instancia, sin dedos.
    // Se mide la banda de 500 Hz desde el primer bloque tras el reinicio.
    const EngineInfo& e = kEngines[kClassic];
    const float f = kFreqSlot0;
    Rig r(e.type, kCoarseStepFrames);
    r.run(kGhostLeadMs, 0, 0);
    r.run(kGhostRegimeEndMs - kGhostLeadMs, f, e.amp);
    const std::vector<float> before = r.mono;
    r.engine.stop();
    ASSERT_TRUE(r.engine.startOffline(kSampleRate, kMaxBlock)) << "el motor no permite reusar la instancia";
    r.mono.clear();
    const long returnFrame = 0;
    const uint64_t probeAtReturn = r.engine.dualTouchSlotBlocksRendered(0);
    r.run(kGhostAfterReturnMs, 0, 0);

    const Rendered ref = render(e.type, DualTouchMixMode::AVERAGE,
                                ghostReference(kGhostRegimeEndMs + kGhostAfterReturnMs, f, e.amp));
    const GhostReading g = readGhost(r.mono, returnFrame, ref.mono, f);
    EXPECT_LE(g.worstBandDb, kGhostBandMaxDb)
        << "AC-052.6 " << e.name << ": tras stop()/startOffline() sin dedos, la banda de " << f
        << " Hz esta a " << g.worstBandDb << " dB del regimen en la ventana " << g.worstWindow
        << " (release fantasma; debe ser <= " << kGhostBandMaxDb << ")";
    EXPECT_EQ(r.engine.dualTouchSlotBlocksRendered(0), probeAtReturn)
        << "AC-052.6 " << e.name << ": el engine del slot 0 se proceso tras reiniciar sin dedos";
    EXPECT_GT(maxAbs(before, outFrame(kGhostRegimeEndMs - 60.0), outFrame(kGhostRegimeEndMs)), kAudible)
        << "control: antes del stop() el dedo no sonaba";
}

TEST(DualTouchGhostControl, Ac0524_AFingerStillHeldWhenTheDualPathReturnsComesBackWithAnAttackAtItsLevel) {
    // d) control positivo de a): el dedo SIGUE apoyado al volver. La voz vuelve con ataque (primer
    // ciclo <= 0,5 del pico, y sin escalon: criterio de AC-052.4) y a su nivel (+-0,5 dB).
    // Seno a 1 kHz (periodo = 48 cuadros = la ventana del primer ciclo), paso fino, y el apagado cae
    // a 1/8 de ciclo: la fase congelada da |sin| = 0,707, asi que sin ataque el primer ciclo es 1.
    const double away = kGhostRegimeEndMs + kOffZeroCrossingMs;
    Rig r(kClassic, kFineStepFrames);
    r.run(kGhostLeadMs, 0, 0);
    r.run(away - kGhostLeadMs, kFineFreq, kFineAmp);
    r.engine.setOscillatorEnabled(false);
    r.run(60.0, kFineFreq, kFineAmp);
    r.engine.setOscillatorEnabled(true);
    const long returnFrame = r.frames();
    r.run(150.0, kFineFreq, kFineAmp);

    const double totalMs = static_cast<double>(r.frames()) / kFramesPerMs;
    Script refScript(kFineStepFrames, totalMs);
    refScript.touch(0, kGhostLeadMs, totalMs, kFineFreq, kFineAmp);
    const Rendered ref = render(kClassic, DualTouchMixMode::AVERAGE, refScript);

    const long regimeFrom = outFrame(kGhostLeadMs + kSettleMs);
    const long regimeTo = outFrame(away);
    const double regimeStep = maxStep(r.mono, regimeFrom, regimeTo);
    const double regimePeak = maxAbs(r.mono, regimeFrom, regimeTo);
    ASSERT_GT(regimePeak, kAudible);

    const long returnOut = returnFrame + kOutputLatencyFrames;
    EXPECT_LE(maxStep(r.mono, returnOut - ms(kEdgeGuardMs), returnOut + ms(kAttackClickWindowMs)),
              regimeStep * kClickTolerance)
        << "AC-052.4: escalon al volver al camino dual con el dedo apoyado (la envolvente quedo abierta)";
    const double firstCycle = peakAfterOnset(r.mono, returnOut - ms(kEdgeGuardMs), returnOut + ms(kAttackClickWindowMs),
                                             kOnsetLevel, kAttackProbeFrames);
    ASSERT_GE(firstCycle, 0.0) << "la voz no vuelve";
    EXPECT_LE(firstCycle, regimePeak * kAttackFirstMsMaxFraction)
        << "AC-052.4: el primer ciclo tras volver con el dedo apoyado esta a " << firstCycle / regimePeak
        << " del pico (con ataque de ~5 ms ~0,2)";
    const long levelAt = returnOut + ms(kSettleMs);
    EXPECT_NEAR(bandDb(r.mono, levelAt, kFineFreq) - bandDb(ref.mono, levelAt, kFineFreq), 0.0, kLevelToleranceDb)
        << "AC-052.4: la voz no vuelve a su nivel";
}

// ===========================================================================
// AC-052.3 — el release suena en la ULTIMA frecuencia
// ===========================================================================

TEST(DualTouchLastFrequency, Ac0523_AfterAFrequencyChangeTheReleaseRingsAtTheNewFrequency) {
    // Un dedo a 500 Hz salta a 620 Hz entre bloques (en regimen) y despues se levanta. El release
    // tiene que estar en la banda de 620 (envolvente del spec) y la de 500 en el piso.
    // Bug que atrapa: el release latchea la frecuencia del ATAQUE, o la vigente al levantar (0 Hz).
    constexpr float kFreqAfter = 620.0f;
    constexpr double kChangeMs = 160.0, kLiftMs = 320.0, kTotalMs = 480.0;
    // 500 Hz queda a 120 Hz de 620 = 2,4 bins: la fuga de Hann de la voz de 620 a esa distancia es
    // ~-31 dB (primer lobulo lateral), asi que "en el piso" se afirma como >= 20 dB bajo la banda
    // de 620 en la misma ventana, no como un nivel absoluto.
    constexpr double kOldBandBelowNewDb = 20.0;
    const EngineInfo& e = kEngines[kClassic];
    Script g(kCoarseStepFrames, kTotalMs);
    g.touch(0, kFirstDownMs, kChangeMs, kFreqSlot0, e.amp).touch(0, kChangeMs, kLiftMs, kFreqAfter, e.amp);
    Script sus(kCoarseStepFrames, kTotalMs);
    sus.touch(0, kFirstDownMs, kChangeMs, kFreqSlot0, e.amp).touch(0, kChangeMs, kTotalMs, kFreqAfter, e.amp);

    const Rendered lifted = render(e.type, DualTouchMixMode::AVERAGE, g);
    const Rendered sustained = render(e.type, DualTouchMixMode::AVERAGE, sus);
    const long liftOut = outFrame(kLiftMs);
    const std::vector<double> series = releaseSeries(lifted.mono, sustained.mono, kFreqAfter, liftOut);
    const ReleaseVerdict v = judgeRelease(series);
    EXPECT_TRUE(v.ok()) << "AC-052.3: el release no esta en la ultima frecuencia (" << kFreqAfter
                        << " Hz) " << describeSeries(series);
    const long w2 = liftOut + kWindowFrames;
    EXPECT_GE(bandDb(lifted.mono, w2, kFreqAfter) - bandDb(lifted.mono, w2, kFreqSlot0), kOldBandBelowNewDb)
        << "AC-052.3: en [20,40) ms sigue sonando la frecuencia ANTERIOR (" << kFreqSlot0 << " Hz)";
}

TEST(DualTouchLastFrequency, Ac0523_ASplitReadWithZeroFrequencyBeforeTheLiftKeepsTheLastValidFrequency) {
    // El control escribe freq antes que amp: un bloque con freq = 0 y amp > 0 llega justo antes del
    // soltar. El release tiene que estar en la ultima frecuencia VALIDA (> 0), no en 0 Hz.
    // Bug que atrapa: latchear la frecuencia del ultimo bloque con amp > 0 sin validarla.
    constexpr double kLiftMs = 305.0, kTotalMs = 480.0;
    const EngineInfo& e = kEngines[kClassic];
    Script g(kCoarseStepFrames, kTotalMs);
    g.touch(0, kFirstDownMs, 300.0, kFreqSlot0, e.amp).touch(0, 300.0, kLiftMs, 0.0f, e.amp);
    Script sus(kCoarseStepFrames, kTotalMs);
    sus.touch(0, kFirstDownMs, kTotalMs, kFreqSlot0, e.amp);

    const Rendered lifted = render(e.type, DualTouchMixMode::AVERAGE, g);
    const Rendered sustained = render(e.type, DualTouchMixMode::AVERAGE, sus);
    EXPECT_TRUE(lifted.allFinite);
    const std::vector<double> series = releaseSeries(lifted.mono, sustained.mono, kFreqSlot0, outFrame(kLiftMs));
    const ReleaseVerdict v = judgeRelease(series);
    EXPECT_TRUE(v.ok()) << "AC-052.3: tras un bloque con freq=0 y amp>0 el release no esta en la ultima frecuencia valida ("
                        << kFreqSlot0 << " Hz) " << describeSeries(series);
}

TEST(DualTouchLastFrequency, Ac0523_ANonFiniteAmplitudeOrFrequencyBeforeTheLiftIsNotRetainedAndTheReleaseKeepsTheLastValidFrequency) {
    // Un bloque con +Inf o NaN en amp o en freq llega justo antes del soltar (el control no valida).
    // Con amp no finita el dedo NO cuenta como apoyado: el release arranca en ese bloque. Con freq
    // no finita el dedo sigue apoyado y la frecuencia retenida es la ultima valida. En los dos
    // casos la salida es finita y el release suena en la ultima frecuencia VALIDA, con la caida
    // de AC-052.3.
    // Bug que atrapa: retener +Inf como amplitud o como frecuencia; el release hereda el Inf y la
    // mezcla (0 * Inf, Inf / Inf) lo vuelve NaN en la salida.
    constexpr double kBadMs = 300.0, kLiftMs = 305.0, kTotalMs = 480.0;
    constexpr float kInf = std::numeric_limits<float>::infinity();
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    const EngineInfo& e = kEngines[kClassic];

    struct Variant {
        const char* name;
        float freq, amp;
        bool ampIsBad;  // amp no finita => el release arranca en el bloque malo
    };
    const Variant variants[] = {
        {"+Inf en amp", kFreqSlot0, kInf, true},
        {"NaN en amp", kFreqSlot0, kNaN, true},
        {"+Inf en freq", kInf, e.amp, false},
        {"NaN en freq", kNaN, e.amp, false},
    };
    Script sus(kCoarseStepFrames, kTotalMs);
    sus.touch(0, kFirstDownMs, kTotalMs, kFreqSlot0, e.amp);
    const Rendered sustained = render(e.type, DualTouchMixMode::AVERAGE, sus);

    for (const Variant& var : variants) {
        SCOPED_TRACE(var.name);
        Script g(kCoarseStepFrames, kTotalMs);
        g.touch(0, kFirstDownMs, kBadMs, kFreqSlot0, e.amp).touch(0, kBadMs, kLiftMs, var.freq, var.amp);

        const Rendered lifted = render(e.type, DualTouchMixMode::AVERAGE, g);
        EXPECT_TRUE(lifted.allFinite) << "AC-052.3: hay muestras no finitas con " << var.name;
        const double releaseFromMs = var.ampIsBad ? kBadMs : kLiftMs;
        const std::vector<double> series =
            releaseSeries(lifted.mono, sustained.mono, kFreqSlot0, outFrame(releaseFromMs));
        const ReleaseVerdict v = judgeRelease(series);
        EXPECT_TRUE(v.ok()) << "AC-052.3: con " << var.name
                            << " el release no esta en la ultima frecuencia valida (" << kFreqSlot0 << " Hz) "
                            << describeSeries(series);
    }
}
