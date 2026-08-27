package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.domain.tuner.TunerSnapshot
import org.junit.After
import org.junit.AfterClass
import org.junit.Before
import org.junit.FixMethodOrder
import org.junit.runners.MethodSorters
import kotlin.math.PI
import kotlin.math.sin
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * REQ-016 S2 — **los doce que necesitan motor.**
 *
 * `TunerJniTest` probó el cable con la única función del afinador que no toca el
 * motor. Acá cruzan las otras doce, con el motor arrancado sobre el
 * `FakeAudioBackend` que el build de host sustituye — el mismo camino que recorre
 * Tunio en un teléfono, menos el audio real.
 *
 * ## Qué agarra esto que el gate de símbolos no puede
 *
 * `check-jni-symbols.py` compara NOMBRES. Estas aserciones comparan **valores que
 * cruzaron**: un `Float` declarado donde el C++ pone `jdouble`, o un `Int` donde
 * pone `jlong`, compila de los dos lados, linkea, pasa ese gate — y devuelve
 * basura acá. Por eso los objetivos de prueba son valores que **no son potencia
 * de dos** y que tienen que volver **bit a bit**: `441.37f` redondeado o truncado
 * deja de ser `441.37f`.
 *
 * ## El estado nativo es un SINGLETON DE PROCESO
 *
 * `g_wmaEngine` se crea una vez —`nativeStartTuner` es el único de los trece que
 * lo crea— y **no se destruye nunca**. O sea que el estado "todavía no hay motor"
 * existe UNA sola vez por JVM, y es el que hace no-triviales a las respuestas de
 * después: sin él, "devuelve true" no se distingue de "devuelve true siempre".
 *
 * De ahí las dos defensas: [FixMethodOrder] deja el test de ausencia primero
 * dentro de la clase, y ese test **afirma su premisa** antes de medir nada. Si
 * algún día otra clase del arnés arranca el motor antes, esto se pone rojo
 * diciendo exactamente eso, en vez de seguir en verde probando la mitad.
 *
 * 🔴 Vale lo mismo que en [JniHarness]: backend falso, 13 de 310, **no** es
 * verificación en dispositivo.
 */
@FixMethodOrder(MethodSorters.NAME_ASCENDING)
class TunerEngineJniTest {

    companion object {
        /** Ninguno es potencia de dos: ver el KDoc de la clase. */
        private const val TARGET_A = 441.37f
        private const val TARGET_B = 329.63f

        /**
         * Las seis cuerdas de una guitarra en afinación estándar.
         *
         * 🔴 **Una FUNCIÓN y no un `val` compartido, y lo decidió un mutante que
         * sobrevivió.** `nativeSetTunerCandidates` recibe el array PINNEADO, así que
         * un defecto que escriba ahí corrompe el array de Kotlin. Con un `val` de
         * companion, el primer test que llamara a la función le dejaba el destrozo al
         * siguiente — y el siguiente sacaba de ese mismo array **su sujeto y su línea
         * de base**, o sea comparaba corrupto contra corrupto y pasaba en verde.
         * Medido: el mutante moría corriendo ese test SOLO y sobrevivía corriendo la
         * clase entera.
         */
        private fun candidates() = floatArrayOf(82.41f, 110.0f, 146.83f, 196.0f, 246.94f, 329.63f)

        private const val RATE = 48_000
        private const val SLOT_HARMONIC = 0
        private const val SLOT_INVALID = 99

        private const val OWNER = "TunerEngineJniTest"

        /**
         * Las doce que esta clase declara cubrir, más la offline que usa de control
         * positivo. **Trinquete bidireccional** — ver `JniCoverage.ratchet`: lo trajo un
         * mutante que sacó una función del arnés y sobrevivió, porque el conteo bajaba
         * de 13 a 12 y nadie se ponía rojo.
         */
        private val COVERED = setOf(
            "nativeStartTuner", "nativeStopTuner", "nativeIsTunerRunning",
            "nativeSetTunerTarget", "nativeGetTunerTarget", "nativeGetTunerSnapshot",
            "nativeSetTunerCandidates", "nativeLockTunerString",
            "nativeIntonationCapture", "nativeIntonationReset",
            "nativeIntonationState", "nativeIntonationDifferenceCents",
            "nativeAnalyzeTunerBuffer",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)
    }

    private fun <T> jni(name: String, call: (AudioNativeBridge) -> T): T =
        JniHarness.exercise(OWNER, name, call)

    @Before
    fun loadLibrary() = JniHarness.requireNativeLibrary()

    @After
    fun stopTuner() {
        jni("nativeStopTuner") { it.stopTunerSync() }
    }

    /**
     * El estado de ausencia, que sólo existe una vez por proceso.
     *
     * Es lo que vuelve no-trivial a todo lo demás de esta clase: sin este test, un
     * motor que contestara `true` a todo pasaría entero.
     */
    @Test
    fun `a - sin motor las doce contestan ausencia, no ceros plausibles`() {
        // PREMISA, y falla ruidoso si se rompe: este test necesita un proceso en el
        // que nadie llamó todavía a nativeStartTuner. Si alguna vez sale rojo acá,
        // el arreglo NO es bajar la exigencia: es que la clase que arranca el motor
        // no corra antes que ésta.
        assertEquals(
            0f,
            jni("nativeGetTunerTarget") { it.getTunerTargetHz() },
            "premisa rota: alguien ya creó el motor en esta JVM antes que este test",
        )

        assertFalse(jni("nativeIsTunerRunning") { it.isTunerRunning() }, "no arrancó nadie")
        assertFalse(jni("nativeSetTunerTarget") { it.setTunerTargetHz(TARGET_A) }, "sin motor no hay objetivo que poner")
        assertFalse(jni("nativeSetTunerCandidates") { it.setTunerCandidates(candidates()) }, "sin motor no hay candidatos")
        assertFalse(jni("nativeLockTunerString") { it.lockTunerString(2) }, "sin motor no hay cuerda que enganchar")
        assertFalse(jni("nativeIntonationCapture") { it.captureIntonation(SLOT_HARMONIC) }, "sin motor no hay qué capturar")
        assertEquals(0, jni("nativeIntonationState") { it.intonationState() }, "NEED_HARMONIC es el estado sin motor")

        // 🔴 NaN y no 0: el cero es un valor PLAUSIBLE de diferencia de intonación, y
        // este repo ya shippeó dos stubs cuyos ceros derrotaron los fallbacks elvis
        // de sus propios callers. Que el NaN sobreviva el cruce es además la mejor
        // prueba de firma que hay: un tipo equivocado no lo conserva.
        assertTrue(
            jni("nativeIntonationDifferenceCents") { it.intonationDifferenceCents() }.isNaN(),
            "sin motor la diferencia tiene que ser NaN, nunca 0",
        )
        assertNull(jni("nativeGetTunerSnapshot") { it.getTunerSnapshot() }, "sin motor no hay snapshot")

        // Y no explota: `reset` sin motor sale por su guarda.
        jni("nativeIntonationReset") { it.resetIntonation() }
    }

    /**
     * El par completo. Es el gemelo que este repo aprendió a exigir: "no mientas"
     * es trivial para un motor que nunca dice nada, así que hay que pedirle también
     * que **no calle de más**.
     */
    @Test
    fun `arrancar y parar mueve isRunning en los dos sentidos`() {
        assertTrue(jni("nativeStartTuner") { it.startTunerSync() }, "el afinador se basta solo: no tiene precondiciones")
        assertTrue(jni("nativeIsTunerRunning") { it.isTunerRunning() }, "dijo que arrancó y no está corriendo")

        jni("nativeStopTuner") { it.stopTunerSync() }
        assertFalse(jni("nativeIsTunerRunning") { it.isTunerRunning() }, "dijo que paró y sigue corriendo")
    }

    /**
     * El round-trip de `jfloat`, con DOS valores distintos.
     *
     * Uno solo no alcanza: un `getTarget` que devolviera una constante pasaría. Con
     * dos, lo que se prueba es que el valor **viaja**, y que viaja exacto.
     */
    @Test
    fun `el objetivo en Hz vuelve bit a bit y sigue al que se puso`() {
        assertTrue(jni("nativeStartTuner") { it.startTunerSync() })

        assertTrue(jni("nativeSetTunerTarget") { it.setTunerTargetHz(TARGET_A) })
        assertEquals(TARGET_A, jni("nativeGetTunerTarget") { it.getTunerTargetHz() }, "el objetivo no volvió igual")

        assertTrue(jni("nativeSetTunerTarget") { it.setTunerTargetHz(TARGET_B) })
        assertEquals(TARGET_B, jni("nativeGetTunerTarget") { it.getTunerTargetHz() }, "el objetivo no siguió al segundo valor")
    }

    /**
     * AC-016.2 — **el array pinneado.**
     *
     * `nativeSetTunerCandidates` pinnea el `FloatArray` de Kotlin con
     * `GetFloatArrayElements` y lo suelta con `JNI_ABORT`, o sea "no copies nada de
     * vuelta". Si alguien cambiara ese modo a `0`, el array del llamador se
     * sobrescribiría con lo que quedó en el buffer nativo — corrupción silenciosa
     * del lado Kotlin, que ningún gate de símbolos puede ver.
     */
    @Test
    fun `los candidatos cruzan pinneados y el array de Kotlin queda intacto`() {
        assertTrue(jni("nativeStartTuner") { it.startTunerSync() })

        val subject = candidates()

        assertTrue(jni("nativeSetTunerCandidates") { it.setTunerCandidates(subject) }, "con motor, los candidatos entran")

        // 🔴 La línea de base es LITERAL, no una copia de `subject`.
        //
        // Con una copia, el control sale del mismo lugar que el sujeto: si algo lo
        // corrompió antes de que se saque la copia, la comparación da verde contra el
        // destrozo. Eso no es hipotético — es lo que dejó vivo al mutante de
        // `JNI_ABORT` la primera vez.
        assertTrue(
            subject.contentEquals(floatArrayOf(82.41f, 110.0f, 146.83f, 196.0f, 246.94f, 329.63f)),
            "el array de Kotlin volvió MODIFICADO (${subject.joinToString()}): se soltó sin JNI_ABORT",
        )

        // Y con el modo rápido armado, engancharse a una cuerda por índice.
        assertTrue(jni("nativeLockTunerString") { it.lockTunerString(2) }, "con motor se puede enganchar")
    }

    /**
     * Los cuatro de intonación (REQ-001 S9) contra un motor vivo.
     *
     * Sin audio no converge nada, así que `capture` dice `false` — y eso **es** el
     * contrato (`IntonationRefusesToCaptureBeforeAnythingConverged`). El control
     * positivo de que la negativa no es vacía está en el test de ausencia de arriba
     * y en el rechazo de slot inválido de acá: la misma llamada, con el mismo motor
     * vivo, distinguiendo dos causas.
     */
    @Test
    fun `intonacion contra un motor vivo rechaza sin converger y no miente con ceros`() {
        assertTrue(jni("nativeStartTuner") { it.startTunerSync() })

        assertFalse(
            jni("nativeIntonationCapture") { it.captureIntonation(SLOT_HARMONIC) },
            "no convergió nada todavía: capturar sería inventar una medición",
        )
        assertFalse(
            jni("nativeIntonationCapture") { it.captureIntonation(SLOT_INVALID) },
            "un slot que no es HARMONIC ni FRETTED se rechaza antes de tocar el motor",
        )
        assertEquals(0, jni("nativeIntonationState") { it.intonationState() }, "sigue en NEED_HARMONIC")
        assertTrue(
            jni("nativeIntonationDifferenceCents") { it.intonationDifferenceCents() }.isNaN(),
            "sin las dos capturas la diferencia es NaN, no 0",
        )

        // 🔴 HUECO DECLARADO, medido con un mutante: convertir `nativeIntonationReset`
        // en un no-op **sobrevive** a este arnés. No es debilidad del test, es el
        // alcance: sin audio nada converge, el estado nunca sale de NEED_HARMONIC, y un
        // reset que no hace nada es indistinguible de uno que funciona. Acá queda
        // cubierto lo que AC-016.1 promete —que CRUZA la frontera contra un JNIEnv
        // real— y su comportamiento lo cubre `core/tests/test_c_api_tuner.cpp`, que sí
        // puede manejar el backend. Lo mismo vale para el camino CON dato de
        // `nativeGetTunerSnapshot`.
        jni("nativeIntonationReset") { it.resetIntonation() }
        assertEquals(0, jni("nativeIntonationState") { it.intonationState() }, "resetear deja NEED_HARMONIC")
    }

    /**
     * **`null` y no un array de ceros.** Es la aserción que más vale de las trece.
     *
     * El arnés no puede empujar audio por la frontera —no hay ninguna `JNIEXPORT`
     * que lo permita— así que en el host el análisis nunca publica y el snapshot es
     * legítimamente ausente. Que la ausencia viaje como `null` es exactamente lo que
     * MINI-003 dejó escrito: un array de ceros NO es dato ausente, es dato
     * plausible, y este repo ya shippeó dos stubs cuyos ceros derrotaron los
     * fallbacks de sus propios callers.
     *
     * 🔴 **Hueco declarado**: el camino con dato de `nativeGetTunerSnapshot` NO lo
     * cubre este arnés; lo cubre `core/tests/test_c_api_tuner.cpp`, que sí puede
     * manejar el backend. Acá queda cubierto su camino de ausencia, y el control
     * positivo de que la frontera SÍ sabe devolver arrays es el de abajo.
     */
    @Test
    fun `sin audio el snapshot viaja como null, y la frontera igual sabe devolver arrays`() {
        assertTrue(jni("nativeStartTuner") { it.startTunerSync() })

        assertNull(
            jni("nativeGetTunerSnapshot") { it.getTunerSnapshot() },
            "sin nada publicado el snapshot tiene que ser null — un array de ceros sería una medición que nadie hizo",
        )

        // Control positivo sobre la MISMA frontera y el mismo `NewFloatArray`: si
        // devolver arrays estuviera roto, el null de arriba no probaría nada.
        val sine = FloatArray(RATE) { i -> 0.5f * sin(2.0 * PI * 440.0 * i / RATE).toFloat() }
        val produced = assertNotNull(
            jni("nativeAnalyzeTunerBuffer") { it.analyzeTunerBuffer(sine, 1, RATE, TARGET_A) },
            "la frontera no supo devolver un array: el null de arriba no es concluyente",
        )
        assertEquals(TunerSnapshot.VALUE_COUNT, produced.size)
    }

    /**
     * Cada llamada tiene que devolver un array **nuevo**.
     *
     * Este archivo tiene un `FloatArrayPool` global esperando a que alguien lo use
     * para "evitar allocations". El día que alguien reutilice el `jfloatArray` de
     * salida, dos lecturas seguidas empiezan a compartir memoria y el consumidor lee
     * la medición de la llamada siguiente. Acá eso es rojo.
     */
    @Test
    fun `llamadas repetidas devuelven arrays distintos, no uno reciclado`() {
        // Los dos con la MISMA forma y distinta amplitud, y los dos de un segundo:
        // un estímulo demasiado corto no produce resultado, y un assertNotNull sobre
        // material insuficiente es verde por vacío — ya pasó en este repo.
        val loud = FloatArray(RATE) { i -> 0.5f * sin(2.0 * PI * 440.0 * i / RATE).toFloat() }
        val quiet = FloatArray(RATE) { i -> 0.05f * sin(2.0 * PI * 440.0 * i / RATE).toFloat() }

        val first = assertNotNull(jni("nativeAnalyzeTunerBuffer") { it.analyzeTunerBuffer(loud, 1, RATE, TARGET_A) })
        val second = assertNotNull(jni("nativeAnalyzeTunerBuffer") { it.analyzeTunerBuffer(quiet, 1, RATE, TARGET_A) })

        assertTrue(first !== second, "la frontera devolvió DOS VECES el mismo objeto: los arrays se están reciclando")
        assertTrue(
            first[1] > second[1],
            "el RMS del material fuerte (${first[1]}) tiene que superar al del silencio (${second[1]}); " +
                "si son iguales, la segunda llamada pisó la primera",
        )
    }
}
