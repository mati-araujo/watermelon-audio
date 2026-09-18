package com.watermellonstudios.audio.internal.bridge

/**
 * Leer una serie por hop **por la cota, y reintentar si la región creció entre la cota y
 * el análisis** (REQ-043, auditoría de #343). Lógica pura, compartida por los dos bridges
 * —Android e iOS— para que no diverja en silencio, y **testeable sin motor**: la cota y la
 * lectura son lambdas.
 *
 * ## El problema
 *
 * Los bridges dimensionan por `(loopEnd − loopStart) / hop + 1`, leída en dos cruces
 * (`loopEnd`, `loopStart`) ANTES del análisis. `getLoopEnd()` sin región explícita es el
 * largo de la toma, y eso **crece** cuando una toma termina o con `setLoopRegion` desde la
 * UI. Escenario: región de 3 s ⇒ cota 301; entre los cruces pasa a 10 s; el motor llena los
 * 301 y devuelve 301; sin esto salen 301 puntos que cubren 3 s de una pista de 10 — y la
 * promesa "nunca trunca" del contrato no se sostiene.
 *
 * ## La regla
 *
 * Si el motor escribió **exactamente** la cota, se re-lee la cota. Si la re-leída es
 * **mayor**, se reintenta con ella; si no, lo escrito es la serie entera (para pitch,
 * `written == bound` también es legítimo sin crecimiento: pasa cuando `W ≤ L mod hop`).
 * Como mucho [MAX_ATTEMPTS] lecturas; en la última se devuelve lo que hay — el caso
 * patológico de una región que no para de crecer, y ahí una serie corta es mejor que un
 * bucle. El llamador lo sabe por el KDoc de la interfaz: sobre una pista que se está
 * grabando o disparando la copia puede salir rasgada y hay que re-analizar al terminar.
 */
internal object BoundedRead {

    /** Lecturas como máximo: la primera más dos reintentos por crecimiento. */
    const val MAX_ATTEMPTS = 3

    /**
     * @param bound     la cota superior de elementos, medida sobre la región AHORA.
     * @param readOnce  reserva `bound` y analiza; devuelve `(escritos, resultado recortado)`.
     */
    inline fun <T> read(bound: () -> Int, readOnce: (bound: Int) -> Pair<Int, T>): T {
        var current = bound()
        var attempt = 1
        while (true) {
            val (written, result) = readOnce(current)
            if (written < current || attempt >= MAX_ATTEMPTS) return result
            val reread = bound()
            if (reread <= current) return result
            current = reread
            attempt++
        }
    }
}
