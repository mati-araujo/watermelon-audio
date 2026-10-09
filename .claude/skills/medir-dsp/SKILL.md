---
name: medir-dsp
description: Cómo escribir en watermelon-audio un test que MIDE audio renderizado (nivel por voz o por banda, envolventes, release, clicks, Δ dB entre estados) sin medir otra cosa. Usar al escribir o revisar un test de DSP que afirma sobre muestras, o cuando un AC dice "nivel", "sin click", "release", "dB" o "cada N ms".
---

# Medir DSP en watermelon-audio

Una **medición** es un test que renderiza audio y afirma sobre un número derivado de las muestras.
Su modo de falla típico no es dar rojo: es dar **verde midiendo otra cosa** (el modo viejo, el
crossfade, una ventana mal puesta). Cada paso de abajo cierra una de esas puertas, y la etapa no
termina hasta que el **control** del paso 6 haya dado rojo.

Las reglas generales de tests del repo (esperas, sanitizers, receptores que viven más que el motor)
están en su `CLAUDE.md` § Tests y valen acá completas.

## Pasos

1. **Puerta.** Renderizá por `AudioEngine::startOffline(rate, block)` +
   `renderBlock(out, nullptr, frames)`. Entra por la misma cadena que el callback real: protección
   de salida, master, crossfades. Patrón: `core/tests/test_touch_expression_surface.cpp`.
   - La C-API **no renderiza**, a propósito (`test_c_api_synth.cpp`).
   - Si el AC es sobre un componente aislado (un `Effect`, un engine), llamalo directo, y declará
     en el encabezado del test qué capas de arriba quedan sin cubrir.
   - Hecho cuando: el test produce un `std::vector<float>` estéreo con el estado bajo prueba.

2. **Determinismo.** El primer test del archivo renderiza la misma config dos veces y compara
   **muestra a muestra** (`ASSERT_FLOAT_EQ`). Si este se cae, todo lo demás es ruido.
   - Hecho cuando: existe y pasa.

3. **Calentar.** Antes de la ventana de medición, renderizá bloques hasta que se apague todo lo
   transitorio, y contalos en **bloques**, nunca en tiempo de pared. Los transitorios conocidos:
   - crossfade de **cambio de engine**: `SynthEngineDispatcher::ENGINE_CROSSFADE_SAMPLES` (240,
     ~5 ms a 48 kHz);
   - crossfade de **routing** de `EffectChain`: 30 ms, y su **primer bloque es 100 % el modo
     VIEJO**: un test que cambia el routing y procesa un bloque mide el modo anterior;
   - `ParameterSmoother`: `kParamSmoothingMs` = 5 ms en los engines; los osciladores Classic
     suavizan también la amplitud;
   - el ataque y el release que el propio AC define.
   - Hecho cuando: la ventana arranca después del último transitorio, con el margen escrito
     como constante con nombre.

4. **Observable.** Elegí el número que **puede ver** lo que afirma el AC.
   - Nivel de una voz entre varias: energía de **su banda** (Goertzel en su fundamental), no RMS
     de la mezcla, porque el RMS mezcla las voces. El Goertzel está en
     `analysis/tests/support/PartialOracle.h` (`goertzel(x, sr, hz)`, espera la señal **ya
     ventaneada**). Ventaneá con Hann: dos fundamentales cercanas sin ventana se tocan por fuga
     espectral.
   - Elegí las frecuencias de las voces separadas y fuera de armónicos mutuos (no 220/440).
   - Serie temporal "cada N ms": ventanas de N ms. El nombre de la variable lleva la **ventana y la
     referencia** (`dbRespectoRegimenMas300ms`, no `deltaDb`): un número relativo sin ventana
     esconde de qué depende.
   - "Sin click" se mide, no se escucha. Con una voz senoidal pura, ninguna diferencia
     `|x[n] − x[n−1]|` de la transición supera el máximo de esa diferencia en régimen × (1 + tol).
     Con engines de banda ancha (Supersaw, Granular), usá la monotonía de la banda por ventana.
   - "Sonó" es > 0,005; el piso de silencio del motor es ~1e-5. Nunca `> 0`.
   - Hecho cuando: cada AC tiene su observable nombrado y se puede decir por qué ese número
     cambiaría si el AC fallara.

5. **Umbrales.** Cada umbral sale del AC o de una cuenta escrita en un comentario, nunca de lo que
   midió la primera corrida. Antes de restar dos niveles, confirmá que tienen **el mismo piso**:
   restar pisos distintos fabrica un Δ.
   - Hecho cuando: cada constante numérica tiene su origen al lado.

6. **Control.** Tiene que **ponerse rojo**, y en el assert correcto:
   - anulá lo que medís (cero en el buffer de la voz, bypass del componente) y verificá que la
     salida **cambie**. Si no cambia, no lo estabas midiendo;
   - por cada AC, un **mutante** del código de producción que lo viole (volver a la ley vieja,
     sacar el release, sacar la rampa) tiene que matar **ese** test por **ese** assert. Un mutante
     que sobrevive acusa al test; uno que muere en otro assert no prueba el AC;
   - revertí el mutante con un commit WIP o un `git diff` guardado, nunca con `git checkout` sobre
     archivos de la etapa.
   - Hecho cuando: hay una tabla mutante → test → assert → rojo, anotada en el journal de la
     etapa.

7. **Gates.** `bash scripts/run-cpp-tests.sh`, `python3 scripts/check-test-waits.py`,
   `python3 scripts/check-rt-safety.py` si tocaste código RT, y la suite bajo ASan/UBSan con
   `CTEST_JOBS=4`. Un barrido largo que da timeout bajo sanitizer se arregla bajando
   `CTEST_JOBS`, no subiendo el techo.
   - Hecho cuando: todo verde con la salida pegada, no resumida.
