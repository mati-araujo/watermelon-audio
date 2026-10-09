# CI — trampas locales

- `.github/local-gate.json` lo escribe `scripts/gate.sh` y NADIE MAS (ver el `CLAUDE.md` raiz).
  Si choca en un merge, se toma la de master y se re-corre el gate.
- La atestacion saltea en el PR los jobs `ios`, `build` y `cpp-tests-macos`; en `push: master`
  todo corre entero. Los tres de ubuntu (`cpp-tests`, `-asan`, `-tsan`) nunca se atestan, y con
  `ios` atestandose en 9 s **son el camino critico** de un PR atestado. Diseño:
  `docs/ci/local_first.md` (§2).
- `assembleWatermelonXCFramework` corre sólo en push a master (`if: github.event_name !=
  'pull_request'`), no en el PR: ver `harness/CLAUDE.md`.
- `detect_leaks=1` vale en ubuntu, no en macOS (rompe el discovery de gtest).
- Un pin (Xcode, SHA de action, java-version) tiene UNA fuente: `scripts/check-dep-pins.py`.
