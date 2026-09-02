package com.watermellonstudios.audio.domain.engine

/**
 * Metadata de UN parámetro de un synth engine: lo que una UI necesita para generar su
 * control sin saber nada del engine (REQ-028).
 *
 * ## Estos valores NO se escriben acá, y esa es la razón de existir de este archivo
 *
 * Cada engine de C++ los declara en su `getParameterDef()`, y esta clase es sólo la
 * **forma** en la que cruzan: nombre, etiqueta corta, mínimo, máximo y default. Antes de
 * REQ-028 no cruzaban por ningún lado —ni C API, ni JNI, ni Kotlin— y el consumidor que
 * el KDoc de C++ promete («used by UI to auto-generate controls») se alimentaba de una
 * **segunda copia escrita a mano**, con los quince valores tipeados.
 *
 * 🔴 Por eso acá no hay una tabla de valores, ni la va a haber: sería la tercera copia.
 * Los valores se piden con `IAudioNativeBridge.getEngineParameterDef()`, que los lee del
 * engine. Que lo expuesto coincida con lo implementado lo vigila la suite de C++
 * (`test_c_api_engine_params.cpp`), que compara **numéricamente** contra la clase
 * concreta del engine.
 *
 * @property name nombre para mostrar, p. ej. `"Brightness"`
 * @property shortName etiqueta corta para UI compacta, p. ej. `"BRIGHT"`
 * @property minValue mínimo del rango
 * @property maxValue máximo del rango
 * @property defaultValue valor con el que el engine arranca
 */
data class EngineParameterDef(
    val name: String,
    val shortName: String,
    val minValue: Float,
    val maxValue: Float,
    val defaultValue: Float,
) {
    companion object {
        /** Cantidad de strings que el nativo devuelve para el par nombre / etiqueta. */
        const val NAME_COUNT: Int = 2

        /** Cantidad de floats que el nativo devuelve para `min`, `max` y `default`. */
        const val RANGE_COUNT: Int = 3

        /**
         * Arma un def con las dos respuestas del nativo, o **`null`** si cualquiera de
         * las dos falta o viene con una forma que no es la del contrato.
         *
         * 🔴 **Un hueco NO se completa con ceros.** Un `("", "", 0, 0, 0)` es una
         * medición con cara de válida, y el llamador no la puede distinguir de un
         * parámetro real cuyo rango sea 0..0. Es la misma clase que el
         * `("Unknown", "?", 0, 1, 0)` que la C API deja de propagar, y la razón por la
         * que un índice inválido llega hasta acá como `null`.
         *
         * Vive en `commonMain` —y no en cada plataforma— para que haya UN solo criterio
         * de rechazo: el JNI y el cinterop arman con esta misma función, así que el test
         * que la cubre los cubre a los dos.
         *
         * @param names `[name, shortName]`, o `null` si el nativo rechazó el pedido
         * @param range `[minValue, maxValue, defaultValue]`, o `null` idem
         */
        fun fromNative(names: Array<String>?, range: FloatArray?): EngineParameterDef? {
            if (names == null || range == null) return null
            if (names.size != NAME_COUNT || range.size != RANGE_COUNT) return null
            return EngineParameterDef(
                name = names[0],
                shortName = names[1],
                minValue = range[0],
                maxValue = range[1],
                defaultValue = range[2],
            )
        }
    }
}
