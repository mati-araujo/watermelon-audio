---
title: "Respuesta a Tunio — la ventana ciega tras el punteo: confirmada en host (2,04 s), qué es por diseño y qué no, y las puertas con su costo"
type: reference
status: current
created: 2026-09-16
---

# Respuesta a Tunio — 2026-09-16 · la ventana ciega tras el punteo

**watermelon-audio → Tunio.** Contesta *"La ventana ciega tras el punteo: 1,4–3,5 s, y un sesgo del
`detectedHz` que no la puede tapar"* (sobre 2.16.4, g42, criolla). Evaluación, sin código: lo que
sigue está medido con nuestros instrumentos (el corpus real de GeneralUser GS 2.0.3 que ya
compartimos, con el arnés de la conformidad) y, aparte, rejuzgando su log de ticks con un lector
propio. Al final, lo que entra a un REQ y con qué id.

> **Redactada el 2026-09-16 a la noche. SIN ENVIAR.** Las medidas de host son de la sesión
> `9e7943d0`; el script de scratch quedó fuera del árbol a propósito (no es un test: es un
> instrumento de una tarde, y su lugar es el REQ que lo vuelva trinquete).

## 1 · Lo que confirmamos, con nuestros números

**`t_fina` ≈ 2 s es del motor, y lo reproducimos sin su aparato.** Sobre las 41 notas del corpus
(objetivo puesto a t = 0, instrumento declarado, misma cadena que producción; resolución 0,19 s por
chunk del ring, así que cada número es un techo dentro de 0,19):

| | mediana | máx | n |
|---|---|---|---|
| **`t_fina`** — del ataque al primer `cents` no nulo | **2,04 s** | 4,27 s | 41 |
| `t_conv` — al primer CONVERGED (σ ≤ 0,1) | 2,04 s | 4,27 s | 41 |
| \|error\| del primer fino contra el Hz verdadero | 0,04 c | 1,95 c | 41 |

Ustedes: mediana 2,05 / máx 3,09. **Es el mismo número.** Sólo guitarras y ukelele (≥ 82 Hz): 2,04 /
4,27 también; nylon E2 2,04, acero E2 3,16, nylon E4 1,49.

Y una cosa que ustedes no podían ver y que responde su pregunta de §3 antes de formularla:
**`t_fina` = `t_conv` en 38 de 41**. El primer `cents` que publicamos ya es el convergido. **Hoy no
existe un modo intermedio**: el motor no tiene una lectura "menos exacta, antes" que esté
reteniendo — publica cuando está a ±0,1 o no publica.

## 2 · Lo que desmentimos, con su propio log

**"Cada re-punteo de la misma cuerda vuelve `cents` a `null` y reinicia la integración"** — no como
regla. Depende de si la cuerda seguía sonando:

| en su `tanda-2026-09-16-ticks.log` | qué había antes | `t_fina` tras el re-punteo |
|---|---|---|
| E2 a t = 21,0 s (rms 0,127 → re-punteo) | MIDIENDO, `cents` = +0,06 | **0,00 s** — el fino no se soltó |
| E2 a t = 47,0 s | MIDIENDO, `cents` = −25,2 | **0,00 s** |
| E2 a t = 45,1 s | MIDIENDO, sin cents todavía | 0,66 s |
| las marcas de la tanda | **SIN_SENAL** 0,3–2 s antes, rms 0,0015–0,0096 | 3,5 / 2,8 / 2,4 / 1,7 / 2,1 / 1,4 s (su tabla) |

En host, la misma nota alimentada dos veces seguidas **sin silencio en el medio** y sin tocar el
objetivo: `t_fina` del segundo ataque **mediana 0,19 s** (30 de 40 en el primer chunk; máx 2,41 en
`guitarra-nylon_E2`). El ataque nuevo quiebra la recta de fase y el motor **reinicia conservando la
mitad nueva de la ventana** (R-PITCH-63: 6 ventanas se quedan, faltan 6 = 0,56 s), no desde cero.

Lo que sí arranca de cero es el re-punteo **después de silencio**: una ventana de análisis (4096
frames = 85–93 ms) con rms < 0,001 **corta el hilo de fase** (`mHavePrevPhase = false; mCount = 0`)
y desde ahí valen otra vez las 12 ventanas de R-PITCH-62. En una criolla la cuerda cae por debajo
de 0,001 a los ~5–6 s (su E2: 0,0008 a los 6,0 s); el músico que puntea cada 2 s **no** cruza ese
piso, así que su caso es el de la fila 1, no el de las marcas. Las seis marcas de su tanda, en
cambio, venían todas de SIN_SENAL: ahí el "reinicio" es el enganche entero, no el re-punteo.

Y **reescribir `selectedString` con la misma cuerda no reinicia nada**: el hilo aplica un objetivo
sólo cuando *cambió el pedido* (`target != mLastUserTarget`, REQ-030), no cuando difiere de lo
aplicado.

## 3 · Lo que es por diseño, escrito como contrato

Esto queda como contrato en la spec del afinador (delta declarado de REQ-044, abajo), con la forma
de R-PITCH-56:

1. **El primer `cents` no puede llegar antes de 12 ventanas de fase contiguas con señal**: 1,02 s a
   48 kHz, 1,11 s a 44,1, desde que hay objetivo y señal (R-PITCH-62). Hoy es también el primer
   CONVERGED: no hay lectura provisional.
2. **Cada quiebre de la recta de fase (el glide del ataque) cuesta media ventana**: se conservan 6
   de 12 y hacen falta 6 más (0,56 s) antes del siguiente veredicto; el quiebre sólo se juzga con
   ≥ 12 (R-PITCH-63). Es lo que lleva la mediana de 1,1 a 2,0: en el corpus, 0–2 reinicios por
   nota de guitarra, hasta 7 en bajo.
3. **Una ventana con rms < 0,001 corta la integración.** Después de ella corre 1 otra vez. Un
   re-punteo con la cuerda sonando (rms ≥ 0,001) **no** vuelve a cero: cuesta 2, medido 0,19 s de
   mediana.
4. **Reescribir el mismo objetivo no reinicia.** Cambiarlo, sí (y descarta el ring).

Los "±0,7 de IQR" cuando el fino llega **no son el costo de 1**: son la cuerda (la inarmonicidad y
el batido, R-PITCH-64). El costo de la exactitud es 1 solo; el resto de los 2 s es 2, o sea la
física del ataque de nylon (REQ-008: 12–40 c de glide en cuerda real), que **cualquier** lector ve —
su gruesa con p90 +12,6 c es el mismo glide leído sin integrar.

## 4 · Las puertas, con su costo — ustedes no eligen, y nosotros tampoco todavía

Pidieron el resultado por clase (*que `t_fina` baje y/o que el re-punteo no vuelva a cero*). La
segunda mitad ya se cumple con la cuerda sonando (§2); la primera tiene puertas, y **medimos la
barata**:

| puerta | qué cambia | `t_fina` corpus (med / máx) | \|e\| del primer fino (med / máx) | costo |
|---|---|---|---|---|
| **hoy** (12 ventanas) | — | **2,04 / 4,27** | 0,04 / 1,95 c | — |
| **A. veredicto desde 8 ventanas** (el piso físico de las dos mitades) | R-PITCH-62: 12 → 8, quizá **sólo por encima de ~60 Hz** (los 12 los pidieron B0/E1 a 44,1 k, que una guitarra no toca) | **1,11 / 3,16** (t_conv 1,49) | **0,11 / 6,23 c** — el 6,23 es `bajo-fretless_D2`; en guitarras y ukelele máx **2,41 c** (`guitarra-limpia_G3`), 6 archivos > 0,5 c contra 4 hoy | re-declarar R-PITCH-62 y su AC; `test_partial_admission`, `test_convergence_honesty`; re-medir el trinquete del corpus (39 convergidas, 0,30 c máx) y los golden de fase; 1 mutante (el 12). **Un día.** Aparece por primera vez una lectura *fina no convergida* (t_fina < t_conv): ustedes ya distinguen los estados, así que no cambia su contrato |
| B. publicar la lectura provisional antes del veredicto (ventanas 4–11) con su σ, estado MEASURING | R-PITCH-62 entero: la compuerta que REQ-036 puso porque 24 glides sintéticos convergían a 0,34 s con hasta −13,6 c | ~0,4 s | **es el glide**: −13,6 c medido en síntesis, del orden de su gruesa | alto en exactitud, y compra poco: antes de ~0,8 s en nylon la lectura ES el ataque. No la recomendamos |
| C. ventanas más cortas (4096 → 2048) | todo: el rango de captura se parte en dos (fs/2N), el ruido sube, los 84 ms pasan a 42 | ~1,0 / ? | no medido | los golden de DSP, el corpus entero, R-PITCH-6x, el contrato de exactitud. Semanas, y sin garantía |
| D. "cents gruesos" del motor (`detectedHz` vs objetivo) | mueve al motor lo que ya hacen | igual que su gruesa | p90 +12,6 c: el glide | **no es una puerta**: el sesgo es de la señal, no de quién resta |
| E. no cortar en silencio | — | — | lecturas plausibles y falsas: las fases de antes y después del silencio no se relacionan (la clase de REQ-009) | **no es una puerta** |

La A es la única que baja `t_fina` con un costo de exactitud acotado (y medido sobre lo que
ustedes tocan: ≤ 2,4 c en guitarra, dentro de cualquier franja de ±15). Lo que NO baja es lo que no
es la compuerta: con A, `guitarra-acero_E2` sigue en 3,16 s con **cero** reinicios — ahí los
parciales no se admiten por dominio/signo contra el control durante el ataque largo del acero (el
fino aparece y se va 10 veces en la nota), y eso ninguna constante lo mueve.

## 5 · Lo que entra a un REQ, y con qué id

**REQ-044 — la ventana ciega tras el punteo** (draft, sidecar). Clase = la suya: *que `t_fina`
baje sobre una cuerda sostenida; que el re-punteo con la cuerda sonando no vuelva a cero (hoy ya no
lo hace: se afirma con test)*. Puerta candidata A con los números de arriba como AC provisionales
(umbrales DESPUÉS de correr el trinquete con la puerta puesta); el contrato de §3 como delta
(R-PITCH-65). **Criterio de muerte apuntando a su instrumento**: la mediana de ticks con gruesa
mostrada por punteo (la fila de obsolescencia de su REQ-027), y nuestro I-1 en host: `t_fina` sobre
el corpus, por archivo, con trinquete. No a una fecha.

Se decide con el humano cuándo entra (hoy está detrás de REQ-043 y de dos etapas de REQ-041). Lo
que sí les damos ya: el contrato de §3, para que dejen de esperar el fino antes de 1,1 s y para que
sepan que su suplencia sólo hace falta **hasta el primer fino de cada enganche, no de cada punteo**.

## 6 · Lo que NO pudimos reproducir de su log, y qué nos serviría

Con un lector propio sobre `tanda-2026-09-16-ticks.log` reproducimos las filas de §2 (los
re-punteos con la cuerda sonando, tick a tick) y dos de sus seis `t_fina` desde el ataque real
(A2 3,70 s, D3 4,68 s — más largos que sus 2,8 / 2,4, que cuentan *desde el reenganche*); para
E2, G3, B3 y E4 nuestro lector no encuentra un `cents` no nulo entre el ataque de la marca y el
siguiente SIN_SENAL (E2: MIDIENDO 5,3 s sin cents, rms de 0,26 a 0,0008). No es que estén mal:
es que la atribución por cuerda depende de dónde ponga cada lector el ataque y el reenganche. Si
nos mandan el `.tsv` de veredicto con el tick de reenganche por punteo, cotejamos fila por fila.
Para el criterio de muerte de REQ-044 el instrumento es el suyo (mediana de ticks con gruesa por
punteo): la definición de "punteo" y de "reenganche" tiene que ser la misma de los dos lados, y
por eso la pedimos escrita.
