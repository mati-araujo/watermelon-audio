package com.watermellonstudios.audio.internal.bridge

import org.junit.AfterClass
import org.junit.FixMethodOrder
import org.junit.runners.MethodSorters
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * REQ-018 S2 — **el nodo que aparece y se va.**
 *
 * Las 12 restantes del camino de entrada: las que tienen comportamiento observable en el host, y
 * las que **no** — que se ejecutan igual y se declaran como hueco, en vez de taparse con un test
 * de "no explota".
 *
 * ## Lo que esta clase vigila de verdad
 *
 * `nativeGetInputMeteringSnapshot` recorre **null → 7 valores → null**. Es la única transición de
 * estado real del grupo, y es lo que vuelve observable a `nativeReleaseInputNode`, que si no sería
 * sólo una llamada que no revienta.
 *
 * Y el `null` importa más que los 7 valores: **un array de ceros NO es dato ausente, es dato
 * plausible**. Este repo ya shippeó dos stubs cuyos ceros derrotaron los fallbacks elvis de sus
 * propios callers, y `wma_tuner_get_snapshot` documenta la misma regla del otro lado.
 *
 * ## El estado "todavía no hay motor" existe UNA VEZ POR PROCESO
 *
 * `forkEvery = 1` le da a cada CLASE su JVM, pero **no a cada método**: dentro de la clase todos
 * los tests comparten proceso, y el motor nativo no se destruye nunca. Así que las dos
 * observaciones de ausencia —el snapshot en `null` y el centinela de nivel— sólo se pueden hacer
 * antes de que cualquier otro test arranque el motor.
 *
 * De ahí [FixMethodOrder] y que las dos vivan en **un solo test** que ordena primero, con su
 * premisa afirmada. Los demás arrancan el motor ellos mismos, así que no dependen del orden.
 *
 * (Se descubrió midiendo: la primera versión de esta clase tenía las dos aserciones repartidas y
 * salieron rojas — `expected:<-100.0> but was:<-120.0>` — porque otro test ya había creado el
 * motor. Es la misma lección que `TunerEngineJniTest`, un nivel más abajo.)
 *
 * 🔴 Vale lo de siempre: backend FALSO adentro. Frontera JNI/Kotlin, no audio en dispositivo.
 */
@FixMethodOrder(MethodSorters.NAME_ASCENDING)
class InputNodeJniTest {

    companion object {
        private const val OWNER = "InputNodeJniTest"

        /** Trinquete bidireccional — ver `JniCoverage.ratchet`. */
        private val COVERED = setOf(
            "nativeGetInputMeteringSnapshot", "nativeReleaseInputNode",
            "nativeGetInputLevel", "nativeGetInputLevelLinear",
            "nativeIsInputClipping", "nativeIsNoiseGateOpen", "nativeGetInputLatencyMs",
            "nativeStartInputStream", "nativeStopInputStream",
            "nativeIsInputStreamRunning", "nativeIsInputStarting",
            "nativeSetNoiseGateThreshold",
            "nativeStartTuner",
        )

        /** `InputMetering` publica 7 valores; el largo es parte del contrato de la frontera. */
        private const val METERING_VALUES = 7

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)
    }

    private fun <T> jni(name: String, call: (AudioNativeBridge) -> T): T =
        JniHarness.exercise(OWNER, name, call)

    private fun startEngine() =
        assertTrue(jni("nativeStartTuner") { it.startTunerSync() }, "el motor no arrancó")

    /**
     * La transición de tres estados. **El test más valioso de esta tanda.**
     *
     * Se hace en UN solo test y no en tres porque lo que se afirma es la SECUENCIA: que haya `null`
     * antes y `null` después, con datos en el medio, sobre el mismo proceso. Partido en tres, cada
     * pedazo sería verde por separado sin que nadie mire la ida y la vuelta.
     */
    /**
     * Las DOS observaciones de ausencia, juntas y primeras.
     *
     * Juntas porque el estado que describen existe una sola vez por proceso; primeras por
     * [FixMethodOrder]. Si esto sale rojo, el arreglo **no** es relajar los valores: es que ningún
     * otro test de esta clase arranque el motor antes.
     */
    @Test
    fun `a - sin motor el snapshot es null y el nivel es el centinela`() {
        assertNull(
            jni("nativeGetInputMeteringSnapshot") { it.getInputMeteringSnapshot() },
            "sin nodo de entrada el snapshot tiene que ser null — un array de ceros sería una medición " +
                "que nadie hizo. Si acá hay datos, otro test de esta clase ya arrancó el motor: revisá @FixMethodOrder",
        )
        assertEquals(
            -100f,
            jni("nativeGetInputLevel") { it.getInputLevel(0) },
            "el piso SIN motor cambió (o alguien ya arrancó el motor en esta JVM); ver H5 en la spec de REQ-018",
        )
    }

    @Test
    fun `el snapshot de metering va de null a datos y vuelve a null al liberar el nodo`() {
        startEngine()   // crea el nodo de entrada, además del motor

        val live = assertNotNull(
            jni("nativeGetInputMeteringSnapshot") { it.getInputMeteringSnapshot() },
            "con el nodo vivo el snapshot no puede seguir siendo null",
        )
        assertEquals(METERING_VALUES, live.size, "el largo del snapshot de metering es contrato")

        jni("nativeReleaseInputNode") { it.releaseInputNodeSync() }

        assertNull(
            jni("nativeGetInputMeteringSnapshot") { it.getInputMeteringSnapshot() },
            "liberar el nodo tiene que volver a la ausencia; si sigue habiendo datos, se está leyendo un nodo muerto",
        )
    }

    /**
     * 🔴 **H5, anclado a propósito — y es OBSERVACIÓN, no diseño.**
     *
     * `nativeGetInputLevel` devuelve **−100 dB sin motor** y **−120 dB con motor**. Los dos son
     * "silencio", pero son números distintos, y un consumidor no tiene cómo distinguir *"no hay
     * motor"* de *"está muy callado"* salvo por un número mágico que nadie documentó.
     *
     * Se ancla por decisión explícita (D2): si alguien unifica los dos pisos, este test se pone
     * rojo y **lo decide a propósito**, en vez de cambiarle el significado a un consumidor sin
     * enterarse. Lo que NO hay que leer acá es que estos números sean el diseño querido.
     */
    @Test
    fun `con motor el piso de silencio es el otro, y eso esta anclado como hallazgo`() {
        startEngine()

        assertEquals(
            -120f,
            jni("nativeGetInputLevel") { it.getInputLevel(0) },
            "el piso CON motor cambió; ver H5 en la spec de REQ-018 antes de tocar esto",
        )
        assertEquals(
            0f,
            jni("nativeGetInputLevelLinear") { it.getInputLevelLinear(0) },
            "en silencio el nivel lineal es 0",
        )
    }

    @Test
    fun `en silencio no hay clipping ni compuerta abierta`() {
        startEngine()

        assertFalse(jni("nativeIsInputClipping") { it.isInputClipping() }, "sin señal no puede haber clipping")
        assertFalse(jni("nativeIsNoiseGateOpen") { it.isNoiseGateOpen() }, "sin señal la compuerta no puede estar abierta")
        assertEquals(
            0f,
            jni("nativeGetInputLatencyMs") { it.getInputLatencyMs() },
            "sin stream de entrada abierto no hay latencia que informar",
        )
    }

    /**
     * 🔴 **EL HUECO DECLARADO, y este test existe para NOMBRARLO, no para taparlo.**
     *
     * `InputNode::createInputStream` no tiene camino sin Oboe, así que en el host
     * `nativeStartInputStream` devuelve `false` y el stream **nunca se abre**. Estas cuatro cruzan
     * la frontera —que es lo que AC-016.1 promete, y lo que agarra un desajuste de firma— pero su
     * COMPORTAMIENTO no es observable acá.
     *
     * Medido con mutación: volver `nativeStartInputStream` un no-op **SOBREVIVE** a este arnés, y
     * eso está declarado de antemano en el plan de S2. No es debilidad del test: es el límite del
     * host, y taparlo con un `assertFalse` que pase por otra razón sería peor que decirlo.
     *
     * Lo que sí queda cubierto en device: el smoke manual. Nada de lo que corre en el host lo
     * reemplaza.
     */
    @Test
    fun `el stream vivo no es alcanzable en el host, y queda dicho`() {
        startEngine()

        assertFalse(
            jni("nativeStartInputStream") { it.startInputStreamSync() },
            "en el host no hay Oboe: si esto empieza a devolver true, el hueco declarado se cerró y hay que " +
                "actualizar la spec de REQ-018 (H4) en vez de festejar",
        )
        assertFalse(jni("nativeIsInputStreamRunning") { it.isInputStreamRunning() }, "no se abrió, no puede estar corriendo")
        assertFalse(jni("nativeIsInputStarting") { it.isInputStarting() }, "no se abrió, no puede estar arrancando")

        // Cruza la frontera y no rompe nada. Su efecto no es observable sin stream.
        jni("nativeStopInputStream") { it.stopInputStreamSync() }
        assertFalse(jni("nativeIsInputStreamRunning") { it.isInputStreamRunning() }, "sigue sin correr, como antes")
    }

    /**
     * `nativeSetNoiseGateThreshold` no tiene lector por la frontera (D3): lo único que se puede
     * afirmar es que **cruza con un `jfloat`**, que es el eje de FIRMA. Su efecto sólo se ve con
     * señal, y en el host no hay.
     *
     * El valor no es potencia de dos por la misma razón de siempre: `−42,5` no sobrevive a una
     * conversión mal declarada.
     */
    @Test
    fun `el umbral de la compuerta cruza como jfloat, y su efecto es hueco declarado`() {
        startEngine()

        jni("nativeSetNoiseGateThreshold") { it.setNoiseGateThreshold(-42.5f) }

        // Lo único observable: el motor sigue coherente después del cruce.
        assertFalse(jni("nativeIsNoiseGateOpen") { it.isNoiseGateOpen() }, "sin señal la compuerta sigue cerrada")
    }
}
