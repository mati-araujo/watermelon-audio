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
import androidx.compose.ui.graphics.Color
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
 * **No hay slider de depth.** El eje depth de la propuesta era para observar el
 * ítem 11 del smoke —`setDepthValue` es un dead store en las cuatro capas— pero
 * no se puede: `setDepthValue` no existe en `commonMain` en ninguna forma, ni en
 * `AudioEngine` ni en `IAudioNativeBridge`. Es Android-only.
 *
 * Poner el slider igual habría requerido subir al bridge común una función que
 * **ya sabemos que no hace nada**, y eso es peor que no tenerla: dejaría escrito
 * en la API multiplataforma un control muerto. El ítem se mira desde NoisyPad en
 * Android, que es donde el caller existe.
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
                    .background(Color(0xFF1E1E1E))
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
                        .background(Color(0xFF43A047), RoundedCornerShape(50)),
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
