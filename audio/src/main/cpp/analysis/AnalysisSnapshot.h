#pragma once

/**
 * @file AnalysisSnapshot.h
 * @brief La publicacion COHERENTE del resultado de analisis (REQ-001 S1).
 *
 * UN CRUCE DE FRONTERA POR TICK, Y ADEMAS SIN DESGARRO
 * ----------------------------------------------------
 * La forma —una sola llamada que llena un array de floats con orden
 * contractual— sale del idiom de `wma_input_get_metering_snapshot`, como pide la
 * tarea 1.13. **La garantia no**: ese idiom llena el array leyendo siete
 * atomicos INDEPENDIENTES, uno por getter, asi que sus siete valores pueden
 * venir de momentos distintos.
 *
 * Para medidores de nivel eso esta bien y por eso se hizo asi. Para un afinador
 * no: mostrar los cents del tick N con el angulo de fase del N+1 hace saltar el
 * disco del strobe, que es justo lo que AC-001.22 existe para evitar. La tarea
 * 1.5 lo pide explicito — "nunca entrega un estado a medio escribir".
 *
 * COMO SE CONSIGUE: SEQLOCK CON PAYLOAD ATOMICO
 * ---------------------------------------------
 * Un escritor, muchos lectores. El escritor sube un contador a IMPAR, escribe,
 * y lo sube a PAR. El lector lee el contador, copia, y lo re-lee: si cambio, o
 * si lo encontro impar, la copia no vale y reintenta.
 *
 * **El payload es `atomic<float>` aunque el protocolo valide despues**, y eso no
 * es ceremonia: es la leccion que TSan cobro en `AnalysisRing` unas horas antes
 * de escribir esto. Con `float` pelado, la copia del lector corre contra la
 * escritura del escritor y eso es una data race FORMAL — UB, que el compilador
 * puede explotar. Que el protocolo descarte la copia no lo salva: el estandar no
 * dice "lee basura", dice que el programa no tiene significado. Relajado alcanza;
 * el orden lo da el contador.
 *
 * ⚠️ **La `atomic_thread_fence` de abajo es real y TSan NO la modela.** Este repo
 * ya lo tiene documentado por las fences del looper (ver `tests/CMakeLists.txt`).
 * Que TSan no pueda PROBAR el orden no significa que reporte una carrera: los
 * campos son atomicos, asi que carrera no hay. Lo que la fence compra es que la
 * copia no se hunda por debajo de la segunda lectura del contador, que es lo que
 * haria inutil la validacion.
 */

#include <atomic>
#include <cstdint>

#if defined(WMA_TEST_HOOKS)
#include <thread>
/**
 * Las compuertas de los tests. Ver publish() y read().
 *
 * `inline` y NO definidas en un .cpp: el codigo que las usa vive en este header,
 * asi que entra en cualquier TU que se compile con `WMA_TEST_HOOKS` — incluida
 * `watermelon_audio.cpp` en `core_tests`, que no linkea el .cpp del thread.
 * Con las definiciones del otro lado, ese target no linkeaba: el header traia
 * las referencias y el archivo que las definia estaba compilado SIN hooks.
 */
inline std::atomic<bool> gSnapshotHoldMidPublish{false};
inline std::atomic<bool> gSnapshotIsMidPublish{false};
/// La segunda compuerta, del lado del LECTOR. Ver la nota en read().
inline std::atomic<bool> gSnapshotHoldMidRead{false};
inline std::atomic<bool> gSnapshotIsMidRead{false};
#endif

namespace wma::analysis {

/**
 * Orden CONTRACTUAL de los valores. Lo indexa el consumidor del otro lado de la
 * frontera, asi que agregar va SIEMPRE al final y nunca se reordena.
 */
enum SnapshotValue : int {
    /// Rate al que se CAPTURO, no al que salga la senal. Va en el snapshot para
    /// que nadie pueda asumir 48000 en silencio: el motor lo tuvo hardcodeado.
    kSnapCaptureSampleRate = 0,
    /// Nivel RMS de lo analizado en este tick, lineal.
    kSnapLevelRms          = 1,
    /// Frames que el analisis proceso desde el arranque. Es un float a
    /// proposito —el array es de floats— y a 48 kHz aguanta ~48 dias antes de
    /// perder resolucion de a un frame, mucho mas que cualquier sesion.
    kSnapFramesAnalyzed    = 2,
    /// Frames que el analisis NUNCA vio porque el ring los piso.
    kSnapDroppedFrames     = 3,
    /// Ver `SnapshotState`.
    kSnapState             = 4,

    /// Desviacion contra el objetivo. NaN si no hay objetivo o no hay medicion.
    kSnapCents             = 5,
    /// Angulo de la desafinacion acumulada, rad, envuelto a ±π. Lo consume el strobe.
    kSnapPhaseAngle        = 6,
    kSnapUncertainty       = 7,

    // ---- Deteccion gruesa (REQ-001 S4). AGREGADOS AL FINAL, que es el contrato:
    //      el orden lo indexa el consumidor del otro lado de la frontera, asi que
    //      agregar va SIEMPRE al final y nunca se reordena. Es la mitigacion que
    //      declara `plan.md` para este seam compartido.
    /// Altura detectada SIN saberla de antemano, en Hz. 0 = no hay nota.
    kSnapDetectedHz        = 8,
    /// Confianza de esa deteccion, 0..1 (el valor de la NSDF en el pico elegido).
    kSnapDetectionClarity  = 9,

    // ---- Inarmonicidad (REQ-001 S7). Al final, otra vez.
    /// Coeficiente B de la cuerda que suena, de `f_n = n·f₀·√(1+B·n²)`.
    /// NaN cuando no se pudo medir — y NO cero, porque cero es un valor
    /// PLAUSIBLE (cuerda ideal) que un consumidor mostraria como medicion.
    kSnapInharmonicityB    = 10,
    /// 1 si `kSnapInharmonicityB` se MIDIO, 0 si el consumidor tiene que caer al
    /// respaldo derivado de fisica. Va aparte del valor y no como un centinela
    /// dentro suyo: es exactamente lo que pide AC-001.11.
    kSnapInharmonicityMeasured = 11,

    kSnapshotValueCount    = 12,
};

enum SnapshotState : int {
    kStateNoSignal  = 0,  ///< no llega nada por encima del piso
    kStateNoLock    = 1,  ///< hay senal pero el estimador no engancho
    kStateMeasuring = 2,  ///< midiendo, todavia sin converger
    kStateConverged = 3,  ///< la incertidumbre bajo del umbral declarado
};

class AnalysisSnapshot {
public:
    /**
     * Publica un juego completo de valores. Lo llama SOLO el thread de analisis.
     *
     * Los campos que S1 todavia no puede calcular —cents, angulo, incertidumbre—
     * se publican en **NaN**, no en cero. No es un detalle: `0.0` cents es un
     * valor PLAUSIBLE (afinado exacto) y un consumidor lo mostraria como una
     * medicion. NaN es inconfundiblemente "no hay dato". Es la leccion de los
     * dos stubs que mentian, aplicada antes de que exista el consumidor.
     */
    void publish(const float* values) noexcept {
        const uint32_t s = mSeq.load(std::memory_order_relaxed);
        mSeq.store(s + 1, std::memory_order_release);           // impar: escribiendo
        std::atomic_thread_fence(std::memory_order_release);
        for (int i = 0; i < kSnapshotValueCount; ++i) {
            mValues[i].store(values[i], std::memory_order_relaxed);
#if defined(WMA_TEST_HOOKS)
            // COMPUERTA (solo analysis/tests). Detiene al escritor con la mitad
            // de los campos puestos y el contador en IMPAR: el estado a medio
            // escribir que la tarea 1.5 dice que nadie puede llegar a ver.
            // Sin compuerta habria que buscarlo por iteraciones, y este repo ya
            // midio que asi no se pega una ventana de microsegundos.
            if (i == kSnapshotValueCount / 2
                && gSnapshotHoldMidPublish.load(std::memory_order_acquire)) {
                gSnapshotIsMidPublish.store(true, std::memory_order_release);
                while (gSnapshotHoldMidPublish.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                gSnapshotIsMidPublish.store(false, std::memory_order_release);
            }
#endif
        }
        mSeq.store(s + 2, std::memory_order_release);           // par: listo
    }

    /**
     * Copia el ultimo juego COHERENTE. Llamable desde cualquier thread.
     *
     * @return false si nunca se publico nada — y en ese caso **no toca `out`**.
     *         Devolver ceros seria devolver una medicion que nadie hizo.
     */
    bool read(float* out) const noexcept {
        if (out == nullptr) return false;
        // Cota de reintentos: el escritor publica a ritmo de bloque, asi que
        // dos vueltas ya son de sobra. Un techo evita que un escritor
        // patologico cuelgue al lector — que puede ser el thread de UI.
        for (int attempt = 0; attempt < 8; ++attempt) {
            const uint32_t s1 = mSeq.load(std::memory_order_acquire);
            if (s1 == 0) return false;          // nunca se publico
            if (s1 & 1u) continue;              // escritura en curso
            float tmp[kSnapshotValueCount];
            for (int i = 0; i < kSnapshotValueCount; ++i) {
                tmp[i] = mValues[i].load(std::memory_order_relaxed);
#if defined(WMA_TEST_HOOKS)
                // SEGUNDA COMPUERTA, y hace falta una aparte de la del escritor.
                // La del escritor deja el contador en IMPAR, asi que el lector
                // sale por el chequeo de paridad y NUNCA llega a la validacion
                // de abajo — medido: con solo esa compuerta, un mutante que
                // borra la validacion entera SOBREVIVE.
                //
                // Esta detiene al lector a mitad de SU copia, para que el
                // escritor complete un publish entero encima. Ahi `tmp` queda
                // mitad viejo y mitad nuevo con el contador PAR en las dos
                // puntas de la ventana, y lo unico que puede salvarlo es
                // comparar el contador. Es la ventana que la validacion existe
                // para cubrir.
                if (i == kSnapshotValueCount / 2
                    && gSnapshotHoldMidRead.load(std::memory_order_acquire)) {
                    gSnapshotIsMidRead.store(true, std::memory_order_release);
                    while (gSnapshotHoldMidRead.load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                    gSnapshotIsMidRead.store(false, std::memory_order_release);
                }
#endif
            }
            // Sin esta fence la copia de arriba podria hundirse por debajo de
            // la lectura de abajo, y entonces validar no validaria nada.
            //
            // ⚠️ NINGUN TEST PUEDE MATAR ESTA LINEA, y esta medido: el mutante
            // que la borra pasa la suite entera. No es que los tests sean
            // ciegos — es que lo que la fence impide es un REORDENAMIENTO del
            // compilador o del procesador, y eso no se provoca desde un test.
            // En arm64 y con este clang la copia no se hunde, asi que borrarla
            // hoy no cambia nada observable; manana, con otro optimizador, si.
            // Queda por el estandar, no por la evidencia, y esa distincion vale
            // la pena escribirla en vez de dejar la linea sin explicacion.
            std::atomic_thread_fence(std::memory_order_acquire);
            if (mSeq.load(std::memory_order_relaxed) == s1) {
                for (int i = 0; i < kSnapshotValueCount; ++i) out[i] = tmp[i];
                return true;
            }
        }
        return false;   // el escritor no paro: mejor "no hay dato" que uno roto
    }

    /// true si alguna vez se publico algo.
    bool hasData() const noexcept {
        return mSeq.load(std::memory_order_acquire) != 0;
    }

private:
    std::atomic<uint32_t> mSeq{0};
    std::atomic<float> mValues[kSnapshotValueCount];
};

}  // namespace wma::analysis
