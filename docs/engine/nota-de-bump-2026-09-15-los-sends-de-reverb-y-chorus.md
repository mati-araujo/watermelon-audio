# Nota de bump para NoisyPad — los sends de reverb y chorus del SoundFont existen

**Fecha**: 2026-09-15 · **Cambio**: REQ-040 (S1–S3) · **Release**: la siguiente, `feat` ·
**Font medido**: `GeneralUser_GS.sf3` (GeneralUser GS 1.471, el que shipea NoisyPad).

Esta nota dice **qué cambia de sonido y en qué presets**, medido sobre el archivo (el lector de
`pdta`) y contra FluidSynth 2.6.0 con sus efectos encendidos (`-R 1 -C 1`) sobre el
SoundFont-Spec-Test. Si algo de acá no coincide con lo que se oye en el dispositivo, eso es un
hallazgo y se quiere saber.

## Lo que cambia, en tres frases

1. **El font tiene cola.** Un SoundFont declara por zona cuánto de cada voz manda a una reverb y a
   un chorus (`reverbEffectsSend` / `chorusEffectsSend`, 0–100 %) y espera que el sintetizador
   tenga esas dos unidades. tsf no las tenía (las listaba como "NOT YET IMPLEMENTED"), así que
   **todo salía seco**. Desde este cambio el motor lleva **dos unidades fijas adentro del
   sintetizador** —un Freeverb clásico y un chorus de tres voces con los parámetros por default de
   FluidSynth 2.6.0— y cada voz les manda lo que el archivo dice. El wet sale **sumado en la salida
   del instrumento**: pasa por su rack, por el fade y por la grabación del looper como el dry.
2. **Casi todo GeneralUser está programado con cola**: `reverbEffectsSend` en **11 117 de 12 311
   zonas, 225 de 269 presets** (3 / 5 / 7 / 14 / 21 % típicos, hasta 100 % en efectos), y
   `chorusEffectsSend` en **4390 zonas, 140 presets** (3,5 % a 64 %). Y a eso se le suma el
   default #8 del spec con CC91 en su valor de reset de GM: **+6,3 % de reverb a toda voz de todo
   preset** (la tabla del spec: `200 × 40/127`), como en un sinte GM recién encendido. CC93 en
   reset vale 0: el chorus es sólo el generador.
3. **Nada que adoptar y nada que apagar.** Cero superficie nueva (`git diff` vacío sobre
   `watermelon_audio.h`, `IAudioNativeBridge`, `ISoundFontBridge`). La ambiencia del font **no
   tiene perilla**: es el sonido del font, como lo afina su autor y como suena en FluidSynth. Si
   la quieren —para A/B, o porque un usuario la quiera apagar— es un REQ de un día (la costura
   interna ya existe): **pídanla con carta.**

## Lo que van a medir distinto

- **Todas** sus tomas con SoundFont llevan ahora cola: la comparación contra 2.18.x seco va a
  mover niveles y centroides en 225 presets, aun con el rack en bypass. Para aislar el resto de
  un cambio futuro, la referencia es FluidSynth `-R 1 -C 1` con `synth.reverb.room-size=0.2
  damp=0 width=0.5 level=0.9` y `chorus.nr=3 level=2.0 speed=0.3 depth=8` (la receta de
  `scripts/render-spec-reference.sh`).
- La cola **es del cuarto, no de la nota**: sigue sonando ~1 s después de soltar todo y después de
  cambiar de preset; se corta con `reset()` del engine, al cambiar de font y sin font. Con la
  entrada en silencio más de 2 s las unidades se apagan solas (cero exacto, no −90 dB).
- Costo medido en host (−O0, bloque de 128 a 48 kHz, 8 voces): 17 µs por bloque, **0,65 % del
  tiempo real**; dormidas, el 5 % de eso. En el teléfono, con −O2, menos.

## Qué tan igual a FluidSynth, con número

Sobre el SoundFont-Spec-Test contra el render `-R 1 -C 1`:

| prueba | qué | resultado |
|---|---|---|
| #17 A | escalera del send de **reverb** por generador (0/33/66/100 %) | **0,23 dB** por nota en el peor escalón: +0,41 / +1,40 / +2,41 contra +0,34 / +1,18 / +2,29 |
| #18 A | escalera del send de **chorus** por generador | **1,54 dB**: la referencia no es monótona (+1,26 / +2,61 / **+1,28** al 100 %: el dry y el wet se cancelan en la ventana) y la nuestra sí (+0,39 / +1,06 / +2,79). Es la estructura del chorus: FluidSynth usa un FDN modulado (LGPL, no se copia) |
| #17 B/C, #18 B/C | CC91/CC93 **en vuelo** | el motor no los sigue (no tiene superficie de CC): se evalúan en su reset. REQ propio si alguien lo pide |

Las otras 17 pruebas F del spec-test no se movieron (se juzgan secas, con los sends en cero).

## Dónde, por preset (bancos 0 y 128; el % es el generador, sin el +6,3 % del default #8)

| preset | reverb (%, generador) | chorus (%) |
|---|---|---|
| `0:0` Stereo Grand | 7 | 0 |
| `0:1` Bright Grand | 5 | 0 |
| `0:2` Electric Grand | 3 | 7 |
| `0:3` Honky-Tonk | 3 | 0 |
| `0:4` Tine Electric Piano | 3 | 5 |
| `0:5` FM Electric Piano | 7 | 15 / 22 |
| `0:6` Harpsichord | 7 | 0 |
| `0:7` Clavinet | 3 | 7 |
| `0:8` Celeste | 7 | 0 |
| `0:9` Glockenspiel | 7 | 0 |
| `0:10` Music Box | 7 | 0 |
| `0:11` Vibraphone | 5 | 0 |
| `0:12` Marimba | 7 | 0 |
| `0:13` Xylophone | 7 | 0 |
| `0:14` Tubular Bells | 14 | 0 |
| `0:15` Dulcimer | 12 | 0 |
| `0:16` Tonewheel Organ | 3 | 22 |
| `0:17` Percussive Organ | 3 | 7 / 22 |
| `0:18` Rock Organ | 3 | 7 / 22 |
| `0:19` Pipe Organ | 14 | 5 |
| `0:20` Reed Organ | 3 | 0 |
| `0:21` Accordian | 5 | 21 |
| `0:22` Harmonica | 5 | 0 |
| `0:23` Bandoneon | 5 | 3 |
| `0:24` Nylon Guitar | 5 | 0 |
| `0:25` Steel Guitar | 5 | 0 |
| `0:26` Jazz Guitar | 5 | 5 |
| `0:27` Clean Guitar | 7 | 14 |
| `0:28` Muted Guitar | 5 | 7 |
| `0:29` Overdrive Guitar | 3 | 7 |
| `0:30` Distortion Guitar | 3 | 7 |
| `0:31` Guitar Harmonics | 3 | 0 |
| `0:32` Acoustic Bass | 3 | 3 |
| `0:33` Finger Bass | 3 | 5 |
| `0:34` Pick Bass | 3 | 5 |
| `0:35` Fretless Bass | 3 | 5 |
| `0:36` Slap Bass 1 | 3 | 5 |
| `0:37` Slap Bass 2 | 3 | 5 |
| `0:38` Synth Bass 1 | 0 | 7 |
| `0:39` Synth Bass 2 | 3 | 5 |
| `0:40` Violin | 7 | 0 |
| `0:41` Viola | 7 | 0 |
| `0:42` Cello | 7 | 0 |
| `0:43` Double Bass | 7 | 0 |
| `0:44` Stereo Strings Trem | 14 | 0 |
| `0:45` Pizzicato Strings | 14 | 0 |
| `0:46` Orchestral Harp | 7 | 0 |
| `0:47` Timpani | 7 | 0 |
| `0:48` Stereo Strings Fast | 14 | 0 |
| `0:49` Stereo Strings Slow | 14 | 0 |
| `0:50` Synth Strings 1 | 14 | 14 |
| `0:51` Synth Strings 2 | 14 | 14 |
| `0:52` Concert Choir | 14 | 0 |
| `0:53` Voice Oohs | 5 | 3 |
| `0:54` Synth Voice | 5 | 3 |
| `0:55` Orchestra Hit | 7 | 5 |
| `0:56` Trumpet | 7 | 0 |
| `0:57` Trombone | 7 | 0 |
| `0:58` Tuba | 7 | 0 |
| `0:59` Muted Trumpet | 7 | 0 |
| `0:60` French Horns | 7 | 0 |
| `0:61` Brass Section | 7 | 5 |
| `0:62` Synth Brass 1 | 5 | 7 |
| `0:63` Synth Brass 2 | 5 | 5 |
| `0:64` Soprano Sax | 7 | 0 |
| `0:65` Alto Sax | 7 | 0 |
| `0:66` Tenor Sax | 7 | 0 |
| `0:67` Baritone Sax | 7 | 0 |
| `0:68` Oboe | 7 | 0 |
| `0:69` English Horn | 7 | 0 |
| `0:70` Bassoon | 7 | 0 |
| `0:71` Clarinet | 7 | 0 |
| `0:72` Piccolo | 7 | 0 |
| `0:73` Flute | 7 | 0 |
| `0:74` Recorder | 7 | 0 |
| `0:75` Pan Flute | 7 | 0 |
| `0:76` Bottle Blow | 5 | 0 |
| `0:77` Shakuhachi | 7 | 0 |
| `0:78` Irish Tin Whistle | 7 | 0 |
| `0:79` Ocarina | 7 | 0 |
| `0:80` Square Lead | 0 | 21 |
| `0:81` Saw Lead | 5 | 20 |
| `0:82` Synth Calliope | 5 | 5 |
| `0:83` Chiffer Lead | 5 | 7 |
| `0:84` Charang | 5 | 14 |
| `0:85` Solo Vox | 5 | 5 |
| `0:86` 5th Saw Wave | 5 | 5 |
| `0:87` Bass & Lead | 3 | 14 |
| `0:88` Fantasia | 14 / 21 / 50 | 5 / 7 / 15 |
| `0:89` Warm Pad | 14 | 14 |
| `0:90` Polysynth | 5 | 7 |
| `0:91` Space Voice | 14 / 20 | 24 / 50 |
| `0:92` Bowed Glass | 7 / 32 | 12 / 32 |
| `0:93` Metal Pad | 7 | 24 |
| `0:94` Halo Pad | 14 | 14 |
| `0:95` Sweep Pad | 7 | 7 |
| `0:96` Ice Rain | 21 / 50 | 5 / 7 |
| `0:97` Soundtrack | 14 | 14 |
| `0:98` Crystal | 25 / 50 | 24 / 50 |
| `0:99` Atmosphere | 21 | 7 / 28 |
| `0:100` Brightness | 14 | 21 / 30 |
| `0:101` Goblin | 21 | 7 |
| `0:102` Echo Drops | 35 / 75 | 3.5 / 21 |
| `0:103` Star Theme | 21 | 7 / 64 |
| `0:104` Sitar | 5 | 0 |
| `0:105` Banjo | 5 | 0 |
| `0:106` Shamisen | 5 | 0 |
| `0:107` Koto | 5 | 0 |
| `0:108` Kalimba | 5 | 0 |
| `0:109` Bagpipes | 5 | 0 |
| `0:110` Fiddle | 5 | 0 |
| `0:111` Shenai | 5 | 0 |
| `0:112` Tinker Bell | 5 | 0 |
| `0:113` Agogo | 5 | 0 |
| `0:114` Steel Drums | 5 | 0 |
| `0:115` Wood Block | 5 | 0 |
| `0:116` Taiko Drum | 5 | 0 |
| `0:117` Melodic Tom | 5 | 0 |
| `0:118` Synth Drum | 3 | 3 |
| `0:119` Reverse Cymbal | 3 | 0 |
| `0:120` Fret Noise | 5 | 0 |
| `0:121` Breath Noise | 5 | 0 |
| `0:122` Seashore | 7.8 / 8.6 | 0 |
| `0:123` Birds | 0 | 0 |
| `0:124` Telephone 1 | 3.1 | 3.1 |
| `0:125` Helicopter | 3 | 7 |
| `0:126` Applause | 19 | 0 |
| `0:127` Gun Shot | 5 | 3 |
| `128:0` Standard | 3 | 0 |
| `128:1` Standard 2 | 3 | 0 |
| `128:8` Room | 3 | 0 |
| `128:16` Power | 5 | 3 |
| `128:24` Electronic | 0 | 3.5 |
| `128:25` 808/909 | 0 | 0 / 3.5 |
| `128:26` Dance | 0 | 3.5 / 7 |
| `128:32` Jazz | 3 | 0 |
| `128:40` Brush | 3 | 0 |
| `128:48` Orchestral | 7 / 14 / 28 | 0 |
| `128:56` SFX | 0 / 3 / 3.5 / 7 / 10 / 10.5 / 10.8 / 11.6 / 13.9 / 23 / 25.6 / 27 / 100 / 107 | 0 / 3.5 / 7 / 20 / 64.5 / 100 |

## Cómo verificarlo del lado de NoisyPad

- `0:89 Warm Pad` o `0:48 Stereo Strings Fast` (14 % de reverb + 6,3 %): una apoyada corta en el
  XY y soltar. Con 2.18.x el sonido para en la release; con esta, queda una cola de ~1 s.
- `0:81 Saw Lead` (5 % de reverb, **20 % de chorus**): el chorus se oye como el ensanchamiento de
  siempre en un GM.
- Lo que **no** cambia: los engines que no son SoundFont; el nivel del dry (el wet se SUMA); el
  pitch; los presets a velocity 1,0 siguen con la misma curva de brillo.

## La pregunta activa (I-3)

**¿Algún preset suena con demasiada cola, o con chorus donde no lo esperaban? ¿Y la quieren
apagar — para A/B o como opción del usuario?** Lo primero va con nombre y se mide contra
FluidSynth `-R 1 -C 1`; lo segundo es la perilla, un día de trabajo, con carta.
