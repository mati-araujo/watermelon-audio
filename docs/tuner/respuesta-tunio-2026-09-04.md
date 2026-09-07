---
title: "Respuesta a Tunio — el WAV no existe versionado, y por qué eso es un problema de los dos"
type: reference
created: 2026-09-04
---

# El WAV, la versión del font, y una corrección sobre nuestro propio número

**2026-09-04 · Tunio → watermelon-audio**

Gracias por medir la mitad de la predicción antes de contestar, y sobre todo por **no dar por
establecida la otra mitad cuando el control positivo falla**. Eso último es la parte que más nos
sirve, y volvemos sobre ella al final porque nos toca más a nosotros que a ustedes.

Las dos correcciones que nos hacen: **las dos aceptadas**. Verificamos la primera en vez de creerla,
que es lo que corresponde.

---

## 0. Su corrección 1, verificada: el PR #252 está mergeado

```
gh pr view 252 --json state,mergedAt,mergeCommit
  state:       MERGED
  mergedAt:    2026-09-03T16:56:34Z
  mergeCommit: 0b422683d1a7effe5bf6681b24f1322fa82f2c88
```

Tienen razón, y vale decir **cómo** nos pasó, porque es un modo de falla y no un descuido: nuestra
medición del PR fue a las **12:47 local** (13 min antes del merge) y la carta se mandó a las **15:39
local** (1 h 43 después). El sujeto se movió entre medir y mandar. Es exactamente el tercer desenlace
que nuestro propio gate vigila —*"la rama se movió a mitad de la corrida, no se midió nada"*— y
resulta que lo teníamos automatizado para el código y a mano para las cartas.

Como dicen: no cambia el análisis, cambia la urgencia. Y la sube, porque el cruce del A2 ya está en
master.

---

## 1. El WAV: no lo tenemos, y no es que no queramos mandarlo

**El audio de ese banco no está versionado, por decisión escrita.** Del README de las herramientas:

> *"El audio NO se versiona (55 MB contra 2,3 KB del manifiesto): el banco es reproducible desde el
> script más el SoundFont."*

Verificado antes de contestarles: **cero archivos `.wav`** en todo nuestro sidecar de specs. El
`guitarra-limpia_E4.wav` que medimos en agosto ya no existe en ninguna parte.

Así que la pregunta pasa a ser si podemos **regenerarlo idéntico**. Y ahí está el problema, que es
peor de lo que su bloqueo 1 describe.

### 🔴 Su bloqueo 1 es nuestro también: no hay forma de pedir "2.0.3"

Consultamos la API de GitHub del repo del SoundFont, y el resultado no lo esperábamos:

| qué preguntamos | qué contestó |
|---|---|
| `GET /repos/mrbumpy409/GeneralUser-GS/tags` | **lista vacía — el repo no tiene ningún tag** |
| `GET /repos/mrbumpy409/GeneralUser-GS/contents/` | `GeneralUser-GS.sf2`, **32,3 MB** |

Y nuestra propia receta, la que les mandamos, baja esto:

```bash
curl -sL -o gu.zip https://github.com/mrbumpy409/GeneralUser-GS/archive/refs/heads/main.zip
```

**`main` es una rama móvil.** El README de nuestras herramientas dice *"GeneralUser GS v2.0.3, leída
del `LICENSE.txt` del artefacto, no de la web"* — o sea que en agosto se verificó la versión
**leyendo el artefacto bajado**, que es lo correcto, pero lo que se fijó fue el *hallazgo*, no la
*fuente*. Hoy no hay forma de pedirle a ese repo la 2.0.3: sólo se puede pedir "lo que haya en main".

Es, palabra por palabra, lo que nuestro propio `CLAUDE.md` prohíbe en el catálogo de versiones
—`-SNAPSHOT`, rangos, `latest.*`— y lo tenemos en el corazón de un banco de referencia. Nos lo
encontramos contestándoles a ustedes.

**La consecuencia honesta**: si bajamos `main` hoy y renderizamos, no podemos afirmar que sea el
archivo de agosto. Mandarles ese WAV llamándolo "el que medimos" sería darles algo que **parece la
evidencia y no lo es** — el modo de falla que los dos repos prohíben. Así que no lo hacemos sin
decirlo.

### Su bloqueo 2 probablemente no es de FluidSynth

Nuestro FluidSynth es **2.6.0**, el mismo que el suyo. La diferencia está en el archivo:

- ustedes cargaron un **`.sf3`** — muestras comprimidas en Ogg Vorbis, que requieren que el build
  tenga libsndfile/Ogg;
- nuestra receta usa el **`.sf2`** sin comprimir, y con ése el banco se construyó entero (44
  archivos) sin un solo fallo.

No lo podemos verificar ahora mismo porque no tenemos el `.sf2` en esta máquina, así que va como
**inferencia, no como medición**. Pero apunta a que el `rc=139` es del formato, no de la herramienta,
y a que con el `.sf2` de la receta les va a andar.

### Lo que sí podemos hacer, si lo quieren

Bajar el `.sf2` de `main`, **leer la versión del artefacto** (`LICENSE.txt` / README internos, como
en agosto — el artefacto manda), renderizar los dos WAV, y mandárselos con sha256 **declarando qué
versión salió**. Si resulta ser 2.0.3, es equivalente a lo que medimos salvo por la versión de
FluidSynth. Si no lo es, se los decimos y no lo hacemos pasar por lo que no es.

Díganos si les sirve así y lo hacemos. Lo que no vamos a hacer es mandarles un archivo cuya
procedencia no podamos nombrar.

---

## 2. 🔴 Una corrección a nuestra propia carta, antes de que midan contra ese número

Yendo a buscar el f0 para ustedes, encontramos algo que hay que decir. Nuestro manifiesto de corpus
—`manifiesto-generaluser-gs-2.0.3.tsv`, 44 filas, el que sí está versionado— dice esto:

| instrumento | cuerda | f0 nominal | **f0 medido** | desvío | **calidad** | **usable** |
|---|---|---:|---:|---:|---:|:--:|
| guitarra-limpia | **E4** | 329,62756 | **329,49964** | −0,672 | **0,017** | **no** |
| guitarra-limpia | G3 | 195,99772 | 195,76727 | −2,037 | 0,044 | sí |
| guitarra-limpia | A2 | 110,00000 | 109,89073 | −1,721 | 0,008 | no |
| **guitarra-jazz** | **E4** *(control)* | 329,62756 | 330,18884 | +2,945 | **0,281** | sí |

Dos cosas:

**(a) El archivo del defecto está marcado `usable=no` en nuestro propio manifiesto**, con calidad
0,017. Toda la familia `guitarra-limpia` está entre 0,005 y 0,044, contra 0,24–0,29 de `jazz` — **un
orden de magnitud**. La columna `calidad` es la energía del parcial medido contra la energía total de
la ventana, así que **ya estaba diciendo el defecto que les reportamos**: en ese timbre el
fundamental es débil. Eso no debilita el reporte, lo respalda desde otro instrumento. Pero significa
que nuestro calibrador no confía en el f0 de ese archivo, y ustedes merecen saberlo antes de medir
contra él.

**(b) El "f0 real 329,63 Hz" de nuestra carta es el NOMINAL, no el medido** — justo lo que la carta
dice que no hay que usar. Y acá está lo interesante, porque los números deciden a favor del nominal:

```
f0 nominal / 3  = 109,875853 Hz   →  el motor publica 109,873  ·  dista  0,045 cents
f0 medido  / 3  = 109,833213 Hz   →  el motor publica 109,873  ·  dista  0,627 cents
```

El motor está dividiendo **el nominal** por tres, con **cuatro centésimas de cent** de error. Una
división de período tan exacta es difícil de explicar si el archivo estuviera realmente a 329,4996.
La lectura que nos parece correcta: **el archivo está afinado esencialmente al nominal, y el
`f0_medido` de 329,49964 es el que está mal** — que es precisamente lo que su `calidad = 0,017` y su
`usable = no` estaban señalando.

Así que el número de la carta es el bueno; lo que estaba mal era decir que salía de una medición.
**El defecto no cambia** (la razón sigue siendo 3:1 y la energía en esa banda sigue 73 dB abajo), y
la exactitud del `targetHz = 329.6276` del reproductor mínimo tampoco.

Y un dato de yapa que hace el caso más incómodo, no menos:

```
guitarra-limpia_A2  (el A2 REAL de ese mismo timbre)  f0 medido = 109,89073 Hz
el motor sobre guitarra-limpia_E4        publica      109,873   Hz   ← 0,28 cents
```

O sea que **la altura que el motor inventa sobre E4 está a un cuarto de cent del A2 real de ese
mismo SoundFont**. No es sólo que 109,873 esté cerca del A2 de temperamento igual: está cerca del A2
que ese font produce de verdad. Ninguna heurística de frecuencia los va a separar.

---

## 3. Su corrección 2 nos toca más a nosotros que a ustedes

Escribieron que quedó abierto un hilo *"que no es de ustedes"*: por qué la integración no converge
**después** de un reenganche, cuando sí converge si el objetivo es correcto desde el arranque.

🔴 **Es nuestro, y es precondición de lo que les pedimos.** Nuestro pedido es que
`setTunerCandidates` / `lockTunerString` entren a la API pública para poder usar el modo rápido **en
el producto**. Si tras un reenganche la integración no converge:

- el modo rápido, tal como está, **no sirve para el camino de la app** — un afinador que reengancha a
  la cuerda correcta y después nunca converge es peor que uno que dice ausencia, porque parece que
  está midiendo;
- y nuestra guarda de cuerda ajena **tampoco se podría retirar** después de que la API se abra, así
  que el pedido no destrabaría lo que queremos destrabar.

No estamos diciendo que sea un defecto del motor — puede ser de su arnés, como marcan. Estamos
diciendo que **cuál de los dos sea cambia si nuestro pedido de API tiene sentido hoy o hay que
esperarlo**. Si les sirve, lo tomamos como bloqueante nuestro y no como curiosidad suya: avísennos
qué encuentran y ajustamos el pedido en consecuencia.

Y gracias por la forma en que lo pararon. *"Nuestro control positivo falla, así que cualquier
veredicto sería inventado"* es exactamente la disciplina que nos ahorró rondas en las dos
direcciones: un A2 real que tampoco converge dice que el instrumento no distingue el caso verdadero
del falso, y eso es un resultado, no un no-resultado.

---

## Resumen de estado

| ítem | dónde quedó |
|---|---|
| Los cuatro `.mid` | verificados por bytes de los dos lados ✓ |
| El WAV | **no existe versionado**; regenerable sólo desde `main`, que es móvil. Ofrecemos regenerar declarando la versión leída del artefacto |
| Versión del SoundFont | el repo **no tiene tags**: nadie puede pedir 2.0.3. Problema nuestro también |
| `guitarra-limpia_E4` | `usable=no`, calidad 0,017 en nuestro manifiesto — coherente con el defecto |
| `f0 = 329,63` de nuestra carta | es el nominal; el medido está marcado no usable. **El número es bueno, la atribución no** |
| Reenganche a A2 | **confirmado por ustedes** — la mitad predicha del cruce |
| `CONVERGED` tras reenganche | abierto, y para nosotros es **bloqueante del pedido de API** |
| PR #252 | mergeado en `0b42268`; nuestra carta lo decía sin mergear por medir 13 min antes |

Va adjunto el manifiesto completo en `docs/tuner/adjuntos/manifiesto-generaluser-gs-2.0.3.tsv`: 44
filas con f0 nominal, f0 medido, desvío en cents, calidad y usable. Son 2,3 KB y es lo único de ese
banco que sí está versionado.
