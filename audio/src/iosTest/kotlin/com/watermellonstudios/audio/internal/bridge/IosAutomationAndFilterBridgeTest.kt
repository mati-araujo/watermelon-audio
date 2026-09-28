package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.domain.effect.EffectType
import com.watermellonstudios.audio.domain.error.NativeBridgeException
import com.watermellonstudios.audio.domain.input.CaptureOutcome
import kotlinx.coroutines.test.runTest
import kotlin.test.AfterTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertTrue

/**
 * Los doce miembros sueltos de la fachada: automatización, mapeo XY, filtro de voz, las
 * dos mal llamadas "de USB" y los dos lectores de estado.
 *
 * ## Ésta es la suite con más cobertura real de las cuatro, y por una razón concreta
 *
 * La automatización **se puede observar sin arrancar el motor**: el mapeo escribe sobre
 * un parámetro de efecto de verdad, y ese parámetro se lee con `getEffectParameterSync`.
 * O sea que hay un camino completo —configurar el mapeo, mover el eje, leer el efecto—
 * que corre entero en el simulador. Es la diferencia con el arpegiador, donde no había
 * ningún readback que no pasara por el render.
 *
 * ## Lo que NO se puede verificar acá, dicho y no tapado
 *
 * - **El filtro de voz no tiene un solo getter** en la C API (se buscó: cero
 *   `wma_voice_filter_get_*`). Sus cuatro miembros sólo se pueden ejercitar, no
 *   verificar, y por eso el test que los cubre se llama como se llama. Los rangos que la
 *   interfaz declara como contrato quedan sin cobertura observable — hueco declarado.
 * - **El efecto de [IosAudioBridge.configureUsbBackend] tampoco.** Aplica un sample rate
 *   sobre el `BackendManager`, y sin un stream abierto `getStreamInfoArray()` devuelve
 *   `null`, así que no hay dónde leerlo. Se verifica que linkea y no rompe; que el
 *   sample rate llegue es WA-4.3, en device.
 */
class IosAutomationAndFilterBridgeTest {

    private val bridge = IosAudioBridge()

    @AfterTest
    fun cleanup() = kotlinx.coroutines.runBlocking {
        bridge.clearMappingConfig(AXIS_X)
        bridge.clearAllEffects()
        Unit
    }

    // ==================== Lectores de estado ====================

    /** La variante lock-free del conteo sigue a la cadena de verdad. */
    @Test
    fun theEffectChainSizeTracksTheChain() = runTest {
        assertEquals(0, bridge.getEffectChainSize(), "la cadena arranca vacía")

        assertTrue(bridge.addEffect(EffectType.REVERB).isSuccess, "no se pudo agregar el efecto")
        assertEquals(1, bridge.getEffectChainSize(), "el conteo no vio el efecto nuevo")

        assertTrue(bridge.addEffect(EffectType.DELAY).isSuccess, "no se pudo agregar el segundo")
        assertEquals(2, bridge.getEffectChainSize(), "el conteo no vio el segundo")

        bridge.clearAllEffects()
        assertEquals(0, bridge.getEffectChainSize(), "el conteo no vio el vaciado")
    }

    /**
     * Un booleano legible. Es poco, y es todo lo que hay: sin stream abierto el motor no
     * recortó ningún buffer, así que sólo se puede afirmar que la llamada devuelve un
     * valor y no basura.
     */
    @Test
    fun reducedBuffersIsReadable() {
        assertEquals(
            false,
            bridge.isUsingReducedBuffers(),
            "sin stream abierto el motor no puede haber recortado buffers",
        )
    }

    // ==================== Automatización: el camino completo ====================

    /**
     * Mover el eje X mueve el parámetro del efecto mapeado. Es el round-trip que le da
     * sentido a toda la sección.
     *
     * **El extremo inferior NO da `mapMin`, y está medido**: con `mapMin = 0` un
     * `applyAutomation(0)` deja el parámetro en **0.1**, no en 0. El motor recorta al
     * rango propio del parámetro después de aplicar el mapeo, así que un `mapMin` por
     * debajo del mínimo del efecto no se alcanza nunca.
     *
     * Se asevera ese 0.1 en vez de elegir valores que lo eviten: si algún día el rango
     * del parámetro cambia, es mejor que este test lo diga a que pase en silencio.
     */
    @Test
    fun automationDrivesTheMappedEffectParameter() = runTest {
        bridge.addEffect(EffectType.REVERB)
        mapAxisXToFirstParam()

        bridge.applyAutomation(AXIS_X, 1.0f)
        assertEquals(1.0f, bridge.getEffectParameterSync(0, PARAM_ID), "el eje no movió el parámetro")

        bridge.applyAutomation(AXIS_X, 0.5f)
        assertEquals(0.5f, bridge.getEffectParameterSync(0, PARAM_ID), "el mapeo lineal no es lineal")

        bridge.applyAutomation(AXIS_X, 0.0f)
        assertEquals(
            0.1f,
            bridge.getEffectParameterSync(0, PARAM_ID),
            "el recorte al mínimo del parámetro cambió — ver el KDoc de este test",
        )
    }

    /**
     * Desconectar el eje lo desconecta de verdad: el parámetro se queda donde estaba
     * aunque el eje siga moviéndose.
     *
     * Sin esta comprobación, un [IosAudioBridge.clearMappingConfig] que no hiciera nada
     * pasaría desapercibido — el parámetro tendría el último valor igual.
     */
    @Test
    fun clearingTheMappingStopsTheAxisFromDrivingTheParameter() = runTest {
        bridge.addEffect(EffectType.REVERB)
        mapAxisXToFirstParam()
        bridge.applyAutomation(AXIS_X, 1.0f)
        assertEquals(1.0f, bridge.getEffectParameterSync(0, PARAM_ID), "precondición: el eje mueve")

        bridge.clearMappingConfig(AXIS_X)
        bridge.applyAutomation(AXIS_X, 0.0f)

        assertEquals(
            1.0f,
            bridge.getEffectParameterSync(0, PARAM_ID),
            "el eje siguió mandando después de desconectarlo",
        )
    }

    /** La escritura directa, sin pasar por ningún eje. */
    @Test
    fun setAutomationParameterWritesStraightToTheEffect() = runTest {
        bridge.addEffect(EffectType.REVERB)

        bridge.setAutomationParameter(effectIndex = 0, paramId = PARAM_ID, xyValue = 0.25f)

        assertEquals(0.25f, bridge.getEffectParameterSync(0, PARAM_ID), "no escribió el parámetro")
    }

    /**
     * **El guard de no-finito es load-bearing, no cosmética — y esto lo prueba.**
     *
     * Del lado C el recorte es `std::clamp(value, 0.0f, 1.0f)`, y `std::clamp` con `NaN`
     * devuelve `NaN`: sus dos comparaciones son falsas. O sea que el único punto donde un
     * `NaN` se detiene antes de llegar al motor es el `isFinite()` de Kotlin. Si se cae,
     * el parámetro del efecto queda en `NaN` y a partir de ahí el audio es silencio o
     * ruido.
     */
    @Test
    fun aNonFiniteAutomationValueIsDiscardedBeforeReachingTheEngine() = runTest {
        bridge.addEffect(EffectType.REVERB)
        mapAxisXToFirstParam()
        bridge.applyAutomation(AXIS_X, 0.25f)

        bridge.applyAutomation(AXIS_X, Float.NaN)
        assertEquals(0.25f, bridge.getEffectParameterSync(0, PARAM_ID), "un NaN llegó al motor")

        bridge.setAutomationParameter(0, PARAM_ID, Float.POSITIVE_INFINITY)
        assertEquals(0.25f, bridge.getEffectParameterSync(0, PARAM_ID), "un infinito llegó al motor")
    }

    /** Un `mapMin`/`mapMax` no finito descarta la configuración entera. */
    @Test
    fun aNonFiniteMappingRangeIsDiscarded() = runTest {
        bridge.addEffect(EffectType.REVERB)
        bridge.setAutomationParameter(0, PARAM_ID, 0.75f)

        bridge.setMappingConfig(
            axis = AXIS_X, effectIndex = 0, paramId = PARAM_ID,
            curve = 0, polarity = 0,
            mapMin = Float.NaN, mapMax = 1.0f, inverted = false,
        )
        bridge.applyAutomation(AXIS_X, 0.0f)

        assertEquals(
            0.75f,
            bridge.getEffectParameterSync(0, PARAM_ID),
            "el mapeo con rango no finito se aplicó igual",
        )
    }

    // ==================== Lo que sólo se puede ejercitar ====================

    /**
     * Los cuatro del filtro de voz **se ejercitan, no se verifican** — la C API no tiene
     * un solo getter para leerlos de vuelta.
     *
     * Lo que sí cubre: que ninguna de las llamadas, incluidas las que el contrato manda
     * ignorar por estar fuera de rango, deje el motor en un estado del que no pueda
     * seguir operando.
     */
    @Test
    fun theVoiceFilterCanOnlyBeExercisedAndLeavesTheEngineUsable() = runTest {
        bridge.addEffect(EffectType.REVERB)

        bridge.setVoiceFilterEnabled(true)
        bridge.setVoiceFilterCutoff(1_000f)
        bridge.setVoiceFilterCutoff(5f)                    // fuera de rango: se ignora
        bridge.setVoiceFilterCutoff(30_000f)               // fuera de rango: se ignora
        bridge.setVoiceFilterCutoff(Float.NaN)             // no finito: se ignora
        bridge.setVoiceFilterResonance(0.5f)
        bridge.setVoiceFilterResonance(2f)                 // fuera de rango: se ignora
        bridge.setVoiceFilterMode(1)
        bridge.setVoiceFilterMode(99)                      // fuera de rango: se ignora
        bridge.setVoiceFilterEnabled(false)

        assertEquals(1, bridge.getEffectChainSize(), "el motor quedó inutilizable")
    }

    /**
     * Las dos con nombre de USB linkean y corren en iOS.
     *
     * Que linkeen no es trivial y por eso hay un test: el `.a` de iOS no trae todos los
     * `wma_*`, así que si estos dos símbolos no estuvieran, el framework ni se armaría.
     * Su **efecto** —sample rate y full-duplex sobre el `BackendManager`— no se puede
     * leer sin un stream abierto; ver el KDoc de la clase.
     *
     * **AC-045.5 (REQ-045, D3)**: el modo de streaming ya no descarta el tri-estado de la
     * captura, y el modo 1 (`CAPTURE_ONLY`) se rechaza. Las dos cosas viven en la C API y
     * en `CaptureOutcome.fromNativeCode`, compartidas con Android — que es el punto: la
     * misma llamada no puede contestar distinto según el teléfono (eso fue D1).
     */
    @Test
    fun theTwoUsbNamedCallsLinkAndRunOnIos() {
        bridge.configureUsbBackend(sampleRate = 44_100, channels = 2, bitDepth = 16)

        // Sin stream abierto la respuesta honesta es que la captura NO está viva. Lo que
        // se afirma acá es que el valor VIAJA: antes no había valor.
        assertEquals(
            CaptureOutcome.NOT_LIVE,
            bridge.setUsbStreamingMode(FULL_DUPLEX).getOrNull(),
            "sin stream abierto, pedir full-duplex no puede decir LIVE",
        )
        assertEquals(CaptureOutcome.NOT_LIVE, bridge.setUsbStreamingMode(0).getOrNull())

        assertIs<NativeBridgeException.InvalidOperation>(
            bridge.setUsbStreamingMode(CAPTURE_ONLY).exceptionOrNull(),
            "el modo 1 no está implementado y se trataba como 0: quien pedía captura " +
                "recibía reproducción en silencio",
        )
        assertIs<NativeBridgeException.ParameterOutOfRange>(
            bridge.setUsbStreamingMode(7).exceptionOrNull(),
            "un modo que no existe es otra causa que el modo 1, que sí existe",
        )

        assertEquals(0, bridge.getEffectChainSize(), "el motor quedó inconsistente")
    }

    /**
     * **AC-045.5 (REQ-045, D3) — el modulador y los efectos dicen cuándo no hicieron lo
     * pedido, igual que en Android.**
     *
     * iOS tampoco propagaba: `setModulatorType` y los cinco `*Sync` de efectos tiraban el
     * `WmaResult`. La causa sale del MISMO `fromCode` que usa Android, así que las dos
     * plataformas contestan lo mismo — lo que D1 rompía.
     *
     * Acá el índice lo valida la C API (no un guard de Kotlin), y eso hace el test más
     * fuerte que en Android: el rechazo **cruza** de verdad.
     */
    @Test
    fun theRejectedSetterSaysSoOnIos() = runTest {
        assertIs<NativeBridgeException.InvalidParameterId>(
            bridge.setModulatorType(99).exceptionOrNull(),
            "un id de modulador fuera de 0..7 tiene que llegar como failure",
        )
        assertTrue(bridge.setModulatorType(1).isSuccess, "un id válido falló: el de arriba no prueba nada")

        assertIs<NativeBridgeException.ParameterOutOfRange>(
            bridge.setModulatorParameter(0, Float.NaN).exceptionOrNull(),
            "un valor no finito tiene que llegar como failure",
        )
        assertTrue(bridge.setModulatorParameter(0, 0.5f).isSuccess)

        // Cadena vacía: el índice 0 no existe, y lo rechaza la C API.
        assertEquals(0, bridge.getEffectChainSize(), "la premisa es una cadena vacía")
        assertIs<NativeBridgeException.InvalidEffectIndex>(
            bridge.removeEffectSync(0).exceptionOrNull(), "removeEffectSync sobre cadena vacía",
        )
        assertIs<NativeBridgeException.InvalidEffectIndex>(
            bridge.setEffectParameterSync(0, 0, 0.5f).exceptionOrNull(),
            "setEffectParameterSync sobre cadena vacía",
        )
        assertIs<NativeBridgeException.InvalidEffectIndex>(
            bridge.setEffectBypassSync(0, true).exceptionOrNull(),
            "setEffectBypassSync sobre cadena vacía",
        )
        assertIs<NativeBridgeException.InvalidEffectIndex>(
            bridge.reorderEffectsSync(0, 1).exceptionOrNull(), "reorderEffectsSync sobre cadena vacía",
        )

        // El gemelo: con un efecto de verdad, los cuatro dicen sí.
        assertTrue(bridge.addEffectSync(EffectType.REVERB.id), "no pude agregar el efecto del control")
        assertTrue(bridge.setEffectParameterSync(0, 0, 0.5f).isSuccess)
        assertTrue(bridge.setEffectBypassSync(0, true).isSuccess)
        assertTrue(bridge.setEffectsBypassSync(true).isSuccess)
        assertTrue(bridge.setEffectsBypassSync(false).isSuccess)
        assertTrue(bridge.removeEffectSync(0).isSuccess)
    }

    private fun mapAxisXToFirstParam() = bridge.setMappingConfig(
        axis = AXIS_X,
        effectIndex = 0,
        paramId = PARAM_ID,
        curve = 0,       // lineal
        polarity = 0,    // unipolar
        mapMin = 0f,
        mapMax = 1f,
        inverted = false,
    )

    private companion object {
        const val AXIS_X = 0
        const val PARAM_ID = 0
        const val FULL_DUPLEX = 2
        const val CAPTURE_ONLY = 1
    }
}
