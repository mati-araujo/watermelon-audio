# Nota de bump para NoisyPad — el motor reproduce los moduladores del SoundFont

**Fecha**: 2026-09-11 · **Cambio**: REQ-039 (PRs #275, #283 y el de S3) · **Release**: la
siguiente `minor` desde `v2.16.4` · **Font medido**: `GeneralUser_GS.sf3` (GeneralUser GS 1.471,
el que shipea NoisyPad).

Esta nota dice **qué cambia de sonido y en qué presets**, medido sobre el archivo — no sobre la
intuición. Si algo de acá no coincide con lo que se oye en el dispositivo, eso es un hallazgo y
se quiere saber.

> 🔴 **Corregida el 2026-09-14, en su lugar y con marca, a partir del hallazgo #3 de NoisyPad**
> (`0:81 Saw Lead`, −6,9 / −16,9 dB a velocity 76 / 38 — la velocity SÍ mueve el nivel). Dos filas
> de la tabla de abajo estaban **mal**, y las dos por el mismo defecto del instrumento: el lector
> `--presets` recorría **sólo `imod`** (moduladores de instrumento) y no miraba `pmod` (los de
> zona de preset, que por SF2 §9.5 se **suman**). El motor lo hacía bien; la nota, no. Las filas
> originales quedan tachadas con su corrección al lado. La medición y el diagnóstico están en
> `respuesta-noisypad-2026-09-14-v2.17.1.md`.

## Lo que cambia, en una frase

Hasta `v2.16.4` el renderizador (TinySoundFont) **descartaba todos los moduladores** del SoundFont
al cargarlo y aplicaba una sola curva cableada de velocity → nivel (`20·log10(vel)`). Desde este
cambio, **el font suena como fue programado en lo que se resuelve al disparar la nota**: la
velocity mueve el **nivel** con la curva que el archivo declara y mueve el **corte del filtro**
(brillo), que antes no se movía nunca.

## En qué presets, medido

Sobre los **269 presets** de GeneralUser (128 melódicos del banco 0 incluidos):

| qué | cuántos presets | qué se oye |
|---|---|---|
| **velocity → corte del filtro** ~~(1677 moduladores en el archivo, 85 % con velocity también como fuente secundaria: velocity × velocity) · **269 de 269**~~ — **corregido el 14/09**: el archivo declara una curva propia (amount ≠ 0, instrumento + preset) en **131** presets; los 1422 "velocity × velocity" no eran curvas sino **borrados** del default #2 con la identidad SF 2.01 (`amtSrc` velocity/switch), que **no lo anulan** en un synth 2.04 como este (ver la respuesta del 14/09, §3c). El default #2 (−2400) sigue vivo en los **269** — por eso el "269 de 269" era verdad por la razón equivocada | **131** declarados / **269** con el default vivo | una nota suave suena **más oscura**, no sólo más baja. Antes: mismo timbre a cualquier velocity |
| velocity → nivel con la curva del archivo (**800 cB cóncava**, reemplazando la de tsf) | ~~**212**~~ **232** (corregido el 14/09, curva **efectiva** instrumento + preset: 211 de los 212, más 11 de los 26 "anulados" y 10 de las "otras curvas") | a velocity 64 la nota suena **~4 dB más baja** que antes (9,9 dB de atenuación contra 5,95); a velocity 32, **~8 dB** más baja (19,9 contra 12,0) |
| ~~velocity → nivel **anulado** por el archivo (`amount` 0, sin otra curva)~~ **corregido el 14/09: NINGUNO.** El instrumento pone el default #1 en 0 y la **zona global del preset** lo vuelve a declarar (`pmod`, se suma): 500 cB en los 8 órganos y los 2 clavecines, 840 en los 5 pianos, 800 en los otros 11 (`Saw Lead`, `Synth Bass 1`, `Synth Brass 1`, `FM Electric Piano`…) | ~~**26**~~ **0** | ~~la velocity ya NO cambia el nivel~~ **la velocity mueve el nivel con la curva del preset**: `Saw Lead` −7,1 / −17,1 dB a velocity 76 / 38 (medido, motor y archivo; NoisyPad midió −6,9 / −16,9), los órganos −4,4 / −10,7 (500 cB, más suave que antes), `Stereo Grand` −5,2 / −14,7 medido (840 cB más el cambio de capa por velocity) |
| otras curvas (500 cB en órganos y clavecines, 840 en pianos, 700 en trompeta/trombón, mezclas por zona en kits y unos pocos melódicos) | ~~31~~ **37** | entre las dos filas anteriores; los kits de percusión tienen zonas sueltas a 0 |

~~**Los 26 con velocity → nivel anulado** son los que más van a sorprender, porque incluyen los
pianos: `0:0 Stereo Grand`, `0:1 Bright Grand`, `0:3 Honky-Tonk`, `0:5 FM Electric Piano`,
`0:6 Harpsichord`, `0:16/17/18/19/20 Tonewheel / Percussive / Rock / Pipe / Reed Organ`,
`0:38 Synth Bass 1`, `0:62 Synth Brass 1`, `0:81 Saw Lead`, y sus variantes de bancos 8/11/12/13.
Verificado en el archivo: esos presets **no declaran ninguna otra** curva de velocity → atenuación.
Es una decisión del autor del font (los órganos no responden a velocity; los pianos de GeneralUser
llevan la dinámica al filtro), y el motor ahora la respeta.~~

🔴 **Corregido el 14/09: el párrafo de arriba era falso en los 26.** "Verificado en el archivo"
verificó la mitad del archivo. Cada uno de esos presets **sí declara** la curva, en la zona global
del preset; lo que el instrumento anula, el preset lo repone. Los órganos **responden** a velocity
(500 cB: −10,7 dB a velocity 38, contra los −10,3 de la curva vieja de tsf: casi igual que antes),
y los pianos llevan la dinámica al nivel (840 cB) **y** al filtro. Ningún preset melódico de
GeneralUser tiene la velocity anulada.

## Lo que esto significa para el mapeo toque → velocity

- Si NoisyPad usa la velocity del toque como **control de volumen** en un piano, deja de serlo: en
  `Stereo Grand` un toque suave y uno fuerte suenan **al mismo nivel** y con distinto brillo. El
  control de nivel por toque sigue existiendo y es otro camino: `wma_sf_set_touch_expression`
  (expresión por toque, REQ-008), que entra por la ganancia del canal y **no** se mezcla con los
  moduladores — está medido y afirmado (`test_touch_expression_channel_layer.cpp`).
- En los 212 con curva de 800 cB, el rango dinámico por velocity es **más amplio** que antes: la
  misma escala de toques da más diferencia de nivel entre suave y fuerte.
- Nada de esto cambia con la expresión global (`PARAM_EXPRESSION`) ni con el por toque: los dos
  viven aguas abajo de lo que los moduladores calculan (R-MOT-11..16 se conservan).

## Lo que NO cambia todavía (medido, con dueño)

El arnés de conformidad de REQ-039 (el SoundFont-Spec-Test de terceros, 22 pruebas contra
FluidSynth 2.6.0 y contra el spec) dejó medidas cuatro cosas que **no** entran en este cambio y
tienen su MINI:

1. **`initialAttenuation` entra a 0,1 dB por dB declarado** donde el spec-quirk y FluidSynth aplican
   0,4 (MINI-024). Efecto: todo preset con atenuación declarada suena más fuerte de lo programado.
   Es como sonaba antes; no empeora ni mejora con este cambio.
2. **La afinación fina se pierde cuando `scaleTuning ≠ 100`** (MINI-025): hasta −30 c en 31 zonas
   de 14 instrumentos, **todos de percusión o efectos** (Taiko, Bass Drum, Toms, Telephone…).
   Ningún instrumento melódico. Como sonaba antes.
3. **Envolventes y LFO de tsf** difieren del spec en ataque/decay/release y en el recorte del boost
   (MINI-026). Como sonaban antes.
4. **30 moduladores de velocity hacia ataque de envolvente, cantidad de envolvente al filtro, Q y
   offsets** (0,9 % del archivo) todavía no se evalúan (MINI-027). Como sonaban antes.

Y dos cosas fuera de alcance por diseño: los **sends de reverb y chorus** (622 moduladores; el
motor no tiene bus de efectos — REQ-040) y toda **superficie de MIDI CC** (rueda de modulación,
presión, pitch bend, CC7/CC11 por MIDI: 682 moduladores de fuente de canal). La expresión por toque
no pasa por ahí, así que no le afecta.

## Cómo se midió

- `scripts/read-sf2-modulators.py GeneralUser_GS.sf3`: los conteos del archivo. 🔴 Hasta el
  2026-09-11 su tabla de nombres de generadores estaba corrida (los conteos eran correctos); si
  leíste una versión anterior de estos números con nombres como `modLfoToVolume` o `decayModEnv`,
  los nombres correctos son `modEnvToFilterFc` y `attackVolEnv`.
- Por preset: `scripts/read-sf2-modulators.py GeneralUser_GS.sf3 --presets` — ~~el recorrido
  `phdr → pbag → pgen(instrument) → inst → ibag → imod` buscando la identidad del default #1
  (`0x0502 → 48`) y los `velocity → initialFilterFc`.~~ 🔴 **Ese recorrido era el defecto** (corregido
  el 14/09): saltaba `pmod`. Desde el 14/09 resuelve los cuatro ámbitos (instrumento global/zona,
  preset global/zona) con la precedencia del spec y la misma del motor, e imprime la curva
  **efectiva** por preset y qué identidad usa el archivo para borrar el default #2.
- Contra terceros: `SfSpecConformance.TheTwentyTwoAgainstTheirOracles` (44 filas, cada una contra
  su oráculo) y `TheVelocityLaddersFollowWhatTheFileDeclares` (las diez escaleras de velocity:
  siete coinciden con FluidSynth al décimo de dB; las tres del default velocity → filtro siguen el
  spec 2.04 donde FluidSynth eligió no implementarlo).
