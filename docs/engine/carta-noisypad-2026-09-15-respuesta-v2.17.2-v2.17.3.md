---
title: "NoisyPad → watermelon-audio · respuesta a los avisos de v2.17.2 y v2.17.3 (el brillo y el pitch)"
type: reference
status: current
created: 2026-09-15
---

# Respuesta de NoisyPad a los avisos de v2.17.2 y v2.17.3

**Recibida el 2026-09-15.** Redactada del lado de NoisyPad el 14/09 a las 21:31, junto con su bump
`2.17.1 → 2.17.3` (`docs/audio-bump/req_bump_audio_2_17_3.md`, commit `7875121c`), o sea **antes**
de que saliera el aviso de v2.17.4 (15/09), que sigue sin respuesta. Contesta
`respuesta-noisypad-2026-09-14-v2.17.2.md` (MINI-028, el default #2 con identidad 2.01) y
`respuesta-noisypad-2026-09-14-v2.17.3.md` (MINI-025, el pitch fuera del keytrack), incluida la
pregunta I-3 de las dos. Texto íntegro, como llegó.

> 🔴 **Esto es lo que dijo el consumidor, no lo que verificamos.** Lo que sí se verificó contra
> nuestro árbol está anotado al final, separado a propósito.

---
# NoisyPad → watermelon-audio · respuesta a los avisos de v2.17.2 y v2.17.3

**2026-09-14.** Contesta `nota-de-bump-2026-09-14-default-2-identidad-2-01.md` (2.17.2, MINI-028) y
`nota-de-bump-2026-09-14-pitch-fuera-del-keytrack.md` (2.17.3, MINI-025). Adoptadas las dos en un
solo bump (PR de NoisyPad `feat/audio-2-17-3`); todo medido en el Moto G42, export del mix por el
canal de debug, mismo APK antes (2.17.1) y después (2.17.3), mismo guion de toques inyectado por
`adb`. Cents por correlación cruzada del espectro en log-frecuencia (5 c/bin) entre las dos tomas
del mismo golpe —para un sample que se mueve entero es más robusto que el pico— con el pico
parabólico como segundo testigo.

## 1. 2.17.3, el pitch — lo que dice la tabla, al cent

- **`8:116 Concert Bass Drum`, tecla 36 (Drum Grid): −780 c** (r = 0,93; por pico 36,5 → 23,4 Hz =
  −769 c). Las otras 10 teclas del mapa GM (38…51, 39): −780 ×8, −775, −745 (pico −757…−766).
  El keytrack ya estaba bien antes (las 11 celdas dan 78/235/392/…/118 c respecto de la 36 contra
  80/240/400/…/120 esperados con `scaleTuning` 40): lo que se movió es el offset, y lo que ustedes
  dijeron.
- **`128:24 Electronic`, toms 45 / 48 / 50 / 43 (Synth Drum): +350 / +600 / +700 / +250 c** (r 0,96),
  los cuatro exactos a la tabla por correlación (por pico +246/+590/+691/+217: es un sweep).
- **`8:118 808 Tom`, tecla 72 (Note Grid): −350 c** (r 0,86; pico −389; tabla −361,5). Sweep, ±40 c.
- Niveles: −1,1 … +2,7 dB en los que se mueven (la ventana ve otra porción del sample al cambiar de
  velocidad de lectura); 0,0 dB en los que no.

## 2. La pregunta I-3: ¿algo fuera de la lista suena distinto? ¿algo de la lista suena peor?

**Fuera de la lista, pitch: nada.** Con número:
- `128:0 Standard`, 11 teclas del mapa GM (36, 38, 42, 46, 45, 48, 50, 43, 49, 51, 39): **0 c en las
  11** (r 0,89–1,00), nivel **0,0 dB en 10** y −0,2 en una.
- `128:24 Electronic`, las 7 teclas que no son toms (36, 38, 42, 46, 49, 51, 39): **0 c en las 7**;
  el hi-hat cerrado es idéntico cuadro a cuadro de 10 ms (−35, −43, −47, −51… dBFS en las dos).
- `0:81 Saw Lead` (XY a y=0,97/0,60/0,30 y Note Grid tecla 72) y `0:89 Warm Pad` (XY): **0 c**,
  nivel 0,0 dB (Warm Pad +0,6 en una apoyada: ancla del onset en un ataque lento).
- La tabla de parámetros por engine: diff vacío. El smoke de los 7 engines, el rack y el looper:
  iguales salvo la toma del propio smoke, que era un 808 Tom (le cambió el pitch, como corresponde).

**De la lista, peor: ninguno, con lo que sabemos medir.** No hay un preset de los medidos que haya
perdido nivel, que haya ganado un artefacto o que no cierre con la tabla. Lo que NO podemos decir es
que el Concert Bass Drum "suene mejor" a 23 Hz que a 36: a la tecla 36 queda por debajo de lo que un
parlante de teléfono reproduce, y eso es una decisión del autor del font que ustedes ahora honran,
no un defecto del motor. Si un usuario lo reporta, va con número y contra FluidSynth, como pidieron.

**Un límite del lado nuestro, no de ustedes:** el segundo punto que queríamos medir, `Standard` Wood
Block (teclas 76/77, −1594/−1319 c), **no es alcanzable desde NoisyPad**: un kit del banco 128
fuerza el Drum Grid y el mapa GM 4×3/4×4 no tiene 76/77. Por eso el segundo punto fue el kit
Electronic. Anotado como deuda de producto en nuestro doc del bump.

## 3. 2.17.2, el brillo — lo que midieron, reproducido en el grid

`0:81 Saw Lead`, Note Grid tecla 72, dos golpes en la misma celda:
- **golpe suave (velocity ≈ 0,35): centroide 960 → 1212 Hz, +26 %**, nivel −40,0 → −40,0 dBFS,
  pitch 0 c. Ustedes: 1195 → 1546 a velocity 38, +29 %. Mismo fenómeno, misma magnitud.
- golpe fuerte (≈ 0,96): 1251 → 1274 (+1,9 %), nivel −24,9 → −24,9.
- XY (velocity 1,0 + expresión): 927 / 927 / 927 Hz antes y después, niveles iguales. Como dijeron:
  a velocity 127 el default valía 0.
- El nivel no se mueve: −15,1 dB entre los dos golpes en las dos builds (la curva de 800 cB).

Y una observación que la nota anticipa ("algún borrado alcanza a los 269"): en los kits, con
velocity ≈ 0,62, varias teclas suben de centroide con pitch y nivel idénticos —snare de `Standard`
1246 → 1895 Hz, hi-hats y cymbals +5…+20 %—. Es MINI-028 alcanzando a la percusión, no MINI-025.
No es una queja: a esa velocity la batería suena como el autor la afinó. Si quieren ese detalle por
tecla, los JSON por golpe (pico, centroide, RMS, espectro log-f) están en el scratchpad de la sesión.

## 4. Nada pendiente de nuestro lado

Cero superficie nueva de API entre 2.17.1 y 2.17.3 (sólo el KDoc de `sfNoteOn`, que describe lo
que ya hacemos desde A1: velocity 1,0 en el ataque y expresión re-enviada en el mismo frame,
después). Los WAV de las 14 tomas (7 × 2 builds) se pueden mandar si sirven.

---

## Lo que verificamos de nuestro lado (2026-09-15)

Cada número de la carta, contra el archivo (`read-sf2-modulators.py --pitch` / `--presets` sobre
GeneralUser 1.471, el `.sf3` que NoisyPad shippea) y contra el motor (la suite de host). Ninguno
contradice lo que dijimos; tres piden un matiz.

- **§1, el pitch, contra el archivo — cierra al cent.** `8:116` a la 36: **−780** es lo que
  declara la capa `Taiko Drum` (root 60, scale 40, coarse −13, fine 0). `128:24 Electronic`, toms
  43/45/48/50: **+250/+350/+600/+700** (`Synth Drum`, scale 50, coarse 5/7/12/14). `8:118 808 Tom`:
  **−361,5** (`TR-808 Toms` sobre `Sine-1500Hz`, coarse −7 fine −23 con scale 50). Y lo que dicen
  que **no** se movió, no está en la lista: ninguna de las 11 teclas del mapa GM de `128:0 Standard`
  ni las 7 no-tom de `128:24` tiene offset con `scaleTuning ≠ 100`; `0:81` y `0:89` tampoco.
- **Matiz 1 — el bombo de concierto es DOS capas, no un sample.** `Concert Bass Drum 2` apila
  `Taiko Drum` (−780, todas las velocities, sin atenuación) con `Timpani Hard` (**−810**, en cuatro
  capas de velocity: 0–75 / 76–93 / 94–110 / 111–127 con 150 / 100 / 50 / 0 cB). A su velocity
  ≈ 0,62 (MIDI 79) suena la capa 76–93, 4 dB abajo del Taiko. El aviso lo decía como "−780/−810";
  el −780 exacto por correlación es el Taiko dominando, y los −775 / −745 de dos teclas son lo que
  una correlación sobre una mezcla de dos desplazamientos distintos puede dar. No es un residuo del
  motor: en el host las dos voces resuelven 3740 y 3690 c
  (`GeneralUserConcertBassDrumResolvesItsCoarseTuneOutsideTheKeytrack`, sobre el mismo `.sf3`).
- **Matiz 2 — su keytrack "ya estaba bien" viene un 2 % corto, y es consistente.** Los diez
  valores respecto de la 36 (78/235/392/353/471/550/275/511/589/118 contra 80/240/…/120) dan un
  cociente de **0,975–0,983, media 0,981** — 39,2 c por tecla donde el archivo declara 40. Diez de
  diez en la misma dirección no es ruido del estimador. **No es el motor**: en el host el keytrack
  mide exacto al cent contra una senoide con scale 50 (`TheKeytrackScalesOnlyTheDistanceToTheRoot`,
  tolerancia 1 c) y la fórmula de MINI-025 se cotejó con FluidSynth 2.6.0 a 0,00 c. La hipótesis
  es la medición sobre un tambor de dos capas con filtro fijo y barrido de pitch: la correlación
  del espectro entero contra un `initialFilterFc` que no sigue a la tecla sesga el corrimiento
  hacia abajo. Está fuera de lo que 2.17.3 cambió y ellos mismos lo descuentan; queda anotado
  porque un 2 % sistemático en un estimador es lo que después hace parecer "±20 c" a un residuo
  real. Los WAV de la 36 y la 51 de 2.17.1 lo cierran en diez minutos.
- **Matiz 3 — el Wood Block va al revés, y también es doble.** La carta dice "teclas 76/77,
  −1594/−1319": el archivo dice **76 → −1319** (`Wood Block_1`) y **77 → −1594**, y cada tecla
  apila además un `Wood Block_2` a −75 / −350. El aviso lo escribió sin teclas, así que el orden
  no salió de nosotros. Sin consecuencia: no lo pudieron medir (el mapa GM del Drum Grid no tiene
  76/77 — deuda de producto de ellos, no nuestra).
- **§3, el brillo, contra el archivo — cierra.** `0:81 Saw Lead` declara velocity → nivel de
  **800 cB cóncava** y borra el default #2 con identidad 2.01 (`--presets`). Los −15,1 dB entre sus
  dos golpes son **exactamente** la curva a MIDI 43 (0,34 × 127) contra 122 (0,96 × 127):
  15,10 dB calculados con `fluid_concave`. Su +26 % de centroide a velocity ≈ 0,35 contra nuestro
  +29 % a velocity 38 (1195 → 1546, `respuesta-noisypad-2026-09-14-v2.17.2.md`) es el mismo
  fenómeno a velocities distintas; el +1,9 % a 0,96 y el 0 % en el XY son lo que la nota predijo
  ("a velocity 127 el default valía 0"). El snare de `Standard` 1246 → 1895 Hz a velocity 79 es
  el default #2 desapareciendo en las zonas que lo borran (−907 c de corte a esa velocity); la
  tabla `--presets` marca `128:0` como "borrado 2.01 / default" por zona, o sea mezcla.
- **§4, cero superficie**: `git diff v2.17.1..v2.17.3 -- '*.kt'` sin tests toca dos archivos,
  sólo KDoc (`ISoundFontBridge.sfNoteOn` y su eco en `AudioNativeBridge`). Y `v2.17.3..v2.17.4`
  no toca ningún `.kt`: el bump a 2.17.4 tampoco tiene nada que adoptar.
- **Un dato de su doc del bump que no está en la carta**: *"un golpe de sacrificio (el motor
  recorta el primero ~0,1 s)"* al grabar en el looper. Si es reproducible con número (cuánto se
  recorta, a partir de `Record` o del primer note-on), es un disparador nuestro, no un detalle
  de su arnés. Va en la respuesta.
- **Pendiente con ellos**: el aviso de v2.17.4 (las envolventes, el que más se oye) sigue sin
  respuesta — la carta es anterior a él. Los WAV ofrecidos: se piden los dos del bombo (arriba).
