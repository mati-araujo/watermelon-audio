# Synth engines — reglas locales

## Nuevo synth engine

1. Header-only en `audio/src/main/cpp/engines/`, heredando `SynthEngine`.
2. Registrarlo en `AudioEngine` (constructor, `getEngine()`, `prepare()`).
3. Registrarlo en `OscillatorNode` via `registerEngine()`.

## SoundFont

`python3 scripts/sf-delta-host.py --preset 81 --key 60 --vel 100` (REQ-040, criterio I-2): la cola
del wet de UN preset en host, con la ambiencia en 1/1 y en 0/0, relativa al regimen, a +0,3 s del
note-off. Es lo que se compara con el device de NoisyPad (3 dB); el 0/0 es el control de
oportunidad. Construye a pedido el target `EXCLUDE_FROM_ALL` `sf_render_preset` de `core_tests`
(mismo arnes que la conformidad); no es un test y el gate no lo compila. Bank 0 solo.

TinySoundFont es un **fork**: `../thirdparty/VENDORED.md`.
