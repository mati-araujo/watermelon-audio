---
title: "Respuesta a Tunio — REQ-037 cerrado sin implementar, y tres cosas que aprendimos construyéndolo"
type: reference
status: current
created: 2026-09-11
---

# Respuesta a Tunio — 2026-09-11 · REQ-037 cerrado sin implementar

**watermelon-audio → Tunio.** Contesta su segunda respuesta al aviso del 10/09.

> **Enviada el 2026-09-11. Contestada el mismo día**: `carta-tunio-2026-09-11-cierre-req-037.md`
> — 2.16.4 adoptada (su PR #44, leída del `.aar`), las dos bases de índice anotadas, nada
> pendiente. Dos afirmaciones de esta carta envejecieron entre escribirla y publicarla, y están
> corregidas en su lugar con la marca *corregido después de enviada* (§2 y §4c).

Se cruzó con una nota nuestra que no les llegó, y mejor así: esa nota describía algo que
ahora vamos a revertir. Acá va lo que pasó de nuestro lado, lo que decidimos, y tres cosas
que aprendimos construyéndolo y que sí les sirven.

## 1 · Lo que pasó, con hora

Sobre su primera carta —*"es la funcionalidad central, bloqueada desde agosto, nos serviría
un flag"*— implementamos el modo automático del motor como superficie pública:
`ITuner.automaticStringSelection`. Lo mergeamos a `master` el 10/09 a las **20:35**.

Su `EleccionDeCuerda.kt` tiene commit de las **20:30** del mismo día. Cinco minutos.
Dos equipos construyendo la misma capacidad en paralelo sin saberlo.

## 2 · Lo decidimos: se revierte, y nunca va a salir en un artefacto

Ya está revertido y ya salió la release que lo cuenta: **`v2.16.4`**, cortada y publicada
el 11/09 con la feature y su revert (`24092ac`), que se cancelan en la superficie. **No van a
ver `automaticStringSelection` en ningún `.aar`.**

*(Corregido después de enviada: la carta salió diciendo "todavía no hay release que lo contenga,
el último tag es `v2.16.3`, lo revertimos antes de que exista". Entre escribirla y publicarla se
cortó `v2.16.4`; la afirmación que importa —ningún `.aar` con la superficie— no cambió, y Tunio
la verificó sobre el artefacto.)*

No es por orgullo ni por ahorrar: es nuestra regla. Algo entra a la API pública porque un
consumidor real lo necesita, y el único consumidor posible acaba de decir que no lo necesita
y no lo va a usar. Una capacidad pública sin consumidor es contrato de compatibilidad para
siempre a cambio de nada. El propio requisito tenía escrito que si esto pasaba, el
correctivo **no** era implementar.

Y coincidimos con ustedes en el fondo: **la puerta pública para el automático ya existía**,
del lado de ustedes — `detectedHz` con su claridad, y `selectedString` en caliente. El KDoc de
`detectedHz` lo decía desde el principio: *"es lo que le permite a la app saber qué cuerda
está sonando para después empujar el objetivo"*. Nosotros preguntamos *"¿puede el motor
elegir solo?"* y nunca preguntamos *"¿puede el consumidor construir un automático con lo que
ya publicamos?"*. Ustedes hicieron la pregunta correcta.

**REQ-037 queda cerrado sin implementar**, que era el desenlace que los dos preferíamos.

## 3 · Verificamos su regla contra el motor: es equivalente o mejor

Leímos `EleccionDeCuerda.kt` contra lo que sabemos de adentro:

- Reenganchar sólo sobre `NO_SIGNAL` con `spectralSupport != false` y claridad ≥ 0,8 es el
  criterio correcto: es el momento en que el motor ya declaró que la altura no es el objetivo.
- **La guarda del `2·f0` la tienen ustedes y el motor no.** El modo rápido interno elige el
  candidato más cercano en cents y no compensa la octava arriba de esos dos timbres. Para su
  producto, la suya es mejor.
- Cada `selectedString` nuevo empuja el objetivo al motor y **reinicia la integración del
  strobe**. Lo decimos porque es el costo que vigilábamos — pero el modo rápido interno hace
  exactamente lo mismo al reenganchar, así que no es un costo diferencial de su enfoque. Su
  histéresis de 1,5 cents y las cero espurias que midieron lo acotan bien.

## 4 · 🔴 Tres cosas que aprendimos y que les pueden servir

**a. Las dos bases de índice del afinador no coinciden.**

| | base |
|---|---|
| `ITuner.selectedString` | **1-based** — numera cuerdas como el músico |
| `TunerSnapshot.lockedString` | **0-based** — indexa el arreglo de candidatos que el motor recibió |

Hoy no les muerde: eligen por `detectedHz` y escriben `selectedString`. Pero `lockedString`
está en el snapshot público y el día que alguien lo lea asumiendo la base de `selectedString`,
prende el cap de al lado con cara de lectura válida. Queda documentado en los dos KDoc.

**b. `setTunerCandidates` suelta el enganche cada vez que se llama**, aunque le pasen los
mismos Hz — no compara con lo que ya tenía. Retiraron el pedido de abrirlo, así que hoy es
irrelevante para ustedes; lo dejamos anotado por si algún día vuelve.

**c. El CHANGELOG de `v2.16.4` lista la feature y su revert.** No es un error de ustedes ni
algo que haya que adoptar: es la traza de estos cinco minutos. La versión **fue 2.16.4**, no
2.17.0 — un minor sin feature real sería mentirles.

*(Corregido después de enviada: la carta lo decía en futuro —"la próxima release", "va a ser
2.16.4"—; ya está cortada y publicada.)*

## 5 · Lo que queda de nuestro lado

Nada que necesitemos de ustedes. El código existe en la historia (`c2e2b5d`) y se reabre el
día que un consumidor señale dónde lo llamaría — que es la precondición que deberíamos haber
respetado esta vez, y no respetamos.

`cuerdaAjena` con cuerda fijada a mano nos parece correcto que se quede.
