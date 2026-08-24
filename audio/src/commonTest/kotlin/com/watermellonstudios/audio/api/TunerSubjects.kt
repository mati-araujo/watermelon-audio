package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.tuner.TunerSnapshot
import com.watermellonstudios.audio.domain.tuning.TuningConfiguration
import com.watermellonstudios.audio.internal.tuner.FakeTunerBridge
import com.watermellonstudios.audio.internal.tuner.TunerImpl

/**
 * Los sujetos que `TunerContractTest` ejerce (MINI-004).
 *
 * ## Por qué existe este archivo
 *
 * `TunerContractTest` se llamaba **contrato** y su único constructor de sujeto devolvía
 * `FakeTuner` siempre — o sea que verificaba el contrato contra el doble que lo cumple **por
 * construcción**. Eso no dice nada sobre un implementador real, y hasta REQ-010 S1 era
 * inevitable porque no había otro. Con [TunerImpl] ya lo hay.
 *
 * ## No hay runner paramétrico, así que la parametrización es explícita
 *
 * `kotlin.test` no tiene `@ParameterizedTest` —no existe en KMP—, así que cada caso itera
 * [tunerSubjects] a mano. La consecuencia obligatoria: **toda aserción tiene que nombrar el
 * sujeto**, o un rojo no dice cuál implementación falló y hay que adivinar. De eso se
 * encarga [TunerSubject.name] y el helper [comoContrato].
 *
 * ## Las tres sondas, y por qué el guion entra en forma NATIVA
 *
 * Un sujeto es la implementación más lo mínimo para observarla desde afuera:
 *
 *  - el [ITuner] en sí;
 *  - [pushedTargets], los Hz que llegaron "al motor" — `0.0` es *"borrá el objetivo"*, una
 *    orden, no una ausencia de empuje;
 *  - [publicar], que guiona la próxima lectura.
 *
 * [publicar] toma el **FloatArray nativo** y no un [TunerSnapshot] armado, y es a propósito:
 * `TunerImpl` decodifica con `TunerSnapshot.fromNative` y `FakeTuner` recibe el objeto ya
 * hecho. Dándoles a los dos la misma entrada cruda, el contrato también afirma que los dos
 * llegan al mismo snapshot — si uno recibiera el objeto y el otro los floats, esa
 * divergencia quedaría fuera de la prueba.
 */
internal class TunerSubject(
    /** Aparece en cada mensaje de fallo. Sin esto, un rojo no dice quién falló. */
    val name: String,
    val tuner: ITuner,
    private val pushes: () -> List<Double>,
    /** Guiona la próxima lectura con los VALUE_COUNT floats en forma nativa. */
    val publicar: (FloatArray) -> Unit,
) {
    /** Vista viva: se consulta después de cada asignación, no se copia al construir. */
    val pushedTargets: List<Double> get() = pushes()

    /** Prefijo de los mensajes de fallo. `assertX(..., "$sujeto no hizo tal cosa")`. */
    override fun toString(): String = name
}

/**
 * 🔴 **Las implementaciones que el contrato ejerce, y la lista que el guard vigila.**
 *
 * `scripts/check-ituner-implementations.py` escanea el fuente buscando implementaciones de
 * `ITuner` y compara contra esta lista **en las dos direcciones**. Sin eso, la
 * parametrización sería cosmética: el tercer implementador se agrega, nadie lo suma acá, y
 * el contrato vuelve a probar sólo a los dos de antes **sin que nada avise** — la suite
 * sigue verde y sigue llamándose "contrato". Es el criterio de muerte de MINI-004, y la
 * única forma honesta de cubrirlo, porque Kotlin/Native no tiene reflection para descubrir
 * implementaciones en runtime.
 */
internal val IMPLEMENTACIONES_EJERCIDAS = listOf("FakeTuner", "TunerImpl")

/** Un sujeto por implementación, todos sobre la misma [configuration]. */
internal fun tunerSubjects(configuration: TuningConfiguration): List<TunerSubject> = listOf(
    sujetoFakeTuner(configuration),
    sujetoTunerImpl(configuration),
)

private fun sujetoFakeTuner(configuration: TuningConfiguration): TunerSubject {
    val fake = FakeTuner(configuration)
    return TunerSubject(
        name = "FakeTuner",
        tuner = fake,
        pushes = { fake.pushedTargets },
        publicar = { nativo ->
            // El doble consume snapshots ya decodificados: se decodifica acá, con la misma
            // función que usa el impl real, para que la entrada de los dos sea la misma.
            fake.scriptedSnapshots.addLast(
                checkNotNull(TunerSnapshot.fromNative(nativo)) {
                    "el guion trae un array que fromNative rechaza: ${nativo.size} valores"
                },
            )
        },
    )
}

private fun sujetoTunerImpl(configuration: TuningConfiguration): TunerSubject {
    val bridge = FakeTunerBridge()
    return TunerSubject(
        name = "TunerImpl",
        tuner = TunerImpl(bridge, configuration),
        // El puente registra Float; el contrato compara en Double con tolerancia, porque el
        // impl real empuja `hz.toFloat()` y el doble empuja el Double del modelo.
        pushes = { bridge.pushedHz.map { it.toDouble() } },
        publicar = { nativo -> bridge.snapshot = nativo },
    )
}
