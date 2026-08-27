package com.watermellonstudios.audio.internal.bridge

import kotlin.test.fail

/**
 * Cuántas funciones `JNIEXPORT` **ejecutó** el arnés en esta corrida.
 *
 * El numerador se ANOTA al cruzar la frontera ([JniHarness.exercise]), no se
 * declara: un número escrito a mano envejece sin cambiar de color, que es
 * exactamente cómo el bloque de conteos de `CLAUDE.md` quedó stale cuatro veces
 * seguidas.
 *
 * El denominador y la línea `N de M` que el gate imprime son de REQ-016 S3.
 */
internal object JniCoverage {

    private val executed = linkedSetOf<String>()

    @Synchronized
    fun record(nativeName: String) {
        executed += nativeName
    }

    @Synchronized
    fun executedNames(): List<String> = executed.toList()

    /**
     * Cierre de una clase del arnés: **cero funciones ejecutadas es un fallo.**
     *
     * Una corrida que no ejerció nada se ve desde afuera igual que una que ejerció
     * todo —`ok 1s`— y ésa es la forma en que mueren los gates de este repo.
     */
    @Synchronized
    fun requireSomethingExecuted(owner: String) {
        if (executed.isEmpty()) {
            fail(
                "$owner terminó sin haber ejecutado UNA sola función JNIEXPORT. " +
                    "Un arnés que no ejerció nada no es cobertura: es un verde vacío.",
            )
        }
        println("[REQ-016] funciones JNIEXPORT ejecutadas: ${executed.size} -> ${executed.joinToString()}")
    }
}
