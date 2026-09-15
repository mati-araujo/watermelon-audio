package com.watermellonstudios.audio.internal.bridge

import org.junit.After
import org.junit.AfterClass
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse

/**
 * REQ-042 S1 — **la perilla de la ambiencia del font, cruzando de verdad** (AC-042.6).
 *
 * Tres `JNIEXPORT` nuevas ejecutadas contra un `JNIEnv` real: `nativeSfSetAmbience(jfloat,
 * jfloat)` y los dos getters que devuelven `jfloat`. Lo que sólo se puede afirmar cruzando:
 * que los dos `Float` viajan en el ORDEN que Kotlin declara (el set es asimétrico a propósito),
 * que un `NaN` de Kotlin llega como `NaN` de C y se ignora del otro lado, y que lo saturado
 * vuelve saturado.
 *
 * ## Por qué esta clase declara EXACTAMENTE tres, y las otras cinco de `nativeSf*` no están
 *
 * El grupo `nativeSf*` tiene ocho entradas y hasta este REQ tenía **cero** cobertura en el
 * arnés. Las cinco que siguen en el hueco —`nativeSfNoteOn`, `nativeSfNoteOff`,
 * `nativeSfNoteOffAll`, `nativeSfNoteOffAllExcept`, `nativeSfSetTouchExpression`— despachan a
 * VOCES: encolan un evento que consume `render()` en el thread de audio, y su único efecto
 * observable son muestras. En el host no hay bomba de render (el `FakeAudioBackend` guarda el
 * callback y nunca lo invoca) ni font que suene, así que ejercerlas acá sería cobertura
 * **write-only**: el mismo muro que descartó arp/escala en REQ-024 y que
 * `SoundFontJniTest` deja escrito. Las tres de acá son justamente **las que no suenan**: leen y
 * escriben dos atómicos, y por eso son afirmables sin render (decisión 6 de la amplificación:
 * el grupo `sf` queda 3 de 8, y va escrito).
 *
 * ## Ningún test de acá arranca el motor, y eso ES una afirmación
 *
 * `ensureEngine()` lo construye perezosamente y los engines nacen en el constructor del
 * dispatcher, así que la perilla existe antes de `prepare()` y de `start()`. Si algún día
 * hiciera falta arrancar, este archivo se pondría rojo solo.
 *
 * Los `@Test` comparten el motor (singleton de proceso, una JVM por clase), así que cada uno
 * pone lo suyo antes de afirmar y el `@After` devuelve la perilla a 1/1 —el neutro— para no
 * dejarle estado al siguiente. El DEFAULT (1/1 en un motor virgen) no se afirma acá porque
 * dependería del orden de los tests; lo afirma `test_c_api_soundfont_ambience.cpp` sobre un
 * motor nuevo por test.
 *
 * 🔴 Verde acá NO significa "la ambiencia está probada": esto valida la **frontera** sobre un
 * backend falso. Que 0/0 rinda el seco muestra a muestra lo afirma la suite de C++. Ver el
 * KDoc de [JniHarness].
 */
class SoundFontAmbienceJniTest {

    companion object {
        private const val OWNER = "SoundFontAmbienceJniTest"

        private val COVERED = setOf(
            "nativeSfSetAmbience",
            "nativeSfGetAmbienceReverb",
            "nativeSfGetAmbienceChorus",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)
    }

    private fun <T> jni(name: String, call: (AudioNativeBridge) -> T): T =
        JniHarness.exercise(OWNER, name, call)

    private fun poner(reverb: Float, chorus: Float) =
        jni("nativeSfSetAmbience") { it.sfSetAmbience(reverb, chorus) }

    private fun reverb(): Float = jni("nativeSfGetAmbienceReverb") { it.sfGetAmbienceReverb() }

    private fun chorus(): Float = jni("nativeSfGetAmbienceChorus") { it.sfGetAmbienceChorus() }

    @After
    fun backToNeutral() = poner(1.0f, 1.0f)

    /**
     * AC-042.5 — set → get por los dos buses. Asimétrico: un cruce de argumentos del lado C
     * (`chorus, reverb`) pasa cualquier valor simétrico y muere acá.
     */
    @Test
    fun `el set llega a los dos getters en el orden declarado`() {
        poner(0.3f, 0.6f)
        assertEquals(0.3f, reverb(), "el reverb no cruzó, o cruzó al bus equivocado")
        assertEquals(0.6f, chorus(), "el chorus no cruzó, o cruzó al bus equivocado")
    }

    /**
     * AC-042.2 — un `NaN` de Kotlin es un `NaN` de C (`jfloat` es IEEE 754 de los dos lados) y
     * el motor lo ignora por bus: el otro valor, válido, SÍ entra.
     */
    @Test
    fun `un NaN deja su bus como estaba y el otro valor entra igual`() {
        poner(0.3f, 0.3f)
        poner(Float.NaN, 0.4f)
        assertEquals(0.3f, reverb(), "un NaN cambió el reverb")
        assertEquals(0.4f, chorus(), "el chorus válido no entró porque el reverb era NaN")
        assertFalse(reverb().isNaN(), "el NaN llegó al atómico y vuelve como NaN")

        poner(0.7f, Float.NaN)
        assertEquals(0.7f, reverb())
        assertEquals(0.4f, chorus(), "un NaN cambió el chorus")
    }

    /** AC-042.2 — fuera de rango satura a 0..1 y lo que vuelve es lo saturado. */
    @Test
    fun `fuera de rango satura a cero y a uno`() {
        poner(1.5f, -1.0f)
        assertEquals(1.0f, reverb(), "1,5 no saturó a 1")
        assertEquals(0.0f, chorus(), "-1 no saturó a 0")
    }
}
