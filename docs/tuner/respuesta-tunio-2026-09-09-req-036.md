---
title: "Aviso a Tunio — el ataque de la nota ya no se declara convergido, y una nota corta converge antes"
type: reference
status: current
created: 2026-09-09
---

# El ataque de la nota ya no se declara convergido — y una nota corta converge antes

**2026-09-09 · REQ-036 · watermelon-audio → Tunio**

Esto cambia **qué se declara `CONVERGED`** sobre notas con ataque, y **cuándo** llega la primera
lectura fina. No cambia el snapshot (siguen 18 valores), ni la C API, ni el JNI, ni Kotlin. Es un
`fix(tuner)` y va en la próxima release.

---

## 1. Qué pasaba

El estimador de fase regresa la fase desenvuelta sobre una ventana deslizante de 48 × 4096 frames
(4,1 s a 48 k, 4,5 s a 44,1 k) y publica la pendiente como cents y su error estándar como σ. Una
nota pulsada trae un **transitorio de afinación en el ataque** (en el corpus: la guitarra limpia
tarda > 1 s en asentarse; un bajo acústico arranca −27,7 c). Ese transitorio queda dentro de la
regresión hasta 4 s después, y la fase resultante es un palo de hockey: una recta ajustada a un palo
de hockey deja residuos chicos, así que **σ era ciega**. Medido sobre 24 glides sintéticos: lectura
`CONVERGED` a 0,34 s con σ ≤ 0,0004 y hasta −13,6 c de error; sobre el corpus, hasta 0,73 c con
σ 0,054, convergida.

## 2. Qué cambia (R-PITCH-62 y 63)

- **Admisión por tendencia**: cada parcial compara la pendiente de las dos mitades de su ventana.
  Si difieren de forma significativa (|T| > 5) y por más de 0,05 c, esa fase no es una recta y el
  parcial no se admite. **Con menos de 12 ventanas de fase no hay veredicto, y sin veredicto no hay
  lectura fina**: la primera lectura llega a **1,02 s (48 k) / 1,11 s (44,1 k)** desde que el strobe
  arranca, más lo que tarde la gruesa en dar objetivo (~0,1 s). Antes podía llegar a 0,34 s — y en
  el ataque, equivocada.
- **Ventana adaptativa**: cuando la fase se quiebra, los cuatro parciales reinician su ventana en
  el punto de quiebre en vez de arrastrar el ataque 4 s. Sobre síntesis, `CONVERGED` llega a lo sumo
  **1,42 s** después de que la altura se asienta; sobre el corpus, `bajo-acustico_G2` (el glide de
  −27,7 c) y `ukelele_C4` (nota de 2,46 s) **ahora convergen**, a +0,0005 y −0,00 c.
- **Un parcial solo, contradicho por sus hermanos, no es una lectura**: si queda un único parcial
  porque a los demás los descartó el desacuerdo (signo contra la gruesa, mediana), no se publica.
  Lo destapó audio con huecos sostenidos: un armónico solo leía −2,9 c con σ 0,07.
- **Efecto de rebote sobre huecos de captura NO reportados**: hasta hoy el motor convergía sobre
  una lectura equivocada (era el límite declarado de REQ-009 S3). Ahora un hueco deja la fase fuera
  de una recta y a los parciales en desacuerdo, y el motor publica `MEASURING` sin cents. No es
  recuperación —la lectura no se corrige— sino que ya no se publica una mentira mientras tanto.

## 3. Lo que van a ver

| | antes | ahora |
|---|---|---|
| primera lectura fina posible | 4 ventanas (0,34 s) | 12 ventanas (1,02–1,11 s) |
| durante el ataque | `CONVERGED`, sesgada | `MEASURING` |
| corpus (41 notas): convergidas al final | 33 | **39** |
| corpus: error máximo de las convergidas | 0,73 c | **0,30 c** |
| corpus: publicaciones `CONVERGED` con \|error\| > 0,1 c | 744 | 212 |
| nota estable (14 cuerdas × 2 rates), vibrato leve | 0,1 c, sin cambio | 0,1 c, sin cambio |

**`MEASURING` durante el transitorio es el comportamiento nuevo, no una regresión.** Si su UI
mostraba un número apenas la nota arrancaba, ahora va a mostrar "midiendo" ~1 s y después el número
correcto. El presupuesto del corpus (`kRecordedFineBudgetCents`) baja de 1 c a **0,4 c**.

## 4. Lo que NO cambia

El contrato sintético de 0,1 c a 3 s, los golden a 2 y 3 s, la detección gruesa (`detectedHz`,
que es de REQ-038), la bandera de soporte de REQ-031, el reenganche del modo rápido (REQ-030) y su
puerta pública (REQ-037), el layout del snapshot, la C API, el JNI y Kotlin.
