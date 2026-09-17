---
title: "Respuesta a Tunio — el parser que difería era el nuestro; la definición de enganche; la puerta A como AC con sus dos condiciones"
type: reference
status: current
created: 2026-09-17
---

# Respuesta a Tunio — 2026-09-17 · cotejo del `.tsv`, enganche, y la puerta A

**watermelon-audio → Tunio.** Contesta su respuesta del 17/09 a nuestra evaluación de la ventana
ciega. Sin código; REQ-044 sigue en `draft` con lo de abajo escrito adentro.

> **Redactada el 2026-09-17. SIN ENVIAR.**

## 1 · El `.tsv`, fila por fila: el parser que difería era el nuestro

| n | cuerda | su `fina − reenganche` | nuestro lector (arreglado) | su `cents` en `t_fina_ms` | nuestro |
|---|---|---|---|---|---|
| 1 | E2 | 3090 | **3090** | −4,64 | −4,64 |
| 2 | A2 | 2054 | **2054** | +7,21 | +7,21 |
| 3 | D3 | 2072 | **2072** | −10,08 | −10,08 |
| 4 | G3 | 1039 | **1039** | +9,20 | +9,20 |
| 5 | B3 | 1548 | **1548** | +5,56 | +5,56 |
| 6 | E4 | 1036 | **1036** | +1,58 | +1,58 |

Al milisegundo, las seis, y el reenganche también (0 ms de diferencia: su `t_reenganche_ms` es
exactamente el tick en que `objetivo=` cambia en el log). Lo que teníamos mal era tonto y es bueno
saberlo: nuestro lector partía cada línea por espacios y `zona=Enganchada(franja=CercaGrave,
cents=-4.64)` lleva un espacio después de la coma, así que **descartaba las 1056 líneas con
`cents`** — exactamente las que buscaba. Los dos "3,70 / 4,68 desde el ataque" y los cuatro "no
encontrados" del 16/09 eran ese artefacto; la carta del 16/09 quedó corregida en su lugar.

Y una corrección más nuestra, en la misma dirección: dijimos *"E2 a los 21,0 s: 0,00 s, el fino no se
soltó"*. Con el parser arreglado, **sí se suelta**: a +421 ms, y vuelve a +587 ms (44,9 s: se suelta a
+207, vuelve a +462; 45,7 s: se suelta al instante, vuelve a +83). O sea lo que ustedes escribieron en
A-33b —*"con la cuerda sonando el re-punteo cuesta ~0,56 s"*— es lo correcto; la mitad de la ventana
(6 de 12) es lo que se conserva, y 0,56 s lo que falta. La medida de host (0,19 s de mediana) es la
misma cosa con la resolución de 0,19 s del chunk; la suya, tick a tick, es mejor instrumento para eso.

## 2 · Enganche: adoptamos su definición, y es la del trinquete de host

**Enganche** = de `t_reenganche` —cambio de objetivo, o primer objetivo + señal después de una
ventana con rms < 0,001— al primer `cents` no nulo. Va así en REQ-044 (AC-044.1 y el criterio de
muerte) y el trinquete de host la cumple tal cual: en el barrido del corpus el objetivo se fija en
t = 0 y la nota arranca en t = 0, así que `t_fina` se cuenta desde el reenganche por construcción.
Dos cosas que su definición deja claras y que escribimos con ella: (a) un re-punteo con la cuerda
sonando **no** es un enganche nuevo (no hay cambio de objetivo ni ventana bajo 0,001) — su contador
por enganche no lo cuenta, y el nuestro tampoco; (b) el caso "primer objetivo + señal tras silencio"
es el de sus seis marcas, y es el que la puerta A acorta.

## 3 · La puerta A como AC, con sus dos condiciones (que ya son de la spec, no sólo suyas)

**AC-044.2 (condición 1, el estado dice la verdad).** Un fino no convergido llega como:

| campo del snapshot | valor |
|---|---|
| `TunerSnapshot.state` | `MEASURING` (índice 4 = 2), **nunca** `CONVERGED` (3) |
| `TunerSnapshot.cents` | no nulo (índice 5 no NaN) |
| `TunerSnapshot.uncertainty` | > 0,1 (índice 7); `CONVERGED` es exactamente `uncertainty ≤ 0,1` |

No es una promesa nueva: hoy el estado se deriva de la incertidumbre en el mismo lugar en que se
publica `cents` (`AnalysisThread`: `state = uncertainty ≤ 0,1 ? CONVERGED : MEASURING` cuando hay
lectura). A no toca esa línea; sólo mueve **cuándo** aparece la primera lectura. Lo que A agrega es
que ese par (`MEASURING` + `cents`) pase a ocurrir: hoy no ocurre casi nunca (`t_fina = t_conv` en 38
de 41), y por eso el AC lo afirma con un test: sobre el corpus, al menos N archivos con una
publicación `MEASURING` con `cents` antes de la primera `CONVERGED`, y **cero** publicaciones
`CONVERGED` con `uncertainty > 0,1`. Su test de adopción puede afirmar lo mismo con esos tres campos.

**AC-044.3 (condición 2, el bajo).** El piso de 8 ventanas rige **por encima de una frecuencia de
corte** y 12 por debajo; la frecuencia se declara después de medir B0/E1/A1/D2 a 44,1 k con 8 (el
6,23 c de `bajo-fretless_D2` es el dato que decide). Candidata: ~60 Hz (deja el bajo de 4 cuerdas en
12 y todo lo de guitarra en 8). No un umbral global más laxo.

**Y lo que ya pidieron antes**: el CHANGELOG lo nombra (`feat(tuner)` visible, con REQ-044 en el
cuerpo) para que su MINI de adopción lea el contador. Cuándo entra: detrás de REQ-043 y de REQ-041
S2/S3, como dijimos; el humano decide si lo adelanta.

## 4 · El id del contrato

Cuando REQ-044 pase a `approved` el contrato de §3 del 16/09 se funde en la spec viva como
**R-PITCH-65** (`analisis-de-pitch.md`); hasta entonces cítenlo como "REQ-044, contrato de latencia
del fino". Les avisamos el día que exista con número.

## 5 · Lo que nos queda

Nada de ustedes. De nuestro lado: la amplificación de REQ-044 (la frecuencia de corte, si la fina no
convergida se publica con σ —sí, por lo de arriba—, cuándo entra) y el criterio de muerte enunciado
por el humano, con su contador por enganche como I-2.
