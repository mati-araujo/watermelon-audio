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

## 7. El diseño, y por qué cada pieza es así

Las seis preguntas abiertas de la sesión de análisis se cerraron el 2026-07-29. Esto es el
resultado, con lo que se descartó y por qué — que es la parte que no se puede reconstruir
leyendo el código.

### 7.1 El reparto: qué se atesta y qué no

`ios`, `build` y `cpp-tests-macos` se atestan. Los tres de ubuntu **nunca**, por §2 y §3.

Y la atestación vale **sólo en `pull_request`**. En `push: master` el CI paga su costo entero,
siempre. Eso resuelve de un plumazo la pregunta 5 —el disparador del camino "release"— porque
todo commit tageable **es** un commit de master, así que ya está verificado de forma
independiente y con el mismo toolchain que después construye el artefacto.

> **Lo que se descartó:** anclar la corrida completa al PR de release-please
> (`head_ref == 'release-please--branches--master'`). Ahorraba más —~68 de 76 minutos-runner
> por PR en vez de ~38— pero apoyaba la única corrida completa del ciclo en el eslabón más
> débil que tiene el repo: **2 de las últimas 5 corridas de esa rama terminaron en
> `action_required`**, esperando una aprobación manual que nadie dio, y el 2026-07-29 el PR
> #92 llegó a tener cero checks reportados sobre su head. Un release gateado por un workflow
> que no reporta el 40% de las veces no es un gate.

> **Hallazgo colateral, todavía abierto:** hoy el publish **no espera al CI**.
> `release-please.yml` corre `on: push: master` y su job `publish` sólo tiene
> `needs: release-please`; `ci.yml` corre sobre el mismo push, en paralelo. Que master
> siempre pague el CI completo hace que el commit publicado esté verificado, pero no hace
> que el publish *espere*. Cerrarlo es trabajo aparte.

### 7.2 Dónde vive la prueba: `.github/local-gate.json`, commiteado

Un archivo en el árbol, dentro del push. **Lo que decide la pregunta 2 no es el squash-merge**
—que deja de importar apenas master no honra atestaciones— sino esto:

> Toda prueba que viva **fuera del contenido pusheado** corre carrera con su propio CI. Un
> comentario de PR, un commit status o un check-run se postean sobre un SHA que ya tiene que
> existir en el remoto: o sea *después* del push, cuando la corrida de CI ya arrancó. El
> verificador tendría que esperar a que aparezca algo que quizás no aparece, que es
> exactamente el modo de falla de §4.1.

Contra un trailer de commit: transportar versiones de toolchain más resultado por gate lo
vuelve ilegible, y un rebase distraído lo pierde (fail-closed, pero silencioso).

El archivo se **excluye de su propio digest** (§4.4). `gate.sh` lo escribe y hace
`git commit --amend`: el amend agrega sólo el archivo excluido, así que el digest recién
calculado sigue siendo correcto.

### 7.3 El conjunto de inputs: no se enumera

La pregunta 3 decía "hay que enumerarlo, no aproximarlo". **Se resolvió no necesitando la
enumeración.** Un solo digest sha256 sobre los **685 archivos trackeados no-prosa** — el
complemento exacto del filtro que ya usa el job `changes`.

El replanteo que lo habilita: **el digest no es una clave de caché, es una prueba de
frescura.** No sirve para reusar resultados entre árboles distintos; sirve para probar "esto
que corrí es exactamente lo que estás por compilar vos". Con eso, sub-cubrir —el único error
que produce falso verde— se vuelve imposible por construcción: no hay lista que mantener ni
archivo que olvidar. Sobre-cubrir cuesta re-correr un gate que ibas a correr igual.

Los hashes salen del **índice de git** (`git ls-files -s`), no de leer el disco: es exactamente
el contenido que el CI va a checkoutear, y no depende de que el runner tenga `sha256sum` o
`shasum`. Implementación única en `scripts/gate-digest.py`, usada por el emisor y por el
verificador — si hubiera dos, el día que diverjan el CI acepta una atestación que no
corresponde y nadie se entera.

**Propiedad que sale gratis:** como `gate-digest.py` y `verify-attestation.sh` están *dentro*
de los 685, cambiar el algoritmo del digest invalida automáticamente toda atestación vieja.
No hace falta versionar el formato a mano.

### 7.4 Los pins de toolchain

`.github/toolchain-pins.json`, match exacto. Sólo cubre **lo que aporta la máquina**: Xcode,
clang, SDK de iOS, runtime del simulador, cmake, ninja y la JVM lanzadora. Kotlin, AGP, Gradle,
la JVM del daemon y el NDK ya viajan en el árbol, así que **ya los cubre el digest**.

Los pins **nunca restringen al CI**: sin atestación válida, el CI corre con el toolchain que
tenga, como siempre. Bumpear el archivo cambia el digest y fuerza una corrida completa, que es
el comportamiento correcto ante un cambio de toolchain.

### 7.5 El verificador vive DENTRO de cada job

Los jobs `ios`, `build` y `cpp-tests-macos` conservan su nombre y ganan un paso justo después
del checkout. Si valida, los pasos siguientes se saltean por `if:` y el job termina en
**`success`** en segundos. **Cero cambios en la protección de rama**, así que la trampa de
§4.2 no aplica.

> **Lo que se descartó:** un job `attest` separado que saltee a los tres. Ahorraba la
> asignación del runner de macOS, pero pone la lógica fail-closed en expresiones `if:` con
> `always()` y `needs.*.result` — y **un fallo del job verificador saltearía los tres jobs**,
> que es un falso verde. Adentro del job, cualquier cosa inesperada hace que el gate CORRA, y
> si el código está mal el gate lo pone rojo. El rojo lo pone el trabajo real, no el
> verificador.

El contrato de `verify-attestation.sh` es corto y no se negocia: **siempre sale 0**, y emite
`valid=true` únicamente si todo cerró. Hay **una sola escritura** del output, hecha por un trap
de EXIT — deliberadamente no se escribe `false` al principio y `true` al final, porque eso
dependería de que Actions se quede con la última escritura de una misma clave, y todo el
esquema cuelga de ese valor.

### 7.6 `strict: true` se apagó

Con un digest de contenido, cada movimiento de master mataba la atestación: el "Update branch"
cambia el árbol. Mirando el ritmo real de merges de este repo —varios PRs por hora— el camino
rápido habría servido sólo cuando master está quieto.

Lo que `strict` protegía —el conflicto semántico entre dos PRs— lo caza igual el `push: master`,
que ahora corre el CI completo siempre. Queda un reparto limpio: **PR = rápido con prueba
local, master = verificación independiente completa**.

### 7.7 El modelo de confianza, sin maquillaje

Sin criptografía. La amenaza real no es un atacante: es que el archivo lo escriba algo que no
corrió el gate —vos apurado, o una sesión de agente "resolviendo" un CI rojo regenerando el
JSON—. **Contra eso un HMAC no sirve**: `gate.sh` necesita la clave en el entorno para firmar,
así que cualquier cosa que pueda correr `gate.sh` puede firmar sin correrlo.

Lo que sí hace el trabajo: (1) el digest caza el accidente, que es el 99% de los casos reales;
(2) la regla escrita en `CLAUDE.md` —el archivo lo escribe sólo `gate.sh`, a mano es fraude—
que es lo que leen los agentes; (3) `push: master` lo destapa un merge después.

### 7.8 El gate local: qué corre y qué no

`gate.sh` corre exactamente lo que el CI va a saltear, más los dos guardrails que cuestan
segundos, en orden fail-fast. Los sanitizers quedan **opt-in** (`--with-sanitizers`): nunca
están en el camino crítico, el TSan local tarda 865 s contra 295 s del CI **y es más débil**.

Dos requisitos duros, los dos aprendidos midiendo:

1. **`bash scripts/build-ios.sh` va suelto y ANTES de cualquier task de Gradle.** Parece
   redundante y no lo es: los inputs declarados de `buildIosNativeLib` eran sólo el árbol de
   C++, así que un cambio en `build-ios.sh` sin cambios en C++ dejaba la task `UP-TO-DATE` con
   el `.a` viejo. Se agregó el script a los inputs declarados para que la garantía no dependa
   del orden de pasos de nadie.
2. **Higiene de simulador y timeouts sobre `simctl`.** El 2026-07-29, en esta máquina, tres
   operaciones de `simctl` se colgaron seguidas sobre un simulador ya booteado: `bootstatus`
   8 min, `terminate` 37 min, `install` 46 min. **Un runner del CI nunca lo sufre porque
   arranca limpio siempre; una máquina de desarrollo acumula estado entre sesiones.** Tras
   `simctl shutdown all` + matar el `CoreSimulatorService`, el boot entero tarda **15,8 s** —
   contra los ~3m30s del runner.

Los timeouts **no** se calibraron con medianas por paso, a propósito: un timeout es un detector
de cuelgues, no un presupuesto de performance, y calibrarlo sobre una sola medición invita
falsos rojos (§5). El riesgo está concentrado en `simctl` —ninja, gradle, xcodebuild y clang
terminan o fallan—, así que van 90 s ahí, un techo global de 45 min, y un **heartbeat cada
30 s**: lo que dolió no fue la duración, fue el silencio.

> **Y hay un timeout POR PASO además del de `simctl`, que no es redundante.** La primera
> corrida de `gate.sh` se colgó **29 minutos en `simctl launch`, con el timeout de 90 s puesto
> y sin ningún efecto**: el wrapper sólo alcanza las llamadas que hace `gate.sh`, y
> `build-harness.sh` hace sus propios `terminate`/`install`/`launch` por dentro. Envolver el
> *paso* entero es lo único que cubre un `simctl` metido en otro script. El paso que se pasa
> del techo dispara un reset de CoreSimulator y **un** reintento; si vuelve a colgarse, el gate
> es rojo y el CI corre entero.
>
> La lección general: **una guardia que no se probó contra el modo de falla real cubre lo que
> el autor imaginó, no lo que pasa.** El diseño decía "timeouts sobre simctl" y era correcto;
> la implementación cubría la mitad, y la mitad que faltaba era exactamente donde ya había
> fallado el día anterior.

### 7.9 Cómo se probó que no miente

**El experimento manual**, sobre el propio PR (los PRs del mismo repo corren `ci.yml` en la
versión de su rama, así que no hay problema de bootstrap): un positivo y cuatro negativos
—byte mutado, atestación borrada, digest corrompido, pin bumpeado—, y los cuatro tienen que
terminar con los tres jobs corriendo el gate entero.

**Y `scripts/test-attestation.sh`**, que es lo que sigue vivo después: arma 11 árboles mutados
en un clon descartable y afirma que el verificador los rechaza. Vive en el job `cpp-tests` de
ubuntu —el único que nunca se atesta— porque si viviera en un job atestable se saltearía justo
cuando el camino rápido está activo.

> **El test afirma el MOTIVO del rechazo, no sólo el rechazo**, y no es paranoia: la primera
> versión tenía tres casos que pasaban **por el motivo equivocado** —rechazaban por digest en
> vez de por la rama que querían probar— porque el propio harness dejaba archivos dentro del
> árbol verificado. Un test que verifica un fail-closed puede pasar por accidente con una
> facilidad incómoda.

### 7.10 Qué NO cambió, y una predicción que salió mal

Se midió si el gate local podía dar verde apoyado en trabajo viejo de Gradle (§4.5). **No hay
hueco.** Con un mutante que cambia el binario —un símbolo exportado nuevo— el `.a` cambia de
hash y la cascada se dispara entera: 7 de 12 tasks re-ejecutadas, `iosSimulatorArm64Test` pasa
de 2,4 s (`UP-TO-DATE`) a **58,6 s**, y el XCFramework se reconstruye trayendo el símbolo.

La predicción que salió mal fue mía: supuse que el peligro era que `scripts/build-ios.sh` no
estuviera entre los inputs declarados. **El mecanismo que da la garantía es otro** — que el
`.a` es determinista (`libtool -D`) y que `cinterop` declara su **contenido** como input
(`KmpNativeConventionPlugin.kt`, el fix que ya se había hecho una vez). Dos mutantes que no
alteran el código generado —un comentario, un `-D` sin usar— dejaron el `.a` byte a byte
idéntico y correctamente no invalidaron nada.

**Un comentario no es un mutante.** Si el mutante no cambia el binario, el experimento no
prueba nada sobre staleness: prueba que el pipeline es correcto respecto del contenido, que es
otra cosa.

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
