package com.watermellonstudios.audio.harness

import androidx.compose.ui.graphics.Color

/**
 * Los colores del harness que Material3 no da.
 *
 * ## Esto NO es un design system, y la distinción importa
 *
 * Es un archivo de constantes para **este** módulo. No se comparte, no se publica y no
 * pretende crecer. Existe porque los mismos seis colores estaban escritos a mano en cuatro
 * archivos —`0xFF1E1E1E` aparecía tres veces— y un hex repetido es una constante esperando a
 * que alguien cambie sólo dos de las tres copias.
 *
 * **Por qué no hay un design system compartido acá**, que es la decisión que cerró WA-5.5:
 * se midió el harness entero —7 controles, 1409 LOC— buscando qué componentes se repetían, que
 * era exactamente la pregunta que el plan dejó abierta para esta etapa. La respuesta fue **cero
 * reutilización entre archivos**: los únicos dos composables extraídos (`LabeledSlider`,
 * `MeterBar`) se usan sólo dentro de `InputMonitorControl.kt`. No había nada que cosechar.
 *
 * Y del otro lado, el design system de producto **ya existe y está terminado**: `core-ui` de
 * NoisyPad es Compose Multiplatform, con tokens, catálogo y ADR propia. Vive en la app, que es
 * donde corresponde. Traerlo acá —o extraerlo a un tercer artefacto— haría que esta librería,
 * cuyo valor es **no arrastrar UI**, pasara a shippear Compose, a cambio de resolverle un
 * problema que el harness no tiene.
 *
 * ## La regla, por si esto crece
 *
 * Si algún día un control necesita un color, ponelo acá. Si **dos módulos distintos** necesitan
 * el mismo componente, eso ya no es este archivo — es la conversación de design system
 * compartido, y hay que volver a tener la discusión de la dirección de la dependencia. El
 * umbral es *dos consumidores reales*, no *parece reutilizable*.
 *
 * Sólo hay colores. No hay tokens de espaciado ni de tipografía **porque no se midió
 * repetición de eso** — inventarlos sería la misma especulación que este archivo argumenta en
 * contra.
 */
internal object HarnessTokens {

    /** Superficie hundida: el fondo del pad XY, la vista de logs y el riel del medidor. */
    val InsetSurface = Color(0xFF1E1E1E)

    /** El riel del medidor cuando **no hay** medición — distinto de "midió cero". */
    val InsetSurfaceNoSignal = Color(0xFF3A3A3A)

    /** Señal presente y sana: relleno del medidor, thumb del pad XY. */
    val Signal = Color(0xFF43A047)

    /** Clipping. Es el único rojo del harness y significa una sola cosa. */
    val Clipping = Color(0xFFE53935)

    /** Cuerpo de una línea de log. */
    val LogText = Color(0xFFCCCCCC)

    /** Metadata de una línea de log (timestamp, tag): presente, pero un escalón atrás. */
    val LogMeta = Color(0xFF888888)
}
