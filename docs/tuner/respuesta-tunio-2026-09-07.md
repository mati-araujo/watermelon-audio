---
title: "Respuesta a Tunio — su predicción está confirmada, y no hizo falta el corpus"
type: reference
status: current
created: 2026-09-07
---

# Su predicción quedó confirmada, con su número

**2026-09-07 · sobre el PR #255 (mergeado) · watermelon-audio → Tunio**

Tres cosas: tienen razón en la crítica a nuestro test, **reprodujimos el falso positivo entero sin
el corpus**, y verificamos sus tres afirmaciones sobre el SoundFont una por una.

---

## 1. Tienen razón sobre AC-030.1, y es la crítica correcta

```cpp
const auto reenganchado = analizar(cuerda(kA2), kE4, guitarraHz());   // suena un A2 REAL
```

Exacto: ese test prueba que el reenganche **a la cuerda correcta** converge. No dice nada del
reenganche a la equivocada. Nosotros mismos habíamos escrito el 04/09 que el falso positivo pasaba
de tapado a alcanzable, y **no lo convertimos en test** — que es toda la diferencia entre saberlo y
tenerlo vigilado. Lo encontró un lector externo del PR, no nosotros.

**El límite del KDoc también es correcto.** `OfflineAnalysis.h:69` dice *"🔴 Con candidatos ese caso
NO EXISTE, y está medido"*. Cierto para una cuerda ajena; falso para una cuerda correcta leída en un
subarmónico, donde el caso no desaparece sino que empeora. Es una afirmación medida para un caso y
escrita sin condición — la misma clase contra la que ustedes ya habían diseñado una vez. Queda como
delta **R-API-49 (MODIFIED)** en nuestra spec viva.

---

## 2. 🔴 Su predicción está CONFIRMADA — y el control negativo no necesita su archivo

Fuimos a escribir *"bloqueado por el corpus"*, porque es lo que nuestro propio REQ-027 había medido:
*"el error de período no se reproduce con síntesis"*. **Lo medimos antes de afirmarlo y salió al
revés.**

### Su espectro promedio, sintetizado, NO falla

Empezamos por lo obvio: sintetizar el perfil de dB que ustedes midieron sobre `guitarra-limpia_E4`
(f0 a −4,8 · 2f0 a −1,8 · 3f0 a 0,0). Señal de 5 s a 44,1 kHz, objetivo E4, sus seis cuerdas
declaradas:

```
H3 dom, 5 parciales      CONVERGED  hz=329.619  razon f0/hz=1.000   <- NO falla
H3 dom + inarmonicidad   CONVERGED  hz=330.322  razon=0.998         <- NO falla
f0 MUY debil (-20 dB)    CONVERGED  hz=329.620  razon=1.000         <- NO falla
f0 AUSENTE               CONVERGED  hz=329.620  razon=1.000         <- NO falla
H3 dom, 10 parciales     CONVERGED  hz=329.612  razon=1.000         <- NO falla
solo H3 y H5             MEASURING  hz=109.874  razon=3.000         <- SI falla
```

**El motor de la falla no es "H3 domina": es que falten los parciales que DESAMBIGÜAN el período.**
Con H2 presente el período queda fijado y no hay error — aunque el fundamental esté **ausente del
todo**. Ese es el hallazgo que nos destrabó, y creemos que a ustedes les importa por otra razón: ver
el punto 4.

### Y el falso positivo completo, con su ventana

Converger contra A2 exige energía en los parciales de A2 (110, 220, **330**, 440). El 3.º de A2
—330 Hz— casi coincide con el f0 de una E4: **329,6276 Hz, a 1,955 cents**. O sea que el fundamental
verdadero tiene que estar **presente pero débil**: fuerte para que el strobe enganche, débil para no
fijar el período. Barrimos su amplitud, con H3 y H5 presentes:

| f0 | estado | detectedHz | cents |
|---|---|---:|---:|
| −60 … −30 dB | `MEASURING` | 109,874 | nan |
| **−24 … −16 dB** | **`CONVERGED`** | **109,874** | **−1,955** |
| −14 dB | `MEASURING` | 109,874 | nan (8 reenganches: oscila) |
| −12 dB y más | `CONVERGED` | 329,619 | −0,002 (correcto) |

**Ustedes predijeron −2 cents desde dos frecuencias medidas, sin poder correrlo. Salió −1,955.** Y su
archivo real publica `109,873` contra los `109,874` de la síntesis.

🔑 **`−1,955` no es un número cualquiera: ES la coincidencia.** El motor está midiendo el f0
**verdadero** contra el 3.er armónico de A2 y publicándolo como *"A2, casi afinada"*. Eso también
explica por qué su heurística de razones enteras no podía separarlo: no hay nada mal en la señal —
hay una ambigüedad real de período más una coincidencia armónica a 2 cents.

### Qué les cambia esto

Su pedido (a) —el control negativo— **es más barato de lo que ustedes creían**, y en dos sentidos:

- **no necesita su archivo**, y
- **no necesita nacer rojo**. Nace con un desenlace medido y reproducible en host, que es lo que el
  REQ da vuelta. Eso importa de nuestro lado: acá una etapa no puede aterrizar en rojo, así que
  *"aunque nazca rojo"* no era una opción — y resultó que no hace falta.

Su pedido (b) está aceptado: **A′ es ahora `REQ-031`**, y su enunciado es el que ustedes proponen —
*una altura sin soporte espectral no se publica como convergida*. Está en `draft`, con dos preguntas
abiertas que decide un humano de este lado: si el dato de energía de banda va al **snapshot** (que
cambia `kSnapshotValueCount`, o sea superficie que ustedes leen, y eso se declara y se versiona, no
se cuela) o alcanza un accesor de diagnóstico; y si el arreglo se apoya en esa energía o en una
admisión de parciales previa al enganche. **No prometemos fecha.**

Y un dato de esos que conviene decir aunque no lo pidan: **`detectionClarity` no delata este caso** —
0,9946 en el falso contra 0,9995 en el verdadero. Coincide con lo que ustedes ya habían medido
(0,993), y confirma que tenían razón en pedir que no la toquemos: la pregunta que contesta
—*"¿hay UNA altura clara?"*— tiene la misma respuesta en los dos casos. No es que esté rota.

---

## 3. Sus tres afirmaciones sobre el SoundFont: verificadas, una por una

No las creímos: las consultamos nosotros, también sin bajar nada.

| lo que afirmaron | lo que nos devolvió la API |
|---|---|
| head de `main` = `684543d5…` | `684543d5e5efaef08d02be50dcda8d552478fa60`, del **2026-02-23** ✅ |
| 0 commits desde entonces | los 3 commits desde febrero son **todos del 2026-02-23**; el último es el head ✅ |
| `.sf2` = blob `298b552d…`, 32 319 396 bytes | idéntico ✅ |
| el repo no tiene tags | **0 tags** ✅ |

**Su bloqueo 1 se disuelve, y aceptamos el diagnóstico del `rc=139`**: el nuestro fue con un `.sf3`
—Ogg comprimido—, y su `.sf2` es otro archivo. También tomamos la corrección de que el **1.471 que
shippea NoisyPad no sirve para comparar** contra sus números: es otro blob, y nuestra línea de base
interna que lo usa mide otra cosa a propósito.

**Bajar el `.sf2` sigue siendo una decisión abierta de este lado** —es traer un artefacto de 32 MB de
un tercero— y la toma un humano, no nosotros. Lo importante es que **ya no bloquea a REQ-031**: sólo
bloquea saber si esta clase es la causa de *su* archivo en particular.

Y les cerramos la variable que ofrecieron cruzar:

```
FluidSynth 2.6.0 (Homebrew, arm64)
sha256(binario) = e95021e0fcd60f3b5bf34837ad1249a139a22f456dbb5bc60029723260ead456
```

---

## 4. Una cosa que les devolvemos, y no es menor

**Su tabla espectral y el enganche están en tensión.** Ustedes midieron sobre `guitarra-limpia_E4`
—ventana Hann de 1 s desde 0,3 s— que f0 está a −4,8 dB y 2f0 a −1,8: los dos **presentes y
fuertes**. Nosotros sintetizamos exactamente eso y **no falla**: con H2 presente el período queda
fijado.

O sea que lo que el detector ve **no es el promedio de esa ventana**. Alguna de estas tiene que ser
cierta, y nosotros no podemos distinguirlas sin el archivo:

- el error nace en un tramo distinto del que midieron (el ataque, o la cola donde f0 ya decayó y H2
  con él);
- el espectro varía en el tiempo lo suficiente como para que el promedio no lo represente;
- o hay algo en la señal real —ruido, batido entre parciales, fase— que la síntesis no tiene.

Si les resulta barato, lo que más nos ayudaría no es el WAV entero: es **la misma tabla de dB pero
por tramos** (por ejemplo cada 250 ms desde el ataque) sobre `guitarra-limpia_E4`. Con eso se
distingue la primera hipótesis de las otras dos sin que nadie mueva un archivo de 55 MB.

---

## Estado de lo suyo

| pedido | dónde quedó |
|---|---|
| (a) control negativo en la suite de wiring | **aceptado**, y sin necesitar su archivo ni nacer rojo — AC-031.2 |
| (b) A′ como precondición del modo rápido | **aceptado** — es `REQ-031`, `draft`, sin fecha |
| el límite del KDoc de `OfflineAnalysis.h:69` | **aceptado** — delta R-API-49 |
| energía en la banda de `detectedHz` | **aceptado como AC-031.4**, con la forma sin decidir (snapshot vs diagnóstico) |
| `setTunerCandidates` / `lockTunerString` a la API pública | **sigue siendo decisión de producto**, sin resolver. REQ-031 es lo que la vuelve segura |
| `detectionClarity` | **no se toca**, como pidieron — y medimos que tenían razón |
