package com.watermellonstudios.audio.domain.engine

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull

/**
 * REQ-028 S2 — el modelo de `commonMain` y su contrato de rechazo (AC-028.3, AC-028.5).
 *
 * ## Lo que este archivo puede afirmar, y lo que NO
 *
 * Los 15 valores **no están acá**, a propósito. Sólo existen en C++ (AC-028.2: sin una
 * segunda tabla), así que un test de `commonTest` que los declarara sería la TERCERA
 * copia — el defecto que este REQ arregla. Que los seis engines publiquen sus 15
 * parámetros con los cinco campos lo afirma `EngineParamsJniTest`, que cruza la
 * frontera de verdad contra el motor real.
 *
 * Lo que SÍ vive acá es lo único de esta capa que puede estar mal: **el armado**.
 * [EngineParameterDef.fromNative] junta dos respuestas nativas separadas —los nombres y
 * el rango— y las dos pueden faltar o venir con la forma equivocada. Un armador que
 * completara los huecos con ceros sería [[los-dos-stubs-que-mentian]] otra vez: un
 * `("", "", 0, 0, 0)` es una medición con cara de válida.
 *
 * Vive en `commonMain` y no en cada plataforma porque si no habría DOS armadores —el de
 * JNI y el de cinterop— con dos criterios de rechazo y un solo test.
 */
class EngineParameterDefTest {

    private val names = arrayOf("Brightness", "BRIGHT")
    private val range = floatArrayOf(0f, 1f, 0.5f)

    // AC-028.3 — los cinco campos llegan, y llegan en su lugar
    @Test
    fun losCincoCamposSobrevivenElArmado() {
        val def = EngineParameterDef.fromNative(names, range)

        assertEquals(
            EngineParameterDef(
                name = "Brightness",
                shortName = "BRIGHT",
                minValue = 0f,
                maxValue = 1f,
                defaultValue = 0.5f,
            ),
            def,
        )
    }

    /**
     * El orden importa y un test que use `0, 1, 0.5` no lo prueba del todo: `min` y
     * `max` son distinguibles, pero un armado que confundiera `max` con `default`
     * necesita tres valores distintos para caerse. Es
     * [[la-propiedad-hay-que-pedirsela-a-todos]] en chiquito.
     */
    @Test
    fun elOrdenDelRangoNoSePermuta() {
        val def = EngineParameterDef.fromNative(arrayOf("Ratio", "RATIO"), floatArrayOf(-3f, 7f, 2f))!!

        assertEquals(-3f, def.minValue)
        assertEquals(7f, def.maxValue)
        assertEquals(2f, def.defaultValue)
    }

    // AC-028.5 — el rechazo del nativo se PROPAGA; no se completa con ceros
    @Test
    fun unRechazoDeCualquiraDeLasDosMitadesEsUnRechazo() {
        assertNull(EngineParameterDef.fromNative(null, range), "sin nombres no hay def")
        assertNull(EngineParameterDef.fromNative(names, null), "sin rango no hay def")
        assertNull(EngineParameterDef.fromNative(null, null), "sin nada no hay def")
    }

    /**
     * Una respuesta nativa con la forma equivocada tampoco se completa. No es
     * defensivo por gusto: es la única forma que tiene esta capa de notar que el otro
     * lado cambió de contrato, y devolver un def a medias lo haría indetectable.
     */
    @Test
    fun unaFormaInesperadaSeRechazaEnVezDeCompletarse() {
        assertNull(EngineParameterDef.fromNative(arrayOf("Solo el largo"), range))
        assertNull(EngineParameterDef.fromNative(arrayOf("a", "b", "c"), range))
        assertNull(EngineParameterDef.fromNative(names, floatArrayOf(0f, 1f)))
        assertNull(EngineParameterDef.fromNative(names, floatArrayOf(0f, 1f, 0.5f, 9f)))
        assertNull(EngineParameterDef.fromNative(emptyArray(), floatArrayOf()))
    }
}
