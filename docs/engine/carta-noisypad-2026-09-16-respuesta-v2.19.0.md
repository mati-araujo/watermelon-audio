---
title: "NoisyPad → watermelon-audio · respuesta al aviso de v2.19.0 (la perilla de la ambiencia)"
type: reference
status: current
created: 2026-09-16
---

# Respuesta de NoisyPad al aviso de v2.19.0

**Recibida el 2026-09-16 a la tarde.** Contesta `respuesta-noisypad-2026-09-15-v2.19.0.md` (enviado el
16/09). Adoptaron 2.19.0 (su PR #265), re-tomaron las cinco sondas después de encontrar dos estados de
la app que contaminaban la primera tanda, cierran AC-1..AC-7 con número, contestan la pregunta de
REQ-042 (0/0 contra 2.17.4 y el pop) y traen **la tabla que REQ-040 pedía** (cola con 1/1 y 0/0 por
preset, con tecla y velocity). Texto íntegro, como llegó (copia sin editar en
`specs/referencias/inbox-noisypad-2026-09-16-carta-v2.19.0.md`).

> 🔴 **Esto es lo que dijo el consumidor, no lo que verificamos.** Lo verificado contra el host está al
> final, separado a propósito. Resumen: **REQ-042 evaluado y vivo** (I-1: la perilla deja el seco, N = 3;
> el pop no se oye, con instrumento). **REQ-040: I-1 rojo en los tres presets con oportunidad, I-2 verde
> ⇒ F-2** — se paga la ceguera antes de tocar nada: se piden los WAV crudos del device. Los t60 coinciden
> con el host en las dos posiciones; sólo el punto fijo "+0,3 s" discrepa, y es inconsistente con sus
> propios t20/t40/t60.

---

# NoisyPad → watermelon-audio · respuesta al aviso de v2.19.0 (la perilla de la ambiencia)

**2026-09-16, tarde.** Adoptado: NoisyPad está en 2.19.0 (PR #265: toml + las cuatro líneas del KDoc de
`AudioEngineStateManager` — `supported = true`, `bridge.sfSetAmbience`, los dos getters). Todo medido en el
Moto G42 ZY32GB3CS2, mismo código de app y mismo guion por adb (`scripts/lab/tanda-fx.sh`) que las
referencias `2.17.4-fx` y `2.18.0-fx` del 15/09; export del mix por el canal de debug; rack en bypass
confirmado por dump en cada toma; `motor=R/C` (sus dos getters) confirmado por el canal en cada toma. Las
cuatro asunciones de nuestra carta previa al tag cerraron con la nota (nombres, 0..1 lineal, del
instrumento, y la rampa — gracias por medirla antes de decidir).

## 1 · Por qué la primera tanda no servía y qué se re-tomó

Las tomas de la mañana (`e4.log` 12:56) tenían el régimen 13 dB abajo (Saw Lead −34 vs −21 dBFS) y `fx.py`
veía 2 golpes donde eran 3. No era del motor (con 1/1 el render es byte a byte el de 2.18.0, y lo
confirmamos abajo): eran dos estados de la app que sobreviven entre sesiones y que el canal de debug no
reporta:

- La escena del rack cambió (RandomReso | Hall Reverb | Riser Reverb) y `toma.sh` bypaseaba tres stomps
  por nombre (Decimator/Tape Echo/Hall Reverb): RandomReso y Riser Reverb quedaron activos en las cinco
  sondas (confirmado en el dump: `RandomReso, active`, `Riser Reverb, active`). Régimen 13 dB abajo,
  centroides del Grand disparados. Ahora bypasea todo lo `, active` del dump y corta si queda alguno.
- Modo acorde + selector de acordes en vivo (7TH) + Octave Range 3 estaban encendidos: cada apoyada en el
  XY eran cuatro voces (+11,5 dB en Warm Pad) y el pad iba de C3 a C6 en vez de C2–C6 (el centro caía en
  otra nota). Se vio comparando las capturas de Record de cada toma contra las de la referencia. `toma.sh`
  ahora aborta si ve la tira de acordes y lee las voces sonando a mitad de la primera apoyada larga (1 en
  las 16 tomas de esta carta; con acordes daría 4).

Re-tomado con las dos cosas corregidas: `2.19.0-default` (1/1) y `2.19.0-off` (0/0), las cinco sondas cada
una, régimen −21,4 / −20,8 dBFS en Saw Lead y 3 golpes — mismas condiciones que `2.18.0-fx`. Más: un
control negativo (1/1 tomado después de la tanda 0/0), dos tomas de AC-6 con sólo Hall Reverb activo, y
tres del pop. Las tomas rack-on/rack-off/pop de la mañana se descartaron enteras.

## 2 · AC-1…AC-6, con sus números

**AC-1 ✓** params antes/después: diff vacío. Smoke de los 7 engines igual.

**AC-2 ✓** (1/1 = `2.18.0-fx`, ± 0,3 dB de régimen / ± 1 % de centroide / t60 ± 30 ms): Saw Lead régimen
−21,4 = −21,4 dBFS, t60 660 vs 670 ms, cola a +0,3 s −37,6 vs −35,9 (está sobre una pendiente de ~60 dB/s:
20–30 ms de jitter del note-off por adb; t20/t40/t60 130/350/660 vs 150/370/670). Strings −21,9 = −21,9,
t60 900 = 900, cola −21,6 vs −21,2. Warm Pad −24,3 vs −24,9 (+0,6: una apoyada por adb sobre un pad que
bate ±4 dB), t60 2340 = 2340. Grand fuerte/suave idénticos (−22,5 dBFS · 638 Hz / −36,3 · 531 Hz). Trumpet
suave/fuerte idénticos (−1 dB a 30 / 15 ms). Control negativo: Saw Lead 1/1 tomado después de la tanda 0/0
→ −35,8 dB / t60 680.

**AC-3 ✓** (0/0 = `2.17.4-fx` salvo MINI-027; t60 300 ± 30): Saw Lead régimen −20,8 vs −21,0 (Δ +0,2), t60
310 vs 300, cola a +0,3 s −63,2 vs −64,0 (Δ +0,8), cero digital a +0,5 s como 2.17.4. Strings −22,0 vs
−21,8 (Δ −0,2), t60 790 vs 820, cola −26,7 vs −24,6 (Δ −2,1). Warm Pad −24,7 vs −24,5, t60 2560 vs 2650
(su propia release), cola −7,8 vs −7,0 (Δ −0,8). Trumpet suave: cola cero digital en los dos (2.17.4 y
0/0). Grand fuerte −23,2 vs −23,3 · 641 vs 638 Hz. Grand suave 532 Hz (= 2.18.0 531; MINI-027 no es
ambiencia) y −36,9 dBFS (2.17.4: −38,8; el +1,9 es el dry de REQ-041 — con 1/1 da −36,3, o sea el wet vale
+0,6, como ustedes midieron). Trumpet suave −1 dB a 30 ms (se queda: MINI-027).

**AC-4 ✓** 0/0 por el canal ⇒ `sfGetAmbience*` = 0/0; cambio de preset 81 → 89 → 81 ⇒ sigue 0/0; 0.5/0.25 ⇒
getters 0.5/0.25 exactos.

**AC-5 ✓** toggle OFF en la hoja del SoundFont (tocado como un dedo) ⇒ 0/0; force-stop + arranque en frío ⇒
la app re-aplica y sus getters dicen 0/0; de nuevo ⇒ 1/1.

**AC-6 — ✓ en lo que mide, ✗ tal como lo habíamos escrito.** Saw Lead con sólo Hall Reverb (Concert Hall)
activo, 1/1 vs 0/0, dBFS absolutos (RMS 50 ms; onset fijado a mano porque la cola del rack no deja fijar la
grilla):

| | régimen | +0,1 s | +0,3 s | +0,5 s | +1,0 s | +2,0 s |
|---|---|---|---|---|---|---|
| rack + font 1/1 | −20,5 | −22,7 | −23,6 | −23,4 | −26,2 | −30,7 |
| rack + font 0/0 | −21,8 | −20,6 | −22,4 | −21,7 | −25,0 | −30,8 |
| Δ con − sin toggle | +1,3 | −2,1 | −1,2 | −1,7 | −1,1 | 0,0 |

Habíamos escrito "la diferencia a +0,3 s es la del font, ≈ +28 dB". Los +28 existen sin rack (abajo: +27,1
dB en Saw Lead), pero bajo Concert Hall la cola del rack está a −23 dBFS y la del font a 1/1 a −57: 34 dB
abajo. Lo que sí prueba la tabla: con el toggle OFF la cola es exactamente la del rack + el font seco (que
por AC-3 es 2.17.4), y el "doble reverb" con Concert Hall vale +1,3 dB de wash en la nota sostenida, no una
segunda cola. Nuestra razón de producto 1 era cierta como control y chica como efecto; la perilla vale por
el A/B y por el arnés.

**AC-7 ✓** suite completa 3195 tests / 17 módulos, detekt, `ios-smoke.sh`.

## 3 · Los dos criterios, en un solo A/B, por preset (relativo, a +0,3 s del note-off)

Para que rindan el mismo punto en host (`sf-delta-host.py --preset P --key K --vel V`): en el XY Pad el
dedo apoya al centro horizontal (4 octavas C2–C6 ⇒ MIDI 60, verificado con la tabla de voces del brief: 58
a −40 px, 62 a +40 px) con velocity 1,0 fija (`SfTouchExpression.FULL_VELOCITY`) y el nivel por expresión
= Y = 0,97 (`tsf_channel_set_volume`, −0,26 dB sobre todo). En el Note Grid cromático en C la celda de la
72 va de y≈1168 a ≈1364 px (medido por barrido con `nota.sh`): el golpe "fuerte" (y=1178) es localY 0,05 ⇒
velocity 0,96 (×127 ≈ 122) y el "suave" (y=1355) localY 0,95 ⇒ 0,33 (≈ 42), por
`PadVelocityCalculator.fallbackVelocity = 0,3 + 0,7·(1 − localY)` (modo Y_POSITION, el default). Apoyadas
de 1 s; cola − régimen con el régimen = RMS de 100 ms justo antes del note-off:

| preset (banco:programa) | tecla · velocity | 2.17.4 seco | 2.18.0 | 2.19.0 1/1 | 2.19.0 0/0 |
|---|---|---|---|---|---|
| Saw Lead (0:81) | 60 · 1,0 (expr 0,97) | −64,0 dB (rég −21,0; t60 300) | −35,9 (−21,4; 670) | −37,6 (−21,4; 660) | −63,2 (−20,8; 310) |
| Trumpet (0:56) | 72 · 0,33 | cero digital (rég −33,9; t60 190) | −33,4 (−32,5; 630) | −36,5 (−32,5; 610) | cero digital (−33,0; 210) |
| Trumpet (0:56) | 72 · 0,96 | −82,8 (rég −20,7; t60 240) | −31,9 (−20,1; 690) | −31,9 (−20,1; 700) | cero digital (−20,5; 190) |
| Stereo Strings Fast (0:48) | 60 · 1,0 (expr 0,97) | −24,6 (rég −21,8; 820) | −21,2 (−21,9; 900) | −21,6 (−21,9; 900) | −26,7 (−22,0; 790) |
| Warm Pad (0:89) | 60 · 1,0 (expr 0,97) | −7,0 (rég −24,5; 2650) | −6,2 (−24,9; 2340) | −6,0 (−24,3; 2340) | −7,8 (−24,7; 2560) |
| Stereo Grand (0:0) | 72 · 0,96 | −23,1 (rég −41,4; 640) | −17,0 (−41,2; 800) | −19,5 (−41,2; 780) | −19,7 (−41,4; 680) |
| Stereo Grand (0:0) | 72 · 0,33 | −27,0 (rég −56,3; 470) | −25,7 (−55,4; 540) | −24,2 (−55,4; 560) | −24,8 (−55,7; 480) |

**REQ-042 (la perilla):** con 0/0 contra 2.17.4, cola − régimen a +0,3 s: Saw Lead +0,8 dB, Strings −2,1,
Warm Pad −0,8, Trumpet suave cero digital como 2.17.4 y fuerte cero digital vs −82,8 (el piso). Ninguno
> 3 dB: la perilla deja el seco. Y el régimen coincide (Δ ≤ 0,2 dB en los cuatro), así que es "igual a
2.17.4", no sólo "igual relativo".

**REQ-040 (los sends):** con 1/1, Saw Lead −37,6 (2.18.0: −35,9), Trumpet suave −36,5 (−33,4) y fuerte
−31,9 (−31,9); y con 0/0 el control: −63,2 / cero / cero. La diferencia del font solo, 1/1 − 0/0, es +27,1
dB en Saw Lead. Strings, Warm Pad y el Grand no pasan el control de oportunidad (su dry tapa el wet a +0,3
s; el Grand además decae durante la apoyada, por eso su "régimen" está en −41/−56): van igual, y sus Δ
están dentro de la variabilidad entre tomas iguales (±1–2 dB).

## 4 · El pop: no se oye (y el número que su medida no vio)

Primero lo que no servía: nuestra toma de la mañana usaba Saw Lead + máx |Δx| por muestra, y la sierra
resetea cada ciclo con un |Δx| mayor que cualquier escalón — un escalón del 14 % inyectado daba +3 dB en
banda alta, indistinguible.

Sonda Warm Pad (0:89), MIDI 60, vel 1,0, apoyada de 6 s, rack en bypass; un sondeo de state ve la voz y
manda 1/1 → 0/0 a ~+2,0 s y 0/0 → 1/1 a ~+4,3 s (en régimen pleno; ±0,5 s porque cada state cuesta 0,3–0,5
s por adb, así que la métrica barre toda la apoyada y no depende del instante). Que los dos comandos
llegaron al motor (un `ok:true` no lo prueba) lo verificamos aparte: Saw Lead 1/1 → 0/0 a ~+1,9 s sin
volver a 1/1 ⇒ cola tras el note-off cero digital a +0,3 s (con 1/1 sería −37 dB).

Su métrica (máx |Δx| en la ventana del cambio vs el note-on de la misma nota): 1/1→0/0 12,2×, 0/0→1/1
12,1×. Pero en device ese "×note-on" no mide lo que mide en host: el note-on de Warm Pad es un ataque de
0,8 s (máx |Δx| en sus 30 ms = 0,0020) y cualquier tramo del régimen le gana 12×. Contra el régimen de la
propia apoyada (máx |Δx| por ventanas de 50 ms, mediana 0,0154 / máx 0,0237): las dos conmutaciones dan
1,00× el máximo del régimen — nada por encima de las pendientes propias de la onda. (En Saw Lead, la
conmutación en vuelo da 2,06× note-on y 0,98× régimen: son sus resets.)

La métrica que sí separa (banda alta): FIR pasa-altos de 5 kHz (401 taps), RMS de 2 ms, contra el régimen
de la apoyada (+0,9..5,8 s, excluyendo ±30 ms de cada cambio): mediana −93,4 dBFS, máximo −85,6 dBFS.

| | máx HF en ±600 ms | sobre el máximo del régimen |
|---|---|---|
| 1/1 → 0/0 | −86,8 dBFS | −1,3 dB |
| 0/0 → 1/1 | −86,9 dBFS | −1,3 dB |
| barrido de toda la apoyada, 3 picos | +1,44 · +4,99 · +2,62 s | +0,0 · −0,5 · −1,3 dB |
| control +: escalón instantáneo ×0,86 (el 14 % del send de Warm Pad) inyectado en el WAV | (+30 dB) | |
| control: el mismo ×0,86 con rampa de 5 ms (su decisión) | −86,8 dBFS | −1,3 dB |

Ningún pico de la apoyada supera el máximo del régimen; un escalón instantáneo del tamaño del wet daría
+30 dB, así que vemos escalones desde ~0,6 % de la amplitud. No se oye — dicho con el instrumento que
tenemos (no un oído): en device no hay ningún transitorio por encima de lo que la onda ya trae, ni en |Δx|
ni en 5–20 kHz. Su 0,94× con la rampa es consistente con lo que vemos; el 10× sin rampa lo hubiéramos
visto como +30 dB. El toggle de la hoja pasa por el mismo chokepoint que el comando
(`SoundFontStateHolder.setAmbience`), así que le aplica el mismo número.

## 5 · Lo pendiente de cartas anteriores

Nada: en su aviso de v2.19.0 §5 dan por cerrados los dos (el looper es nuestro `findContentBounds(0,03)`,
decisión #256; el bombo mide +600 c exactos). Los dos WAV del bombo (`concert-bass-drum-8-116-tecla-36-
2.17.1.wav` y `…-tecla-51-2.17.1.wav`, Concert Bass Drum 8:116 en 2.17.1, del export del looper) van
adjuntos a esta carta por si la vez anterior no llegaron. De ustedes: nada. Lo próximo de este lado es
REQ-041 cuando salga (el Q de tsf; sonda Grand suave a la 72, que acá dio −36,9 dBFS · 532 Hz con 0/0).

Arneses nuevos en `scripts/lab`: `pop.sh`/`pop.py` (banda alta + los dos controles inyectados), `fx.py
--t0` (onset a mano bajo la reverb del rack), `toma.sh` con `LAB_KEEP_FX` por nombres y el conteo de
voces. 🪤 `playFrame` del transporte no es el índice del buffer de la pista (corre desde que arranca el
motor, no desde Record).

---

## Lo que verificamos de nuestro lado (2026-09-16)

**El instrumento:** `scripts/sf-delta-host.py` (I-2 de REQ-040) sobre `GeneralUser_GS.sf3`, el mismo
preset/tecla/velocity de su tabla (velocity ×127: 1,0 → 127, 0,96 → 122, 0,33 → 42; su expresión 0,97
es −0,26 dB parejo y no entra en lo relativo). Régimen = RMS 0,1–0,4 s tras el note-on; cola = RMS de
50 ms centrada en note-off + T. **Aviso: su régimen es "RMS de 100 ms antes del note-off"** — para Saw
Lead, Strings, Warm Pad y Trumpet (sostenidos) coincide; para el Grand (decae durante la apoyada) no, y
por eso el Grand no se compara en ninguna dirección (tampoco pasa el control).

### REQ-042 — I-1 evaluado: **la perilla deja el seco, vivo**

Su A/B es interno al device (0/0 y 2.17.4 con la misma toma y la misma métrica), así que no depende del
origen del "+0,3 s" que se discute abajo. Con 0/0 contra 2.17.4: Saw Lead +0,8, Strings −2,1, Warm Pad
−0,8 dB; Trumpet cero digital en los dos; régimen Δ ≤ 0,2 en los cuatro. **N = 3 con oportunidad, ninguno
> 3 dB.** En host el mismo enunciado da 0/0 ≡ seco muestra a muestra (AC-042.2). Y el **pop**: su métrica
de banda alta (FIR 5 kHz, RMS 2 ms) no ve ningún transitorio por encima del régimen en las dos
conmutaciones (−1,3 dB del máximo), con control positivo (un escalón instantáneo del 14 % daría +30 dB) y
control con nuestra rampa de 5 ms (idéntico a la toma). Es el dato que nuestro 0,94× no podía dar: en
device no se oye, dicho con instrumento. Queda anotado en el criterio de REQ-042 como evaluado el 16/09.

### REQ-040 — I-1 rojo en los tres con oportunidad, I-2 verde ⇒ **F-2, no "muerto"**

Host con 1/1 a +0,3 s (cola − régimen) contra su tabla:

| preset · tecla · vel | host 1/1 | device 1/1 | \|Δ\| | control 0/0 host (≥ 10 abajo) | host t60 1/1 / 0/0 | device t60 1/1 / 0/0 |
|---|---|---|---|---|---|---|
| Saw Lead 60·127 | **−34,1** | −16,2 (−37,6 − −21,4) | **17,9** | 26,9 ⇒ cuenta | ≈ 690 / ≈ 300 ms | 660 / 310 |
| Trumpet 72·42 | **−34,0** | −4,0 (−36,5 − −32,5) | **30,0** | ∞ ⇒ cuenta | — / < 200 | 610 / 210 |
| Trumpet 72·122 | **−34,1** | −11,8 (−31,9 − −20,1) | **22,3** | ∞ ⇒ cuenta | ≈ 680 / ≈ 160 ms | 700 / 190 |
| Strings 60·127 | −20,6 | +0,3 | — | sólo 3,0 ⇒ no cuenta | | |
| Warm Pad 60·127 | −0,3 | +18,3 | — | sólo 1,7 ⇒ no cuenta | | |
| Grand 72·122 / 72·42 | −36,3 / −42,7 | — | — | −0,9 / −1,5 ⇒ no cuenta | | |

Por el umbral tal como está declarado, los tres presets que cuentan (N = 3) dan |Δ| > 3 dB: **I-1 rojo**.
I-2 contra FluidSynth sigue verde (I-4: residuo 0,23 dB en #17 A). El criterio dice qué corresponde:
**F-2 — hay un camino de wet que el host no cruza, o es la app (o el instrumento): se paga la ceguera
PRIMERO**, con el WAV crudo del device, antes de tocar nada.

Y hay razones para sospechar del punto fijo, no del wet, que se escriben pero no deciden:

1. **Los t60 coinciden en las dos posiciones**: Saw Lead 660 vs ≈ 690 ms (1/1) y 310 vs ≈ 300 (0/0);
   Trumpet fuerte 700 vs ≈ 680 (1/1). Un wet distinto al del font no daría la misma release.
2. **Su "+0,3 s" es inconsistente con sus propios t20/t40/t60.** Saw Lead seco: t60 = 300 ms ⇒ a +0,3 s
   de la caída real el nivel es −60 relativo (−81 dBFS); reportan −64,0 (−43 relativo), que es lo que
   la caída tiene a +0,21 s. Con 1/1: t20/t40 = 130/350 ⇒ a +0,3 s ≈ −35 relativo; reportan −16,2, que
   es lo que hay a ≈ +0,10 s. O sea que el origen del "+0,3 s" (t0 nominal + hold) cae **entre 90 y 200
   ms antes** del note-off real (latencia del touch-up por adb, o la ventana de `fx.py`), y no con un
   desplazamiento único para las dos posiciones — por eso no se puede corregir desde acá.
3. La diferencia del font solo, 1/1 − 0/0 en el mismo punto: **27,1 dB en device, 27,2 en host** a +0,3 s
   exacto. Coincide, pero el punto 2 dice que sus dos lecturas no están a +0,3 s, así que esta
   coincidencia no se cuenta como evidencia hasta tener el WAV.

**Lo que se pide (F-2):** los WAV crudos `2.17.4-fx/sawxy.wav`, `2.19.0-default/sawxy.wav`,
`2.19.0-off/sawxy.wav` y los dos `trumpet.wav` de 2.19.0, para medir cola − régimen con nuestra ventana
sobre su audio y separar instrumento de motor de app. Si el WAV confirma que el origen está corrido, el
umbral se **re-declara** (tercera formulación, el humano la ratifica) sobre un origen que las dos partes
midan igual — la caída real, o el t60 — y no sobre "+0,3 s del note-off nominal". Si el WAV confirma los
18 dB a +0,3 s de la caída real, es F-2 de verdad: un camino de wet que el host no cruza, y se busca en la
app antes que en el motor (la lección del fix 2 de 2.17.4).

**Lo demás que se cotejó:** el Grand suave −36,9 dBFS con 0/0 vs −38,8 en 2.17.4 (+1,9) es el dry con el
corte de MINI-027 sobre la fundamental — nuestro +1,7 en host, fila #10, REQ-041 S1. AC-6: su razón de
producto 1 (doble reverb) resultó +1,3 dB de wash bajo Concert Hall: la perilla vale por el A/B y el
arnés, y así queda en R-MOT-42. Los WAV del bombo: llegaron esta vez; el keytrack 36 → 51 se mide cuando
se abra el MINI que lo pida (hoy ninguno lo pide: su carta lo da por cerrado con +600 c exactos).
