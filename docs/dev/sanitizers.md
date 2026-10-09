# Sanitizers locales: por que CTEST_JOBS=4

Movido del `CLAUDE.md` raiz por MINI-044 (2026-10-09). La regla corta vive en `audio/src/main/cpp/CLAUDE.md`.

```bash
# Los mismos 1180 bajo sanitizers. NO son opcionales: el CI tiene un job para
# cada uno y encontraron dos bugs reales que el resto del gate no ve.
# OJO: `detect_leaks=1` (lo que usa ci.yml) NO existe en macOS y rompe el
# discovery de gtest — en esta maquina va sin el.
#
# Desde el cambio a `DISCOVERY_MODE PRE_TEST` eso ya NO rompe el build: el
# listado de tests pasó de tiempo de build a tiempo de ctest, asi que el
# sintoma es `discover_tests failed to run command` con exit 8 de ctest, en
# vez de un `ninja: build stopped` que no menciona ningun test. Sigue siendo
# un error — pero ahora dice donde.
CTEST_JOBS=4 ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  SANITIZE=address,undefined bash scripts/run-cpp-tests.sh --timeout 180
CTEST_JOBS=4 TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
  SANITIZE=thread bash scripts/run-cpp-tests.sh --timeout 180
# El TSan local (libc++) es MAS DEBIL que el del CI (libstdc++): una carrera
# real sobrevivio 15 corridas aca y fue roja a la primera alla. Para carreras
# el CI es la autoridad.
#
# 🔴 LOS SANITIZERS LOCALES VAN CON `CTEST_JOBS=4`, y no es opcional.
# Con ctest en paralelo a full (18/08) TSan bajaba a 192 s pero DABA TIMEOUT en
# `RateInvariance.EveryEffectStaysFiniteAndBoundedAtEveryRate` y en
# `NyquistLimits.NoNewDivergenceAppearsBelowFortyKilohertz`: con 10 procesos de
# TSan compitiendo por el ancho de banda de memoria, esos barridos pasan de
# ~105 s a mas de 180. Con `CTEST_JOBS=4` quedan en 114 s y 86 s (63 % y 48 %
# del techo) y la corrida entera tarda 361 s — todavia 3,7x mejor que los
# 1344 s en serie.
#
# La salida correcta cuando esto da timeout es BAJAR `CTEST_JOBS`, nunca subir
# el techo: bajo paralelismo "cuanto tarda este test" deja de ser una propiedad
# del test y pasa a incluir la contencion, y subir el techo taparia el
# crecimiento real. El presupuesto que importa es el de los sanitizers.
#
# NO es el default del script a proposito: el CI no pasa `--timeout` y ahi el
# `-j` completo mide 45 % mejor. Poner 4 por defecto pesimizaria el CI para
# resolver una restriccion de esta maquina.
```
