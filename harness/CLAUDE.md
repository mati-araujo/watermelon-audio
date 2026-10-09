# :harness (WA-5.5) — reglas locales

App de prueba multiplataforma (Compose Multiplatform). **No se publica**, y es estructural: no
aplica `maven-publish`, los workflows publican con `:audio:publishAll...` path-qualified, y
`scripts/check-no-ui-in-library.sh` falla si Compose aparece en el classpath resuelto de `:audio`.

- `src/commonMain`: la UI entera (`HarnessApp`). `androidMain`: `MainActivity` + manifest
  (`RECORD_AUDIO`). `iosMain`: `MainViewController`.
- `iosApp/` embebe el framework de `:harness` (`HarnessKit.framework`), **NO** el XCFramework de
  WA-4.1: son vias de consumo alternativas y usar las dos duplica el motor.
- `Info.plist` necesita `NSMicrophoneUsageDescription` y `CADisableMinimumFrameDurationOnPhone`
  (sin esta ultima Compose aborta al arrancar desde `PlistSanityCheck`). Lo unico que lo agarra es
  el ARRANQUE que hace `scripts/build-harness.sh`.

```bash
./gradlew :harness:assembleDebug                         # APK
./gradlew :harness:linkDebugFrameworkIosSimulatorArm64   # HarnessKit.framework
```

`./gradlew :audio:assembleWatermelonXCFramework` ya NO es gate ni corre en el CI del PR (sólo en
push a master, desde 2026-08-05): no tiene consumidores y lo que probaba lo cubre el framework del
harness, con los mismos simbolos `wma_*`. Paridad vigilada por `scripts/test-attestation.sh`.
