---
title: "Respuesta a Tunio — el subarmónico ya no converge, y el snapshot les dice cuándo no creerle"
type: reference
status: current
created: 2026-09-07
---

# El falso positivo que predijeron está cerrado, y tienen con qué verlo

**2026-09-07 · sobre el PR #260 (mergeado en `b18695b`) · watermelon-audio → Tunio**

Esta es la continuación de la [respuesta de esta mañana](respuesta-tunio-2026-09-07.md): REQ-031
está ejecutado, mergeado y va en la próxima release (**2.16.0**, en curso). Tres cosas: qué cambia
en lo que leen, qué NO cambia, y una medición que da vuelta algo que les habíamos dicho.

---

## 1. Qué cambia: un valor nuevo al final del snapshot

El snapshot pasa de **17 a 18 floats**. El índice **17** es la **bandera compañera de `detectedHz`**
que pidieron el 03/09 (*"energía en la banda de `detectedHz`"*), con una diferencia de forma que
explicamos en el punto 3:

| valor | significa |
|---|---|
| `1` | la altura de `detectedHz` tiene energía en la señal, **en su fundamental o en su octava** |
| `0` | no la tiene en ninguno de los dos: es un **submúltiplo** que explica los mismos datos, no algo medido. *"Vi 109,87 y no le creo"* |
| `NaN` | no hay altura que calificar (`detectedHz` es 0) |

En Kotlin es `TunerSnapshot.spectralSupport: Boolean?`, `null` exactamente cuando `detectedHz` es
`null`. Se publica **siempre**, no sólo cuando el estado no es convergido — que es lo que pidieron.

**Y decide el estado.** Con la bandera en `0`:

- el estado es **`NO_LOCK`**, no `MEASURING`. Eligieron bien al describir el spinner eterno como
  peor que declarar ausencia; no lo van a ver más sobre una altura inventada;
- **`detectedHz` se sigue publicando**, marcado. Es más útil que el silencio;
- **`cents`, `phaseAngle`, `uncertainty` e `inharmonicityB` salen `null`**. Una desviación contra una
  cuerda que no es la que suena no mide nada, y publicarla junto a `NO_LOCK` sería la misma
  contradicción con otra ropa.

Bandera y estado salen de **una sola computación**: no van a ver nunca `CONVERGED` con la bandera en
`0`. Está fijado por un test que recorre publicación por publicación, no sólo la última.

🔴 **`NO_LOCK` ahora cubre dos situaciones**, y la bandera es lo que las separa: *"falta elegir contra
qué medir"* (bandera `1`, lo de siempre) y *"vi una altura y la señal no la sostiene"* (bandera `0`).
Si su UI trata `NO_LOCK` como "pedí un objetivo", miren la bandera antes: en el segundo caso ya lo
tienen.

### Su caso, medido antes y después

Su predicción del 03/09: `guitarra-limpia_E4` leída como A2 a **−2 cents**. Sobre `master` antes del
arreglo, sintetizado (E4 con f0 a −20 dB, sin H2, con H3 y H5, sus seis cuerdas declaradas):

```
antes   CONVERGED  detectedHz=109.874  cents=-1.955   clarity=0.9946
ahora   NO_LOCK    detectedHz=109.874  cents=null     spectralSupport=false
```

Y lo que no se movió, con y sin candidatos, dentro del presupuesto de 0,1 cents:

```
cuerda sana E4                       CONVERGED  spectralSupport=true
bordona E2, f0 a -40 dB de H2        CONVERGED  spectralSupport=true   <- R-PITCH-35, el lado que ustedes verificaron en hardware
timbre de su tabla (H3 dominante)    CONVERGED  spectralSupport=true   (-1,8 dB de soporte)
silencio                             NO_SIGNAL  spectralSupport=null
```

---

## 2. Qué NO cambia

- **Append-only**: los índices 0..16 no se movieron. Su librería compilada contra 17 sigue leyendo el
  prefijo; una compilada contra 18 contra un motor viejo devuelve `null` en el snapshot entero, como
  siempre (`size >= VALUE_COUNT`).
- **Sin función nueva** en la C API ni en JNI: el valor viaja en el array que ya cruza.
- **`detectionClarity` intacta**, como pidieron. Y ahora es un requisito escrito (R-PITCH-59): la
  claridad **no separa** una altura verdadera de un submúltiplo (0,9946 sobre el falso, 0,9995 sobre
  el verdadero), porque responde *"¿hay UNA altura clara?"* y esa pregunta tiene la misma respuesta
  en los dos casos.
- **`setTunerCandidates` / `lockTunerString` a la API pública**: sigue siendo decisión de producto.
  Lo que sí cambió es que ahora es **segura**: el reenganche a la cuerda equivocada ya no puede
  terminar en convergido.

---

## 3. Lo que la medición dio vuelta, y por qué les importa

Les dijimos que el criterio sería *"energía en la banda de `detectedHz`"*. **Medido, eso solo no
sirve**: una bordona legítima con el fundamental 40 dB por debajo de H2 tiene el fundamental **más
hundido** que el falso (−44,1 contra −37,5 dB). Cualquier umbral sobre el fundamental solo que
rechace el falso mata cuerdas reales — el falso negativo del lado que ustedes verificaron en
hardware. Lo que separa las dos poblaciones no es el nivel sino la **forma**: en el falso faltan el
fundamental **y su octava**; en la cuerda real sin fundamental, H2 es el pico. Por eso la bandera
pregunta `max(X(f), X(2f))`.

Y una segunda vuelta que no esperábamos: sobre la ventana real del lazo (2048 frames, 46 ms) **sin
ventana de Hann** las poblaciones se tocan por fuga espectral, y acumular ticks no lo arregla. Con
Hann sobre los mismos frames: 59,5 dB de separación. Lo contamos porque es la clase de cosa que su
banco de pruebas puede reproducir, y porque el umbral (−25 dB) quedó donde estaba.

---

## 4. Lo que les pedimos

Sigue en pie lo de esta mañana: **la tabla de dB por tramos** (cada 250 ms desde el ataque) sobre
`guitarra-limpia_E4`. La síntesis reproduce el **desenlace** de su archivo (`109,874` contra sus
`109,873`), no el archivo: para saber si ésta es la causa de *su* grabación —y no otra que produce
el mismo número— hace falta ver qué ve el detector por tramos.

Y si les resulta barato: corran su banco contra 2.16.0 con `spectralSupport` a la vista. Si aparece
un `false` sobre una cuerda que ustedes saben real, ése es exactamente el dato que queremos, y no lo
vamos a arreglar bajando el umbral.

---

## Estado de lo suyo

| pedido | dónde quedó |
|---|---|
| (a) control negativo en la suite | **cerrado** — nació rojo (`CONVERGED`, 109,874, −1,955) y ahora es verde |
| (b) A′ como precondición del modo rápido | **cerrado** — REQ-031, en `master` (`b18695b`) |
| el límite del KDoc de `OfflineAnalysis.h` | **cerrado** — acotado a la cuerda ajena; R-API-49 modificado |
| energía en la banda de `detectedHz` | **cerrado como bandera compañera** en el snapshot, índice 17 — con la forma medida del punto 3 |
| `setTunerCandidates` / `lockTunerString` públicas | **decisión de producto**, ahora segura |
| `detectionClarity` | **no se tocó**, y quedó escrito por qué |
| la tabla por tramos | **sigue pendiente de su lado** |
