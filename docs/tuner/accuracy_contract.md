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
| **Strobe** (S6) | error contra objetivo, 3 s, peor de 14 cuerdas, **dentro del rango útil** | **0,00095 cents** | 0,1 |
| **Strobe** | incertidumbre (1σ) a 3 s, peor caso (B0 del bajo) | **0,0017 cents** | — |
| **Detección gruesa** (S4) | error de identificación, peor caso (C7) | **0,21 cents** | 50 |
| **Detección gruesa** | claridad mínima sobre el rango A0–C7 | **0,967** | — |
| **Modo rápido** (S5) | saltos de cuerda durante un barrido desde floja | **0** | 0 |
| **Modo rápido** | latencia de conmutación de cuerda | **85,3 ms** | 150 |
| **Intonación** (S9) | error de la diferencia de dos medidas | **< 0,0001 cents** | 0,1 |
| **Intonación** | fuga de 50 ppm de reloj a la diferencia | **< 0,01 cents** | — |
| **Inarmonicidad** (S7) | error relativo de B, peor caso (bajo E1) | **0,30 %** | 10 % |
| **Inarmonicidad** | corrección perceptual máxima del catálogo | **2,47 cents** | 35 (techo) |

**Rango soportado:** A0 (27,5 Hz) a C7 (2093 Hz), medido por los dos extremos.

### 🔴 La condición de RIQUEZA ARMÓNICA, que hasta REQ-027 estaba implícita

Las cifras del strobe se miden sobre una señal de **cuatro parciales**. Eso NO era una elección
consciente: era simplemente lo que generaban los ocho tests de extremo a extremo
(`for (int n = 1; n <= 4; ++n)`), y esa uniformidad escondió un defecto durante toda la vida del
motor. Sobre `2.14.0`, con señal pobre en armónicos y afinada EXACTO, el motor publicaba hasta
**38,70 cents con estado `CONVERGIDO`** — cuatro órdenes de magnitud fuera de esta tabla.

Desde REQ-027 el contrato vale para **cualquier riqueza armónica de 1 a 4 parciales**, y hay tests
que lo afirman por separado en `test_partial_admission.cpp`. Las dos defensas que lo sostienen:

- un parcial **sin energía en su bin** no entra en la combinación (integraba fuga espectral, que da
  una rampa de fase lineal, o sea σ chica y una lectura confiadamente equivocada);
- el arbitraje por signo tiene **zona muerta**, así que con la cuerda bien afinada el fundamental
  —a veces el único parcial con energía— no se descarta por un empate técnico entre dos números que
  valen cero.

### Qué significa la incertidumbre publicada (cambió en REQ-027)

σ ya **no** se propaga de las σ por parcial: sale de los **residuos** del ajuste
`cents_n = C + 600·log2(1+B·n²)`. La distinción no es cosmética. La σ del estimador de fase es una
**precisión** (1e-7 a 1e-4 cents) —cuán bien encaja una recta— y no una **exactitud**. Publicar la
propagada era decir 0,003 al lado de un error de 38,7.

Como efecto, la lectura publicada es **C, la desviación del FUNDAMENTAL**, y no un promedio
ponderado de los cuatro parciales. En una cuerda real e inarmónica esos parciales discrepan por
física —con B = 1e-3 leen 0,865 / 3,455 / 7,756 / 13,740 cents— y el peso 1/σ² era MAYOR en los
altos, o sea en los más estirados.

**Rango útil de la lectura fina (REQ-003, REQ-057).** El strobe no mide cualquier desajuste: el
desenvuelto de fase acota la captura de cada parcial a `|Δf| < fs/(2·H)`, donde `H` es el **hop**
de la serie de fases, no el largo de la ventana. Cada parcial tiene cuatro rejillas intercaladas
(ventana N = 4096, desfasadas H = 1024 frames): cada una es el estimador de antes, y la serie
densa que forman entre las cuatro fija la vuelta de 2π del desenvuelto. El rango **depende del
objetivo y del sample rate** y se deriva —no se tabula, porque una tabla queda stale con el primer
cambio de rate—:

    rango_cents(n·f0) = 1200 · log2(1 + κ · fs / (2·H·n·f0))      H = 1024, κ = 0,9

Un parcial vota sólo si la detección gruesa cae dentro de SU rango (orden n), y el índice 14 es la
misma fórmula con n = 1. κ = 0,9 es un margen contra el borde: `fs/(2H)` es el nulo del lóbulo
principal de Hann, y cerca de ahí un parcial lee mal (medición del checkpoint de diseño de
REQ-057 S1, en su etapa). Con la fórmula, a 44,1 / 48 kHz: E2 ±365,7 / ±394,6 c, G3 ±163,2 /
±177,0, E4 ±98,9 / ±107,4, A4 ±74,6 / ±81,1 y E5 ±50,2 / ±54,5. Lo vigila
`InterleavedGrids.TheUsableRangeIsTheDenseFormulaWithKappaNineTenths`.

Fuera de ese rango el motor publica **ausencia** —NaN— en vez de un número. La detección gruesa se
sigue publicando, así que el usuario conserva "qué nota es" cuando pierde "cuánto exactamente".

**Un medidor de ±50 cents existe en toda cuerda del catálogo (B0 a E5), a 44,1 y 48 kHz**
(`InterleavedGrids.EveryCatalogStringIsMeasuredWithinPlusMinusFiftyCents`; el borde de E5 a
44,1 kHz —lee a ±50, no a ±51— lo fija `TheEdgeOfTheCatalogE5At44kReadsAtFiftyAndNotAtFiftyOne`),
**salvo un hueco declarado** a 44,1 kHz (no pasa a 48 kHz): B0 desde +25 cents y E1 por encima de
+25 cents no tienen lectura fina (la compuerta de tendencia reinicia en bucle; preexistente). Lo fija
`InterleavedGrids.TheDeclaredGapB0AndE1Above25CentsAt44kHasNoFineReadingButTheCoarseOne`, que da rojo el día que se cierre.
Con 1 o 2 parciales admitidos —el caso común a ±50 c en las agudas— la σ es la de antes, todavía
optimista: su verificación es de REQ-058.

**El motor lo publica**, en el índice 14 del snapshot y en cents, computado contra el objetivo y
el rate vigentes (`usableRangeCents` en Kotlin, `null` sin objetivo). Un consumidor no tiene que
conocer `H` ni rehacer la conversión: dibuja el tramo donde la aguja es confiable y advierte en
el resto. Que ese número y la guarda no puedan divergir lo verifica
`AnalysisThread.ThePublishedRangePredictsWhereTheFineReadingExists` a 0,97 y 1,03 del rango
publicado, que falla tanto si el rango se **infla** como si se **achica**.

**Cadencia (REQ-057, D31).** El snapshot se publica cada 1024 frames de audio analizado (≈ 23 ms a
44,1 kHz), con el rms de esos 1024 frames, y nunca antes de los primeros 1024. La serie de
publicaciones no depende del tamaño de bloque con el que llega el audio
(`BlockSize.ThePublicationSeriesDoesNotDependOnTheBlockSize`, y sobre `corpus-v1`
`CorpusRobustness.TheRatchetHoldsForEveryBlockSize` con bloques de 16, 256, 1000 y 8192).

El modo rápido decide una vez por veredicto grueso nuevo dentro del tick, no una vez por drenado:
sus contadores (soltar el enganche tras `kSilentUpdatesToRelease` lecturas sin altura, el «quieto»
de `kStableCentsPerUpdate`) cuentan veredictos. En vivo, soltar el enganche en silencio pasa a
tardar tres veredictos del detector en vez de tres drenados (propuesta, a medir en device).

**Costo.** Las cuatro rejillas cuadruplican el fino; el costo en host (Debug) lo imprime
`InterleavedGridsCost.TheFourGridsCostAFractionOfRealTimeAndTheNumberIsPrinted` (en su corrida de
REQ-057 S1, host de desarrollo a 48 kHz: el fino de 4 parciales de 0,134 % a 0,537 % del tiempo real y el análisis
entero, 1,81 %; la cifra vigente es la que imprime el test). En device no está medido.

**Latencias por modo:** detección gruesa ≤ 100 ms desde el onset; conmutación de cuerda en modo
rápido 85,3 ms; el strobe declara convergencia cuando 1σ ≤ 0,1 cents, lo que en la cuerda más
grave del catálogo (B0) ocurre pasados los **0,5 s** y en A4 es inmediato.

🔴 **σ NO ES SUFICIENTE PARA DECLARAR CONVERGENCIA, y esa es la segunda vez que pasa (REQ-009).**
`1σ ≤ 0,1 cents` es **necesario y no suficiente**: σ mide la linealidad del ajuste, y una señal
con un **salto de fase** —el ring de análisis pisó frames, o la captura entregó audio no
contiguo— sigue ajustando bien a una recta. Medido: con el ring desbordando, el motor publicaba
`CONVERGIDO` con la lectura a **1,04 cents** del valor real —10× el presupuesto— y σ en
**0,024**, o sea holgadamente por debajo del umbral.

Por eso el motor **descarta la integración entera** en cuanto pierde un solo frame, en vez de
apoyarse en σ. Una discontinuidad invalida la fase acumulada por la misma razón que la invalida
un cambio de objetivo. El costo es **latencia de aguja, no exactitud**: la lectura tarda más en
aparecer y ninguna sale de mezclar dos trozos de señal. Con la guarda puesta, el peor error entre
todas las lecturas publicadas como convergidas mientras el ring desborda es de **3,8·10⁻⁶ cents**
(20 corridas × 150 ventanas); sin ella, **0,1875** — 4× el presupuesto.

🔴 **Y HAY UN SEGUNDO EJE: LA CAPTURA (REQ-009 S3).** El ring de análisis no es el único lugar
donde el audio deja de ser contiguo. La plataforma también tira audio de entrada —Oboe cuenta
xruns, CoreAudio descarta bloques cuando su ring de captura desborda y entrega silencio cuando se
queda corto— y ese hueco **el motor no lo puede ver**: llega como frames consecutivos, con
`droppedFrames` en **0**. Medido: hasta **2,15 cents** de error, 21× el presupuesto, con el motor
diciendo `CONVERGIDO`.

**σ tampoco lo ataja acá, y está peor que en el eje del ring: está ANTI-correlacionada.** En la
peor fila del barrido —2,15 cents— σ vale **0,00098**, la más chica de todas las filas con falla.
Es la cuarta vez que este repo lo mide. Por eso la detección **tiene que venir de la plataforma**,
que ya sabe que tiró audio.

| eje | quién lo detecta | qué mide el snapshot |
|---|---|---|
| ring de análisis | el propio motor (`droppedFrames`) | Δ por ventana |
| **captura** | **el backend / el stream de entrada** | una **costura posicionada** |

**El aviso lleva posición, no cantidad**, y son tres topologías con tres fuentes distintas: en
Android el afinador **no pasa por el ring del backend** —`wma_tuner_start` hace que el nodo de
entrada abra su propio stream de Oboe—, así que la fuente ahí son los xruns de **ese** stream; en
iOS y en USB la captura entra por el callback de salida y la fuente es el ring del backend.

🔴 **Y la posición puede caer ADELANTE del escritor.** Cuando el detector tiene su propia cola, el
hueco no se entrega en el bloque siguiente: se entrega cuando esa cola se drene. El ring de
captura de CoreAudio es de **1 segundo** de estéreo, así que un overrun se detecta con ~**48000
frames** por delante — **5,9× la capacidad entera** del ring de análisis (8192 frames). Un aviso
sin esa distancia hace que el lector descarte audio sano, se ponga al día, y cruce el salto real
sin costura pendiente: `CONVERGIDO` sobre una lectura equivocada. **Sobra-descartar es latencia;
faltar es una lectura falsa.**

**Lo que el motor NO promete acá, dicho antes de que sorprenda**: una plataforma que pierde audio
de captura y **no lo reporta** es indetectable. El motor converge sobre ese hueco y no tiene cómo
saberlo. Está escrito como test (`CaptureDiscontinuity.NobodyReportedItSoNobodyCanKnow`) para que
el día que entre un backend nuevo sin reportar, se sepa exactamente qué se pierde.

**El motor lo publica**, en el índice 15 del snapshot (`inputDiscontinuity` en Kotlin): distingue
*"todavía no convergí"* —esperar— de *"la entrada llegó rota"* —revisar el cable—, que son dos
acciones opuestas para el usuario. **No** es `droppedFrames > 0`: ese contador es acumulado y
monótono, así que quedaría trabado el resto de la sesión. Verificado por `AnalysisThreadReq009.*`,
que afirma las dos direcciones — que la marca se levante con el hueco y que **se baje sola** al
recuperarse. El eje de captura lo verifican `CaptureDiscontinuity.*` (el camino entero, las tres
topologías) y `CaptureGapMailbox.*` / `CaptureSeamAhead.*` (el cruce de threads de iOS y USB, con
compuerta).

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
strobe_worst_error_cents = 0.000954
strobe_worst_sigma_cents = 0.001651
coarse_worst_error_cents = 0.2124
coarse_min_clarity = 0.967426
fast_mode_sweep_switches = 0
intonation_worst_error_cents = 0.0001
inharmonicity_max_correction_cents = 2.4678
crystal_50ppm_cents = 0.0866
-->
