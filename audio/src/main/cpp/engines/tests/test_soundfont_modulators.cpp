/**
 * test_soundfont_modulators.cpp — REQ-039 S2, tarea 2.2 (AC-039.3).
 *
 * La **transferencia** de un modulador SF2: fuente primaria, `modAmtSrcOper`, las
 * tres curvas, polaridad y dirección.
 *
 * ## 🔴 De dónde salen los valores esperados
 *
 * **No del motor.** El AC lo exige y la razón es concreta: un valor esperado que
 * sale del sistema bajo prueba no es un oráculo, sólo una copia. Acá salen de dos
 * lados, los dos externos:
 *
 * 1. **La fórmula del spec SF2 §8.10**, calculada aparte —en el propio test, con
 *    `std::log10`, sin llamar a la implementación—.
 * 2. **Cuatro números que produjo otra gente**: el README del `SoundFont-Spec-Test`
 *    documenta que con 96 dB cóncava la diferencia entre las velocities 127 y 111
 *    es **2,34 dB**, y FluidSynth 2.6.0 rindió 37,11 / 55,60 / 18,50 dB de rango en
 *    las pruebas #13 A/B/C (medido en S1, con la referencia renderizada acá).
 *
 * Que los dos coincidan es lo que hace creíble a la fórmula. Si sólo tuviera (1),
 * el test verificaría que sé copiar una ecuación.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "../SoundFontModulators.h"

namespace {

using wma::sfmod::applyTransform;
using wma::sfmod::contribution;
using wma::sfmod::Curve;
using wma::sfmod::normalize7bit;
using wma::sfmod::sourceIndexOf;
using wma::sfmod::sourceIsMidiCC;
using wma::sfmod::Transform;
using wma::sfmod::transformOf;

/** Sin transferencia: el escalar neutro para cuando no hay fuente secundaria. */
Transform sinFuente() {
    Transform t;
    t.curve = Curve::Linear;
    t.bipolar = false;
    t.decreasing = false;  // con entrada 1,0 devuelve 1,0
    return t;
}

Transform hecho(Curve c, bool bipolar, bool decreasing) {
    Transform t;
    t.curve = c;
    t.bipolar = bipolar;
    t.decreasing = decreasing;
    return t;
}

/**
 * La cóncava del spec, calculada ACÁ y a mano — el oráculo de (1).
 *
 * Deliberadamente escrita distinto de la implementación: ésta despeja
 * `-(20/96)·log10(x²)` tal como aparece en el spec, en vez de la forma
 * `-(40/96)·log10(x)` que usa el header. Si las dos coinciden en los extremos y en
 * el medio, el álgebra está bien de los dos lados.
 */
double concavaDelSpec(double x) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    const double u = 1.0 - x;
    return std::min(1.0, -(20.0 / 96.0) * std::log10(u * u));
}

/** El modulador por defecto #1: velocity → initialAttenuation, cóncava unipolar decreciente. */
double atenuacionPorDefecto(int velocity, double amountCentibels) {
    return amountCentibels * concavaDelSpec(1.0 - velocity / 127.0);
}

// ---------------------------------------------------------------------------
// (2) Los oráculos externos. Éstos son los que hacen creíble a la fórmula.
// ---------------------------------------------------------------------------

/**
 * 🔑 El número del README de upstream: 2,34 dB entre velocity 127 y 111, con el
 * modulador por defecto de 96 dB. No sale de nuestro motor NI de FluidSynth: sale
 * del documento que define la prueba.
 */
TEST(SoundFontModulators, ReproduceElNumeroQueDocumentaElSpecTest) {
    const Transform concavaDec = hecho(Curve::Concave, /*bipolar=*/false, /*decreasing=*/true);
    const double a127 = 960.0 * applyTransform(normalize7bit(127), concavaDec);
    const double a111 = 960.0 * applyTransform(normalize7bit(111), concavaDec);

    // El README documenta 2,34 dB; los centibeles se dividen por 10.
    EXPECT_NEAR((a111 - a127) / 10.0, 2.34, 0.01)
        << "el escalon 127->111 con 96 dB concava es un numero PUBLICADO, no una preferencia";

    // Y la velocity máxima no atenúa nada: de eso depende que el default #1
    // reemplace a la aproximación de tsf en vez de sumarse a ella.
    EXPECT_NEAR(a127, 0.0, 1e-4);
}

/**
 * 🔑 El rango que rindió FluidSynth 2.6.0 en las pruebas #13 A/B/C, medido en S1
 * sobre la referencia renderizada. Tres amounts distintos sobre la misma curva.
 */
TEST(SoundFontModulators, ReproduceElRangoQueRindioFluidSynth) {
    const Transform concavaDec = hecho(Curve::Concave, false, true);

    struct Caso {
        const char* nombre;
        double amount;
        double fluidsynthDb;
        double tolerancia;
    };
    // La tolerancia crece con la atenuación a propósito: lo que FluidSynth reporta
    // es el RANGO de un render con envolvente, no la atenuación teórica, así que la
    // nota más floja se acerca al piso de la medición. Medido: −0,00 a 37 dB,
    // +0,06 a 55 dB. Una tolerancia plana escondería esa estructura.
    const std::vector<Caso> casos = {
        {"#13 A  96 dB", 960.0, 37.11, 0.02},
        {"#13 B 144 dB", 1440.0, 55.60, 0.10},
        {"#13 C  48 dB", 480.0, 18.50, 0.10},
    };

    for (const auto& c : casos) {
        const double a127 = c.amount * applyTransform(normalize7bit(127), concavaDec);
        const double a15 = c.amount * applyTransform(normalize7bit(15), concavaDec);
        EXPECT_NEAR((a15 - a127) / 10.0, c.fluidsynthDb, c.tolerancia) << c.nombre;
    }
}

/**
 * 🔑 Y por qué `#13 C` coincidía con el motor mientras las otras nueve no.
 *
 * S1 lo midió y lo llamó casualidad sin poder explicarla: el motor da 18,5 dB de
 * rango en las diez sub-pruebas, y `#13 C` (48 dB cóncava) también. Ahora se ve la
 * causa — una cóncava de 48 dB da 18,55 dB, y la curva de velocity **cableada** en
 * `tsf.h:1619` la aproxima de casualidad.
 *
 * Este test fija esa explicación: si alguien "arregla" la constante y `#13 C` deja
 * de coincidir, la coincidencia era otra cosa y hay que volver a mirar.
 */
TEST(SoundFontModulators, LaConcavaDe48dbExplicaLaCoincidenciaDeLaPrueba13C) {
    const Transform concavaDec = hecho(Curve::Concave, false, true);
    const double rango48 =
        480.0 * (applyTransform(normalize7bit(15), concavaDec) -
                 applyTransform(normalize7bit(127), concavaDec)) / 10.0;

    // Lo que el motor da hoy en las diez escaleras, medido en S1: 18,49–18,62 dB.
    EXPECT_GT(rango48, 18.4) << "si la concava de 48 dB no cae en la banda del motor, la";
    EXPECT_LT(rango48, 18.7) << "coincidencia de #13 C tiene otra causa y hay que buscarla";
}

// ---------------------------------------------------------------------------
// (1) La fórmula del spec, calculada aparte.
// ---------------------------------------------------------------------------

/** La implementación tiene que coincidir con el álgebra del spec en todo el rango. */
TEST(SoundFontModulators, LaConcavaCoincideConLaFormulaDelSpecEnTodoElRango) {
    const Transform concavaInc = hecho(Curve::Concave, false, false);
    // Se barre con paso 1/127 (los valores que un controlador MIDI puede tomar) y no
    // con potencias de dos: `0,5` es exacto en binario y esconde defectos de float.
    for (int v = 0; v <= 127; ++v) {
        const double x = v / 127.0;
        EXPECT_NEAR(applyTransform(static_cast<float>(x), concavaInc), concavaDelSpec(x), 1e-4)
            << "velocity " << v;
    }
}

/** Y el default #1 completo, contra su fórmula, para las 128 velocities. */
TEST(SoundFontModulators, ElDefaultUnoCoincideConSuFormulaEnLas128Velocities) {
    const Transform concavaDec = hecho(Curve::Concave, false, true);
    for (int v = 0; v <= 127; ++v) {
        const double esperado = atenuacionPorDefecto(v, 960.0);
        const double medido = 960.0 * applyTransform(normalize7bit(v), concavaDec);
        EXPECT_NEAR(medido, esperado, 0.05) << "velocity " << v;
    }
}

// ---------------------------------------------------------------------------
// Las propiedades que TODAS las curvas tienen que cumplir.
// ---------------------------------------------------------------------------

/**
 * Los extremos son el contrato: sin ellos una curva puede tener la forma correcta y
 * el rango equivocado, y eso se ve como "suena parecido pero más bajo".
 *
 * Se le pide a las CUATRO curvas, no a la que se está mirando: una propiedad que se
 * verifica sobre un caso no es una propiedad.
 */
TEST(SoundFontModulators, TodasLasCurvasUnipolaresVanDeCeroAUno) {
    for (const Curve c : {Curve::Linear, Curve::Concave, Curve::Convex, Curve::Switch}) {
        const Transform inc = hecho(c, /*bipolar=*/false, /*decreasing=*/false);
        EXPECT_NEAR(applyTransform(0.0f, inc), 0.0f, 1e-5) << "curva " << static_cast<int>(c);
        EXPECT_NEAR(applyTransform(1.0f, inc), 1.0f, 1e-5) << "curva " << static_cast<int>(c);

        const Transform dec = hecho(c, false, /*decreasing=*/true);
        EXPECT_NEAR(applyTransform(0.0f, dec), 1.0f, 1e-5) << "curva " << static_cast<int>(c);
        EXPECT_NEAR(applyTransform(1.0f, dec), 0.0f, 1e-5) << "curva " << static_cast<int>(c);
    }
}

/** Monótonas las tres continuas: una curva que se da vuelta en el medio no es una curva. */
TEST(SoundFontModulators, LasTresCurvasContinuasSonMonotonas) {
    for (const Curve c : {Curve::Linear, Curve::Concave, Curve::Convex}) {
        const Transform inc = hecho(c, false, false);
        float previo = -1.0f;
        for (int v = 0; v <= 127; ++v) {
            const float y = applyTransform(normalize7bit(v), inc);
            EXPECT_GE(y, previo) << "curva " << static_cast<int>(c) << ", velocity " << v;
            previo = y;
        }
    }
}

/** La dirección invierte de verdad, en las cuatro curvas. */
TEST(SoundFontModulators, LaDireccionEspejaLaCurvaEnTodasLasCurvas) {
    for (const Curve c : {Curve::Linear, Curve::Concave, Curve::Convex, Curve::Switch}) {
        const Transform inc = hecho(c, false, false);
        const Transform dec = hecho(c, false, true);
        for (int v = 0; v <= 127; ++v) {
            const float x = normalize7bit(v);
            EXPECT_NEAR(applyTransform(x, dec), applyTransform(1.0f - x, inc), 1e-5)
                << "curva " << static_cast<int>(c) << ", velocity " << v;
        }
    }
}

/** Bipolar sale en [-1,1] y cruza el cero en el centro. */
TEST(SoundFontModulators, LasCurvasBipolaresCubrenElRangoConSigno) {
    for (const Curve c : {Curve::Linear, Curve::Concave, Curve::Convex}) {
        const Transform bip = hecho(c, /*bipolar=*/true, false);
        EXPECT_NEAR(applyTransform(0.0f, bip), -1.0f, 1e-5) << "curva " << static_cast<int>(c);
        EXPECT_NEAR(applyTransform(1.0f, bip), 1.0f, 1e-5) << "curva " << static_cast<int>(c);
        EXPECT_NEAR(applyTransform(0.5f, bip), 0.0f, 1e-5) << "curva " << static_cast<int>(c);
    }
}

/** La `switch` salta y no interpola: es la única discontinua a propósito. */
TEST(SoundFontModulators, LaCurvaSwitchSaltaEnElMedio) {
    const Transform sw = hecho(Curve::Switch, false, false);
    EXPECT_NEAR(applyTransform(0.49f, sw), 0.0f, 1e-5);
    EXPECT_NEAR(applyTransform(0.50f, sw), 1.0f, 1e-5);
    EXPECT_NEAR(applyTransform(0.51f, sw), 1.0f, 1e-5);
}

// ---------------------------------------------------------------------------
// La fuente secundaria (`modAmtSrcOper`) y el desempaquetado.
// ---------------------------------------------------------------------------

/**
 * La secundaria **escala** el aporte, no lo desplaza.
 *
 * 🔴 No es un caso raro: 1435 de los 2812 moduladores del font bundleado la
 * declaran (medido con `scripts/read-sf2-modulators.py`), así que un motor que la
 * ignore se equivoca en más de la mitad.
 */
TEST(SoundFontModulators, LaFuenteSecundariaEscalaElAporte) {
    const Transform primaria = hecho(Curve::Linear, false, false);
    const Transform secundaria = hecho(Curve::Linear, false, false);

    const float completo = contribution(1000.0f, 1.0f, primaria, 1.0f, secundaria);
    const float mitad = contribution(1000.0f, 1.0f, primaria, 0.5f, secundaria);
    const float nada = contribution(1000.0f, 1.0f, primaria, 0.0f, secundaria);

    EXPECT_NEAR(completo, 1000.0f, 1e-3);
    EXPECT_NEAR(mitad, 500.0f, 1e-3);
    EXPECT_NEAR(nada, 0.0f, 1e-3) << "con la secundaria en cero el modulador no aporta NADA";
}

/** Sin fuente secundaria el aporte queda intacto — el caso de los diez por defecto. */
TEST(SoundFontModulators, SinFuenteSecundariaElAporteNoSeToca) {
    const Transform concavaDec = hecho(Curve::Concave, false, true);
    for (const int v : {0, 15, 63, 111, 127}) {
        const float conEscalar =
            contribution(960.0f, normalize7bit(v), concavaDec, 1.0f, sinFuente());
        const float sinEscalar = 960.0f * applyTransform(normalize7bit(v), concavaDec);
        EXPECT_NEAR(conEscalar, sinEscalar, 1e-3) << "velocity " << v;
    }
}

/**
 * El desempaquetado de `modSrcOper`: los cinco campos del spec §8.2.
 *
 * El orden de los bits es la clase de detalle que se escribe mal una vez y nadie
 * relee, así que se afirma campo por campo sobre un valor armado a mano.
 */
TEST(SoundFontModulators, DesempaquetaLosCincoCamposDeModSrcOper) {
    // velocity (2), no CC, decreciente (bit 8), unipolar, cóncava (curva 1 << 10)
    const std::uint16_t defaultUno = 0x0502;
    EXPECT_EQ(sourceIndexOf(defaultUno), 2);
    EXPECT_FALSE(sourceIsMidiCC(defaultUno));
    const Transform t = transformOf(defaultUno);
    EXPECT_TRUE(t.decreasing);
    EXPECT_FALSE(t.bipolar);
    EXPECT_EQ(t.curve, Curve::Concave);

    // CC7 bipolar creciente lineal: índice 7 con el bit de CC y el de polaridad.
    const std::uint16_t ccBipolar = 0x0287;
    EXPECT_EQ(sourceIndexOf(ccBipolar), 7);
    EXPECT_TRUE(sourceIsMidiCC(ccBipolar));
    const Transform t2 = transformOf(ccBipolar);
    EXPECT_FALSE(t2.decreasing);
    EXPECT_TRUE(t2.bipolar);
    EXPECT_EQ(t2.curve, Curve::Linear);
}

/**
 * 🔴 El divisor es 127, no 128.
 *
 * Con 128 la velocity máxima daría 0,992 en vez de 1, y el default #1 dejaría un
 * resto de atenuación en la nota más fuerte — un error chico, constante y muy
 * difícil de ver de oído.
 */
TEST(SoundFontModulators, LaVelocityMaximaNormalizaExactamenteAUno) {
    EXPECT_FLOAT_EQ(normalize7bit(127), 1.0f);
    EXPECT_FLOAT_EQ(normalize7bit(0), 0.0f);

    const Transform concavaDec = hecho(Curve::Concave, false, true);
    EXPECT_NEAR(960.0f * applyTransform(normalize7bit(127), concavaDec), 0.0f, 1e-3)
        << "con velocity 127 el default #1 no puede atenuar nada";
}

// ===========================================================================
// AC-039.4 — composición y precedencia entre los CUATRO ámbitos (tarea 2.3).
//
// 🔑 Las reglas están MEDIDAS contra FluidSynth 2.6.0, no leídas de memoria. Se
// generó un font por regla —moduladores idénticos en los cuatro campos, amounts
// distintos— y se midió el rango de nivel entre velocity 127 y 15:
//
//   sólo el default (960 cB) .......... 37,12 dB   (la línea de base)
//   global 960 vs zona 480 ............ 18,60 dB   = 480 solo  -> el local REEMPLAZA
//   instrumento 960 vs preset 480 ..... 55,51 dB   = 1440      -> el preset SUMA
//   uno igual al default con amount 0 ..  0,00 dB               -> reemplaza al default
//   dos en el mismo ámbito (1440, 480) . 18,60 dB   = 480       -> gana el ÚLTIMO
//
// Los tests de abajo afirman el AMOUNT EFECTIVO que esas mediciones implican. Es
// la magnitud que la composición decide; el nivel en dB ya lo cubre la
// transferencia, probada arriba contra sus propios oráculos.
// ===========================================================================

using wma::sfmod::Modulator;
using wma::sfmod::resolve;
using wma::sfmod::Scopes;

/** El modulador por defecto #1: velocity -> initialAttenuation, cóncava unipolar decreciente. */
Modulator defaultUno(std::int16_t amount = 960) {
    Modulator m;
    m.srcOper = 0x0502;  // velocity, no CC, decreciente, unipolar, cóncava
    m.destOper = 48;     // initialAttenuation
    m.amount = amount;
    m.amtSrcOper = 0;
    m.transOper = 0;
    return m;
}

/** El amount efectivo del modulador con esa identidad, tras componer. */
std::int16_t amountEfectivo(const std::vector<Modulator>& resueltos, const Modulator& identidad) {
    for (const auto& m : resueltos) {
        if (m.sameIdentity(identidad)) return m.amount;
    }
    ADD_FAILURE() << "no quedo ningun modulador con esa identidad tras componer";
    return 0;
}

/** 🔑 El delta que S1 dejó pedido: entre global y local del MISMO nivel, gana el local. */
TEST(SoundFontModulators, EnElInstrumentoElAmbitoLocalReemplazaAlGlobal) {
    Scopes s;
    s.instrumentGlobal.push_back(defaultUno(960));
    s.instrumentZone.push_back(defaultUno(480));

    const auto r = resolve({}, s);
    EXPECT_EQ(amountEfectivo(r, defaultUno()), 480)
        << "FluidSynth rinde 18,60 dB en este caso, que es lo mismo que 480 solo: reemplaza";
    EXPECT_EQ(r.size(), 1u) << "reemplazar significa que queda UNO, no dos";
}

/** Y la asimetría: el preset NO reemplaza — suma sobre lo que dejó el instrumento. */
TEST(SoundFontModulators, ElPresetSumaSobreElInstrumentoEnVezDeReemplazarlo) {
    Scopes s;
    s.instrumentZone.push_back(defaultUno(960));
    s.presetZone.push_back(defaultUno(480));

    const auto r = resolve({}, s);
    EXPECT_EQ(amountEfectivo(r, defaultUno()), 1440)
        << "FluidSynth rinde 55,51 dB, que es 1440 cB: el preset es un OFFSET, no un reemplazo";
    EXPECT_EQ(r.size(), 1u);
}

/**
 * 🔴 La asimetría, afirmada como asimetría.
 *
 * Los dos tests de arriba pasarían igual si alguien escribiera las dos reglas como
 * "sumar": el de instrumento daría 1440 y fallaría… pero si alguien las escribiera
 * las dos como "reemplazar", el de preset daría 480 y fallaría. Este las compara
 * **entre sí** para que el par no se pueda satisfacer con una sola regla.
 */
TEST(SoundFontModulators, LasDosReglasSonDISTINTASYNoUnaSola) {
    Scopes enInstrumento;
    enInstrumento.instrumentGlobal.push_back(defaultUno(960));
    enInstrumento.instrumentZone.push_back(defaultUno(480));

    Scopes cruzandoNiveles;
    cruzandoNiveles.instrumentZone.push_back(defaultUno(960));
    cruzandoNiveles.presetZone.push_back(defaultUno(480));

    const auto a = amountEfectivo(resolve({}, enInstrumento), defaultUno());
    const auto b = amountEfectivo(resolve({}, cruzandoNiveles), defaultUno());
    EXPECT_NE(a, b) << "si las dos composiciones dan lo mismo, una de las dos reglas esta mal";
    EXPECT_EQ(a, 480);
    EXPECT_EQ(b, 1440);
}

/** Dentro de un mismo ámbito gana el último declarado. */
TEST(SoundFontModulators, EnElMismoAmbitoGanaElUltimo) {
    Scopes s;
    s.instrumentGlobal.push_back(defaultUno(1440));
    s.instrumentGlobal.push_back(defaultUno(480));

    const auto r = resolve({}, s);
    EXPECT_EQ(amountEfectivo(r, defaultUno()), 480) << "FluidSynth rinde 18,60 dB: gana el ultimo";
    EXPECT_EQ(r.size(), 1u);
}

/**
 * 🔑 Declarar uno igual a un default con amount 0 lo ANULA.
 *
 * Es la sub-prueba #13 E del spec-test, donde FluidSynth rinde **0,00 dB de rango**
 * —la velocity deja de mover el nivel— contra los 18,52 que da el motor hoy. Y es
 * la razón por la que los diez por defecto no podían quedar para otra etapa: sin
 * ellos no hay qué anular.
 */
TEST(SoundFontModulators, DeclararloConAmountCeroAnulaElDefault) {
    Scopes s;
    s.instrumentGlobal.push_back(defaultUno(0));

    const auto r = resolve({defaultUno(960)}, s);
    EXPECT_EQ(amountEfectivo(r, defaultUno()), 0)
        << "FluidSynth rinde 0,00 dB de rango: el declarado REEMPLAZA al default";

    // Y con el amount en cero la transferencia no aporta nada, para ninguna velocity.
    const Transform concavaDec = hecho(Curve::Concave, false, true);
    for (const int v : {0, 15, 63, 127}) {
        EXPECT_NEAR(contribution(0.0f, normalize7bit(v), concavaDec, 1.0f, sinFuente()), 0.0f, 1e-6)
            << "velocity " << v;
    }
}

/** Un modulador de identidad distinta convive: no se pisan por tener el mismo destino. */
TEST(SoundFontModulators, DosIdentidadesDistintasConvivenAunqueCompartanDestino) {
    Modulator porVelocity = defaultUno(960);
    Modulator porTecla = defaultUno(480);
    porTecla.srcOper = 0x0503;  // NoteOnKeyNumber en vez de velocity: OTRA identidad

    Scopes s;
    s.instrumentGlobal.push_back(porVelocity);
    s.instrumentGlobal.push_back(porTecla);

    const auto r = resolve({}, s);
    ASSERT_EQ(r.size(), 2u) << "misma destino pero distinta FUENTE: son dos moduladores";
    EXPECT_EQ(amountEfectivo(r, porVelocity), 960);
    EXPECT_EQ(amountEfectivo(r, porTecla), 480);
}

/** El amount NO entra en la identidad: si entrara, nada se reemplazaría nunca. */
TEST(SoundFontModulators, ElAmountNoFormaParteDeLaIdentidad) {
    const Modulator a = defaultUno(960);
    const Modulator b = defaultUno(480);
    EXPECT_TRUE(a.sameIdentity(b)) << "difieren SOLO en el amount: son el mismo modulador";

    Modulator otroDestino = defaultUno(960);
    otroDestino.destOper = 8;  // initialFilterFc
    EXPECT_FALSE(a.sameIdentity(otroDestino));
}

/** Sin nada declarado, los diez por defecto pasan intactos. */
TEST(SoundFontModulators, SinModuladoresDeclaradosLosDefaultsPasanIntactos) {
    const auto r = resolve({defaultUno(960)}, Scopes{});
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].amount, 960);
}

}  // namespace
