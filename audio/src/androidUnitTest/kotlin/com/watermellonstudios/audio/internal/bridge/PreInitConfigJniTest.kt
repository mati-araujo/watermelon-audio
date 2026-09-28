package com.watermellonstudios.audio.internal.bridge

import kotlinx.coroutines.runBlocking
import org.junit.AfterClass
import org.junit.FixMethodOrder
import org.junit.runners.MethodSorters
import java.io.File
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import kotlin.test.fail

/**
 * REQ-045 S2 — **la configuración llamada antes del init LLEGA**, ejecutado contra un
 * `JNIEnv` real (AC-045.4).
 *
 * ## El defecto
 *
 * Cincuenta entradas pasaban el handle del motor sin asegurar que existiera:
 *
 * ```cpp
 * wma_set_bpm(g_wmaEngine, bpm);   // g_wmaEngine == nullptr -> no pasó nada
 * ```
 *
 * La C API rechaza el handle nulo y devuelve, así que **no hay crash, no hay log y no hay
 * valor de retorno**: el consumidor configuró el motor y el motor nunca se enteró. Es la
 * otra forma de "un fallo que no cruza la frontera" —un no-op mudo— y NoisyPad lo mitiga
 * re-aplicando su configuración entera después del arranque (D2 de sus 25 cartas).
 *
 * La decisión 2 del REQ es que la configuración llega: `ensureEngine()` crea el motor **sin
 * abrir stream**, así que es la opción "encolar" de la carta, gratis.
 *
 * ## Por qué el conjunto ENTERO y no una muestra
 *
 * Un test de `setBpm` deja las otras 67 sin afirmar, y una configuración perdida no se
 * distingue de una aplicada mirando el código: las dos compilan y ninguna dice nada. Así
 * que se ejerce el conjunto entero, **derivado del árbol** por
 * `scripts/check-jni-preinit.py` y versionado en `scripts/jni-preinit-config.txt`.
 *
 * 🔴 **Ese archivo es el trinquete, en las dos direcciones.** Si el árbol gana una
 * configuración y nadie la agrega acá, `a - …` se pone rojo; si acá se saca una, también.
 * El lint y este test leen la MISMA lista a propósito: dos listas paralelas derivan por
 * separado, que es exactamente cómo se erosiona una cobertura sin cambiar de color.
 *
 * ## Las dos aserciones, y por qué la premisa SÍ se puede afirmar acá
 *
 * `isEngineInitialized()` es una lectura: pregunta por el motor **sin crearlo**. Así que a
 * diferencia del test de ausencia de S1, acá la premisa ("todavía no hay motor") se afirma
 * de verdad antes de cada llamada, y no queda apoyada en el orden de los métodos.
 *
 * Lo que la hace repetible es [HostTestHooks.resetEngine], una palanca del `.so` del arnés:
 * el motor es un singleton de proceso, así que el estado "no hay motor" existe una sola vez
 * por JVM. Con una JVM por clase (`forkEvery = 1`) eso alcanzaba para una configuración.
 *
 * 🔴 Backend FALSO adentro: valida la frontera JNI/Kotlin, **no** audio en dispositivo. Ver
 * el KDoc de [JniHarness].
 */
@FixMethodOrder(MethodSorters.NAME_ASCENDING)
class PreInitConfigJniTest {

    companion object {
        private const val OWNER = "PreInitConfigJniTest"

        /**
         * El BPM del test de lectura-de-vuelta. **No** es 120: ése es el default que la C
         * API devuelve sin motor, así que con 120 un valor que LLEGÓ no se distinguiría de
         * uno que nunca se aplicó — los dos leerían lo mismo. Es la misma razón por la que
         * S1 eligió canales y modo distintos del default inventado.
         */
        private const val BPM = 97f

        private const val NO_FADE = 0

        /**
         * Cada configuración del conjunto y **cómo se la llama desde la superficie de
         * producción**.
         *
         * Se entra por el envoltorio de `AudioNativeBridge` y no por la `external fun`
         * porque los guards de Kotlin son parte del camino: uno que devolviera temprano
         * dejaría la configuración perdida igual, y por ahí tiene que pasar el test. Los
         * valores son válidos a propósito — un valor que el envoltorio rechaza mediría el
         * guard, no la frontera.
         */
        private val CONFIGURACIONES: Map<String, (AudioNativeBridge) -> Unit> = linkedMapOf(
            "nativeAddEffect" to { b -> b.addEffectSync(0) },
            "nativeClearMappingConfig" to { b -> b.clearMappingConfig(0) },
            "nativeEnableVoiceSystem" to { b -> b.enableVoiceSystem(true) },
            "nativeLooperSetCapabilities" to { b -> b.looperSetCapabilities(8L shl 20, 4, 30) },
            "nativeLooperSetEnabled" to { b -> b.looperSetEnabled(true) },
            "nativeLooperSetExportSampleRate" to { b -> b.looperSetExportSampleRate(44100) },
            "nativeLooperSetFreeLength" to { b -> b.looperSetFreeLength(true) },
            "nativeLooperSetMasterVolume" to { b -> b.looperSetMasterVolume(0.8f) },
            "nativeLooperSetTailMs" to { b -> b.looperSetTailMs(120) },
            "nativeLooperSetTrackMuted" to { b -> b.looperSetTrackMuted(0, true) },
            "nativeLooperSetTrackPan" to { b -> b.looperSetTrackPan(0, 0.25f) },
            "nativeLooperSetTrackPercussionMode" to { b -> b.looperSetTrackPercussionMode(0, true) },
            "nativeLooperSetTrackPlayCount" to { b -> b.looperSetTrackPlayCount(0, 2) },
            "nativeLooperSetTrackSendToFx" to { b -> b.looperSetTrackSendToFx(0, true) },
            "nativeLooperSetTrackSpeed" to { b -> b.looperSetTrackSpeed(0, 1.5f) },
            "nativeLooperSetTrackVolume" to { b -> b.looperSetTrackVolume(0, 0.7f) },
            "nativeSetArpEnabled" to { b -> b.setArpEnabled(true) },
            "nativeSetArpGateLength" to { b -> b.setArpGateLength(0.5f) },
            "nativeSetArpLatch" to { b -> b.setArpLatch(true) },
            "nativeSetArpOctaveRange" to { b -> b.setArpOctaveRange(2) },
            "nativeSetArpPattern" to { b -> b.setArpPattern(1) },
            "nativeSetArpProbability" to { b -> b.setArpProbability(0.9f) },
            "nativeSetArpRatchet" to { b -> b.setArpRatchet(true) },
            "nativeSetArpScaleIntervals" to { b -> b.setArpScaleIntervals(intArrayOf(0, 2, 4, 5, 7, 9, 11)) },
            "nativeSetArpSubdivision" to { b -> b.setArpSubdivision(0.25f) },
            "nativeSetArpSwing" to { b -> b.setArpSwing(0.1f) },
            "nativeSetArpVelocity" to { b -> b.setArpVelocity(0.8f) },
            "nativeSetArpVelocityVariation" to { b -> b.setArpVelocityVariation(0.2f) },
            "nativeSetAudioMode" to { b -> runBlocking { b.setAudioMode(1) } },
            "nativeSetBpm" to { b -> b.setBpm(BPM) },
            "nativeSetDualTouchMixMode" to { b -> b.setDualTouchMixMode(1) },
            "nativeSetDualTouchMode" to { b -> b.setDualTouchMode(true) },
            "nativeSetEffectsBypass" to { b -> b.setEffectsBypassSync(true) },
            "nativeSetEngineParameter" to { b -> b.setEngineParameter(0, 0.5f) },
            "nativeSetEngineType" to { b -> b.setEngineType(1) },
            "nativeSetFeedbackAmount" to { b -> b.setFeedbackAmount(0.3f) },
            "nativeSetFrequencyAndAmplitude" to { b -> b.setFrequencyAndAmplitude(440f, 0.5f) },
            "nativeSetFrequencyRange" to { b -> b.setFrequencyRange(100f, 1000f) },
            "nativeSetInputGain" to { b -> b.setInputGain(3f) },
            "nativeSetInputSource" to { b -> b.setInputSourceSync(0) },
            "nativeSetMappingConfig" to { b -> b.setMappingConfig(0, 0, 0, 0, 0, 0f, 1f, false) },
            "nativeSetMasterVolume" to { b -> b.setMasterVolume(0.6f) },
            "nativeSetMaxVoices" to { b -> b.setMaxVoices(8) },
            "nativeSetModulatorParameter" to { b -> b.setModulatorParameter(0, 0.5f) },
            "nativeSetModulatorType" to { b -> b.setModulatorType(1) },
            "nativeSetMonitoringEnabled" to { b -> b.setMonitoringEnabledSync(true) },
            "nativeSetMonitoringVolume" to { b -> b.setMonitoringVolume(0.5f) },
            "nativeSetNoiseGateEnabled" to { b -> b.setNoiseGateEnabled(true) },
            "nativeSetNoiseGateThreshold" to { b -> b.setNoiseGateThreshold(-40f) },
            "nativeSetOscillatorType" to { b -> b.setOscillatorType(1) },
            "nativeSetParallelMix" to { b -> b.setParallelMix(0.5f) },
            "nativeSetRoutingMode" to { b -> b.setRoutingMode(1) },
            "nativeSetSecondaryOscillatorType" to { b -> b.setSecondaryOscillatorType(1) },
            "nativeSetSoundFontPreset" to { b -> b.setSoundFontPreset(0) },
            "nativeSetSynthVolume" to { b -> b.setSynthVolume(0.7f) },
            "nativeSetUseBackendManager" to { b -> b.setUseBackendManager(true) },
            "nativeSetVocoderCarrierFrequency" to { b -> b.setVocoderCarrierFrequency(220f) },
            "nativeSetVocoderCarrierSource" to { b -> b.setVocoderCarrierSource(true) },
            "nativeSetVocoderModulatorSource" to { b -> b.setVocoderModulatorSource(true) },
            "nativeSetVoiceFilterCutoff" to { b -> b.setVoiceFilterCutoff(1000f) },
            "nativeSetVoiceFilterEnabled" to { b -> b.setVoiceFilterEnabled(true) },
            "nativeSetVoiceFilterMode" to { b -> b.setVoiceFilterMode(1) },
            "nativeSetVoiceFilterResonance" to { b -> b.setVoiceFilterResonance(0.5f) },
            "nativeSetVoiceStealingStrategy" to { b -> b.setVoiceStealingStrategy(0) },
            "nativeSetXY" to { b -> b.setXY(0.5f, 0.5f, false) },
            "nativeSfSetAmbience" to { b -> b.sfSetAmbience(1f, 1f) },
            "nativeSfSetTouchExpression" to { b -> b.sfSetTouchExpression(0, 1f) },
            "nativeTransportSetBeatsPerBar" to { b -> b.transportSetBeatsPerBar(4) },
        )

        /**
         * El conjunto **derivado del árbol** por `check-jni-preinit.py --update`.
         *
         * Falla si no lo encuentra: un conjunto que no se pudo leer daría un trinquete
         * vacío, y "0 de 0" se compara igual de verde. Misma regla que
         * [JniExports.fromTree].
         */
        private fun conjuntoDeclarado(): Set<String> {
            var dir: File? = File(System.getProperty("user.dir")).absoluteFile
            while (dir != null) {
                val f = File(dir, "scripts/jni-preinit-config.txt")
                if (f.isFile) {
                    return f.readLines()
                        .map { it.trim() }
                        .filter { it.isNotEmpty() && !it.startsWith("#") }
                        .toSet()
                }
                dir = dir.parentFile
            }
            fail(
                "no encontré 'scripts/jni-preinit-config.txt' subiendo desde " +
                    "'${System.getProperty("user.dir")}'. Sin el conjunto derivado del árbol no hay " +
                    "trinquete: esta clase compararía su lista contra nada.",
            )
        }

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, CONFIGURACIONES.keys)
    }

    private val bridge get() = AudioNativeBridge.getInstance()

    /**
     * **AC-045.4 — el trinquete del conjunto, en las dos direcciones.**
     *
     * Va primero (`a -`) porque es la premisa de todo lo demás: si la lista de esta clase y
     * la que deriva el lint se separan, el test de abajo seguiría verde ejerciendo MENOS.
     */
    @Test
    fun `a - la lista de esta clase es la que el lint deriva del arbol`() {
        val declarado = conjuntoDeclarado()
        val ejercido = CONFIGURACIONES.keys
        val faltan = declarado - ejercido
        val sobran = ejercido - declarado
        assertTrue(
            faltan.isEmpty() && sobran.isEmpty(),
            buildString {
                append("el conjunto derivado del árbol tiene ${declarado.size} configuraciones y ")
                append("esta clase ejerce ${ejercido.size}.\n")
                if (faltan.isNotEmpty()) {
                    append("  SIN EJERCER: ${faltan.joinToString()}\n")
                    append("  Son configuraciones nuevas del árbol. El arreglo es agregarlas a\n")
                    append("  CONFIGURACIONES, NO sacarlas de scripts/jni-preinit-config.txt.\n")
                }
                if (sobran.isNotEmpty()) {
                    append("  EJERCE DE MÁS: ${sobran.joinToString()}\n")
                    append("  Ya no están en el conjunto derivado: o el lint cambió de regla, o el\n")
                    append("  nombre está mal escrito y esta clase mide algo que no existe.\n")
                }
            },
        )
    }

    /**
     * **AC-045.4 — `setBpm(97)` antes del init, y el motor lo tiene después de arrancar.**
     *
     * El caso literal de la carta, de punta a punta: se configura sin motor, se arranca, y
     * el valor está. Sin `ensureEngine()` esto leía 120 —el default de la C API— y nadie se
     * enteraba.
     *
     * La premisa se AFIRMA (no se asume): `isEngineInitialized()` es una lectura que no crea
     * el motor, así que la ausencia es observable.
     */
    @Test
    fun `b - setBpm antes del init llega al motor`() {
        assertFalse(
            bridge.isEngineInitialized(),
            "esta clase necesita una JVM virgen y el motor ya existía. Con motor, la propiedad " +
                "que mide (\"la config llega ANTES del init\") no se puede observar: cualquier " +
                "implementación pasaría.",
        )

        jni("nativeSetBpm") { it.setBpm(BPM) }

        assertTrue(
            bridge.isEngineInitialized(),
            "setBpm no creó el motor: la configuración se perdió sin rastro (D2).",
        )
        assertTrue(
            runBlocking { bridge.startEngineWithFade(NO_FADE) }.isSuccess,
            "el arranque falló, así que la lectura de abajo no diría nada del BPM",
        )
        assertEquals(
            BPM,
            bridge.getBpm(),
            "el motor arrancó con un BPM que no es el que se configuró antes del init. Si dice " +
                "120, es el default de la C API: la configuración nunca llegó.",
        )
        assertTrue(runBlocking { bridge.stopEngineWithFade(NO_FADE) }.isSuccess)
    }

    /**
     * **AC-045.4 — la propiedad vale para el conjunto ENTERO.**
     *
     * Por cada configuración: se vuelve al estado sin motor, se afirma esa ausencia, se la
     * llama, y el motor tiene que existir. El mensaje de fallo nombra la configuración, así
     * que un rojo dice exactamente cuál se pierde.
     */
    @Test
    fun `c - las 68 configuraciones del conjunto crean el motor`() {
        for ((nombre, llamada) in CONFIGURACIONES) {
            HostTestHooks.resetEngine()
            assertFalse(
                bridge.isEngineInitialized(),
                "$nombre: el reset no dejó el proceso sin motor, así que esta vuelta no mide la " +
                    "propiedad pre-init.",
            )

            jni(nombre) { llamada(it) }

            assertTrue(
                bridge.isEngineInitialized(),
                "$nombre no creó el motor: llamada antes del init, esa configuración se pierde " +
                    "sin rastro — no hay crash, no hay log y no hay retorno (D2). El arreglo es " +
                    "`if (!ensureEngine()) return;` en su JNIEXPORT.",
            )
        }
    }

    /**
     * **AC-045.4 — donde hay lectura de vuelta, el VALOR llegó, no sólo el motor.**
     *
     * 🔴 Este test existe porque `isEngineInitialized()` es un observable **demasiado
     * débil**, y se midió: seis de las cincuenta entran por `wma_input_*`, que exige el
     * InputNode y no sólo el motor (`if (!engine || !engine->inputNode) return;`). Con
     * `ensureEngine()` a secas esas seis creaban el motor —así que el test de arriba pasaba
     * en verde— y la configuración **se perdía igual**. Por eso llevan `ensureInputNode()`,
     * y por eso el valor se lee de vuelta acá.
     *
     * Las seis están todas; el resto son control. Los valores son distintos de cualquier
     * default plausible: con el default, un valor que LLEGÓ no se distingue de uno que no.
     *
     * Hueco declarado: `setNoiseGateThreshold` no tiene getter en la C API (se buscó: cero
     * `wma_input_get_noise_gate_threshold`), así que de las seis es la única sin lectura de
     * vuelta. Lo que la cubre es compartir el `ensureInputNode()` con las otras cinco.
     */
    @Test
    fun `d - el valor configurado antes del init se lee de vuelta`() {
        // Las seis que necesitan el InputNode, no sólo el motor.
        verificar("nativeSetInputGain", { it.setInputGain(7.5f) }) { it.getInputGain() to 7.5f }
        verificar("nativeSetInputSource", { it.setInputSourceSync(2) }) { it.getInputSource() to 2 }
        verificar("nativeSetNoiseGateEnabled", { it.setNoiseGateEnabled(true) }) {
            it.isNoiseGateEnabled() to true
        }
        verificar("nativeSetMonitoringEnabled", { it.setMonitoringEnabledSync(true) }) {
            it.isMonitoringEnabled() to true
        }
        verificar("nativeSetMonitoringVolume", { it.setMonitoringVolume(0.375f) }) {
            it.getMonitoringVolume() to 0.375f
        }
        // Control, en tres subsistemas distintos del de entrada.
        verificar("nativeLooperSetMasterVolume", { it.looperSetMasterVolume(0.625f) }) {
            it.looperGetMasterVolume() to 0.625f
        }
        verificar("nativeLooperSetTailMs", { it.looperSetTailMs(137) }) { it.looperGetTailMs() to 137 }
        verificar("nativeTransportSetBeatsPerBar", { it.transportSetBeatsPerBar(7) }) {
            it.transportGetBeatsPerBar() to 7
        }
    }

    /** Resetea, configura pre-init, y afirma que la lectura de vuelta trae lo configurado. */
    private fun verificar(
        nombre: String,
        configurar: (AudioNativeBridge) -> Unit,
        leer: (AudioNativeBridge) -> Pair<Any, Any>,
    ) {
        HostTestHooks.resetEngine()
        assertFalse(bridge.isEngineInitialized(), "$nombre: el reset no dejó el proceso sin motor")

        jni(nombre) { configurar(it) }

        val (leido, esperado) = leer(bridge)
        assertEquals(
            esperado,
            leido,
            "$nombre se llamó antes del init y el motor NO tiene el valor: leyó $leido donde se " +
                "configuró $esperado. Crear el motor no alcanza si la configuración vive un nivel " +
                "más abajo (el InputNode): ahí va `ensureInputNode()`.",
        )
    }

    private fun jni(name: String, call: (AudioNativeBridge) -> Unit) =
        JniHarness.exercise(OWNER, name) { b -> call(b) }
}
