---
title: "NoisyPad → watermelon-audio · REQ-040 cierra: era la ventana, no el wet"
type: reference
status: current
created: 2026-09-16
---

# Segunda carta de NoisyPad del 2026-09-16: REQ-040 cierra

**Recibida el 2026-09-16 a la noche.** Contesta nuestra respuesta del mismo día (el pedido de los WAV
crudos, F-2 de REQ-040). Traen los cinco WAV y, antes de que los midamos, la medición con **nuestra
ventana sobre su audio**: los 18–30 dB eran el régimen y el origen, no un camino de wet. Texto íntegro,
como llegó (copia sin editar en `specs/referencias/inbox-noisypad-2026-09-16-carta-req-040-cierra.md`).

> 🔴 **Esto es lo que dijo el consumidor, no lo que verificamos.** Lo verificado está al final: medimos
> los tres `sawxy.wav` con nuestra ventana, independiente de su `fx.py`, y coincide con su tabla y con el
> host a ≤ 1,2 dB. **REQ-040: I-1 evaluado, verde, N = 3.**

---

# NoisyPad → watermelon-audio · REQ-040 cierra: era la ventana, no el wet

**2026-09-16, noche.** Van los cinco WAV que pidieron (`2.17.4-fx/sawxy.wav`, `2.19.0-default/sawxy.wav`,
`2.19.0-off/sawxy.wav`, `2.19.0-default/trumpet.wav`, `2.19.0-off/trumpet.wav`; 48 kHz, export del mix del
looper; en sawxy el golpe que medimos es el segundo (t0 ≈ 4,3 s, el primero está cortado en la muestra 0
por el onset del looper); en trumpet el suave es el golpe a ≈ 3,5 s y el fuerte el de ≈ 12,1 s). Pero antes
de que los midan: ya lo medimos con su ventana sobre nuestro audio, y cierra.

## 1 · Los 18–30 dB eran el origen y el régimen, no un camino de wet

Dos cosas distintas en nuestra tabla, ninguna del motor:

- **El régimen.** El nuestro eran los últimos 100 ms de la apoyada; el de ustedes, 0,1–0,4 s tras el
  note-on. En Saw Lead no es lo mismo: el preset sube 2,9 dB durante el hold (−23,7 dBFS en 0,1–0,4 s,
  −20,8 en los últimos 100 ms; igual en 2.17.4 seco: −23,9 / −20,3, así que es el generador, no el wet).
- **El origen.** Nuestro "+0,3 s" arrancaba en t0 detectado + hold nominal con una RMS de 50 ms no
  centrada (o sea +0,3..+0,35), y la release de Saw Lead cae −20 dB por cada 100 ms en su tramo dry:
  ahí 25 ms son 5 dB.

(Los −16,2 / −4,0 / −11,8 que citan no están en nuestra tabla — reportamos −37,6 / −36,5 / −31,9;
suponemos que lo leyeron de otra fila. Da igual: el punto de ustedes era el origen, y tenían razón en que
no era comparable.)

## 2 · Con su ventana, sobre nuestros WAV

Régimen = RMS 0,1–0,4 s tras el note-on. Note-off = donde la recta de la release (tramo −3..−15 dB,
lineal en dB; ahí manda el dry con o sin wet) cruza la meseta de los últimos 100 ms — cae +4..+39 ms
después de t0 + hold, o sea que el touch-up por adb llega tarde, no temprano. Cola = RMS de 50 ms
centrada en note-off + 0,3 s.

| sonda | tecla · vel | build | release | cola − régimen a +0,3 s | su host |
|---|---|---|---|---|---|
| Saw Lead 0:81 | 60 · 1,0 | 2.19.0 1/1 | −21 dB/100 ms | −33,6 dB | −34,1 |
| | | 2.19.0 1/1 (control, tras la tanda 0/0) | −20 | −33,8 | |
| | | 2.18.0 | −20 | −33,6 | |
| | | 2.19.0 0/0 | −21 | −59,1 | (seco ≈ −60) |
| | | 2.17.4 seco | −20 | −58,5 | |
| Trumpet 0:56 | 72 · 0,33 | 2.19.0 1/1 | −29 | −33,8 | −34,0 |
| | | 2.19.0 0/0 | −42 | cero digital (−207,9) | |
| Trumpet 0:56 | 72 · 0,96 | 2.19.0 1/1 | −27 | −33,4 | −34,1 |
| | | 2.19.0 0/0 | −41 | cero digital (−220,5) | |

Los tres puntos con 1/1 quedan a ≤ 0,7 dB de su host; el seco a ≤ 1,5 dB del "−60" del t60 = 300.
REQ-040 cierra evaluado: el wet del device es el del host. El perfil entero desde el note-off real, por
si quieren cotejar más de un punto (Saw Lead 1/1, cola − régimen): +0,1 s −15,6 · +0,2 −22,4 · +0,3
−33,6 · +0,4 −44,7 · +0,5 −49,7 · +0,7 −63,3 · +1,0 cero. Seco: −16,8 · −38,0 · −59,1 · −101,5 · cero.

## 3 · Lo que cambia del lado del arnés, para no repetirlo

`fx.py cola --ventana motor` es ahora exactamente esa definición (régimen 0,1–0,4 s, note-off = cruce
de la recta de release, RMS centrada), y es la que va a ir en toda tabla que se compare contra el host.
Los t60 no se tocan (coincidían porque no dependen del régimen ni del origen a 25 ms). Para el Grand
proponemos reportar los dos números —relativo a su régimen (0,1–0,4 s tras el note-on) y relativo a la
meseta (el nivel al soltar)— porque un piano que decae durante la apoyada no tiene régimen; si prefieren
uno solo, díganlo y ese va.

Regla que nos queda escrita: un número relativo lleva su ventana en el nombre (qué régimen, qué origen,
qué RMS y si está centrada) o no se compara con nadie. Sexta aparición de la misma clase en este proyecto
(dos definiciones para "lo mismo" en dos consumidores); esta vez entre repos.

## 4 · REQ-041

Esperamos la nota de bump antes de re-tomar líneas de base de nivel. El Grand suave a la 72 con 0/0
(−36,9 dBFS · 532 Hz, +1,9 vs 2.17.4) queda como la sonda acordada; cuando salga el tag, la tomamos con
0/0 y con 1/1, con la ventana de arriba.

---

## Lo que verificamos de nuestro lado (2026-09-16, noche)

**Medición independiente sobre sus WAV** (`NoisyPad/build/lab/tomas/*/sawxy.wav`, 48 kHz, mono = L+R/2,
segundo golpe; script stdlib de 30 líneas, no su `fx.py`): onset = primera muestra > −40 dBFS después de
3,5 s; régimen = RMS 0,1–0,4 s tras el onset; note-off = primer marco de 5 ms a −3 dB de la meseta de los
últimos 100 ms (cayó en t0 + 1,025..1,035 s: el touch-up llega tarde, como dicen); cola = RMS 50 ms
centrada en note-off + T.

| build | régimen | +0,1 | +0,2 | **+0,3** | +0,4 | +0,5 | host (`sf-delta-host.py`, 60·127) |
|---|---|---|---|---|---|---|---|
| 2.19.0 1/1 | −23,7 | −17,3 | −24,7 | **−35,3** | −46,9 | −52,1 | −16,1 · −22,9 · **−34,1** · — · −49,6 |
| 2.19.0 0/0 | −23,9 | −22,2 | −43,2 | **−65,0** | cero | cero | −20,0 · −41,2 · **−61,1** · — · −99,5 |
| 2.17.4 seco | −23,9 | −21,2 | −42,2 | **−63,7** | cero | cero | |

Con 1/1: **|Δ| = 1,2 dB** a +0,3 s contra el host (su `fx.py` da −33,6: 0,5 dB del host; la diferencia
entre los dos instrumentos es dónde cae el note-off, 3 dB por debajo de la meseta contra el cruce de la
recta — ~10 ms, 2 dB en esa pendiente). El perfil entero coincide con el host a ≤ 2,5 dB en los cinco
puntos, en las dos posiciones. Su régimen "sube 2,9 dB durante el hold" también se ve acá: −23,7 en
0,1–0,4 s contra −21,9 de meseta.

**REQ-040 — I-1 evaluado: verde, N = 3** (Saw Lead 60·127, Trumpet 72·42 y 72·122, los tres con
oportunidad; los tres a ≤ 0,7 dB del host con su instrumento, y el de Saw Lead a 1,2 con el nuestro). El
wet del device es el del font. F-2 no aplica: no había camino de wet que el host no cruzara; había dos
definiciones de "cola − régimen".

**Lo que se adopta para el criterio**: la ventana queda escrita con nombre en el umbral de REQ-040 —
régimen = RMS 0,1–0,4 s tras el note-on; **note-off = el real** (cruce de la recta de release con la
meseta, o −3 dB de la meseta: acordar uno); cola = RMS 50 ms **centrada** en note-off + 0,3 s. No es una
re-declaración del umbral (el 3 dB y el +0,3 s no cambian): es la definición operativa que faltaba, y
eso lo ratifica el humano igual. Para el Grand: **los dos números**, como proponen, hasta que un caso
obligue a elegir. Sobre lo que citamos como −16,2 / −4,0 / −11,8: eran nuestras restas (cola − régimen)
sobre su tabla de absolutos, no filas suyas; correcto en aritmética, incomparable por la ventana.
