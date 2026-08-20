# Contrato de exactitud del afinador

**REQ-001 · S10 · AC-001.7 · NFR-1 / NFR-5 / NFR-8**

Este documento dice qué puede prometer el producto **y qué no**. La regla de la etapa que lo
produjo es una sola:

> **Ningún número entra acá sin la corrida que lo produjo.**

Cada cifra sale de un `.resp` commiteado en `audio/src/main/cpp/analysis/tests/golden/`, y el
test `AccuracyContract.TheDeclaredFiguresMatchTheirGoldens` **falla si este archivo y esos
golden dejan de coincidir**. Un contrato que puede quedar stale contra su propia evidencia es un
contrato que va a quedar stale.

---

## 🔴 Lo que el producto NO puede prometer

**Exactitud absoluta de ±0,1 cent en un teléfono.** Sería falso, y por una razón que no se
arregla con mejor DSP: el cristal que gobierna el ADC tiene una tolerancia típica de **±50 ppm**,
y eso son **0,087 cents** de error de escala — el 87 % del presupuesto entero, antes de que el
afinador haya hecho una sola cuenta.

Lo que sí se promete es **exactitud RELATIVA**: dos medidas del mismo dispositivo se comparan
entre sí con la precisión declarada abajo, porque el error de reloj es de **modo común** y se
cancela en la resta. Por eso el modo intonación —que es exactamente una resta de dos medidas—
es el modo donde el ±0,1 cent es más defendible, y el modo afinación contra una referencia
externa es donde menos.

*Un afinador que declara 0,1 y entrega 0,4 es peor producto que uno que declara 0,5 y lo cumple:
es la clase de mentira que un usuario con un Peterson al lado descubre en treinta segundos.*

---

## Lo medido

| Modo | Métrica | Medido | Presupuesto |
|---|---|---|---|
| **Strobe** (S6) | error contra objetivo, 3 s, peor de 14 cuerdas | **0,0011 cents** | 0,1 |
| **Strobe** | incertidumbre (1σ) a 3 s, peor caso (B0 del bajo) | **0,0045 cents** | — |
| **Detección gruesa** (S4) | error de identificación, peor caso (C7) | **0,21 cents** | 50 |
| **Detección gruesa** | claridad mínima sobre el rango A0–C7 | **0,967** | — |
| **Modo rápido** (S5) | saltos de cuerda durante un barrido desde floja | **0** | 0 |
| **Modo rápido** | latencia de conmutación de cuerda | **85,3 ms** | 150 |
| **Intonación** (S9) | error de la diferencia de dos medidas | **< 0,0001 cents** | 0,1 |
| **Intonación** | fuga de 50 ppm de reloj a la diferencia | **< 0,01 cents** | — |
| **Inarmonicidad** (S7) | error relativo de B, peor caso (bajo E1) | **0,30 %** | 10 % |
| **Inarmonicidad** | corrección perceptual máxima del catálogo | **2,47 cents** | 35 (techo) |

**Rango soportado:** A0 (27,5 Hz) a C7 (2093 Hz), medido por los dos extremos.

**Latencias por modo:** detección gruesa ≤ 100 ms desde el onset; conmutación de cuerda en modo
rápido 85,3 ms; el strobe declara convergencia cuando 1σ ≤ 0,1 cents, lo que en la cuerda más
grave del catálogo (B0) ocurre pasados los **0,5 s** y en A4 es inmediato.

---

## Lo que todavía no está medido, y por qué

| | |
|---|---|
| **CPU sostenido en dispositivo** (NFR-1) | 🔴 **sin medir.** Todo número de costo que imprima la suite es un **techo**, no la medida: el build de host compila en `Debug`. Requiere hardware. |
| **Sesión de 10 min sin throttling** (NFR-5) | 🔴 **sin medir.** Requiere hardware. |
| **Robustez contra corpus grabado** (NFR-8) | 🔴 **sin correr.** El corpus no existe todavía; el mecanismo para bajarlo y verificarlo sí (`scripts/fetch-corpus.sh`), y los tests que dependen de él salen **SKIPPED**, nunca PASSED. |

Estas tres filas están vacías **a propósito**. Una corrida que no verificó no puede pasar por
una que sí — la misma regla que gobierna `regen-golden.sh` y la atestación del gate local.

---

<!-- CONTRACT-DATA — lo lee el test de auto-verificación. NO editar a mano sin recapturar
     los golden: el test compara estos valores contra ellos y falla si divergen.
strobe_worst_error_cents = 0.001092
strobe_worst_sigma_cents = 0.004472
coarse_worst_error_cents = 0.2124
coarse_min_clarity = 0.967426
fast_mode_sweep_switches = 0
intonation_worst_error_cents = 0.0001
inharmonicity_max_correction_cents = 2.4678
crystal_50ppm_cents = 0.0866
-->
