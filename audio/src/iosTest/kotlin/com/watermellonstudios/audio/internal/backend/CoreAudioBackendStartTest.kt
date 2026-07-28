package com.watermellonstudios.audio.internal.backend

import com.watermellonstudios.audio.internal.audio.AudioSessionManager
import com.watermellonstudios.audio.internal.cinterop.WMA_ERROR_STREAM
import com.watermellonstudios.audio.internal.cinterop.WMA_OK
import com.watermellonstudios.audio.internal.cinterop.wma_engine_create
import com.watermellonstudios.audio.internal.cinterop.wma_engine_destroy
import com.watermellonstudios.audio.internal.cinterop.wma_engine_start
import com.watermellonstudios.audio.internal.cinterop.wma_engine_stop
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.ObjCObjectVar
import kotlinx.cinterop.alloc
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.ptr
import kotlinx.cinterop.value
import platform.AVFAudio.AVAudioEngine
import platform.Foundation.NSError
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

/**
 * El camino de arranque REAL de `CoreAudioBackend`, en el simulador.
 *
 * ## El hueco que llena
 *
 * `CoreAudioBackend` **no tenía ni un test**, y no por descuido sino por dos decisiones que
 * se tomaron por separado y dejaron el medio sin cubrir:
 *
 *  - los **767 tests de C++** arrancan el motor y esperan `WMA_OK`, pero corren contra
 *    `FakeAudioBackend` (`CApiFixture` lo instala como backend del sistema). O sea que
 *    prueban la máquina de estados del motor, **no** el camino de CoreAudio;
 *  - `CinteropSmokeTest` dice, textualmente, *"deliberadamente NO arranca el motor…
 *    `wma_engine_start()` abre un stream de CoreAudio y si el simulador no tiene salida de
 *    audio disponible el test se vuelve flaky"*.
 *
 * Las dos son correctas. El resultado igual es que **nada ejercitaba
 * `CoreAudioBackend::start()`**, que es exactamente lo que falla cuando NoisyPad intenta
 * sonar en el simulador (su F5-E27).
 *
 * ## Por qué este test NO afirma que el arranque funciona
 *
 * Porque no puede: si lo hiciera sería el test flaky que el smoke evitó a propósito, y se
 * pondría rojo en cualquier máquina sin salida de audio disponible. **Lo que afirma es lo
 * que no depende del entorno** — que el arranque sea *determinístico* y que un fallo deje
 * el motor en un estado del que se pueda salir— e **imprime** el resultado, que es el dato
 * de diagnóstico.
 *
 * Un fallo de arranque que dejara el motor colgado sería un bug de la librería en cualquier
 * plataforma; que el stream no abra en un simulador sin dispositivo, no.
 *
 * ## Cómo leer su salida
 *
 * ```
 * [CoreAudioBackend] start() -> WMA_OK            → el stream abre acá
 * [CoreAudioBackend] start() -> WMA_ERROR_STREAM  → no abre; mirar el log del sistema
 * ```
 *
 * Con `WMA_ERROR_STREAM`, el log del proceso muestra la causa de CoreAudio —típicamente
 * `AQMEIO … finding/initializing Default-InputOutput` precedido de
 * `HALDefaultDevice: Could not find default device for dOut`.
 *
 * ## ⚠️ EL VEREDICTO ES SOBRE EL PROCESO DE TEST, NO SOBRE LA APP
 *
 * **Esto se descubrió usándolo, y es la corrección más importante de este KDoc.** Un
 * `WMA_ERROR_STREAM` acá **NO prueba que la app no vaya a sonar en ese mismo simulador**.
 *
 * Medido: en el mismo simulador y en el mismo momento, NoisyPad instalada abría el stream
 * sin un solo fallo mientras este test seguía reportando `WMA_ERROR_STREAM` y el
 * `AVAudioEngine` pelado seguía fallando con `-10851`.
 *
 * La diferencia plausible es cómo corre cada uno: el runner de Kotlin/Native lanza el
 * binario de tests con `simctl spawn`, o sea **un proceso pelado sin bundle de app**,
 * mientras que la app corre instalada. CoreAudio en el simulador no le da la misma salida a
 * los dos. No está probado que sea exactamente eso; lo que sí está medido es que **los dos
 * entornos difieren**.
 *
 * Consecuencia práctica, y la razón de esta nota: la tabla de atribución de
 * [unAVAudioEnginePeladoArrancaOnNo] discrimina entre "bug del backend" y "el entorno **de
 * este runner** no tiene salida" — que es útil, porque un backend que devolviera un código
 * inventado se vería igual—, pero **no sirve para concluir nada sobre la app**. Para eso hay
 * que correr la app y mirar su log.
 *
 * Lo que este archivo sigue probando sin asteriscos son los otros dos: que el código de
 * retorno pertenece al contrato y que un arranque fallido no deja el motor colgado. Esos no
 * dependen del entorno y valen en cualquier máquina.
 */
@OptIn(ExperimentalForeignApi::class)
class CoreAudioBackendStartTest {

    /**
     * La sesión se configura igual que la app, porque sin eso el arranque falla por un
     * motivo que no es el que se quiere medir: `AVAudioEngine` no se asocia a ninguna
     * sesión y el error dice `associating with audio session (0x0)`.
     */
    private fun withPreparedSession(block: () -> Unit) {
        val session = AudioSessionManager()
        session.configure(enableInput = false)
        session.activate()
        try {
            block()
        } finally {
            session.deactivate()
        }
    }

    /**
     * **El diagnóstico.** Arranca el motor de verdad y reporta qué pasó.
     *
     * Lo único que se afirma es que `start` devuelve un código **definido** del contrato:
     * `WMA_OK` si el stream abrió, `WMA_ERROR_STREAM` si no. Cualquier otro valor sería un
     * bug real —el backend inventando un código— y ahí sí corresponde el rojo.
     */
    @Test
    fun elArranqueRealDevuelveUnCodigoDelContrato() = withPreparedSession {
        val engine = wma_engine_create()
        assertNotNull(engine, "wma_engine_create() devolvió null")

        try {
            val result = wma_engine_start(engine, 0)

            val nombre = when (result) {
                WMA_OK -> "WMA_OK"
                WMA_ERROR_STREAM -> "WMA_ERROR_STREAM"
                else -> "código inesperado ($result)"
            }
            println("[CoreAudioBackend] start() -> $nombre")

            assertTrue(
                result == WMA_OK || result == WMA_ERROR_STREAM,
                "start() devolvió $result, que no es ni WMA_OK ni WMA_ERROR_STREAM. " +
                    "Un backend que inventa códigos es un bug del contrato, no del entorno."
            )
        } finally {
            wma_engine_destroy(engine)
        }
    }

    /**
     * **El discriminador: un `AVAudioEngine` PELADO, sin una línea de esta librería.**
     *
     * Es el test que separa "el backend está mal" de "esta máquina no puede abrir un stream".
     * No toca el motor: crea un `AVAudioEngine` de Apple, le pide el `outputNode` —que es lo
     * que fuerza a instanciar la unidad de IO— y lo arranca.
     *
     * Se lee junto con [elArranqueRealDevuelveUnCodigoDelContrato]:
     *
     * | pelado | `CoreAudioBackend` | conclusión |
     * |---|---|---|
     * | arranca | falla | **bug del backend** — hay que mirar `CoreAudioBackend.mm` |
     * | falla | falla | el runner de tests no tiene salida de audio; el backend no tiene la culpa |
     * | arranca | arranca | el stream abre también acá |
     *
     * **La segunda fila NO dice nada sobre la app** — ver el aviso del KDoc de la clase: se
     * midió a la app abriendo el stream en el mismo simulador donde este test decía que no.
     *
     * Tampoco afirma que arranque, por lo mismo que el otro: imprime.
     */
    @Test
    fun unAVAudioEnginePeladoArrancaOnNo() = withPreparedSession {
        val engine = AVAudioEngine()
        // Tocar `outputNode` NO es decorativo: es lo que instancia la unidad de IO. Sin esto
        // el `start` podría no llegar nunca al hardware y el test no discriminaría nada.
        val output = engine.outputNode
        assertNotNull(output, "AVAudioEngine.outputNode devolvió null")

        val error = kotlinx.cinterop.memScoped {
            val ref = alloc<ObjCObjectVar<NSError?>>()
            val ok = engine.startAndReturnError(ref.ptr)
            if (ok) null else ref.value
        }
        println(
            "[AVAudioEngine pelado] start() -> " +
                (if (error == null) "OK" else "FALLA: ${error.localizedDescription}")
        )
        engine.stop()
    }

    /**
     * **Un arranque fallido no puede dejar el motor colgado.**
     *
     * Es el contrato que sí vale en cualquier entorno, y el que de verdad importa para el
     * consumidor: si el usuario abre la app sin salida de audio disponible —auriculares que
     * se desconectan, una llamada entrante, un simulador— el motor tiene que poder pararse y
     * destruirse sin crashear y sin quedar en un estado del que no se sale.
     */
    @Test
    fun unArranqueFallidoNoDejaElMotorColgado() = withPreparedSession {
        val engine = wma_engine_create()
        assertNotNull(engine)

        try {
            val first = wma_engine_start(engine, 0)

            // `stop` tiene que ser seguro haya arrancado o no. Éste es el punto: si `start`
            // falló a mitad de camino y dejó el backend a medio abrir, acá se nota.
            wma_engine_stop(engine, 0)

            val second = wma_engine_start(engine, 0)
            assertEquals(
                first, second,
                "start() dio $first y después $second sobre el mismo motor. El arranque " +
                    "tiene que ser determinístico: si el primero falló por falta de " +
                    "dispositivo, el segundo tiene que fallar igual — un resultado que " +
                    "cambia solo significa que el fallo dejó estado sucio."
            )

            wma_engine_stop(engine, 0)
        } finally {
            // Si `destroy` crashea tras un arranque fallido, el proceso de test muere acá y
            // eso ES el hallazgo.
            wma_engine_destroy(engine)
        }
    }
}
