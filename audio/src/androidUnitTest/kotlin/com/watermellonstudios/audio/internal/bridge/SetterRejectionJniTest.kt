package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.domain.error.NativeBridgeException
import com.watermellonstudios.audio.domain.input.CaptureOutcome
import kotlinx.coroutines.runBlocking
import org.junit.AfterClass
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

/**
 * REQ-045 S2 — **los setters que dicen cuándo no hicieron lo pedido** (AC-045.5), ejecutado
 * contra un `JNIEnv` real.
 *
 * ## El defecto
 *
 * El cruce JNI de estos setters **ya devolvía** el código de la C API: `nativeSetModulatorType`
 * es una `jint` desde siempre. Lo que se perdía era un renglón más arriba, en Kotlin:
 *
 * ```kotlin
 * override fun setModulatorType(type: Int) {
 *     nativeSetModulatorType(type)   // el jint se cae al piso
 * }
 * ```
 *
 * `wma_set_modulator_type` rechaza un id fuera de `0..7` con
 * `WMA_ERROR_INVALID_PARAMETER_ID`, el JNI lo devuelve entero, y el envoltorio lo tira: el
 * consumidor pidió un modulador que no existe y leyó silencio. S0 midió **siete** descartes
 * de esta forma (decisión 6 del REQ).
 *
 * 🔴 **Es una clase distinta de la que agarra `check-jni-results.py`.** Ese gate mira la
 * forma C++; acá el C++ está impecable y el que descarta es Kotlin. La decisión 6 dejó
 * fuera un gate permanente para la forma Kotlin (su parser tiene dos clases de falso
 * positivo conocidas, S0), así que lo que la sostiene es este test.
 *
 * ## Y va con su gemelo
 *
 * "No mentir" es trivial para una implementación que dice que no a todo, así que cada
 * rechazo va con su camino de éxito afirmado igual de fuerte. `setEffectsBypassSync` **sólo**
 * tiene camino de éxito y eso está dicho: su `wma_*` no puede fallar con un motor vivo, y
 * desde 2.4 el motor siempre existe. Lo que prueba es que el `jint` viaja.
 *
 * 🔴 Backend FALSO adentro: valida la frontera JNI/Kotlin, **no** audio en dispositivo.
 */
class SetterRejectionJniTest {

    companion object {
        private const val OWNER = "SetterRejectionJniTest"

        /** Fuera de `0..7`, el rango que `wma_set_modulator_type` declara. */
        private const val MODULADOR_INEXISTENTE = 99

        /** `CAPTURE_ONLY`: el modo que el motor NO implementa (decisión 3). */
        private const val USB_CAPTURE_ONLY = 1
        private const val USB_FULL_DUPLEX = 2
        private const val USB_PLAYBACK_ONLY = 0

        private val COVERED = setOf(
            "nativeSetModulatorType",
            "nativeSetModulatorParameter",
            "nativeSetEffectsBypass",
            "nativeAddEffect",
            "nativeRemoveEffect",
            "nativeSetUsbStreamingMode",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)
    }

    private val bridge get() = AudioNativeBridge.getInstance()

    /** La causa tipada de un `Result.failure`, o un fallo que la nombra. */
    private inline fun <reified E : NativeBridgeException> causa(
        resultado: Result<*>,
        que: String,
    ): E {
        val error = resultado.exceptionOrNull()
        assertNotNull(error, "$que devolvió ÉXITO: el rechazo del motor se perdió en Kotlin (D3).")
        return assertIs<E>(error, "$que falló, pero con la causa equivocada: $error")
    }

    /**
     * **AC-045.5 — un id de modulador que no existe llega como `Result.failure`.**
     *
     * Con su gemelo: un id válido tiene que decir sí, o el test estaría aprobando una
     * implementación que rechaza todo.
     */
    @Test
    fun `un modulador inexistente se rechaza y uno valido no`() {
        val malo = jni("nativeSetModulatorType") { it.setModulatorType(MODULADOR_INEXISTENTE) }
        causa<NativeBridgeException.InvalidParameterId>(
            malo,
            "setModulatorType($MODULADOR_INEXISTENTE), fuera de 0..7,",
        )

        assertTrue(
            jni("nativeSetModulatorType") { it.setModulatorType(1) }.isSuccess,
            "un id de modulador válido falló: el test de arriba estaría aprobando un bridge " +
                "que rechaza todo",
        )
    }

    /**
     * **AC-045.5 — el parámetro del modulador, por los DOS rechazos que tiene.**
     *
     * Uno lo decide el nativo (`param_id < 0`) y el otro Kotlin (un valor no finito, que ni
     * llega a cruzar). Los dos terminaban en el mismo silencio, así que los dos tienen que
     * terminar en un `failure`.
     */
    @Test
    fun `el parametro del modulador rechaza el id negativo y el valor no finito`() {
        causa<NativeBridgeException.InvalidParameterId>(
            jni("nativeSetModulatorParameter") { it.setModulatorParameter(-1, 0.5f) },
            "setModulatorParameter(-1, …), con id negativo,",
        )

        // Éste SÍ cruza desde el review de S2: el `if (!value.isFinite())` que lo paraba en
        // Kotlin adelantaba el rechazo del valor al del `paramId`, y con `(-1, NaN)` las dos
        // plataformas contestaban distinto. Ahora el orden lo fija la C API.
        causa<NativeBridgeException.ParameterOutOfRange>(
            jni("nativeSetModulatorParameter") { it.setModulatorParameter(0, Float.NaN) },
            "setModulatorParameter(0, NaN)",
        )

        // Y la PARIDAD misma, que es el punto: con id inválido Y valor no finito manda el
        // id, en las dos plataformas (el test gemelo vive en iosTest).
        causa<NativeBridgeException.InvalidParameterId>(
            jni("nativeSetModulatorParameter") { it.setModulatorParameter(-1, Float.NaN) },
            "setModulatorParameter(-1, NaN): manda el id, no el valor,",
        )

        assertTrue(
            jni("nativeSetModulatorParameter") { it.setModulatorParameter(0, 0.5f) }.isSuccess,
            "un parámetro de modulador válido falló",
        )
    }

    /**
     * **AC-045.5 — un índice de efecto que no existe llega como `Result.failure`.**
     *
     * Los cuatro envoltorios de efectos que descartaban el `jint` guardaban el índice en
     * Kotlin y **devolvían sin hacer nada**: con la cadena vacía, `removeEffect(0)` era un
     * no-op mudo. Ahora dice `InvalidEffectIndex`.
     *
     * Los cuatro se paran en el guard de Kotlin, así que no cruzan: se llaman directo. El
     * que sí cruza —y por eso está anotado— es el camino de éxito con un efecto real.
     */
    @Test
    fun `un indice de efecto inexistente se rechaza en los cuatro`() {
        runBlocking { bridge.clearAllEffects() }
        val vacia = bridge.getEffectChainSize()
        assertEquals(0, vacia, "la premisa es una cadena vacía; con efectos, el índice 0 valdría")

        causa<NativeBridgeException.InvalidEffectIndex>(
            bridge.removeEffectSync(0), "removeEffectSync(0) sobre una cadena vacía",
        )
        causa<NativeBridgeException.InvalidEffectIndex>(
            bridge.setEffectParameterSync(0, 0, 0.5f), "setEffectParameterSync(0, …) sobre una cadena vacía",
        )
        // La paridad con iOS, medida: con índice inválido Y valor no finito manda el
        // ÍNDICE. Antes del review, Android decía InvalidEffectIndex y iOS
        // ParameterOutOfRange para esta misma llamada.
        causa<NativeBridgeException.InvalidEffectIndex>(
            bridge.setEffectParameterSync(99, 0, Float.NaN),
            "setEffectParameterSync(99, 0, NaN): manda el índice, no el valor,",
        )
        causa<NativeBridgeException.InvalidEffectIndex>(
            bridge.setEffectBypassSync(0, true), "setEffectBypassSync(0, …) sobre una cadena vacía",
        )
        causa<NativeBridgeException.InvalidEffectIndex>(
            bridge.reorderEffectsSync(0, 1), "reorderEffectsSync(0, 1) sobre una cadena vacía",
        )

        // El gemelo: con un efecto de verdad, los mismos caminos dicen sí y CRUZAN.
        assertTrue(jni("nativeAddEffect") { it.addEffectSync(0) }, "no pude agregar el efecto del control positivo")
        assertTrue(bridge.setEffectParameterSync(0, 0, 0.5f).isSuccess, "setEffectParameterSync sobre un efecto real falló")
        assertTrue(bridge.setEffectBypassSync(0, true).isSuccess, "setEffectBypassSync sobre un efecto real falló")
        assertTrue(
            jni("nativeRemoveEffect") { it.removeEffectSync(0) }.isSuccess,
            "removeEffectSync sobre un efecto real falló",
        )
    }

    /**
     * **AC-045.5 — el bypass global propaga el `jint`.**
     *
     * Sin rechazo posible y dicho: con un motor vivo `wma_effect_set_global_bypass` sólo
     * devuelve `WMA_OK`, y desde 2.4 el motor siempre existe. Lo que este test afirma es que
     * el código **viaja** en vez de caerse al piso — la forma, no un veredicto negativo.
     */
    @Test
    fun `el bypass global propaga su resultado`() {
        assertTrue(
            jni("nativeSetEffectsBypass") { it.setEffectsBypassSync(true) }.isSuccess,
            "el bypass global devolvió failure con el motor vivo",
        )
        assertTrue(bridge.isEffectsBypassedSync(), "dijo éxito y no bypasseó: el resultado miente")
        assertTrue(bridge.setEffectsBypassSync(false).isSuccess)
    }

    /**
     * **AC-045.5 — el modo de streaming USB devuelve el `CaptureOutcome`, y el 1 se rechaza.**
     *
     * `BackendManager::setFullDuplexEnabled` descartaba el `CaptureOutcome` de
     * `requestCapture`, y la C API era `void`: pedir captura y que no pasara nada era
     * indistinguible de pedirla y que pasara.
     *
     * El modo 1 (`CAPTURE_ONLY`) **no está implementado**: el código de hoy lo trataba como
     * 0, o sea que el consumidor pedía captura y recibía reproducción, en silencio. La
     * decisión 3 del REQ es rechazarlo explícitamente sin tocar el modo vigente — que **el
     * modo no se toca** lo afirma la suite de C++ (`test_capture_requests.cpp`), que es la
     * única que puede leer el flag del backend.
     */
    @Test
    fun `el modo usb devuelve el resultado de la captura y rechaza el modo 1`() {
        val fullDuplex = jni("nativeSetUsbStreamingMode") { it.setUsbStreamingMode(USB_FULL_DUPLEX) }
        assertTrue(
            fullDuplex.isSuccess,
            "pedir full-duplex falló con el motor vivo: ${fullDuplex.exceptionOrNull()}",
        )
        // El VALOR, no sólo que haya valor: un `assertNotNull` sobre un `CaptureOutcome?`
        // no puede fallar por tipo, así que no afirmaba nada (review de S2). Sin stream
        // abierto la única respuesta honesta es que la captura no está viva.
        assertEquals(
            CaptureOutcome.NOT_LIVE,
            fullDuplex.getOrNull(),
            "sin stream abierto, pedir full-duplex no puede decir LIVE ni PENDING",
        )

        causa<NativeBridgeException.InvalidOperation>(
            jni("nativeSetUsbStreamingMode") { it.setUsbStreamingMode(USB_CAPTURE_ONLY) },
            "setUsbStreamingMode(1), que el motor no implementa,",
        )
        // Distinta causa a propósito: el 1 EXISTE y no está implementado
        // (`InvalidOperation`); el 7 no es un modo (`ParameterOutOfRange`). Colapsarlas
        // haría indistinguible "todavía no" de "nunca".
        causa<NativeBridgeException.ParameterOutOfRange>(
            bridge.setUsbStreamingMode(7),
            "setUsbStreamingMode(7), que no es un modo",
        )

        // El gemelo, y además el que devuelve el proceso a su estado: sin stream abierto la
        // respuesta honesta es que la captura NO está viva.
        val playback = bridge.setUsbStreamingMode(USB_PLAYBACK_ONLY)
        assertEquals(
            CaptureOutcome.NOT_LIVE,
            playback.getOrNull(),
            "sin stream abierto y sin pedir captura, el resultado tiene que ser NOT_LIVE",
        )
    }

    private fun <T> jni(name: String, call: (AudioNativeBridge) -> T): T =
        JniHarness.exercise(OWNER, name) { b -> call(b) }
}
