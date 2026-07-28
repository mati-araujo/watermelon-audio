package com.watermellonstudios.audio.harness

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import com.watermellonstudios.audio.api.AudioEngine
import com.watermellonstudios.audio.domain.oscillator.OscillatorType

/**
 * Control 2 de 7 — pad XY y selector de oscilador.
 *
 * Es el control que ejercita el **camino de tiempo real**: `setXY` corre una vez
 * por frame de gesto, y es la única llamada del programa con una nota explícita
 * sobre su costo por plataforma —Android tiene un coalescer que junta updates
 * para amortizar JNI; iOS no lo tiene porque cinterop no cobra lo mismo—. Ese
 * comentario en `IosAudioBridge` dice, textual, que si una medición muestra lo
 * contrario el lugar del coalescer es ahí. **Este pad es cómo se hace esa
 * medición.**
 *
 * ## Lo que NO tiene, y por qué
 *
 * **No hay slider de depth**, y el motivo dejó de ser "no se puede" para pasar a
 * ser "no hay qué llamar". El eje depth de la propuesta era para observar el ítem
 * 11 del smoke —`setDepthValue` era un dead store en las cuatro capas—; el
 * 2026-07-27 esa cadena **se borró entera de este repo** en vez de cablearse.
 * Nunca llegó a `commonMain`, y ya no existe acá. (Queda pendiente sacar su
 * llamada redundante del lado de NoisyPad, que la hacía al lado del
 * `applyAutomation` que sí funciona.)
 *
 * La decisión que quedó escrita: subir al bridge común una función que **ya
 * sabíamos que no hacía nada** habría sido peor que no tenerla, porque dejaba un
 * control muerto en la API multiplataforma. El eje depth real es la mapping axis
 * 2 — `applyAutomation(2, value)`, igual que X e Y.
 */
@Composable
fun XYPadControl(engine: AudioEngine, modifier: Modifier = Modifier) {
    var x by remember { mutableStateOf(0.5f) }
    var y by remember { mutableStateOf(0.5f) }
    var oscillator by remember { mutableStateOf(OscillatorType.SAW) }
    var size by remember { mutableStateOf(IntSize.Zero) }

    fun push(offset: Offset) {
        if (size.width <= 0 || size.height <= 0) return
        x = (offset.x / size.width).coerceIn(0f, 1f)
        // Y invertido: arriba en pantalla es 1.0 para el motor. Sin esto el pad
        // se siente al reves y parece un bug del engine.
        y = 1f - (offset.y / size.height).coerceIn(0f, 1f)
        engine.setXY(x, y)
    }

    Card(modifier = modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text("Pad XY", style = MaterialTheme.typography.titleMedium)

            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .aspectRatio(1.6f)
                    .clip(RoundedCornerShape(6.dp))
                    .background(HarnessTokens.InsetSurface)
                    .onSizeChanged { size = it }
                    .pointerInput(Unit) {
                        detectTapGestures(
                            onPress = { push(it) },
                        )
                    }
                    .pointerInput(Unit) {
                        detectDragGestures { change, _ -> push(change.position) }
                    },
            ) {
                // El cursor se dibuja con el mismo par (x, y) que se le manda al
                // motor, no con la posicion del dedo: si el clamp de arriba
                // cambiara algo, se ve.
                Box(
                    modifier = Modifier
                        .fillMaxWidth(0.04f)
                        .aspectRatio(1f)
                        .padding(0.dp)
                        .background(HarnessTokens.Signal, RoundedCornerShape(50)),
                )
            }

            Text(
                "x ${x.twoDecimals()}   y ${y.twoDecimals()}",
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace,
            )

            Row(
                modifier = Modifier.horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                OscillatorType.entries.forEach { candidate ->
                    Button(
                        enabled = candidate != oscillator,
                        onClick = {
                            engine.setOscillator(candidate)
                            oscillator = candidate
                        },
                    ) {
                        Text(
                            candidate.displayName,
                            style = MaterialTheme.typography.labelSmall,
                            maxLines = 1,
                        )
                    }
                }
            }
        }
    }
}
