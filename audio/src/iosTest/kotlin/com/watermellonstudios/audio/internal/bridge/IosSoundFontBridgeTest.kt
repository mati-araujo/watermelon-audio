package com.watermellonstudios.audio.internal.bridge

import kotlin.test.AfterTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * `ISoundFontBridge` sobre cinterop, corriendo en el simulador.
 *
 * ## Lo que esta suite SÍ cubre, y por qué no cubre lo obvio
 *
 * Lo obvio sería cargar un `.sf2` y verificar presets. **No hay uno en este repo**, y
 * el único a mano es el font bundleado de NoisyPad, que pesa ~10 MB: meter ese binario
 * como fixture cuesta más de lo que rinde, y el parser de SF2 ya tiene sus tests del
 * lado C++. Lo que NO tiene tests es la capa nueva — el marshalling de cinterop y los
 * guards — y es lo que esta suite ataca.
 *
 * Los dos tests que valen más son los de rechazo, y no son triviales:
 *
 * - **basura de 4 KB → `false`.** Si el puntero o el tamaño se marshallaran mal (el
 *   error clásico: pasar el largo en elementos donde C espera bytes, o al revés), el
 *   parser leería fuera del buffer. El resultado no sería un `false` prolijo sino un
 *   crash o un `true` sobre memoria ajena. Que devuelva `false` **y el proceso siga
 *   vivo** es la prueba de que el par (puntero, tamaño) llega bien.
 * - **array vacío → `false`.** En iOS esto no es cortesía: sin el guard de
 *   [IosAudioBridge], `addressOf(0)` sobre el array de largo cero tira
 *   `ArrayIndexOutOfBoundsException`. Está medido: sacando el guard, este test falla
 *   con exactamente esa excepción.
 *
 * No arranca el motor, igual que [IosAudioBridgeTest]: ninguna de estas llamadas lo
 * necesita — cargar parsea sincrónicamente y los getters leen estado, no el render.
 */
class IosSoundFontBridgeTest {

    private val bridge = IosAudioBridge()

    @AfterTest
    fun cleanup() {
        bridge.unloadSoundFont()
    }

    // ==================== Estado de reposo ====================

    /**
     * Sin font cargado, los cinco lectores dicen "no hay nada" en vez de inventar.
     *
     * Los dos `null` son el caso que más importa: sus funciones en C devuelven `bool`
     * y escriben en out-params, así que una implementación que ignorara el `bool`
     * devolvería `[0, 0]` — un rango de teclas perfectamente creíble y falso.
     */
    @Test
    fun withNoSoundFontLoadedEveryReaderReportsEmpty() {
        assertFalse(bridge.isSoundFontLoaded(), "no debería haber un font cargado al arrancar")
        assertEquals(0, bridge.getSoundFontPresetCount(), "no puede haber presets sin font")
        assertNull(bridge.getSoundFontPresetName(0), "no hay nombre que devolver")
        assertNull(bridge.getSoundFontPresetKeyRange(0), "no hay rango que devolver")
        assertNull(bridge.getSoundFontPresetBankProgram(0), "no hay bank/program que devolver")
    }

    // ==================== Guards de carga ====================

    /**
     * El guard que en iOS separa un `false` de una `ArrayIndexOutOfBoundsException`.
     *
     * Si este test empieza a fallar con esa excepción en vez de con la aserción, lo que
     * se cayó es el guard de [IosAudioBridge] — no la aserción.
     */
    @Test
    fun anEmptyByteArrayIsRejectedWithoutCrashing() {
        assertFalse(bridge.loadSoundFont(ByteArray(0)), "un array vacío no puede cargar nada")
        assertFalse(bridge.isSoundFontLoaded(), "el motor no debería haber quedado con algo cargado")
    }

    @Test
    fun aBlankPathIsRejected() {
        assertFalse(bridge.loadSoundFontFromPath(""), "la cadena vacía no es una ruta")
        assertFalse(bridge.loadSoundFontFromPath("   "), "sólo espacios tampoco es una ruta")
        assertFalse(bridge.isSoundFontLoaded(), "no debería haber cargado nada")
    }

    // ==================== El marshalling, de verdad ====================

    /**
     * 4 KB de basura llegan al parser y vuelven como `false`, con el proceso entero.
     *
     * Ver el KDoc de la clase: éste es el test que puede distinguir un marshalling
     * bueno de uno roto. El contenido no es aleatorio para que sea reproducible.
     */
    @Test
    fun garbageBytesReachTheParserAndAreRejected() {
        val garbage = ByteArray(4096) { (it % 251).toByte() }

        assertFalse(bridge.loadSoundFont(garbage), "4 KB de basura no son un SoundFont")
        assertFalse(bridge.isSoundFontLoaded(), "un rechazo no puede dejar el motor 'cargado'")
        assertEquals(0, bridge.getSoundFontPresetCount(), "un rechazo no puede dejar presets")
    }

    /**
     * Una ruta que no existe recorre el camino entero —`String` a `const char*`, mmap
     * fallido— y vuelve como `false`. Es el round-trip del otro overload de carga.
     */
    @Test
    fun aNonExistentPathRoundTripsToFalse() {
        assertFalse(
            bridge.loadSoundFontFromPath("/no/existe/esto.sf2"),
            "una ruta inexistente no puede cargar",
        )
        assertFalse(bridge.isSoundFontLoaded(), "no debería haber cargado nada")
    }

    // ==================== Lo que se puede llamar sin font ====================

    /**
     * Los de polifonía, la expresión por toque y el selector de preset son llamables sin
     * font cargado.
     *
     * No es una hipótesis: es el orden real en el que ocurren las cosas — un `noteOff`
     * de limpieza puede llegar después de un [unloadSoundFont]. Lo que se verifica es
     * que ninguno corrompa el motor, y la prueba de eso es que los lectores sigan
     * respondiendo coherentemente después.
     */
    @Test
    fun theNoteApiIsSafeToCallWithoutASoundFontLoaded() {
        bridge.setSoundFontPreset(0)
        bridge.setSoundFontPreset(-1)
        bridge.sfNoteOn(touchId = 0, midiNote = 60, velocity = 0.8f)
        bridge.sfNoteOff(touchId = 0)
        bridge.sfNoteOffAll()
        bridge.sfNoteOffAllExcept(keepTouchId = 0)
        bridge.sfSetTouchExpression(touchId = 0, expression = 0.5f)
        bridge.unloadSoundFont()

        assertFalse(bridge.isSoundFontLoaded(), "el motor quedó inconsistente")
        assertEquals(0, bridge.getSoundFontPresetCount(), "el motor quedó inconsistente")
    }

    /**
     * `unloadSoundFont()` dos veces seguidas no es un error.
     *
     * Importa porque el `@AfterTest` de esta misma suite lo llama siempre, incluso
     * cuando el test ya descargó.
     */
    @Test
    fun unloadingTwiceIsIdempotent() {
        bridge.unloadSoundFont()
        bridge.unloadSoundFont()

        assertTrue(!bridge.isSoundFontLoaded(), "descargar dos veces no puede dejar algo cargado")
    }
}
