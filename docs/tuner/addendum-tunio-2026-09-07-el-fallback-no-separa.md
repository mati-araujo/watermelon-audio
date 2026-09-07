---
title: "Addendum a Tunio — su fallback, tal como lo pidieron, no los separaría"
type: reference
status: current
created: 2026-09-07
---

# El fallback no alcanza, y es mejor que lo sepan antes de que lo publiquemos

**2026-09-07 · addendum a la respuesta de hoy · watermelon-audio → Tunio**

Corto. Fuimos a decidir **cómo** implementar lo que pidieron y la medición dijo que el pedido, tal
como está enunciado, **no les serviría**. Preferimos decirlo ahora y no entregárselo.

## Lo que pidieron

> *"que el snapshot diga **cuánta energía hay en la banda de `detectedHz`** —un nivel del parcial, o
> una bandera «el fundamental estimado no tiene soporte espectral»—. Con eso el consumidor distingue
> 110 Hz-con-energía de 110 Hz-inventado."*

## Por qué ese solo número no distingue

Medimos la energía en la banda del fundamental publicado, en dB relativos al parcial más fuerte de
esa misma altura:

| señal | estado | X(det) |
|---|---|---:|
| el falso (f0 a −24 dB) | `CONVERGED` | **−37,5** |
| el falso (f0 a −20 dB) | `CONVERGED` | −42,3 |
| **una bordona LEGÍTIMA (f0 a −40 dB de H2)** | `CONVERGED` (correcto) | **−44,1** |

🔴 **Las poblaciones se tocan, y en la dirección peor**: una cuerda grave real —de esas que el motor
tiene que seguir midiendo, y que nuestro `R-PITCH-35` protege explícitamente— tiene el fundamental
**más hundido** que el caso falso de ustedes. Cualquier umbral que rechace su falso mata bordonas
reales.

Si les hubiéramos publicado ese número, habrían quedado **peor que hoy**: creyendo que tienen el
discriminador y aplicándolo con un umbral que produce falsos negativos sobre cuerdas buenas. Es la
misma forma que nuestro MINI-006 midió para otra cosa —dos poblaciones que se solapan y ningún
umbral que las parta— y por eso no ponemos uno.

## Lo que sí separa: el par, no el número

| caso | X(det) | **X(2·det)** | X(3·det) |
|---|---:|---:|---:|
| falso −24 dB | −37,5 | **−37,6** | 0,0 |
| falso −20 dB | −42,3 | **−42,4** | 0,0 |
| bordona −30 dB | −31,2 | **0,0** | −3,5 |
| bordona −40 dB | −44,1 | **0,0** | −3,5 |
| cuerda sana | 0,0 | −6,0 | −12,0 |

En el falso faltan **el fundamental Y su octava**. En una cuerda real a la que le falta el
fundamental, **H2 es el pico**. El criterio es `max(X(det), X(2·det))`: peor aceptado **−12,0**,
mejor rechazado **−37,5** — **25 dB** de margen.

**Así que lo que les vamos a dar es el par o el veredicto ya calculado, no el número suelto.** Cuál
de los dos —y si va en el snapshot, que cambia `kSnapshotValueCount` y es superficie que ustedes
leen— sigue sin decidirse de nuestro lado. Lo que ya está decidido es que **un solo número no va**.

## Y la otra variante que probamos, para que no la pidan

Bajar el umbral de selección de pico del detector (`kPeakThreshold`, 0,9 → 0,80) **hace que el falso
deje de converger** — y es una trampa. No pasa a leer la E4 bien: pasa a detectar **927 Hz con
claridad 0,76**, picos espurios de baja correlación, y lo único que lo salva es que nuestra compuerta
de ausencia los tapa. Cambia un dato plausible-y-falso por basura compensada.

Queda descartada, y de paso destapó un problema nuestro: **la suite entera (1224 tests) pasa con ese
valor cambiado**, aunque una spec archivada afirmaba que la constante estaba vigilada por un test de
mutación. El mutante registrado era `0,9 → 0` —aniquilante—, que no dice nada sobre un cambio
plausible. Va como objeto propio de este lado.

## Lo que no cambia

Todo lo de la respuesta de hoy sigue igual: su predicción **confirmada** (−1,955 contra los −2 que
predijeron), el control negativo **sin necesitar su archivo**, A′ abierto como `REQ-031`, y el
límite del KDoc aceptado. Y sigue en pie el pedido que les hicimos: **la tabla de dB por tramos** de
`guitarra-limpia_E4`, que es lo que distingue *"el error nace en otro tramo"* de *"el espectro varía
en el tiempo"*.
