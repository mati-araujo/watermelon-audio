---
title: "Tunio → watermelon-audio · cierre de REQ-037, y 2.16.4 adoptada"
type: reference
status: current
created: 2026-09-11
---

# Respuesta de Tunio al cierre de REQ-037

**Recibida el 2026-09-11.** Contesta `respuesta-tunio-2026-09-11-req-037-cerrado.md`. Texto
íntegro, como llegó.

> 🔴 **Esto es lo que dijo el consumidor, no lo que verificamos.** Lo que sí se verificó contra
> nuestro árbol está anotado al final, separado a propósito.

---

**0 · Recibido, y de acuerdo con el cierre.** Cinco minutos. Lo tomamos como lo que es: los dos
leímos el mismo KDoc de `detectedHz` y llegamos al mismo lugar por puertas distintas. Que se haya
revertido antes de salir en un artefacto es la decisión correcta y la que hubiéramos pedido.

**1 · 2.16.4 ya está adoptada, y lo afirmamos sobre el artefacto, no sobre la carta.** Lo
adoptamos el mismo 11/09 (MINI-007, nuestro PR #44). Su carta decía "no van a ver
`automaticStringSelection` en ningún `.aar`"; nosotros lo leímos del `.aar`: 713 clases, las
mismas; `ITuner` idéntica; 1918 líneas de firmas de `api` + `domain.tuner` sin una diferencia; los
mismos doce `.so`. La suite pasó 67/67 sin tocar un test — que era justo la alarma: si hubiera
hecho falta adaptar uno, algo de la superficie habría cambiado.

Y una nota para su libro, no para el nuestro: 2.16.4 no cuenta como evidencia sobre el defecto de
octava. Nuestro criterio de muerte de la guarda del `2·f0` es por versión del motor que toque el
tuner, y ésta no lo toca. Sigue puesta.

**2 · Las dos bases de índice: la dejamos escrita donde se va a leer.** `selectedString` 1-based,
`lockedString` 0-based. Hoy no nos muerde por la razón que dicen —elegimos por `detectedHz` y
escribimos `selectedString`— y por eso mismo es la clase de trampa que nos gusta menos: no falla,
contesta. Queda anotada en la etapa que está abierta y va al KDoc del único archivo nuestro que
traduce `TunerSnapshot` campo por campo. Gracias por decirla antes de que la pisara nadie.

**3 · Lo del strobe.** Que cada `selectedString` nuevo reinicie la integración lo sabíamos por
medirlo, no por leerlo; que el modo rápido interno pague lo mismo al reenganchar es el dato que
nos faltaba para dejar de mirarlo como costo diferencial. La histéresis de 1,5 cents se queda.

**4 · De nuestro lado.** Nada pendiente con ustedes. `cuerdaAjena` con cuerda fijada a mano se
queda, como dicen. Y el CHANGELOG de 2.16.4 con la feature y su revert nos parece la forma
correcta de contar cinco minutos.

---

## Lo que verificamos de nuestro lado

- **`v2.16.4` es el artefacto que describen**: es el tag que cortó el revert de #279 (#280, #282) y
  está publicado en las cuatro coordenadas del registro. Que hayan leído del `.aar` la misma
  `ITuner` que 2.16.3 es lo esperado: la única diferencia entre 2.16.3 y 2.16.4 es la feature y su
  revert, que se cancelan en la superficie.
- **"2.16.4 no cuenta como evidencia sobre el defecto de octava"**: correcto, y es nuestro también.
  Ninguna versión desde 2.16.3 toca el afinador; el próximo que lo haga lleva su aviso.
- **Nada queda pendiente de este intercambio.** REQ-037 está archivado; la respuesta del 11/09 se
  marca enviada y contestada con esta carta.
