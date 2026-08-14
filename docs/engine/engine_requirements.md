# Requerimiento: Solidez del motor de audio — watermelon-audio

**Proyecto:** watermelon-audio (**v2.0.2**). Coordenada KMP: `com.watermellonstudios:audio`
**Prefijo de items:** `WD-N.N` (Watermelon Durability). **No** continúa la numeración `WA-N.N`:
esa pertenece al programa KMP/iOS Readiness (`docs/kmp/kmp_requirements.md`), que cerró
funcionalmente el 2026-08-13 y tiene otro objetivo.
**Origen:** auditoría arquitectónica del 2026-08-13 contra `master` @ `c9f3ca9`.
**Fecha:** 2026-08-13

**Objetivo estratégico:** que el motor sea **correcto y verificable** en las dimensiones que
hoy no lo son —seguridad del thread RT, contrato de los efectos, invariancia de sample rate y
verificación del DSP— sin convertir la librería en un SDK de terceros.

---

> [!IMPORTANT]
> ## Las tres decisiones que dan forma a este documento
>
> Se tomaron el 2026-08-13, antes de escribir un solo requerimiento, porque cada una cambia
> **qué** se hace, no cómo.
>
> | Decisión | Elegido | Consecuencia directa |
> |---|---|---|
> | **D1 — Qué es Watermelon a 3 años** | **Motor de apps musicales propias, sin abrir a terceros** | Entra la solidez (C1–C4, C7, C8). **No** entra el split `:wma-instrument`, ni la API pública para terceros, ni la doc de usuario completa, ni BCV. **Sí** entra `getLatencySamples()` y un `AudioBufferView` **interno** — porque uno arregla un defecto vivo y el otro se encarece monótonamente |
> | **D2 — Orden vs. validación en device (G2)** | **Arreglar el thread RT primero, device después** | La sesión de hardware se gasta una vez, sobre código arreglado. Validar 2.0.2 mediría los bugs, no el motor — y el arreglo de FTZ es justo lo que cambia el costo de CPU que Instruments iba a medir |
> | **D3 — libusb LGPL-2.1 en artefacto MIT** | **Separar el artefacto ahora** | 3 días. Cierra la exposición antes de que exista un tercer consumidor, achica el AAR por defecto y saca libusb de los builds iOS/desktop donde no hace nada |
>
> **El lema de D1: no construyas el SDK, pero no lo cierres.** Cada cosa diferida en §7 lleva
> un **disparador explícito** — la condición que la reabre. Diferir no es descartar.

---

## 1. Objetivo y no-alcance

### Objetivo

1. Que el thread de audio **cumpla las reglas que el propio repo documenta** — hoy no las cumple
   en cuatro lugares del path que shippea en release.
2. Que exista una forma de **probar que el DSP hace lo que dice** — hoy hay 696 tests de gtest y
   cero que midan una respuesta en frecuencia, un THD, una RT60 o una latencia.
3. Que el contrato de `Effect` sea **completo**: que declare su latencia, que su `reset()` sea
   obligatorio, y que no compense su falta de contrato con un normalizador por bloque.
4. Que el sample rate sea **un dato, no una suposición** — hoy hay 242 ocurrencias no-test de
   `48000` y dieciséis engines preparados a ese valor sin releerlo nunca.
5. Que el host pueda **saber cómo está el audio** sin leer logcat.
6. Que los puentes Kotlin↔C **se generen**, para que un cambio en la C API cueste una vez y no tres.

### No-alcance (consecuencia de D1)

- Separar `:wma-instrument` del motor. El API público sigue siendo el de NoisyPad y está bien.
- API pública para consumidores externos, `explicitApi()`, binary-compatibility-validator.
- Documentación de usuario (los nueve documentos de la auditoría §23). **Excepción:** corregir el
  README, que hoy afirma que la librería es sólo Android y no menciona iOS — eso es falso, no
  incompleto.
- Grafo de audio, procesadores multi-entrada, resampler como procesador, AEC, beamforming.
- Refactor del god object `AudioEngine` (3.637 LOC).
- Builds reproducibles, SBOM, path Swift verificado.

Todo lo de arriba está en §7 con su disparador.

---

## 2. Diagnóstico

La auditoría del 2026-08-13 leyó el código, no los docs, y encontró 25 red flags. El resumen
honesto en una línea: **la capa de backends, el aislamiento de plataforma, el build system y la
cultura de ingeniería son fuertes; el contrato de DSP, el modelo de buffer y la verificación no.**

Lo que hay que retener del diagnóstico, porque justifica el orden de las fases:

- **El thread RT no respeta sus propias reglas.** `IAudioBackend.h:181–187` lista exactamente las
  prohibiciones correctas, incluido `std::shared_ptr`. El callback las viola en cuatro lugares, y
  dos de ellos **sobreviven a `NDEBUG`**.
- **La mitigación de denormales está inerte.** `flushDenormals()` es estado **por thread** —
  el propio header lo dice— y los dos únicos call sites corren en el thread del llamador. El
  thread RT nunca la ejecuta, en ninguna plataforma. Los flushes manuales a `1e-20f` repartidos
  por ocho archivos son el fallback, y faltan justo en `BiquadFilter`, `StateVariableFilter`,
  `DelayLine` y `ParametricEQ`.
- **No hay un solo test de correctitud de DSP.** El censo de aserciones sobre 696 casos: 481
  `EXPECT_EQ`, 199 `EXPECT_FALSE`, 170 `EXPECT_FLOAT_EQ`. Cero FFT, cero THD, cero golden. Los
  tests de efectos verifican que están cableados, no que suenan.
- **`setRoutingMode(0–5)` está expuesto en `IEffectManager`.** Los cinco modos paralelos suman
  ramas alineadas por muestra, y ningún efecto declara latencia. Un limiter (5 ms = 240 muestras
  @48k) en una rama y un filtro en la otra dan un peine con notches cada ~200 Hz. **Esto es
  alcanzable por NoisyPad hoy**, no es un riesgo futuro.

---

## 3. Fases y el orden que no es libre

```
Fase 1  El thread RT  ──────────────┐
            (WD-1.x)                │  D2: el device va acá, no antes
                                    ├──► G2 · validación en hardware
Fase 2  Verificabilidad ◄───────────┘
            (WD-2.x)   el baseline golden se toma DESPUÉS de WD-1.2
                                    │
Fase 3  Contrato de Effect ◄────────┘   verificado contra ese baseline
            (WD-3.x)
                                    │
Fase 4  Puentes generados ──────────┤   antes de tocar la C API
            (WD-4.x)                │
                                    │
Fase 5  Observabilidad ─────────────┤   mide el beneficio de la Fase 1
            (WD-5.x)                │
                                    │
Fase 6  Distribución (WD-6.1) ──────┤   independiente, paralelizable
                                    │
Fase 7  AudioBufferView interno ◄───┘   necesita la Fase 4 hecha
            (WD-7.1)
```

**Tres restricciones de orden, con su porqué:**

1. **WD-1.2 (FTZ) va antes del baseline golden de WD-2.2.** Activar flush-to-zero **cambia
   resultados numéricos** — no es sólo una optimización, es un cambio de modo de punto flotante.
   Tomar el baseline antes lo invalida entero al día siguiente. El orden es: arreglar FTZ →
   capturar golden → arreglar el resto contra ese golden.
2. **La Fase 4 va antes de la Fase 7.** WD-7.1 toca la C API. Con los puentes a mano, cada cambio
   de la C API se paga tres veces (JNI + cinterop + interfaz común, 4.604 LOC espejados).
   Generarlos primero convierte la Fase 7 de seis semanas en cuatro.
3. **La Fase 3 va después de la Fase 2.** Sin golden vectors no se puede probar que sacar el
   auto-gain o cambiar el clamp de coeficientes no cambió lo que no debía cambiar. Arreglar DSP
   sin baseline es cambiar el sonido a ciegas.

---

## 4. Fase 1 — El thread RT (C1)

> **Qué la cierra:** el lint de RT-safety verde, ASan/TSan verdes, y `bash scripts/gate.sh`
> pasando. Nada acá necesita hardware para ser *correcto* — son violaciones de regla
> independientemente de lo que muestre un device. El hardware sirve para medir el *beneficio*,
> y por eso G2 va después (D2).

---

### WD-1.1 — Sacar el logging del path RT, y un lint que lo impida volver

**Problema.** `wma::logMessage` se llama desde adentro de `AudioEngine::onAudioReady` en builds
de release. Los macros `LOGI`/`LOGW` sí se compilan a `((void)0)` bajo `NDEBUG`
(`AudioEngine.cpp:108–124`), pero dos bloques **saltean los macros** y llaman a la función
directo, así que sobreviven. Un tercer caso: `EffectChain.cpp` define `LOGE` sin condicional
(línea 16) y lo llama desde `processOneEffect` al detectar NaN — o sea, por bloque.

**Evidencia inicial (la de la auditoría).**
- `core/AudioEngine.cpp:2078–2103` — bloque `WMA_AUDIT` / `USB_CB`, cada 300 callbacks
- `core/AudioEngine.cpp:2158–2172` — bloque `USB_DIRECT_OUT`, cada 300 callbacks
- `effects/EffectChain.cpp:16` + `processOneEffect` — `LOGE` en la detección de NaN

> ⚠️ **La auditoría subestimó esto por un factor de diez.** El lint encontró **65** violaciones
> en 424 funciones alcanzables, no tres. Lo que faltaba:
>
> | Dónde | Qué | Por qué se me escapó |
> |---|---|---|
> | `nodes/InputNode.cpp` | **13 logs**, y el archivo define `LOGI/LOGW/LOGE` **sin condicional de `NDEBUG`** | Miré el archivo por su ring buffer, no por sus macros. Es la concentración más grande de logging RT que sobrevive a release — mayor que los dos bloques `WMA_AUDIT` que sí reporté |
> | `core/AudioEngine.cpp` `renderInputFx` | 2 `wma::logMessage` directos más | Leí `onAudioReady` y no seguí el call-graph |
> | `voice/VoiceManager.cpp`, `VoicePool.cpp` | 9 logs en los handlers de note-on/off | No sabía que los despacha la cola lock-free **desde el thread de audio** |
> | `sequencer/ArpSequencer.h` | 2 logs en `rebuildPattern()` | Usa un macro con prefijo propio (`ARP_LOGI`); mi grep buscaba `LOGI` |
> | `looper/AudioLooper.h` | `LOOPER_LOGE` junto a un `resize()` en el path RT | Ídem — prefijo propio |
> | `effects/EffectChain.cpp` | 3 más: `reset()`, buffer overflow, silence detection | `reset()` no parece RT por el nombre, pero `Effect.h` documenta que lo despacha `onAudioReady` |
>
> **La lección para el resto del programa: leer no alcanza para inventariar una superficie.**
> El lint no fue el entregable de WD-1.1, fue el instrumento que hizo el diagnóstico.

**Escenario de falla.** A 48 kHz con buffer de 128 frames, 300 callbacks son 800 ms. Cada 800 ms
el thread de audio hace un `vsnprintf` de once argumentos más un syscall de logging adentro de un
deadline de 2,7 ms. En un Android de gama media con throttling térmico esto es una causa
plausible de xrun, y se manifiesta como un click intermitente que no se reproduce en un flagship.

**Criterio de aceptación.**
1. Ninguna función alcanzable desde `onAudioReady` llama a `wma::logMessage`, `LOGI`, `LOGW`,
   `LOGE`, `printf`, `snprintf` ni ningún formateador.
2. Existe `scripts/check-rt-safety.py` que recorre el call-graph del callback y falla si aparece
   cualquiera de esos símbolos, más `new`, `malloc`, `resize`, `reserve`, `push_back`,
   `shared_ptr`, `lock()` (permitido `try_lock`), `sleep`, `std::string`.
3. El script está en `scripts/gate.sh` y en `ci.yml`.
4. La detección de NaN pasa a incrementar un contador (consumido por WD-5.1), no a loguear.

**Dos mecanismos que el plan no preveía y que resultaron necesarios:**

- **`// RT-SAFE-ALLOW: <razón>`** — escape hatch por línea, con razón **obligatoria** (el lint
  falla si falta). Hay falsos positivos legítimos: una *referencia* a una `condition_variable`
  no es una espera, y el `resize()` de `AudioLooper` es un fallback deliberado y documentado.
  Una excepción tiene que costar escribir por qué, no ser gratis.
- **`scripts/rt-safety-baseline.txt`** — trinquete para la deuda que un WD **posterior** cierra.
  Hoy tiene dos entradas, las dos de WD-1.3 (`shared_ptr` en el callback). No es una lista de
  excepciones: el lint falla si aparece una violación que no está ahí, **y también si una
  entrada de ahí ya no se reproduce**. Lo segundo es lo que impide que el archivo se pudra —
  un baseline con entradas muertas deja de decir la verdad sobre la deuda.

  Lo importante es la distinción: `RT-SAFE-ALLOW` es para lo que **se sabe inofensivo**; el
  baseline es para lo que **es un problema y todavía no está arreglado**. Meter WD-1.3 en un
  `RT-SAFE-ALLOW` habría sido mentir en un comentario.

**Verificación — tres modos de falla, los tres ejercitados:**

| Modo | Cómo se probó | Resultado |
|---|---|---|
| El lint detecta una violación | `--self-test` inyecta un `LOGE` en un cuerpo alcanzable | exit 1 ✅ |
| El lint detecta una violación **real** | mutación: se reinsertó un `LOGE` en `processOneEffect` | exit 1, señalando el archivo ✅ |
| El baseline no se pudre | se agregó una entrada a una función inexistente | exit 1 ✅ |

El `--self-test` corre **antes** que el lint, en `gate.sh` y en `ci.yml`. No es ceremonia: si el
parser se rompe, el lint queda en verde permanente, que es el peor estado posible para un
guardrail. Cuesta 2 s.

**Resultado medido:** 65 → 2 violaciones (las dos del baseline, con dueño). 795/795 tests verdes.

**Esfuerzo** 3 d estimados · **real ~1 d de edición + el lint** · **Riesgo** bajo

---

### WD-1.2 — Flush-to-zero en el thread RT (no en el del llamador)

**Problema.** FPCR (ARM) y MXCSR (x86) son estado **por thread**. `platform/Platform.h:37` lo
documenta: *"Call once per thread that processes audio (audio callback thread…)"*. Hay
exactamente dos call sites y ninguno corre en un thread RT:

- `core/AudioEngine.cpp:374` — dentro de `AudioEngine::start()`, bajo `mStateMutex`, en el thread
  del llamador (JNI/UI).
- `backends/OboeBackend.cpp:54` — dentro de `OboeBackend::start()`, mismo caso.

`CoreAudioBackend` no la llama en ningún lado. El render block de `AVAudioSourceNode` corre en un
thread creado por AVFoundation que jamás pasa por código de Watermelon que toque FPCR.

**Escenario de falla.** Una cola de reverb decayendo a través de un `BiquadFilter` —que **no**
tiene flush manual— entra en rango subnormal a los pocos cientos de ms de silencio. Ahí el costo
es 10–100× por operación. El repo claramente sabe que importa: hay flushes manuales a `1e-20f` en
`FilterEffect`, `TapeEchoEffect`, `HallReverbEffect`, `RiserReverbEffect`, `HpfDelayEffect`,
`ReverbEffect`, `FDN` y `Oscillators`. Faltan en `BiquadFilter`, `StateVariableFilter`,
`DelayLine`, `EarlyReflections`, `VocoderBank`, `ParametricEQ` y los envelope followers.

> ⚠️ **Y había un segundo defecto, más silencioso, que sólo apareció al escribir el test.**
> La rama de arm64 estaba gateada por `#if defined(__aarch64__) && defined(USE_NEON)`. **FPCR
> es del ISA base de ARM64, no de NEON** — no existe un arm64 sin FPCR. Atarlo a un define de
> capacidad SIMD hacía que cualquier build que no lo seteara se quedara sin flush **en
> silencio**, y el build de los tests de host es exactamente uno de esos: toda la suite de 795
> tests y los tres jobs de sanitizers venían corriendo con denormales habilitados, sin que
> nadie pudiera notarlo. Lo destapó el test de WD-1.2 fallando en sus cuatro aserciones.
>
> Un guard mal puesto que apaga una mitigación sin dejar rastro es peor que la ausencia de la
> mitigación: la ausencia se nota al buscarla, esto no.

**Criterio de aceptación.**
1. `flushDenormalsRtSafe()` se ejecuta en cada entrada al callback, **sin guarda
   `thread_local`**. Aplica a los tres paths: Oboe, CoreAudio y libusb.

   > **Desvío deliberado del plan original**, que pedía una guarda `thread_local bool`. En una
   > librería compartida el primer acceso a un `thread_local` puede pasar por `__tls_get_addr`
   > y alocar el bloque de TLS — o sea un malloc en el thread de audio, que es justo lo que se
   > quiere evitar. El trabajo que la guarda ahorraba es un read-modify-write de un registro
   > de control (~decenas de ciclos, ~375 veces por segundo): del orden del 0,001% de un core.
   > **La guarda costaba más riesgo que el trabajo que evitaba.**
2. Se borran los flushes manuales a `1e-20f` que quedan redundantes. **No antes** de que el punto
   1 esté verificado — si se borran primero, se pierde la única mitigación que hay.
3. Un test lee FPCR/MXCSR desde adentro del callback (vía `FakeAudioBackend`) y afirma que el bit
   FZ está seteado.

> ⚠️ **Este requerimiento cambia resultados numéricos.** Es la razón de la restricción de orden
> #1 de §3: el baseline golden de WD-2.2 se captura **después** de que esto esté mergeado.

**Esfuerzo** 1 d (+1 d para el borrado de los manuales) · **Riesgo** bajo · **Depende de** —

---

### WD-1.3 — `shared_ptr` fuera del callback

**Problema.** `core/AudioEngine.cpp:2181–2190` copia un `std::shared_ptr<InputNode>` adentro de
`onAudioReady`. `IAudioBackend.h:186` lo prohíbe explícitamente.

```cpp
std::shared_ptr<InputNode> inputNodePtr;
if (mInputNodeMutex.try_lock()) {
    inputNodePtr = mInputNode;      // incremento atómico de refcount
    mInputNodeMutex.unlock();
}
…                                    // salida de scope: decremento
```

El `try_lock` es defendible. La copia no. Y el costo del refcount es lo de menos: si el thread de
UI llama `setInputNode(nullptr)` y suelta su referencia mientras el thread de audio tiene la
última, **el destructor de `InputNode` corre en el thread de audio** — liberando dos
`LockFreeRingBuffer` de 96.000 floats cada uno, dos `std::vector`, y cerrando el stream de captura
a través de `BackendAdapterDeleter`. Eso es `free()` más un teardown de dispositivo adentro del
deadline.

**Criterio de aceptación.**
1. El callback lee un `std::atomic<InputNode*>` crudo. Cero operaciones de refcount en el path RT.
2. La reclamación del nodo ocurre en el thread de control, detrás de una barrera de drenaje.
3. Existe un test que hace `setInputNode(nullptr)` mientras un thread bombea callbacks, y
   TSan/ASan quedan verdes.

> ⚠️ **Y de paso apareció que la barrera no cubría todo lo que decía cubrir.** El `CallbackGuard`
> vivía adentro de `processAudioBlock()`, y el fast-path de USB de `onAudioReady()` retorna
> **antes** de llegar ahí. Esos bloques no contaban como callback en vuelo, así que ni el
> drenaje de `stop()` ni el del retiro los veían. Subirlo al punto de entrada arregló `stop()`
> gratis.

> 🔴 **La lección que más vale de este WD no es el arreglo, es el test.**
>
> Los primeros tres tests que escribí verificaban el contrato nuevo y pasaban. Los mutê
> —restauré el `shared_ptr` en el callback y quité el drenaje— y **pasaron igual, quince
> corridas**. No detectaban el bug. La ventana de la carrera dura microsegundos y bombear
> callbacks a ciegas no la pega.
>
> El test que sirve necesita **una compuerta en el doble**: atrapar al callback adentro, con el
> nodo en uso, y recién ahí retirar desde otro thread. Ahí el código viejo falla determinísticamente
> y el nuevo pasa.
>
> Generalizable al resto del programa: **para un bug de concurrencia, un test que no bloquea
> deliberadamente la ventana casi nunca la pega.** Y si no se muta, no hay forma de saberlo —
> se ve verde igual.

**Esfuerzo** 2 d estimados · **real ~1 d** · **Riesgo** medio · **Verificado** por mutación en
las dos direcciones, más ASan y TSan

---

### WD-1.4 — Reclamación de efectos sin `sleep`

**Problema.** `effects/EffectChain.cpp:166` y `:214` usan `std::this_thread::sleep_for(20ms)` como
barrera antes de destruir un `Effect`. El swap de snapshot es correcto y genuinamente lock-free
del lado del lector; la **reclamación** es una adivinanza de timing.

```cpp
// Asumiendo ~10ms de latencia de audio, 20ms es seguro
std::this_thread::sleep_for(std::chrono::milliseconds(20));
```

Es segura bajo esa suposición e insegura afuera: un thread de audio desalojado por uno de mayor
prioridad, un evento de throttling térmico, una migración big.LITTLE, o simplemente un device
configurado con buffer de 1024 frames a 44,1 kHz (23,2 ms) la rompen. El resultado es un
use-after-free sobre una llamada virtual desde el thread de audio. Además bloquea el thread
llamador 20 ms por cada `removeEffect`, que es por qué hubo que inventar `clearAllEffects()`.

**Criterio de aceptación.**
1. El callback publica un contador de generación (`relaxed`). El thread de control recicla cuando
   observa dos incrementos, o tras un timeout — y si el timeout vence, **loguea y filtra** en vez
   de liberar. Filtrar es preferible a un UAF.
2. `removeEffect` no bloquea al llamador más de lo que tarda el mutex de estructura.
3. Test: `removeEffect` con el motor **parado** (cero callbacks) no cuelga ni libera antes de
   tiempo. Test: `removeEffect` bajo carga con buffers de 1024 frames, verde bajo ASan.

**Esfuerzo** 2 d · **Riesgo** medio · **Depende de** —

---

### WD-1.6 — El vocoder toma el mutex de la cadena desde el thread de audio

> **Este requerimiento no salió de la auditoría. Lo encontró el lint de WD-1.1**, en su
> primera corrida, y es más grave que varias cosas que sí estaban en el informe.

**Problema.** La cadena era:

```
AudioEngine::feedVocoderModulator()          [thread de audio, por bloque]
  -> EffectChain::setVocoderModulatorBuffer()
     -> findVocoderIndex()
        -> std::lock_guard<std::mutex> lock(chainMutex)   <-- BLOQUEA
```

`chainMutex` es el mismo que sostienen `addEffect()`, `removeEffect()` y `clearAllEffects()`
— y este último lo sostiene mientras construye y destruye efectos, o sea **mientras aloca**.
Agregar un efecto con el vocoder corriendo ponía al thread de audio a esperar a un thread de
UI que estaba adentro del allocator: inversión de prioridad de manual, y un dropout garantizado.

Y además era una carrera. Después de soltar el lock, los cuatro callers indexaban `effects`
—el vector **propio**, no el snapshot— apoyados en un comentario que decía *"effects vector is
stable once added"*. Es falso: `removeEffect()` hace `effects.erase()`, que desplaza elementos
y puede realocar.

**Arreglo.** El índice del vocoder se publica en un atómico, mantenido bajo `chainMutex` por
`updateSnapshot()` —que ya corre en el thread de control en cada cambio estructural— y los
cuatro setters leen el **puntero del snapshot activo**, que es la vista que existía justamente
para que el thread de audio no toque `effects`.

**Esfuerzo** 1 d · **Riesgo** medio (concurrencia) · **Verificado** por TSan y por el lint

---

### WD-1.5 — Statics de función fuera del path RT

**Problema.** Seis o más contadores `static int` viven adentro de funciones RT y casi-RT
(`AudioEngine.cpp:2078, 2107, 2158, 2192, 2320`, y `monitorXRuns` del adapter de Oboe). Más allá
del incremento no atómico, son **globales de proceso**: dos instancias de `AudioEngine` los
comparten, así que el comportamiento del callback depende de cuántos motores existan.

**Criterio de aceptación.** Ningún `static` local de función en el call-graph del callback; todos
pasan a miembros.

> **Salió mucho más barato que lo estimado: quedaban 3, no "6 o más".** Los demás se cayeron
> solos con los bloques de log que los envolvían, en WD-1.1 — un `static int` que gatea un
> `LOGI` no sobrevive a que se borre el `LOGI`.
>
> De los 3, dos eran del path RT y se arreglaron (`monitorXRuns`, `updateMultiTouch`). El
> tercero —`static bool initialized` en `getGlobalRegistry()`, `EffectChain.cpp`— **no está en
> el call-graph del callback**, así que queda fuera de alcance por definición del propio WD.
> Pero es una carrera de datos real: `static EffectRegistry reg` sí tiene inicialización
> thread-safe por el estándar, `initialized` no, y dos threads que llamen `getNumParams()` a la
> vez pueden ambos ver `false` y registrar los efectos dos veces. Queda señalado acá para que
> no se pierda; el arreglo son 3 líneas.

**Esfuerzo** 1 d estimado · **real ~1 h** · **Riesgo** bajo

---

> ### 🔶 G2 — Validación en hardware (va acá, por D2)
>
> No es un WD porque pertenece al programa KMP y sigue abierto ahí. Se **agenda después de la
> Fase 1** y antes de la Fase 3. Qué se mide, ahora que el código está arreglado:
> latencia round-trip real, xruns bajo carga, Instruments sobre el render block con FTZ activo,
> y el costo de CPU de la cadena con 0/1/4/8 efectos. Ese último número es el baseline de WD-5.1.

---

## 5. Fase 2 — Verificabilidad (C4)

> **Qué la cierra:** que un cambio en un algoritmo de DSP rompa un test. Hoy no puede pasar.

---

### WD-2.1 — Path de render offline (seam de test, no feature pública)

**Problema.** No hay forma de renderizar audio sin un dispositivo. Los tests actuales lo esquivan
con `FakeAudioBackend` y `test_platform_backends.cpp` — es un buen workaround, pero es un
workaround: la cadena completa (chain → looper → output stage) sólo es alcanzable a través de
`AudioEngine`, que posee un backend. Ninguna de las 253 funciones `wma_*` toma un buffer de
entrada y un frame count.

**Decisión de alcance (consecuencia de D1).** Esto se construye como **seam de test interno**, no
como API pública. Vive detrás de `@InternalWatermelonApi` del lado Kotlin y no se documenta como
capacidad de la librería. Es lo que hace posible WD-2.2 y WD-2.3; no es el primer paso hacia un SDK.

**Criterio de aceptación.**
1. Existe un modo de construir el motor sin backend, alimentarlo con un buffer y leer la salida,
   de forma determinista y sin threads.
2. La misma cadena de efectos produce **bit a bit** el mismo resultado offline que a través de
   `FakeAudioBackend` con el mismo tamaño de bloque.
3. Renderizar N frames en un bloque y en K bloques de N/K da el mismo resultado, salvo por la
   latencia declarada de la cadena. **Este test es el que destapa efectos con estado que depende
   del tamaño de bloque** — un defecto que hoy sería invisible.

**Esfuerzo** 1 sem · **Riesgo** bajo (aditivo) · **Depende de** —

---

### WD-2.2 — Suite golden de DSP

**Problema.** 696 casos de gtest y ninguno mide una propiedad de señal. Los tests de efectos,
muestreados en `test_guitar_delay_reverb.cpp`, afirman: que los defaults coinciden con Kotlin, que
la salida es finita, que la energía es mayor que cero, y que L difiere de R. Son **smoke tests**.
No pueden detectar que la RT60 de un reverb está mal, que el cutoff de un filtro está una octava
corrido, que el ratio de un compresor no coincide con su parámetro, o que un EQ tiene la Q
invertida. Cobertura: 28 archivos de implementación de efectos, 7 de test, 1.063 líneas — unas 38
líneas de test por efecto.

**Criterio de aceptación.** Por cada efecto, el subconjunto de estas mediciones que aplique:

| Medición | Método | Aplica a |
|---|---|---|
| Respuesta al impulso | IR capturada vs golden commiteado, tolerancia por muestra | EQ, filtro, delay, cabinet, parte temprana de reverbs |
| Respuesta en frecuencia | sweep log → FFT → magnitud/fase vs referencia analítica | filtros, EQ — afirma cutoff, pendiente y Q |
| THD+N | seno puro → energía armónica vs fundamental | distorsión, amp sim, soft clipper, oversampler (afirma supresión de aliasing) |
| Curva de transferencia | escalera de nivel → dB in/out vs ratio+knee | compresor, limiter, gate |
| RT60 | impulso → tiempo a −60 dB | los siete reverbs |
| Property-based | params y señales aleatorias → finito, acotado, sin NaN; `reset()` → salida idéntica | todos. **Este es el que habría cachado el hueco de `reset()` de WD-3.2** |

**Fixtures.** Generados por código, no commiteados — el precedente del repo es el fixture SF2, y
la lección registrada es que **hay que mutar el fixture además del código**. Se commitean sólo los
golden de salida. Regenerarlos pasa por una tarea explícita para que el diff sea visible.

```
testdata/
  (generados)  impulse_48k · sweep_20-20k_48k · sine_1k_-6dBFS_{44k1,48k,96k}
               staircase_-60..0dB_48k · pink_noise_48k
  golden/      <efecto>_<preset>.f32
```

> ⚠️ **El baseline se captura después de WD-1.2.** Ver la restricción de orden #1 de §3.

**Esfuerzo** 3 sem · **Riesgo** bajo · **Depende de** WD-1.2, WD-2.1

---

### WD-2.3 — Latencia declarada vs. medida, e invariancia de sample rate

**Problema.** Dos huecos que sólo se pueden cerrar con la suite de WD-2.2 armada, y que son los
que hacen que WD-3.1 y WD-3.4 no se degraden con el tiempo.

**Criterio de aceptación.**
1. **Latencia:** por cada procesador, un impulso a la entrada, buscar el pico a la salida, y
   afirmar que la posición coincide con `getLatencySamples()`. Un procesador que miente sobre su
   latencia falla el test. Esto es lo que hace que WD-3.1 **se mantenga** arreglado.
2. **Invariancia de rate:** el mismo resultado musical a 44,1 / 48 / 96 kHz dentro de tolerancia.
   Para un Karplus-Strong: la frecuencia fundamental medida por FFT no se corre. Esto es lo que
   habría cachado WD-3.4 antes de shippear.
3. **Estabilidad numérica:** barrer parámetros a los límites de rango en los tres sample rates y
   afirmar salida finita y acotada. Esto es lo que habría cachado WD-3.5.

**Esfuerzo** 1 sem · **Riesgo** bajo · **Depende de** WD-2.2

---

### WD-2.4 — Tests instrumentados de Android

**Problema.** `androidTest` no existe. Cero tests instrumentados. iOS tiene seis archivos de test
dedicados que ejercitan `IosAudioBridge`, `IosLooperBridge`, `IosSoundFontBridge` e
`IosArpeggiatorBridge` contra cinterop real. Android no tiene ninguno para `AudioNativeBridge`,
que es **el más grande de los dos por un factor de 2,3** (3.229 vs 1.375 LOC). La asimetría va
al revés de lo que sugiere el riesgo.

**Criterio de aceptación.** Un suite instrumentado que espeje al de iOS: marshalling de JNI,
`Result<T>` en los paths que fallan, y los mutexes por categoría. No cubre USB (necesita hardware).

**Esfuerzo** 1 sem · **Riesgo** bajo · **Depende de** —

---

## 6. Fase 3 — El contrato de `Effect` (C2 + C3)

> **Qué la cierra:** los golden de WD-2.2 verdes después de cada cambio, y los tres tests de
> WD-2.3 verdes. Sin la Fase 2 hecha, esta fase es cambiar el sonido a ciegas.

---

### WD-3.1 — `getLatencySamples()` y compensación en los modos paralelos

**Problema.** No existe `getLatencySamples()` en `Effect`. `LookaheadLimiter` introduce 5 ms de
lookahead. `DelayEffect`, los reverbs, los efectos con `Oversampler` y `CabinetSimulator`
(convolución) suman retardo algorítmico. `EffectChain` ofrece igual `PARALLEL`, `SPLIT_2X2`,
`SERIAL_PARALLEL`, `PARALLEL_SERIAL` y `FEEDBACK`, que suman ramas alineadas por muestra.

**Esto es alcanzable hoy.** `setRoutingMode(0–5)` está expuesto en `IEffectManager:218` y llega
hasta `wma_set_routing_mode`. No es un riesgo futuro.

**Escenario de falla.** Limiter (5 ms ≈ 240 muestras @48k) en la rama A, filtro (0) en la rama B,
modo `PARALLEL`. La suma es un peine de 240 muestras con notches cada ~200 Hz. Es un defecto
tonal audible y reproducible que ningún test detecta porque ninguno mide respuesta en frecuencia.

**Criterio de aceptación.**
1. `virtual int getLatencySamples() const { return 0; }` en `Effect`, implementado con el valor
   real en todos los que tienen retardo algorítmico.
2. Los modos de routing que suman ramas alinean con compensación de retardo.
3. `EffectChain::getLatencySamples()` devuelve la latencia de la cadena según el modo activo.
4. Test de WD-2.3.1 verde para todos los efectos. Test específico: limiter ∥ filtro en `PARALLEL`
   no produce notches — se mide con FFT, no a ojo.

**Nota de compatibilidad.** `Effect` **no es público** — los efectos se crean vía `EffectRegistry`
desde un enum. Esto es source-breaking sólo adentro. No requiere versión mayor.

**Esfuerzo** 1 sem · **Riesgo** medio · **Depende de** WD-2.2, WD-2.3

---

### WD-3.2 — `reset()` pasa a virtual puro

**Problema.** `Effect::reset()` tiene default `{}`. **11 de 26 efectos no lo overridean**, y
varios son inequívocamente stateful: `VocoderEffect` (banco de filtros), `ParametricEQ` (cascada
de biquads), `FilterEffect` (`z1/z2`), `CompressorEffect` (envolvente), `ComplexTremEffect`,
`RandomResoEffect`, `BeatGrainEffect`, `AutoPanEffect`, `DeciHpfEffect`, `AmpSimulator`,
`DecimatorEffect`.

**Escenario de falla.** La transición chaos_pad → input_fx que `reset()` existe para arreglar
—documentada en `Effect.h:52–74`— sigue filtrando estado a través de una cadena que contenga un
EQ o un vocoder. El mecanismo se construyó; la cobertura nunca se auditó. Un default no-op en un
virtual relevante para seguridad es el default equivocado.

**Criterio de aceptación.**
1. `virtual void reset() = 0;`. Cada efecto escribe una implementación explícita o un `{}`
   explícito con comentario que diga por qué no tiene estado. El compilador lo fuerza.
2. El test property-based de WD-2.2 pasa para los 26: procesar → `reset()` → procesar la misma
   entrada da salida idéntica.

**Esfuerzo** 2 d · **Riesgo** bajo (forzado por el compilador) · **Depende de** WD-2.2

---

### WD-3.3 — Eliminar el auto-gain por efecto

**Problema.** `EffectChain.cpp`, en `processOneEffect`:

```cpp
constexpr float GAIN_CEILING = 1.5f;
float peak = /* escaneo de todas las muestras */;
if (peak > GAIN_CEILING) {
    float gain = GAIN_CEILING / peak;
    for (int s = 0; s < totalSamples; ++s) output[s] *= gain;
}
```

Aplica **una ganancia constante distinta a cada bloque**, sin ataque, sin release, sin suavizado
en el borde. Un transitorio que pica a 3,0 en el bloque *n* y a 1,4 en el *n+1* produce un escalón
de ×0,5 a ×1,0 en el borde: un click, cada vez, en cada efecto de la cadena. Es un limiter por
bloque, por efecto, sin lookahead, con ataque infinito y release infinito — que es la definición
del artefacto que presumiblemente se agregó para prevenir.

Además cuesta dos pasadas completas extra por buffer por efecto (pico + el escaneo de NaN), antes
de que los efectos hagan trabajo.

**Alternativa considerada y descartada:** suavizar la ganancia a lo largo del bloque. Se descarta
porque un limiter por efecto es el lugar equivocado para control de nivel, con suavizado o sin él.
La protección va una vez, al final, en `OutputStage`.

**Criterio de aceptación.**
1. `processOneEffect` no normaliza. La protección de salida queda sólo en `OutputStage`.
2. El escaneo de NaN se conserva pero se fusiona en una sola pasada e incrementa el contador de
   WD-5.1 en vez de loguear (ver WD-1.1).
3. Los golden de WD-2.2 se re-capturan **conscientemente** en este cambio, con el diff revisado —
   este requerimiento cambia el sonido a propósito.

**Esfuerzo** 1 d · **Riesgo** bajo · **Depende de** WD-2.2

---

### WD-3.4 — El sample rate se propaga a todo lo que lo necesita

**Problema.** `core/SynthEngineDispatcher.cpp:25–73` prepara **dieciséis** engines con
`prepare(48000, 4096)` literal. `onStreamConfigChanged` llama a `mOscBank.prepare`,
`mEffectChain.setSampleRate` y `mOutputStage.prepare` — **no al dispatcher**.

**Escenario de falla.** En un device que negocia 44,1 kHz, los engines quedan preparados para 48.
Una cuerda de Karplus-Strong afinada por longitud de línea de retardo queda **8,8% sharp** — 1,5
semitonos. 44,1 kHz es el rate nativo de una fracción grande de dispositivos Bluetooth y USB.

Un segundo caso, más chico: `EffectChain.cpp:50` computa
`mCrossfadeSamples = 48000 * 0.030f` en el constructor y nunca lo recalcula en `setSampleRate()`.
A 96 kHz el crossfade de routing dura 15 ms; a 44,1 kHz, 32,6 ms.

**Criterio de aceptación.**
1. `onStreamConfigChanged` re-prepara los engines del dispatcher, y todo lo demás que tenga estado
   dependiente del rate. Se audita con un grep de `48000` sobre las 242 ocurrencias no-test y se
   clasifica cada una: constante legítima, default, o bug.
2. `mCrossfadeSamples` se recalcula en `setSampleRate()`.
3. Test de WD-2.3.2 verde: fundamental medida por FFT no se corre entre 44,1 / 48 / 96 kHz.

**Esfuerzo** 3 d · **Riesgo** medio (toca el ciclo de vida de dieciséis engines) · **Depende de** WD-2.3

---

### WD-3.5 — Clamp de coeficientes contra Nyquist

**Problema.** `EffectParameter.kt` acota rangos en Kotlin (`FilterFrequency: 20–20000`,
`FilterResonance: 0.1–10`). Esos límites son **independientes del rate**: un cutoff de 20 kHz a
44,1 kHz de sample rate está por encima de Nyquist (22,05 kHz) menos cualquier margen, y los
coeficientes de un biquad RBJ se vuelven inestables cuando ω₀ → π. Nada en C++ vuelve a acotar
contra el rate real.

**Escenario de falla.** Cutoff a 20 kHz en un device a 44,1 kHz. `tan(π · 20000/44100)` cae en la
región donde el warping de la transformada bilineal explota. La salida auto-oscila o produce NaN
— que el scrubber convierte en silencio, así que el usuario escucha que el filtro "deja de
funcionar" sin que aparezca un error en ningún lado.

**Criterio de aceptación.**
1. `BiquadFilter::setCoefficients` y `StateVariableFilter` acotan a `min(valor, 0.45 · fs)`.
2. Test de WD-2.3.3 verde: barrido de cutoff hasta el límite a 44,1 kHz, salida finita y acotada.

**Esfuerzo** 1 d · **Riesgo** bajo · **Depende de** WD-2.3

---

## 7. Fase 4 — Puentes generados y modelo de error (C7)

### WD-4.1 — Generar los puentes desde la C API

**Problema.** Tres superficies mantenidas a mano sobre una C API de 253 funciones:
`AudioNativeBridge.kt` (3.229 LOC), `IosAudioBridge.kt` (1.375) e `IAudioNativeBridge.kt` (386).
`scripts/c-api-gap.py` existe precisamente porque driftean, y reporta: 280 entry points de JNI,
255 funciones de C API, **gap 86**, 63 funciones de C API inalcanzables desde JNI, USB en 0/36.

Tener la herramienta está bien. **Necesitarla es el hallazgo.** Dos bugs de producción ya
registrados en este repo son de esta clase exacta: el puente de iOS mapeando las dos variantes de
fade a `fade_time_ms = 0`, y iOS devolviendo `success` desde una `wma_*` que devuelve `void` y
había rechazado la llamada.

**Criterio de aceptación.**
1. Un generador (~300 LOC) produce las tres superficies desde el header de C.
2. `c-api-gap.py` reporta gap 0 para lo portable. El gap de USB queda declarado como
   Android-only en el propio generador, no como omisión.
3. El generador corre en `gate.sh` y falla si el código generado difiere del commiteado.

**Beneficio de segundo orden:** cualquier cambio futuro en la C API —incluida la Fase 7— cuesta
una vez. Es la razón de la restricción de orden #2 de §3.

**Esfuerzo** 1 sem · **Riesgo** bajo · **Depende de** —

---

### WD-4.2 — Ningún mutador `void`, y `BackendError` cruza la C API

**Problema.** Dos huecos del modelo de error:

1. La C API mezcla setters que devuelven `WmaResult` con setters que devuelven `void`, sin regla.
   `wma_set_routing_mode` es `void`. Un `void` no puede transportar un rechazo — es la clase de
   bug que ya causó que iOS devolviera `success` habiendo hecho nada. Se arregló la instancia; la
   clase sigue abierta.
2. `BackendError` (`UNDERRUN`, `OVERRUN`, `DEVICE_DISCONNECTED`, `TRANSFER_ERROR`) existe en C++ y
   no tiene equivalente en `WmaResult`. Un host no puede distinguir "tu dispositivo desapareció"
   de "error desconocido". Son justo los errores que un host tiene que manejar.

**Criterio de aceptación.**
1. Toda función que puede ser rechazada devuelve `WmaResult`. Las que genuinamente no pueden
   fallar (getters puros) devuelven su valor. **Cero mutadores `void`.** Es una auditoría mecánica
   de 253 funciones y cierra la clase de forma permanente.
2. `WmaResult` gana los códigos que faltan para cubrir `BackendError`.
3. El generador de WD-4.1 propaga el `Result<T>` correcto a las dos plataformas por construcción.

**Esfuerzo** 4 d · **Riesgo** bajo · **Depende de** WD-4.1

---

## 8. Fase 5 — Observabilidad (C8)

### WD-5.1 — `WmaDiagnostics` por polling

**Problema.** La librería reporta su salud a logcat. Un host no puede asumir que su usuario lee
logs, y todo lo que vale la pena reportar pasa en un thread que no puede loguear. Hoy no son
accesibles: xruns (se leen con `getXRunCount()` y se loguean), overruns, starvation de ring, NaN
scrubeados, tiempo de procesamiento del callback (**nunca se mide**), carga de CPU, frames
perdidos.

**Criterio de aceptación.**

```c
struct WmaDiagnostics {
    uint64_t callbacks;      uint64_t underruns;
    uint64_t overruns;       uint64_t nonFiniteSamplesScrubbed;
    uint64_t modeChanges;    uint64_t deviceChanges;
    float    lastBlockUsec;  float    peakBlockUsec;
    float    cpuLoadPercent;              // lastBlockUsec / blockDurationUsec
    int      reportedLatencyFrames;       // de WD-3.1
    int      currentSampleRate;  int currentBlockFrames;
};
WMA_API void wma_engine_get_diagnostics(const WmaEngine*, WmaDiagnostics* out);
WMA_API void wma_engine_reset_diagnostics(WmaEngine*);
```

Tres reglas lo hacen RT-safe: los contadores son atómicos `relaxed` incrementados en RT y **nunca
leídos ahí**; el timing usa un reloj monotónico leído dos veces por callback (~40 ns, práctica
estándar); el host **pollea** — el thread RT nunca empuja, nunca llama un callback, nunca toca
una cola que pueda bloquear.

El contador `nonFiniteSamplesScrubbed` es el consumidor de WD-1.1.4 y WD-3.3.2.

**Esfuerzo** 1 sem · **Riesgo** bajo · **Depende de** WD-1.1, WD-3.1

---

### WD-5.2 — Eventos asíncronos y recovery acotado

**Problema.** Dos cosas relacionadas:

1. `BackendErrorCallback` existe adentro de C++ (`IAudioBackend.h:70`) pero no hay
   `wma_engine_set_error_callback`. La pérdida de dispositivo la maneja `BackendManager` puertas
   adentro y el host se entera, si acaso, poleando `AudioState`.
2. `core/AudioEngine.cpp:2222–2238` spawnea un `std::thread` por cada error, con `join()` del
   anterior sobre el thread que entregó el error, una lambda que captura `this` y duerme 500 ms,
   y **sin límite de reintentos**. Si el motor se destruye en esa ventana, `start()` corre sobre
   un objeto muerto. Además `onBackendError` hace `mState.store(EngineState::Stopped)` directo
   (línea 2212), salteando la tabla de transiciones que existe justo para prevenir el bug
   documentado 1.900 líneas antes.

**Criterio de aceptación.**
1. Un callback de eventos registrable por la C API, despachado desde un worker —nunca desde RT—
   con un enum estable: dispositivo perdido, formato cambiado, interrupción, xrun sostenido.
2. Un único worker de recovery de vida larga con condition variable, política de reintentos
   acotada con backoff, y flag de shutdown chequeado después del sleep.
3. `onBackendError` pasa por `transitionToState()`.
4. Test: destruir el motor durante la ventana de recovery no crashea (ASan verde).

**Nota de diseño.** Bajo D1 el motor sigue auto-recuperándose, que es lo correcto para una app.
Si algún día se abre a terceros, lo correcto pasa a ser **no** auto-recuperar y dejar que el host
decida — queda anotado como disparador en §10.

**Esfuerzo** 1 sem · **Riesgo** medio · **Depende de** —

---

## 9. Fases 6 y 7

### WD-6.1 — Separar el artefacto de libusb (D3)

**Problema.** `LICENSE` dice MIT sin calificar. El binario publicado linkea libusb 1.0.27
**estáticamente**, que es LGPL-2.1. El `NOTICE` está bien escrito y de buena fe: reclama
cumplimiento del §6 porque el fuente de libusb está en el repo y el usuario puede relinkear
recompilando. Por qué eso es frágil:

- Las obligaciones del §6 corren hacia **quien recibe el binario**, no hacia quien tiene acceso
  al repo. El artefacto va a GitHub Packages; el repo fuente es privado.
- El §6(a) pide el fuente correspondiente completo **o** los object files linkeables, entregados
  con el binario o por oferta escrita. "Está en un repo que no podés ver" no satisface ninguno.
- El escáner de licencias de un consumidor lee MIT y lo aprueba. El binario que recibe lleva
  obligaciones LGPL. Ese es el modo de falla que produce incidentes de compliance.

Hoy el único consumidor es NoisyPad, privado, mismo dueño: la exposición real es casi cero. Se
arregla ahora porque es barato ahora (D3).

**Criterio de aceptación.**
1. `com.watermellonstudios:audio` (MIT, sin libusb) y `com.watermellonstudios:audio-usb`
   (LGPL-aware) se publican por separado.
2. Los builds de iOS y de host no linkean libusb — hoy lo arrastran sin usarlo.
3. `LICENSE` menciona explícitamente los componentes LGPL del artefacto `-usb`.
4. Un check de CI verifica que cada directorio bajo `thirdparty/` aparece en `NOTICE`.

**Esfuerzo** 3 d · **Riesgo** bajo · **Depende de** —

---

### WD-7.1 — `AudioBufferView` + `AudioFormat`, internos

**Problema.** `Effect::process(float* input, float* output, int numFrames)` lleva el frame count y
nada más. Canales, sample rate, interleaving y formato son invariantes sostenidas en comentarios
y forzadas en ningún lado. `numFrames * 2` aparece en doce lugares de `EffectChain.cpp`. `input`
es no-`const`, así que todo caller hace `const_cast` (`EffectChain.cpp:318`).

Peor: `AudioBuffer` direcciona el **mismo** `std::vector<float>` como planar
(`getWritePointer(channel)` → `data() + channel * mNumFrames`) y como interleaved
(`getInterleavedPointer()` → `data()`). Las dos vistas no pueden ser válidas a la vez y nada marca
cuál está vigente. `copyFromInterleaved` protege con `if (mNumChannels < 2) return;` y después
indexa `i * 2` — un buffer de 4 canales pasa la guarda y lee las muestras equivocadas.

**Por qué entra bajo D1.** Es el único hallazgo cuyo costo crece **con cada efecto y con cada
consumidor**. Hoy son 26 efectos y un consumidor. Es lo más barato que va a ser nunca. Se hace
como **tipo interno**: no se expone en la API pública, no se documenta como capacidad. El objetivo
no es habilitar mono/multicanal/WASM, es **dejar de foreclosarlos** y volver chequeables las
invariantes que hoy son comentarios.

**Criterio de aceptación.**
1. `AudioFormat { sampleRate, channels, layout, sampleFmt }` y
   `AudioBufferView { channels, numChannels, numFrames, strideSamples, format }`, no-owning.
2. Aserciones en build de debug sobre **cada** invariante que hoy es comentario — incluida la
   regla de aliasing de `EffectChain.h:361–366`, que hoy vive como prosa en un método privado.
3. `Effect::process` toma `const` a la entrada. Se borra el `const_cast`. Los efectos que
   necesitan in-place lo declaran.
4. Migración incremental detrás de un adapter: los 26 efectos siguen funcionando durante la
   transición. Los golden de WD-2.2 quedan verdes en cada paso.
5. `AudioBuffer` deja de ofrecer las dos vistas simultáneas.

**Esfuerzo** 3 sem (con la Fase 4 hecha; 5 sin ella) · **Riesgo** medio · **Depende de** WD-2.2, WD-4.1

---

## 10. Lo que queda afuera, y qué lo reabre

Diferir no es descartar. Cada ítem lleva el disparador que lo vuelve a poner en la mesa.

| Diferido | Hallazgo | Disparador |
|---|---|---|
| Split `:wma-instrument` / API pública de terceros | R9 | Existe un segundo consumidor real, o intención comercial concreta de licenciar el motor |
| BCV + `explicitApi()` + api dump | R7 | **El mismo disparador.** Con un solo consumidor el valor es bajo; con dos es obligatorio, y hay que hacerlo *antes* del segundo, no después |
| Documentación de usuario (los 9 documentos) | R18 | Ídem. **Excepción hecha ahora:** corregir el README, que afirma que la librería es sólo Android |
| Refactor del god object `AudioEngine` | R22 | Un cambio de la Fase 3 o 7 resulta impracticable por el acoplamiento. Es un síntoma medible, no una opinión |
| Grafo de audio / `Processor` multi-entrada | §16 | Se necesita un resampler como etapa, AEC, o una topología dinámica en runtime. **El `AudioGraph` anterior se borró por inalcanzable — no reconstruirlo por teoría** |
| Backend de macOS | §7 | Hay una app de escritorio en el horizonte. Costo estimado: 1 semana, y es la prueba más barata de que el seam de plataforma aguanta |
| Builds reproducibles + SBOM | R25 | Distribución a terceros, o requisito de diligencia |
| Path Swift verificado (sample app en CI) | §26 | Un consumidor iOS nativo que no sea KMP. Hoy el XCFramework **no tiene consumidores** y sus pasos de CI no corren en PRs |
| No auto-recuperar y dejar decidir al host | WD-5.2 | Apertura a terceros. Para una app propia, auto-recuperar es lo correcto |

---

## 11. Riesgos del programa

1. **Que este documento envejezca como el de KMP.** `docs/kmp/kmp_requirements.md` tiene 3.300+
   líneas, está stale desde el 30/07, y tiene filas que **contradicen sus propias notas de
   cierre**. La contramedida acá es estructural: este documento **no lleva tabla de estado por
   ítem**. El estado vive en los tests y en el gate. Un WD está cerrado cuando su criterio de
   aceptación es un test verde en `gate.sh`, no cuando una fila dice ✅.

2. **Que las referencias `archivo:línea` envejezcan.** Ya pasó en el doc de KMP. Todas las
   referencias de acá se escribieron contra `c9f3ca9`. **Buscar por nombre de símbolo, no por
   número de línea.**

3. **Que el baseline golden se tome en el momento equivocado.** Es la restricción de orden #1 y es
   silenciosa: si se captura antes de WD-1.2, todos los golden quedan inválidos y el error se
   descubre cuando la Fase 3 falla por razones que no son de la Fase 3.

4. **Que la Fase 3 cambie el sonido sin que nadie lo note.** WD-3.3 cambia el sonido **a
   propósito**. La contramedida es que la re-captura de golden sea una tarea explícita con diff
   revisado, no un `--update-golden` que alguien corre para poner el CI en verde.

5. **Que no haya hardware.** El repo ya lo documenta: hay AVD y simulador de iOS, no hay iPhone ni
   Android físico, el emulador no tiene salida de audio real, y no hay USB. **Nada de la Fase 1 a
   la 7 necesita hardware para ser correcto** — es una propiedad deliberada del recorte. G2 mide
   el beneficio, no la corrección.

6. **Que WD-1.2 se malinterprete como optimización.** Es un cambio de modo de punto flotante y
   cambia resultados. Tratarlo como "una mejora de performance" y mergearlo sin re-capturar el
   baseline rompe la Fase 2 en silencio.

---

## 12. Resumen de esfuerzo

| Fase | Items | Esfuerzo | Necesita hardware |
|---|---|---|---|
| 1 — Thread RT | WD-1.1 … 1.5 | ~2 sem | no |
| G2 — Device | — | 1 sesión | **sí** |
| 2 — Verificabilidad | WD-2.1 … 2.4 | ~6 sem | no |
| 3 — Contrato de `Effect` | WD-3.1 … 3.5 | ~2,5 sem | no |
| 4 — Puentes y errores | WD-4.1 … 4.2 | ~1,5 sem | no |
| 5 — Observabilidad | WD-5.1 … 5.2 | ~2 sem | no |
| 6 — Distribución | WD-6.1 | 3 d | no |
| 7 — `AudioBufferView` | WD-7.1 | ~3 sem | no |
| | | **~17,5 sem** | |

Las fases 4, 5 y 6 son paralelizables entre sí y con la 2. El camino crítico real es
**1 → 2 → 3 → 7**, unas 13,5 semanas.

**Si hay que recortar:** las fases 1, 2 y 3 son el núcleo — arreglan bugs vivos y hacen que el
resto sea demostrable. La 6 son tres días y cierra un tema legal. La 7 es la única cuyo costo
crece si se pospone. Las fases 4 y 5 son las que se pueden diferir sin que nada empeore.
