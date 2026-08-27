package com.watermellonstudios.audio.internal.bridge

import org.junit.AfterClass
import org.junit.Before
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * REQ-018 S1 — **los que devuelven lo que se les puso.**
 *
 * Segunda tanda del arnés JNI. Se eligió el camino de **entrada/captura** y no el grupo más
 * grande por una razón que no es de tamaño: es donde este repo encontró defectos reales tres
 * veces (REQ-006, el `48000` literal; REQ-012, el DSP que no seguía al rate y que **nadie
 * llamaba en producción**; REQ-009, la costura del ring). El subsistema con más historial de
 * defectos silenciosos cruzaba la frontera JNI y ningún test ejecutaba ese cruce.
 *
 * ## Por qué DOS valores por par, y ninguno potencia de dos
 *
 * Un solo valor no distingue *"el valor viaja"* de *"el getter devuelve una constante"*. Y una
 * potencia de dos es representable exacto en float, así que esconde defectos de conversión — la
 * lección está escrita en `test_c_api_tuner.cpp`. De ahí `−3,25`, `0,375`, `0,625`.
 *
 * Lo que esto agarra y `check-jni-symbols.py` no puede: un `Float` declarado donde el C++ pone
 * `jdouble`, o un `Int` donde pone `jlong`. Compila de los dos lados, linkea, pasa ese gate por
 * comparar sólo NOMBRES, y devuelve basura acá.
 *
 * 🔴 Backend FALSO adentro, igual que el resto del arnés: valida la frontera JNI/Kotlin, **no**
 * audio en dispositivo. Ver el KDoc de [JniHarness].
 */
class InputJniTest {

    companion object {
        private const val OWNER = "InputJniTest"

        /**
         * Lo que esta clase declara cubrir. **Trinquete bidireccional** — ver
         * `JniCoverage.ratchet`: ejercer de menos es rojo, y ejercer de más también, para que
         * sumar cobertura aparezca en el diff del PR en vez de colarse.
         */
        private val COVERED = setOf(
            "nativeSetInputSource", "nativeGetInputSource",
            "nativeSetInputGain", "nativeGetInputGain",
            "nativeSetNoiseGateEnabled", "nativeIsNoiseGateEnabled",
            "nativeSetMonitoringEnabled", "nativeIsMonitoringEnabled",
            "nativeSetMonitoringVolume", "nativeGetMonitoringVolume",
            "nativeSetUsbInputVolume", "nativeGetUsbInputVolume",
            "nativeSetUsbInputMute", "nativeIsUsbInputMuted",
            "nativeModeRequiresInput",
            "nativeStartTuner",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)
    }

    private fun <T> jni(name: String, call: (AudioNativeBridge) -> T): T =
        JniHarness.exercise(OWNER, name, call)

    /**
     * El motor tiene que existir para que estos setters lleguen a algún lado.
     * `nativeStartTuner` es el único de los 310 que lo crea — de ahí que esté en [COVERED].
     */
    @Before
    fun engineUp() {
        assertTrue(jni("nativeStartTuner") { it.startTunerSync() }, "el motor no arrancó")
    }

    @Test
    fun `la fuente de entrada viaja, y sigue al segundo valor`() {
        jni("nativeSetInputSource") { it.setInputSourceSync(2) }
        assertEquals(2, jni("nativeGetInputSource") { it.getInputSource() }, "la fuente no volvió igual")

        jni("nativeSetInputSource") { it.setInputSourceSync(1) }
        assertEquals(1, jni("nativeGetInputSource") { it.getInputSource() }, "la fuente no siguió al segundo valor")
    }

    /** `−3,25` no es potencia de dos: un `jfloat` mal declarado no lo conserva. */
    @Test
    fun `la ganancia de entrada cruza en dB y vuelve bit a bit`() {
        jni("nativeSetInputGain") { it.setInputGain(6.5f) }
        assertEquals(6.5f, jni("nativeGetInputGain") { it.getInputGain() }, "la ganancia no volvió igual")

        jni("nativeSetInputGain") { it.setInputGain(-3.25f) }
        assertEquals(-3.25f, jni("nativeGetInputGain") { it.getInputGain() }, "la ganancia no siguió al segundo valor")
    }

    @Test
    fun `la compuerta de ruido se prende y se apaga, en los dos sentidos`() {
        jni("nativeSetNoiseGateEnabled") { it.setNoiseGateEnabled(true) }
        assertTrue(jni("nativeIsNoiseGateEnabled") { it.isNoiseGateEnabled() }, "se prendió y dice que no")

        jni("nativeSetNoiseGateEnabled") { it.setNoiseGateEnabled(false) }
        assertFalse(jni("nativeIsNoiseGateEnabled") { it.isNoiseGateEnabled() }, "se apagó y dice que sí")
    }

    /**
     * El monitoreo, en los dos sentidos.
     *
     * Importa que se apague de verdad: en un instrumento amplificado, monitoreo encendido sin
     * pedirlo es **realimentación**, no una molestia. Es la misma razón por la que
     * `test_c_api_tuner.cpp` tiene su gemelo `StartingTheTunerNeverTurnsOnMonitoring`.
     */
    @Test
    fun `el monitoreo se prende y se apaga, en los dos sentidos`() {
        jni("nativeSetMonitoringEnabled") { it.setMonitoringEnabledSync(true) }
        assertTrue(jni("nativeIsMonitoringEnabled") { it.isMonitoringEnabled() }, "se prendió y dice que no")

        jni("nativeSetMonitoringEnabled") { it.setMonitoringEnabledSync(false) }
        assertFalse(jni("nativeIsMonitoringEnabled") { it.isMonitoringEnabled() }, "se apagó y dice que sí")
    }

    @Test
    fun `el volumen de monitoreo viaja exacto`() {
        jni("nativeSetMonitoringVolume") { it.setMonitoringVolume(0.375f) }
        assertEquals(0.375f, jni("nativeGetMonitoringVolume") { it.getMonitoringVolume() }, "el volumen no volvió igual")

        jni("nativeSetMonitoringVolume") { it.setMonitoringVolume(0.8125f) }
        assertEquals(0.8125f, jni("nativeGetMonitoringVolume") { it.getMonitoringVolume() }, "el volumen no siguió al segundo valor")
    }

    /**
     * Los cuatro de USB **sin ningún dispositivo conectado**, y no es un atajo.
     *
     * `nativeSetUsbInputVolume` y `nativeSetUsbInputMute` no hablan con libusb: leen y escriben los
     * atómicos de software de `jni_usb.cpp` (`g_usbInputVolume`, `g_usbInputMuted`), que son el
     * camino de fallback cuando el hardware no expone control de volumen. Ese camino es
     * ejecutable en el host y es el que corre en cualquier interfaz que no traiga control propio.
     */
    @Test
    fun `el volumen de entrada USB viaja sin que haya un dispositivo`() {
        jni("nativeSetUsbInputVolume") { it.setUsbInputVolume(0.625f) }
        assertEquals(0.625f, jni("nativeGetUsbInputVolume") { it.getUsbInputVolume() }, "el volumen USB no volvió igual")

        jni("nativeSetUsbInputVolume") { it.setUsbInputVolume(0.1875f) }
        assertEquals(0.1875f, jni("nativeGetUsbInputVolume") { it.getUsbInputVolume() }, "el volumen USB no siguió al segundo valor")
    }

    @Test
    fun `el mute de entrada USB se prende y se apaga`() {
        jni("nativeSetUsbInputMute") { it.setUsbInputMute(true) }
        assertTrue(jni("nativeIsUsbInputMuted") { it.isUsbInputMuted() }, "se muteó y dice que no")

        jni("nativeSetUsbInputMute") { it.setUsbInputMute(false) }
        assertFalse(jni("nativeIsUsbInputMuted") { it.isUsbInputMuted() }, "se desmuteó y dice que sí")
    }

    /**
     * La tabla ENTERA, no un punto.
     *
     * Preguntar por un solo modo no distingue una tabla de una constante. Medido en el host:
     * `false, true, true, false` para los modos 0..3 — o sea que hay dos modos que requieren
     * entrada y dos que no, y el test lo exige así.
     */
    @Test
    fun `que modos requieren entrada es una tabla de cuatro puntos`() {
        val table = (0..3).map { mode -> jni("nativeModeRequiresInput") { it.modeRequiresInput(mode) } }

        assertEquals(
            listOf(false, true, true, false),
            table,
            "la tabla de modos que requieren entrada cambió; si es a propósito, se decide y se actualiza acá",
        )
    }
}
