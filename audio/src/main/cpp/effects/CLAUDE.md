# Efectos — trampas locales

Las reglas del thread RT (`../CLAUDE.md`) valen aca: `reset()` es RT.

## Nuevo efecto

1. Crearlo en `audio/src/main/cpp/effects/`.
2. Registrarlo en `EffectRegistry` (registro dinamico, NO un switch).
3. Agregarlo a `CMakeLists.txt`.
4. `EffectType` en `EffectTypes.h` (C++) y `EffectType.kt` (commonMain).
5. Parametros en `EffectParameter.kt` y `EffectConstants.kt` (commonMain).
6. C API: `wma_effect_*` en `watermelon_audio.h/cpp` si hace falta.

## Contrato de `reset()`

- `tests/reset-baseline.txt` es un **TRINQUETE** (igual que `scripts/rt-safety-baseline.txt`): el
  test falla si aparece deuda nueva Y si una entrada declarada ya no se reproduce. VACIO desde
  WD-3.2 (tuvo 16 de 23 y se pagaron todas).
- `Effect::reset()` es **virtual pura**: un efecto nuevo no puede olvidarlo; uno sin estado
  escribe un `{}` explicito con su razon.
- Lo no-determinista POR DISEÑO no va al baseline: va a `nonDeterministicByDesign()` en
  `test_golden_properties.cpp`, y la exclusion esta MEDIDA por su propio test.

## Golden de DSP (WD-2.2)

`bash scripts/regen-golden.sh` RECAPTURA los golden. Es una tarea explicita y aparte: recapturar
no puede ser un efecto colateral de correr los tests. En modo regeneracion los tests quedan
SKIPPED, no PASSED (una corrida que ESCRIBE no pasa por una que VERIFICA). Los `.resp` son texto:
**su diff es la revision**; si aparece un preset que el cambio no tocaba, eso es el hallazgo.
