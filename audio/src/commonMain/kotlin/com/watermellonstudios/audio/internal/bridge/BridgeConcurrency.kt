package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.callback.AudioLogger
import com.watermellonstudios.audio.callback.NoOpAudioLogger
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

/**
 * Serialización por categoría para los puentes nativos (WA-1.4).
 *
 * `AudioNativeBridge` (Android) e `IosAudioBridge` ejecutan el **mismo** motor C++ a
 * través de la misma C API, así que necesitan exactamente la misma disciplina de
 * concurrencia. Tenerla acá, en commonMain, es lo que evita que cada plataforma
 * invente la suya y diverjan en silencio — que es la clase de bug que sólo aparece
 * en una plataforma y tarda semanas en atribuirse.
 *
 * ## Por qué mutexes por categoría y no uno global
 *
 * Las operaciones de categorías distintas no se pisan en el motor: agregar un efecto
 * no interfiere con una transición de modo. Un mutex único las serializaría sin
 * necesidad y metería latencia en el path de UI.
 *
 * ## Qué NO pasa por acá
 *
 * Los parámetros de tiempo real (`setXY`, `setFrequencyAndAmplitude`) son
 * **lock-free por diseño**: se llaman una vez por frame de gesto y del otro lado hay
 * `std::atomic`. Meterlos acá sería un error de performance, no una mejora de
 * seguridad.
 *
 * @param dispatcher dónde corre el trabajo bloqueante. Se inyecta para que los tests
 *   puedan usar un dispatcher controlado.
 * @param logger sink de errores. Por defecto no-op: la librería no decide por el
 *   consumidor a dónde van los logs.
 */
class BridgeConcurrency(
    private val dispatcher: CoroutineDispatcher = Dispatchers.Default,
    private val logger: AudioLogger = NoOpAudioLogger,
) {

    /** Familias de operaciones que se serializan entre sí, pero no con las demás. */
    enum class Category {
        /** start, stop, pause, resume. */
        LIFECYCLE,

        /** Alta, baja, reorden y parámetros de la cadena de efectos. */
        EFFECTS,

        /** Transiciones de modo. */
        MODE,

        /** Operaciones sobre el nodo de entrada. */
        INPUT,
    }

    private val lifecycle = Mutex()
    private val effects = Mutex()
    private val mode = Mutex()
    private val input = Mutex()

    private fun mutexFor(category: Category): Mutex = when (category) {
        Category.LIFECYCLE -> lifecycle
        Category.EFFECTS -> effects
        Category.MODE -> mode
        Category.INPUT -> input
    }

    /**
     * Corre [block] en [dispatcher], serializado contra las demás operaciones de
     * [category], y convierte cualquier excepción en `Result.failure`.
     *
     * ## La cancelación se propaga, no se captura
     *
     * `CancellationException` es una `Exception`, así que un `catch (e: Exception)`
     * alrededor de código de corrutinas **se traga la cancelación** y la devuelve
     * como si fuera un fallo de la operación. El scope padre nunca se entera de que
     * fue cancelado y la concurrencia estructurada deja de funcionar. Acá se
     * re-lanza explícitamente antes de cualquier otro manejo.
     *
     * Ese era el comportamiento de los 22 bloques `catch (e: Exception)` de
     * `AudioNativeBridge` antes de WA-1.4; centralizarlo lo arregla una vez para las
     * dos plataformas en vez de replicar el bug a iOS.
     *
     * @param operation nombre de la operación, sólo para el log de error.
     */
    suspend fun <T> guarded(
        category: Category,
        operation: String,
        block: suspend () -> Result<T>,
    ): Result<T> = serialized(category) {
        try {
            block()
        } catch (e: CancellationException) {
            throw e
        } catch (e: Throwable) {
            logger.error(TAG, "$operation: excepción no controlada", e)
            Result.failure(e)
        }
    }

    /**
     * Serialización sola: corre [block] en [dispatcher] bajo el mutex de [category],
     * **sin** mapear excepciones a `Result`.
     *
     * Es para las lecturas, que devuelven un valor crudo (`Int`, `Boolean`, un
     * snapshot) y no un `Result<T>`. Envolverlas en [guarded] obligaría a inventarles
     * un `Result` que nadie pidió, y a elegir un valor "de fallo" que no existe.
     *
     * La contrapartida es explícita: acá una excepción **se propaga**. Es lo correcto
     * para una lectura —no hay nada sensato que devolver si el motor falló— y es lo
     * que estas operaciones ya hacían antes de WA-1.4.
     */
    suspend fun <T> serialized(
        category: Category,
        block: suspend () -> T,
    ): T = withContext(dispatcher) {
        mutexFor(category).withLock {
            block()
        }
    }

    private companion object {
        const val TAG = "BridgeConcurrency"
    }
}
