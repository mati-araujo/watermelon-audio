#pragma once

/**
 * AbsenceGate.h — la decision de "no hay nada que afinar", PURA y con memoria.
 *
 * REQ-019 S1. Antes esto eran dos lineas dentro del lazo de `AnalysisThread`, y
 * ahi el defecto no se podia ver ni probar: la rama tonal le creia a UNA sola
 * lectura sin altura, asi que un hueco transitorio del detector grueso apagaba
 * la aguja sobre una cuerda perfectamente audible.
 *
 * MEDIDO (MINI-010, 2026-08-28), con 10 procesos TSan concurrentes: 22 rojos de
 * 120, los 22 con exactamente 4 muestras en ausencia, siempre las mismas —
 * `[3,4,5,6]`, `pisados=0`, `discont=0`, `hz=0`, `clarity=0.138`. O sea: la señal
 * NO tenia huecos y el detector no encontraba altura durante la transicion de
 * nivel. Para el usuario, la aguja apagandose sola en medio de una afinacion.
 *
 * LAS DOS RAMAS NO SE TRATAN IGUAL, Y ESA ES LA IDEA ENTERA
 * ---------------------------------------------------------
 *   · nivel  (`rms < kSilenceFloor`) → ausencia INMEDIATA (AC-019.4). El silencio
 *     de verdad se detecta por nivel y no tiene por que pagar demora.
 *   · tonal  (el detector no ve altura) → ausencia tras N lecturas SEGUIDAS.
 *
 * Sin ese reparto, AC-019.2 seria incumplible: cualquier histeresis demora la
 * ausencia, y demorarla TAMBIEN para el silencio real convierte el gemelo
 * `TheEngineDeclaresAbsenceWhenOnlyRoomNoiseRemains` en una carrera contra su
 * propia ventana.
 *
 * 🔴 NO ESTRENA UN UMBRAL. `FastModeTracker::kSilentUpdatesToRelease` ya existe
 * para exactamente esta pregunta y ya trae su justificacion: "una sola lectura
 * sin señal puede ser el hueco entre dos pulsaciones; el silencio de verdad
 * dura". Se reusa a proposito — REQ-014 ya midio que inventar constantes para
 * esta compuerta es el camino equivocado (ninguna de nivel cumple las dos
 * condiciones, estan 4,4x separadas en la direccion imposible).
 *
 * 🔴 Y NO SE PUDO REUSAR EL ESTADO DE `FastModeTracker`, que era lo primero que
 * uno piensa: su `update()` corre detras de `candidateCount() > 0`, y el
 * escenario del defecto no tiene lista de cuerdas — el objetivo entra por
 * `wma_tuner_set_target`. Se reusa la CONSTANTE, no el objeto.
 *
 * No es RT: vive en el thread de analisis, igual que el resto de este lazo.
 */

#include "FastModeTracker.h"

namespace wma::analysis {

class AbsenceGate {
public:
    /// Lecturas tonales vacias seguidas antes de declarar ausencia. Es la MISMA
    /// que usa el enganche de `FastModeTracker`, y a proposito: la pregunta es la
    /// misma ("¿esto es un hueco o es silencio?") y la cadencia tambien.
    static constexpr int kQuietUpdatesToDeclare = FastModeTracker::kSilentUpdatesToRelease;

    /**
     * @param belowSilenceFloor   el nivel esta por debajo del piso: silencio real.
     * @param detectorRan         hubo rate preparado, o sea que la evidencia tonal
     *                            se produjo. Sin esto no se puede afirmar ausencia
     *                            apoyandose en una evidencia que no existe.
     * @param tunableSourcePresent hay una altura y es plausible como el objetivo.
     * @param freshVerdict        el detector produjo un veredicto NUEVO en esta
     *                            pasada (`McLeodPitch::process()` lo devuelve). Sin
     *                            esto la compuerta contaria la misma evidencia
     *                            varias veces — ver la nota adentro.
     * @return true si hay que declarar ausencia AHORA.
     */
    bool update(bool belowSilenceFloor, bool detectorRan,
                bool tunableSourcePresent, bool freshVerdict) noexcept {
        if (belowSilenceFloor) {
            // Silencio real: ausencia sin demora, Y se satura el contador. Saturar
            // importa para la transicion silencio -> ruido sin altura: sin eso, al
            // primer bloque de ruido el contador arrancaria de cero y el motor
            // diria "midiendo" durante N lecturas sobre una habitacion vacia, que
            // es el spinner eterno que AC-019.2 prohibe.
            mQuietRun = kQuietUpdatesToDeclare;
            return true;
        }
        if (!detectorRan || tunableSourcePresent) {
            mQuietRun = 0;
            return false;
        }
        // 🔴 SOLO SE CUENTA UN VEREDICTO NUEVO, y esta es la parte que costo una
        // etapa entera aprender.
        //
        // La primera version contaba LECTURAS, y eso es un error de UNIDADES: la
        // ventana del detector es NO SOLAPADA y de 4096 frames de entrada
        // (2048 decimadas x2 a 44,1 kHz), mientras que el consumidor lee un
        // snapshot por bloque. Con bloques de 1024 eso son CUATRO lecturas por
        // veredicto, asi que un unico veredicto malo se contaba cuatro veces.
        //
        // Medido (REQ-019.2): con la version que contaba lecturas, el defecto
        // bajaba de 22/120 a 3/120 y el conteo de ausencias pasaba de 4 a
        // exactamente 2 — que es 4 menos los 2 que una histeresis de 3 alcanza a
        // tapar. La aritmetica delataba la unidad equivocada.
        //
        // Y por eso subir el umbral habria sido el arreglo malo: compensa un error
        // de unidades con un numero mas grande, y el numero correcto dependeria del
        // tamaño de bloque del CONSUMIDOR. Contando veredictos, `N` significa lo
        // que dice: N ventanas de analisis distintas sin encontrar altura.
        if (!freshVerdict) return mQuietRun >= kQuietUpdatesToDeclare;
        if (mQuietRun < kQuietUpdatesToDeclare) ++mQuietRun;
        return mQuietRun >= kQuietUpdatesToDeclare;
    }

    /// Para un arranque limpio (`prepare()`, cambio de objetivo): la memoria de la
    /// sesion anterior no puede decidir sobre la nueva.
    void reset() noexcept { mQuietRun = 0; }

    /// Sonda de tests: cuantas lecturas vacias seguidas lleva.
    int quietRun() const noexcept { return mQuietRun; }

private:
    int mQuietRun = 0;
};

}  // namespace wma::analysis
