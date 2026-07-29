# CI local-first — análisis y restricciones de diseño

**Fecha:** 2026-07-29 · **`master`** = `374ea14` · **Estado:** análisis cerrado, diseño **sin
empezar**.

Este documento existe para que la sesión que implemente el esquema local-first arranque con
números medidos en vez de intuiciones. **Todo lo que dice está medido en este repo, en este
mes**; donde hay estimación, lo dice.

---

## 1. El costo actual, medido

Distribución de duración por job, sobre **las últimas 18 corridas** de `ci.yml` (n por job
entre 6 y 16, sólo jobs en verde):

| job | runner | n | mín | **mediana** | máx |
|---|---|---:|---:|---:|---:|
| `ios` | macOS | 14 | 735 s | **1048 s** | 1315 s |
| `build` | ubuntu | 14 | 276 s | **368 s** | 395 s |
| `cpp-tests-tsan` | ubuntu | 13 | 259 s | **295 s** | 315 s |
| `cpp-tests-asan` | ubuntu | 14 | 170 s | **238 s** | 254 s |
| `cpp-tests-macos` | macOS | 6 | 137 s | **165 s** | 194 s |
| `cpp-tests` | ubuntu | 14 | 113 s | **162 s** | 172 s |
| `changes` | ubuntu | 16 | 3 s | **6 s** | 7 s |

- **Camino crítico por PR ≈ el job `ios` ≈ 17,5 min** (mediana), con un rango de casi 10
  minutos entre la mejor y la peor corrida.
- **Minutos-runner por PR ≈ 38 min**, de los cuales **~20 min son de macOS**.

> [!CAUTION]
> **El rango del job `ios` (735–1315 s) es el dato más importante de esta tabla, y no es
> ruido de medición: es ruido real.** El mismo workflow, sobre el mismo commit, midió 759 s y
> 1143 s. Cualquier optimización que prometa menos de ~400 s de mejora **no se puede
> distinguir de cero con una sola corrida**. Ver §5.

## 2. Qué es reproducible localmente y qué no

El hardware local es un **Apple M2 Pro, 10 núcleos (6P + 4E), 16 GB**, con Xcode 26.6 y
simulador. El runner de macOS de GitHub tiene **3 núcleos**.

| job | ¿local? | por qué |
|---|---|---|
| `ios` | **Sí, entero** | todos sus pasos son Xcode + Gradle + simulador, que la máquina tiene |
| `cpp-tests-macos` | **Sí** | mismo Apple clang, misma libc++ |
| `build` | **Sí** | mismo SDK/NDK de Android, mismas 4 ABIs |
| `cpp-tests` | **No** | usa **g++/libstdc++**; local es Apple clang. Distintos diagnósticos |
| `cpp-tests-asan` | **No fielmente** | `detect_leaks=1` **no existe en macOS** y rompe el discovery de gtest (ya documentado en `CLAUDE.md`) |
| `cpp-tests-tsan` | **NO, y está probado** | ver el recuadro |

> [!CAUTION]
> **El TSan local es demostrablemente más débil que el del CI, y hay dos mediciones
> independientes.**
>
> 1. Sesión del 2026-07-28: una carrera real sobrevivió **15 corridas locales** y fue roja a
>    la primera en el CI.
> 2. Sesión del 2026-07-29: la carrera de `RoundTripMeasurer::poll()` —que el TSan del CI
>    encontró— dio **0 detecciones en 60 corridas locales** del test que la dispara.
>
> **El TSan de Linux/libstdc++ es la única autoridad para carreras.** Ninguna atestación
> local puede sustituirlo, y esto no es una cuestión de configuración: es el sanitizer.

**El reparto que sale de ahí, y es el que ordena todo el diseño:**

```
locally reproducible:   ios (1048s) + cpp-tests-macos (165s) + build (368s)  =  1581s   (69%)
Linux-only:             cpp-tests (162s) + asan (238s) + tsan (295s)         =   695s   (30%)
```

> **Lo caro es lo que la máquina local hace mejor. Lo irreemplazable es barato y corre en
> paralelo.** Los tres jobs de ubuntu suman 695 s de minutos-runner, corren en paralelo entre
> sí (máx. 295 s de reloj) y **nunca están en el camino crítico**.

## 3. Dónde estuvieron los defectos que el CI encontró

Esto es lo que hay que preservar, y conviene mirarlo antes de mover nada:

| defecto | lo encontró | ¿lo habría visto un gate local? |
|---|---|---|
| UAF en el retiro de SoundFonts (07-28) | **TSan del CI** | **No** — ASan local no ve carreras, TSan local tampoco lo vio |
| Carrera en `RoundTripMeasurer::poll()` (07-29) | **TSan del CI** | **No** — 0/60 local |
| Carrera del condvar en el fake (07-27) | **TSan del CI** | **No** |
| Overflow con signo en `LooperExporter` (07-27) | **UBSan del CI** | Probablemente sí (UBSan sí corre en macOS) |
| `run-cpp-tests.sh` roto en bash 3.2 (WA-0.3, primera corrida) | job de macOS | **Sí** — es un bug *de macOS*, la máquina local es macOS |

**Tres de las cinco capturas registradas vienen del TSan de Linux.** Ese job no se toca.

## 4. La forma del esquema, y su propiedad no negociable

La idea es: correr el gate localmente sobre el árbol final, dejar una **prueba verificable**
de qué se corrió sobre qué contenido, y que el CI **verifique la prueba** en vez de repetir el
trabajo.

**La propiedad que hace que esto sea seguro y no un bypass:**

> La atestación es un **camino rápido, no una excepción**. Si el digest no coincide, si falta,
> si las versiones de herramientas no matchean, o ante cualquier duda → **el gate corre
> igual**. Nunca se saltea por ambigüedad.

Es el mismo criterio que ya tiene el job `changes` (*"ante cualquier duda sobre el rango se
corre TODO; el default seguro es gastar de más, nunca saltear un gate"*), y por la misma razón.

**Restricciones que ya sabemos, y que costaron caro aprender:**

1. **El job que verifica tiene que REPORTAR siempre.** La protección de rama tiene 7 checks
   requeridos. Un job que no reporta deja el check `pending` para siempre y **bloquea el merge
   sin remedio** — es exactamente lo que pasó con `paths-ignore` y por lo que existe el job
   `changes`. El verificador debe ser el check requerido, y terminar en verde tanto si validó
   la atestación como si corrió el gate entero.
2. **Agregar un check requerido para un job que `master` todavía no tiene bloquea a todos los
   PRs abiertos, y el rebase NO lo destraba.** Medido el 2026-07-29. Primero mergear el PR que
   agrega el job, después tocar la protección.
3. **El digest tiene que cubrir las versiones de herramientas, no sólo los archivos.** Un
   verde con Xcode 26.6 no dice nada sobre Xcode 27. Mínimo a fijar: Xcode/clang, NDK, CMake,
   Kotlin/AGP, y la runtime del simulador.
4. **La atestación tiene que excluirse de su propio digest.**
5. **`UP-TO-DATE` deja de ser una trampa y pasa a ser el mecanismo.** Hoy la advertencia es
   *"las tasks de Gradle dan verde UP-TO-DATE sin correr nada"*. Con un digest de contenido eso
   se invierte: si los inputs no cambiaron, el resultado anterior **es** válido. Lo que hay que
   garantizar es que el digest cubra de verdad todo lo que puede cambiar el resultado.

**Lo que se está cambiando, dicho sin maquillaje:** para la superficie macOS/Android, el CI
deja de ser una verificación independiente y pasa a ser un **registrador de la verificación
local**. El modelo de confianza es "confío en mi máquina". Para un repo de un solo
desarrollador es razonable, pero hay que escribirlo, no dejarlo implícito. Los tres jobs de
Linux siguen siendo independientes de verdad.

## 5. El método, que es la mitad del trabajo

> [!CAUTION]
> **En la sesión del 2026-07-29 fallé tres predicciones seguidas de mejora del CI**, todas
> apoyadas en una medición por paso que era correcta (11 corridas, dos series que no se
> solapan). El efecto por paso era real y **no predecía nada del total**, por dos razones que
> no había verificado: el costo se mudaba al paso siguiente en vez de desaparecer, y el piso
> de ruido del job era gigante.
>
> **Antes de prometer cualquier mejora de un total agregado: medir cuánto varía ese total sin
> tocar nada.** Dos o tres corridas del mismo commit. Si la mejora esperada no supera el rango
> observado, no es una mejora: es una hipótesis.

Corolario que sí funcionó: el único cambio de CI que sobrevivió esa sesión (`cpp-tests-macos`)
se justificó por **latencia de feedback** —un número que se mide solo, 137–194 s contra
328–371 s— y no por camino crítico.

## 6. Lo que el simulador cuesta, porque condiciona el gate local

Tener un simulador de iOS **vivo** en un runner de 3 núcleos le cuesta ~200 s al resto del
job. **No es el arranque: es tenerlo corriendo.** Sus daemons siguen comiendo CPU varios
minutos después de que `simctl bootstatus` ya devolvió.

Se probaron cuatro ubicaciones del boot y las cuatro pagaron el peaje; sólo cambiaba qué paso
lo pagaba:

| vecino del boot | ese paso | job `ios` |
|---|---|---|
| suite C++ | 112 → 328 s | 759 s |
| XCFramework | 105 → 359 s | 750 s |
| en serie al final | harness 127 → 325 s | 1030 s |
| `build-ios.sh` | 36 → 274 s | 735 s / 1087 s |

La medición que rompe la teoría del "arranque": `build-ios.sh` costó 274 s **con el boot ya
terminado y esperado antes de arrancar**.

**Por qué importa para local-first:** en la máquina local hay 10 núcleos en vez de 3, así que
el mismo simulador cuesta proporcionalmente mucho menos. Es una de las razones por las que el
gate local es más rápido que el job que reemplaza, y no sólo por el hardware bruto.

## 7. Preguntas abiertas para la sesión de diseño

1. **¿Qué gates se atestan y cuáles corren siempre?** La propuesta que sale de §2 y §3 es:
   atestar `ios`, `cpp-tests-macos` y `build`; **nunca** atestar los tres de ubuntu.
2. **¿Dónde vive la prueba?** Archivo commiteado en el PR, comentario del PR vía `gh`, trailer
   del commit, o check-run posteado por API. Cada uno tiene un modo de falla distinto frente
   al squash-merge, que este repo usa siempre.
3. **¿Cuál es exactamente el conjunto de inputs de cada gate?** Sin eso el digest o cubre de
   menos (falso verde) o de más (nunca acierta). Hay que enumerarlo, no aproximarlo.
4. **¿Cómo se prueba que el verificador no miente?** Mutar un byte de un archivo cubierto y
   confirmar que el CI **rechaza la atestación y corre el gate**. Sin ese experimento el
   esquema no está verificado, sólo escrito.
5. **¿Qué pasa antes de un release?** El usuario ya fijó el criterio: el CI paga su costo
   entero ahí. Falta decidir el disparador (¿el PR de release-please? ¿un tag? ¿una etiqueta?)
   y que ese camino **ignore toda atestación**.
6. **¿Un solo `scripts/gate.sh`?** Hoy son 12 comandos que hay que recordar y correr en orden.
   Un único entry point que corra todo, falle rápido y emita la atestación es prerequisito
   práctico de todo lo demás.

---

## Apéndice — el gate local, cronometrado

Medido el 2026-07-29 en el M2 Pro, **pasada incremental** (árbol ya construido, que es el caso
real al preparar un PR). Los dos runs de sanitizer siempre reconstruyen desde cero porque
`run-cpp-tests.sh` hace `rm -rf` de `build-san`.

| comando | local | job de CI equivalente | CI (mediana) |
|---|---:|---|---:|
| `check-cpp-portability.sh` | 2 s | (paso de `cpp-tests`) | — |
| `c-api-gap.py` | 0 s | — (no está en CI) | — |
| `check-no-ui-in-library.sh` | 7 s | (paso de `build`) | — |
| `run-cpp-tests.sh` (779) | **81 s** | `cpp-tests-macos` | 165 s |
| `build-ios.sh` (2 slices) | **8 s** | (paso de `ios`) | 274–343 s |
| `gradle testDebugUnitTest` | 3 s | (paso de `build`) | — |
| `gradle assembleDebug` | **13 s** | (paso de `build`) | ~132 s |
| `gradle assembleRelease` | **16 s** | (paso de `build`) | ~145 s |
| `gradle compileIosMainKotlinMetadata` | 14 s | — (no está en CI) | — |
| `gradle iosSimulatorArm64Test` | 62 s | (paso de `ios`) | 13–69 s |
| `gradle assembleWatermelonReleaseXCFramework` | 117 s | (paso de `ios`) | 126–359 s |
| `build-harness.sh` (Android + iOS + arranque) | **56 s** | (partido entre `ios` y `build`) | 90–325 s |
| `run-cpp-tests.sh` **ASan+UBSan** | 288 s | `cpp-tests-asan` | 238 s |
| `run-cpp-tests.sh` **TSan** | **865 s** | `cpp-tests-tsan` | **295 s** |
| **total serial** | **1532 s (25,5 min)** | | |

**Los dos números que hay que sacar de esta tabla:**

1. **El set atestable local —todo menos los tres gates de Linux— corre en 379 s (6,3 min)**, y
   reemplaza los **1048 s de camino crítico** del job `ios` más los 368 s de `build`. El caso
   extremo es `build-ios.sh`: **8 s en local contra 274–343 s en el job `ios`**, y no es sólo
   hardware — es el simulador vivo de §6.

2. **El TSan local tarda 865 s: casi 3× lo que tarda en el CI (295 s), y encima es más débil.**
   Eso cierra la discusión de mover sanitizers a local por partida doble. Y explica algo del
   gate actual: **los dos runs de sanitizer son 1153 s de los 1532 s (75 %)**. O sea que el
   gate local, al delegar los sanitizers al CI —donde son más rápidos y más fuertes—, **pasa de
   25,5 min a 6,3 min**. El gate local se acelera moviendo trabajo *hacia* el CI, no al revés.

**El esquema que sale de los números, para dimensionar:**

```
hoy:     camino crítico = job `ios` ≈ 1048 s  (17,5 min de espera después de pushear)
después: local 379 s (6,3 min, antes de pushear)  +  CI = los 3 de ubuntu en paralelo ≈ 295 s
```

> [!WARNING]
> **Una imprecisión de esta misma medición, que es justo el problema que hay que resolver
> bien.** El detector de "esto dio `UP-TO-DATE` sin correr nada" que usé mira la salida de
> ninja y marcó `compileIosMainKotlinMetadata` como *sin trabajo* cuando Gradle sí trabajó
> 14 s. **La atestación depende exactamente de esa distinción**: si el mecanismo no puede
> decir con certeza si un gate corrió de verdad sobre el contenido actual, la prueba no vale.
> Un digest de contenido resuelve esto mejor que cualquier heurística sobre logs — pero hay
> que enumerar los inputs, no adivinarlos (pregunta abierta 3 de §7).

**Condiciones de la medición:** Apple M2 Pro (10 núcleos: 6P + 4E), 16 GB, Xcode 26.6,
simulador iPhone 17 / iOS 26.4 ya booteado, árbol ya construido (pasada incremental, que es el
caso real al preparar un PR). Los dos runs de sanitizer son la excepción: `run-cpp-tests.sh`
hace `rm -rf` de `build-san`, así que **siempre** reconstruyen desde cero.
