# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.10.0](https://github.com/mati-araujo/watermelon-audio/compare/v2.9.2...v2.10.0) (2026-08-25)


### Features

* **input:** el DSP de entrada sigue al rate real ([#201](https://github.com/mati-araujo/watermelon-audio/issues/201)) ([651dd19](https://github.com/mati-araujo/watermelon-audio/commit/651dd19ad6aed53b14d3c83f35b2ca54cca3306b))
* **input:** el rate real dispara la reconfiguracion del DSP de entrada ([#205](https://github.com/mati-araujo/watermelon-audio/issues/205)) ([526f3bd](https://github.com/mati-araujo/watermelon-audio/commit/526f3bd1e92106092d93f474332e43fd560a2d8b))
* **input:** el thread de captura ahora se puede drenar ([#199](https://github.com/mati-araujo/watermelon-audio/issues/199)) ([62d3225](https://github.com/mati-araujo/watermelon-audio/commit/62d32250fef79986545c50f60addd80dc877ade5))
* **input:** reconfigurar el rate deja de ser un hueco invisible ([#204](https://github.com/mati-araujo/watermelon-audio/issues/204)) ([91254eb](https://github.com/mati-araujo/watermelon-audio/commit/91254ebeccc46883a84fa0bbcedefeac53cfca17))


### Bug Fixes

* **input:** el cableado del rate llega aunque el nodo no este publicado ([#207](https://github.com/mati-araujo/watermelon-audio/issues/207)) ([9ab7f8d](https://github.com/mati-araujo/watermelon-audio/commit/9ab7f8d8c9387e08ebf96fa8aae4d2d334d2cdbb))
* **input:** el stream propio del nodo tambien re-prepara el DSP ([#206](https://github.com/mati-araujo/watermelon-audio/issues/206)) ([c72047f](https://github.com/mati-araujo/watermelon-audio/commit/c72047f164b7f8ec1429c8f4c14e686c850050e3))

## [2.9.2](https://github.com/mati-araujo/watermelon-audio/compare/v2.9.1...v2.9.2) (2026-08-24)


### Bug Fixes

* **effects:** sumar una rama sin alinearla deja de ser una operación posible ([#197](https://github.com/mati-araujo/watermelon-audio/issues/197)) ([1dfa40e](https://github.com/mati-araujo/watermelon-audio/commit/1dfa40efd80bd26108858527b621a7970562ddc8))

## [2.9.1](https://github.com/mati-araujo/watermelon-audio/compare/v2.9.0...v2.9.1) (2026-08-24)


### Bug Fixes

* **tuner:** el afinador se basta solo, y ahora lo dice (MINI-005) ([#193](https://github.com/mati-araujo/watermelon-audio/issues/193)) ([a0f4b2f](https://github.com/mati-araujo/watermelon-audio/commit/a0f4b2f3b31b90637a1972cff094ab52d791f9c4))

## [2.9.0](https://github.com/mati-araujo/watermelon-audio/compare/v2.8.2...v2.9.0) (2026-08-24)


### Features

* **soundfont:** la expresión por toque, alcanzable desde afuera (REQ-008) ([#191](https://github.com/mati-araujo/watermelon-audio/issues/191)) ([8df103e](https://github.com/mati-araujo/watermelon-audio/commit/8df103e07eeee9c23881b98af6564c1a86947656))

## [2.8.2](https://github.com/mati-araujo/watermelon-audio/compare/v2.8.1...v2.8.2) (2026-08-24)


### Bug Fixes

* **tuner:** el motor deja de declarar CONVERGIDO sobre un salto (REQ-009 S2+S3) ([#189](https://github.com/mati-araujo/watermelon-audio/issues/189)) ([c0fae21](https://github.com/mati-araujo/watermelon-audio/commit/c0fae21988364b46b15c71888ca7765c8450f37a))

## [2.8.1](https://github.com/mati-araujo/watermelon-audio/compare/v2.8.0...v2.8.1) (2026-08-24)


### Bug Fixes

* **effects:** las ramas paralelas se suman alineadas (REQ-011) ([#187](https://github.com/mati-araujo/watermelon-audio/issues/187)) ([9b6ea96](https://github.com/mati-araujo/watermelon-audio/commit/9b6ea9611c8534cd8d0047dc7cc7bb057ab0e4d8))

## [2.8.0](https://github.com/mati-araujo/watermelon-audio/compare/v2.7.0...v2.8.0) (2026-08-22)


### Features

* **tuner:** la puerta pública del afinador, probada desde afuera ([#183](https://github.com/mati-araujo/watermelon-audio/issues/183)) ([fe50ad3](https://github.com/mati-araujo/watermelon-audio/commit/fe50ad398f3fd6ddd9294ffddb160e41811d98d5))

## [2.7.0](https://github.com/mati-araujo/watermelon-audio/compare/v2.6.0...v2.7.0) (2026-08-21)


### Features

* **tuner:** TunerImpl sobre el puente, con la disciplina de empuje ([#179](https://github.com/mati-araujo/watermelon-audio/issues/179)) ([d0ec61c](https://github.com/mati-araujo/watermelon-audio/commit/d0ec61c1f6003c450471665f02a1d6ca70c8b88c))

## [2.6.0](https://github.com/mati-araujo/watermelon-audio/compare/v2.5.0...v2.6.0) (2026-08-21)


### Features

* **api:** el ruteo por pista llega a Kotlin en las dos plataformas (REQ-007.2) ([#173](https://github.com/mati-araujo/watermelon-audio/issues/173)) ([9c7dffc](https://github.com/mati-araujo/watermelon-audio/commit/9c7dffc4377840a329923815ab564531871e86bb))

## [2.5.0](https://github.com/mati-araujo/watermelon-audio/compare/v2.4.1...v2.5.0) (2026-08-21)


### Features

* **looper:** una pista marcada pasa por la cadena de efectos (REQ-007.1) ([#172](https://github.com/mati-araujo/watermelon-audio/issues/172)) ([31961c1](https://github.com/mati-araujo/watermelon-audio/commit/31961c1a7177ecef7294e55098499a9cf9cd1182))
* **tuner:** publicar hasta donde vale la lectura fina, en cents (REQ-003.2) ([#170](https://github.com/mati-araujo/watermelon-audio/issues/170)) ([69873b9](https://github.com/mati-araujo/watermelon-audio/commit/69873b908995f931e89ed88d5f6011170bf2c7c4))

## [2.4.1](https://github.com/mati-araujo/watermelon-audio/compare/v2.4.0...v2.4.1) (2026-08-21)


### Bug Fixes

* **tuner:** el strobe deja de publicar lo que no puede medir (REQ-003.1) ([#168](https://github.com/mati-araujo/watermelon-audio/issues/168)) ([24299b2](https://github.com/mati-araujo/watermelon-audio/commit/24299b22da3b618dff77bdd6a3f92a864bfa0b3d))

## [2.4.0](https://github.com/mati-araujo/watermelon-audio/compare/v2.3.2...v2.4.0) (2026-08-20)


### Features

* **chord:** el acorde se puede generar sobre un buffer del llamador (REQ-004.1) ([#166](https://github.com/mati-araujo/watermelon-audio/issues/166)) ([80e87b9](https://github.com/mati-araujo/watermelon-audio/commit/80e87b92745d73374bb9f2d30b06db35f73afa36))

## [2.3.2](https://github.com/mati-araujo/watermelon-audio/compare/v2.3.1...v2.3.2) (2026-08-20)


### Bug Fixes

* **core:** que un cambio de rate en caliente llegue a los engines ([#163](https://github.com/mati-araujo/watermelon-audio/issues/163)) ([053e877](https://github.com/mati-araujo/watermelon-audio/commit/053e877f321b40aaa9cfd71dc7d5bc7d7a429d81))

## [2.3.1](https://github.com/mati-araujo/watermelon-audio/compare/v2.3.0...v2.3.1) (2026-08-20)


### Bug Fixes

* **core:** re-preparar el motor sin el thread de audio adentro ([#161](https://github.com/mati-araujo/watermelon-audio/issues/161)) ([fb6179a](https://github.com/mati-araujo/watermelon-audio/commit/fb6179a37312f6ff71ecd6379b5737516166a68b))

## [2.3.0](https://github.com/mati-araujo/watermelon-audio/compare/v2.2.0...v2.3.0) (2026-08-20)


### Features

* **docs:** contrato de exactitud que no puede quedar stale ([#154](https://github.com/mati-araujo/watermelon-audio/issues/154)) ([b93dca8](https://github.com/mati-araujo/watermelon-audio/commit/b93dca803b4eaed61d181eba75c963cd94e749ab))


### Bug Fixes

* el afinador esperaba mal, y el helper alimentaba peor ([#159](https://github.com/mati-araujo/watermelon-audio/issues/159)) ([4124698](https://github.com/mati-araujo/watermelon-audio/commit/4124698a59ed2bef72e5b2ec80622caff8a029b9))

## [2.2.0](https://github.com/mati-araujo/watermelon-audio/compare/v2.1.2...v2.2.0) (2026-08-19)


### Features

* **tests:** el build de host compila el InputNode real, y de paso lo de-duplica ([#144](https://github.com/mati-araujo/watermelon-audio/issues/144)) ([886f1aa](https://github.com/mati-araujo/watermelon-audio/commit/886f1aa510f647b7f5c90cd95919f83252fcc541))


### Bug Fixes

* **dsp:** el smoother en reposo deja de depender del tamano de bloque ([#140](https://github.com/mati-araujo/watermelon-audio/issues/140)) ([a85c4d0](https://github.com/mati-araujo/watermelon-audio/commit/a85c4d0c09dbe58ce3bb9cde809b4b82f5077bb7))
* **effects:** la cadena deja de aplicar ganancia propia (WD-3.3) ([#143](https://github.com/mati-araujo/watermelon-audio/issues/143)) ([36ba5d3](https://github.com/mati-araujo/watermelon-audio/commit/36ba5d32d441630d48e6e995131401237b676626))


### Performance Improvements

* **ci:** ctest en paralelo, que corria en serie en todos lados ([#138](https://github.com/mati-araujo/watermelon-audio/issues/138)) ([a8cd4e3](https://github.com/mati-araujo/watermelon-audio/commit/a8cd4e372e90f818bc6bcde029e0d473403ff553))

## [2.1.2](https://github.com/mati-araujo/watermelon-audio/compare/v2.1.1...v2.1.2) (2026-08-18)


### Bug Fixes

* **engines:** Karplus-Strong afina y sostiene (WD-3.4) ([#135](https://github.com/mati-araujo/watermelon-audio/issues/135)) ([7fd7e0c](https://github.com/mati-araujo/watermelon-audio/commit/7fd7e0c7ad83b44b9a86107552c96aa9aacb3049))

## [2.1.1](https://github.com/mati-araujo/watermelon-audio/compare/v2.1.0...v2.1.1) (2026-08-17)


### Bug Fixes

* **dsp:** clamp contra Nyquist que sobrevive al cambio de rate (WD-3.5) ([#132](https://github.com/mati-araujo/watermelon-audio/issues/132)) ([c83d8e6](https://github.com/mati-araujo/watermelon-audio/commit/c83d8e694e5c702f59e65393b1257a1f699e021a))
* **effects:** la ganancia de lazo de SPRING_REVERB deja de pasar de 1 (WD-3.6) ([#133](https://github.com/mati-araujo/watermelon-audio/issues/133)) ([0ae3331](https://github.com/mati-araujo/watermelon-audio/commit/0ae333165d83a7bd51fc3b7d76fabc240ab1ac83))

## [2.1.0](https://github.com/mati-araujo/watermelon-audio/compare/v2.0.2...v2.1.0) (2026-08-17)


### Features

* **effects:** compensacion de latencia entre ramas (WD-3.1, completo) ([#126](https://github.com/mati-araujo/watermelon-audio/issues/126)) ([0e5a3b9](https://github.com/mati-araujo/watermelon-audio/commit/0e5a3b9a6ceae8630fd860474d2c37e092c6fb4d))
* **engine:** motor sin device y contrato de latencia — pasos 0 y 1 de D1-bis ([#125](https://github.com/mati-araujo/watermelon-audio/issues/125)) ([6407b7f](https://github.com/mati-araujo/watermelon-audio/commit/6407b7f181d934abf27198e8e175b4f2798b6ebd))


### Bug Fixes

* **effects:** reset() pasa a virtual pura y se paga la deuda de los 16 (WD-3.2) ([#129](https://github.com/mati-araujo/watermelon-audio/issues/129)) ([578d2cd](https://github.com/mati-araujo/watermelon-audio/commit/578d2cd285243df1be1f8c89f705618c6d43a195))
* **rt:** el thread de audio no cumplía sus propias reglas — Fase 1 del programa WD ([#123](https://github.com/mati-araujo/watermelon-audio/issues/123)) ([dffcda1](https://github.com/mati-araujo/watermelon-audio/commit/dffcda114fd4c50853cf856d4ba60a1e67947194))

## [2.0.2](https://github.com/mati-araujo/watermelon-audio/compare/v2.0.1...v2.0.2) (2026-08-13)


### Bug Fixes

* **core:** carrera entre los lectores de estado y el reopen del stream ([#117](https://github.com/mati-araujo/watermelon-audio/issues/117)) ([45114d5](https://github.com/mati-araujo/watermelon-audio/commit/45114d515f0f44b95ad5de476e2bcc910268b7ef))

## [2.0.1](https://github.com/mati-araujo/watermelon-audio/compare/v2.0.0...v2.0.1) (2026-08-05)


### Bug Fixes

* **ci:** la precondición de árbol limpio de gate.sh valía para un instante ([#115](https://github.com/mati-araujo/watermelon-audio/issues/115)) ([5df6a5c](https://github.com/mati-araujo/watermelon-audio/commit/5df6a5c94b5338e1d35e667db2781f7ca54f9ab7))
* **core:** el master volume no atenuaba la entrada monitoreada ([#113](https://github.com/mati-araujo/watermelon-audio/issues/113)) ([702dac1](https://github.com/mati-araujo/watermelon-audio/commit/702dac1b13d441b052dfdfd33c39b8c4d731f61a))

## [2.0.0](https://github.com/mati-araujo/watermelon-audio/compare/v1.14.1...v2.0.0) (2026-08-03)


### ⚠ BREAKING CHANGES

* **core:** se borran IModeTransitionHandler.setCrossfadePosition, IModeStateWriter.setCrossfadePosition y ModeProperties.crossfadePosition. NoisyPad los usa en 5 call sites y no compilara hasta que los saque; pinea 1.13.2, asi que no se rompe hasta que bumpee. El reemplazo no es uno a uno: el balance instrumento/entrada ahora son dos controles ortogonales, synthVolume y monitoringVolume, sin linkeo equal-power en el motor porque esa curva es decision de UI.

### Features

* **core:** nivel de instrumento, y borrar el crossfade de MIX ([#111](https://github.com/mati-araujo/watermelon-audio/issues/111)) ([9d8d982](https://github.com/mati-araujo/watermelon-audio/commit/9d8d982600d34f0a8ae7d8027d5de7c3e98ea26b))


### Bug Fixes

* **ci:** el watchdog de gate.sh retenia el stdout 45 min despues de salir verde ([#109](https://github.com/mati-araujo/watermelon-audio/issues/109)) ([d48cbde](https://github.com/mati-araujo/watermelon-audio/commit/d48cbde3ac031e43a666b7989fcdba073c659d11))

## [1.14.1](https://github.com/mati-araujo/watermelon-audio/compare/v1.14.0...v1.14.1) (2026-07-30)


### Bug Fixes

* **core:** los medidores de salida estaban muertos — pasan a OutputStage ([#104](https://github.com/mati-araujo/watermelon-audio/issues/104)) ([bea50ce](https://github.com/mati-araujo/watermelon-audio/commit/bea50ce8fd40a90156bfe72b9419d3e567954738))

## [1.14.0](https://github.com/mati-araujo/watermelon-audio/compare/v1.13.2...v1.14.0) (2026-07-29)


### Features

* **ci:** el gate corre local y el CI verifica la prueba en vez de repetir el trabajo ([#94](https://github.com/mati-araujo/watermelon-audio/issues/94)) ([b4cd09d](https://github.com/mati-araujo/watermelon-audio/commit/b4cd09de21e693bb8b6c673601b82642ffba16c5))
* **ci:** el publish espera al CI verde del commit antes de publicar ([#96](https://github.com/mati-araujo/watermelon-audio/issues/96)) ([f802098](https://github.com/mati-araujo/watermelon-audio/commit/f802098e75e4e0b0a1292e11c6821d409e0a14a8))


### Bug Fixes

* **usb:** carrera en RoundTripMeasurer::poll() — totalBursts se leia fuera del guard de fase ([#91](https://github.com/mati-araujo/watermelon-audio/issues/91)) ([5ceb247](https://github.com/mati-araujo/watermelon-audio/commit/5ceb2479078d4710874f077bf0db669ec537b266))


### Performance Improvements

* **ci:** la suite C++ de Apple clang sale a un job propio — por latencia de feedback, no por camino crítico ([#90](https://github.com/mati-araujo/watermelon-audio/issues/90)) ([6bd3d4e](https://github.com/mati-araujo/watermelon-audio/commit/6bd3d4e26b8222dc4ff16ea27b54e5def596370d))

## [1.13.2](https://github.com/mati-araujo/watermelon-audio/compare/v1.13.1...v1.13.2) (2026-07-29)


### Bug Fixes

* **engines:** el SoundFont ya no queda clavado a la tasa que tenia al cargarse ([#82](https://github.com/mati-araujo/watermelon-audio/issues/82)) ([044135e](https://github.com/mati-araujo/watermelon-audio/commit/044135ef688451e4e4004b7ca9db5f1228f0b1ca))
* **engines:** hazard pointer para el retiro de SoundFonts — la ranura unica era un UAF ([#84](https://github.com/mati-araujo/watermelon-audio/issues/84)) ([926be3b](https://github.com/mati-araujo/watermelon-audio/commit/926be3b5a330f4602c44b8aa351b64363efc08cf))

## [1.13.1](https://github.com/mati-araujo/watermelon-audio/compare/v1.13.0...v1.13.1) (2026-07-28)


### Performance Improvements

* **ci:** reducir el tiempo del job de iOS — el .a no era reproducible y tiraba la caché entera ([#78](https://github.com/mati-araujo/watermelon-audio/issues/78)) ([104e6e4](https://github.com/mati-araujo/watermelon-audio/commit/104e6e48377ce984f9d0b2c11c0cdf0d2f059e34))

## [1.13.0](https://github.com/mati-araujo/watermelon-audio/compare/v1.12.0...v1.13.0) (2026-07-28)


### Features

* **kmp:** la fachada Kotlin del bridge a commonMain — los 96 miembros que NoisyPad necesita ([#76](https://github.com/mati-araujo/watermelon-audio/issues/76)) ([0c7b896](https://github.com/mati-araujo/watermelon-audio/commit/0c7b8964c3ff0554862f54a4491073139e5cbd45))

## [1.12.0](https://github.com/mati-araujo/watermelon-audio/compare/v1.11.0...v1.12.0) (2026-07-28)


### Features

* **kmp:** el tope de efectos que SÍ usa NoisyPad, WA-1.4 a 26/26, y el SoundFont clavado a su tasa ([#73](https://github.com/mati-araujo/watermelon-audio/issues/73)) ([e75f009](https://github.com/mati-araujo/watermelon-audio/commit/e75f00967823fc5a0f6997f6f93b4f6222dc8986))

### Nota agregada a mano — el tope de efectos CAMBIA de comportamiento en gama baja

> Se agregó **después** de cortar el tag: el intento de meterla en el PR de release corrió
> carrera con el merge y el squash tomó el head sin ella. O sea que **las release notes de
> `v1.12.0` en GitHub no la traen** — sólo este archivo. Si hace falta que también estén allá,
> hay que editar la release a mano.

El `feat` de arriba no lo dice, y es lo que un consumidor necesita saber antes de subir de
versión: **`EffectManagerFactory.create(scope)` ahora recorta `maxEffects` de 12 a 6 en un
dispositivo de gama baja.** No hay que cambiar ninguna línea para que ocurra — pasa solo, al
actualizar la dependencia.

**Por qué se hizo así.** Es el camino que usa NoisyPad, y era el único tope vivo en producción:
`AudioEngineFactory.create()` ya ajustaba al dispositivo desde WA-1.2, pero NoisyPad **no usa
`AudioEngineFactory` en ninguna parte**. Recortar sólo en la entrada sin config explícita es lo
único que arregla producción sin obligar al consumidor a tocar código.

**Qué NO cambia:**

- Un consumidor que pasa su propia `EffectManagerConfig` —el `create` de tres argumentos— se
  respeta tal cual. El ajuste sólo aplica a las entradas sin config.
- Los dispositivos que no son de gama baja siguen en 12.
- El tope **nunca sube**: quien pidió 4 sigue teniendo 4.

**Lo que hay que mirar si esto molesta:** `IEffectManager.maxEffects` es visible en la UI de
NoisyPad (el `"n / 12"` del browser de efectos pasa a `"n / 6"`), y una escena de usuario con 7+
efectos guardada antes carga **parcial** en un equipo de gama baja — `loadScene` loguea cada
`addEffect` fallido y sigue, no rompe. Las 40 escenas de fábrica no se ven afectadas: la más
cargada tiene 6 efectos, medido antes de tomar la decisión.

**La moraleja, que es la misma que la de la 1.9.1 al revés:** un cambio de *comportamiento* sin
cambio de *firma* no lo detecta ningún versionado automático. El `minor` acá es correcto, pero
lo es por el tipo de commit, no porque release-please haya entendido lo que cambia. Por eso la
nota.

## [1.11.0](https://github.com/mati-araujo/watermelon-audio/compare/v1.10.0...v1.11.0) (2026-07-28)


### Features

* **kmp:** WA-1.3, el smoke 1/2/6 y WA-3.6 — el ítem 2 destapó que el recorte de maxEffects no tenía lector ([#69](https://github.com/mati-araujo/watermelon-audio/issues/69)) ([bb5fee6](https://github.com/mati-araujo/watermelon-audio/commit/bb5fee68f96fd62f4d25a3ffbc7f4c58570a296a))

## [1.10.0](https://github.com/mati-araujo/watermelon-audio/compare/v1.9.1...v1.10.0) (2026-07-28)


### Features

* los dos stubs que mentían — uno se borra, el otro dice la verdad ([#64](https://github.com/mati-araujo/watermelon-audio/issues/64)) ([a932f12](https://github.com/mati-araujo/watermelon-audio/commit/a932f129b84a6b521a31e5a20ec6086cd2a4a8f5))

## [1.9.1](https://github.com/mati-araujo/watermelon-audio/compare/v1.9.0...v1.9.1) (2026-07-27)


### Performance Improvements

* **ci:** −33% el job de iOS, y una ronda de deuda técnica, unificación y limpieza ([#61](https://github.com/mati-araujo/watermelon-audio/issues/61)) ([efea45f](https://github.com/mati-araujo/watermelon-audio/commit/efea45f99d3d8b7905c11a370147ba0a465f39c8))

### Nota agregada a mano — una remoción de API viajó dentro de ese `perf`

El commit de arriba **borra dos typealias públicos**, `UsbDeviceCompatibility.CompatibilityStatus`
y `UsbDeviceCompatibility.CompatibilityResult`. Como el tipo de commit era `perf`, release-please
calculó un **patch**, y la remoción no aparece en ningún lado de este changelog. Se deja escrito
acá en vez de re-cortar la versión, porque la ruptura es inobservable: los dos símbolos ya venían
`@Deprecated` con `ReplaceWith`, estaban anidados dentro del `object` (o sea que su nombre
calificado era `UsbDeviceCompatibility.CompatibilityStatus`, no un alias de nivel superior), y no
los importa ni este repo ni NoisyPad — que usa los tipos reales, `UsbCompatibilityStatus` y
`UsbCompatibilityResult`.

**La moraleja, que es lo que vale para la próxima:** el número de versión fue un síntoma. El
defecto real es que una remoción de API pública viaje dentro de un commit `perf(ci):`, porque ahí
el versionado automático no tiene forma de enterarse.

## [1.9.0](https://github.com/mati-araujo/watermelon-audio/compare/v1.8.1...v1.9.0) (2026-07-27)


### Features

* **ios:** el input path de iOS captura — Fase 3, WA-2.5/2.6, WA-4.1 y el harness WA-5.5 ([#59](https://github.com/mati-araujo/watermelon-audio/issues/59)) ([5dd73bb](https://github.com/mati-araujo/watermelon-audio/commit/5dd73bb44b1b704d188af99177c2c22bcc0424b7))

## [1.8.1](https://github.com/mati-araujo/watermelon-audio/compare/v1.8.0...v1.8.1) (2026-07-23)


### Bug Fixes

* **ci:** publicar aunque release-please falle post-tag (!cancelled) ([#56](https://github.com/mati-araujo/watermelon-audio/issues/56)) ([7577882](https://github.com/mati-araujo/watermelon-audio/commit/757788258b5886d99c7f3dbba9ee6fabf1eb187e))

## [1.8.0](https://github.com/mati-araujo/watermelon-audio/compare/v1.7.1...v1.8.0) (2026-07-23)


### Features

* **backends:** CoreAudioBackend para iOS/macOS (WA-2.4) ([f488993](https://github.com/mati-araujo/watermelon-audio/commit/f4889935a67562c7e001fd6bf38df72116af9459))
* CoreAudioBackend para iOS — WA-2.4 (output) ([3453003](https://github.com/mati-araujo/watermelon-audio/commit/3453003d1f5a891f1d7253b0bafbdebf6bc1fb7c))
* **ios:** PlatformApple + InputNode portable — el .a de iOS linkea sin gaps (WA-2.2 + prep WA-3) ([d1092e5](https://github.com/mati-araujo/watermelon-audio/commit/d1092e576888c34314486623206663aab7227b17))
* **ios:** PlatformApple + InputNode portable — el .a de iOS linkea sin gaps (WA-2.2, WA-3 prep) ([cdd75e4](https://github.com/mati-araujo/watermelon-audio/commit/cdd75e4679c7ffc4cb1fa5e31713abce372b4d17))

## [1.7.1](https://github.com/mati-araujo/watermelon-audio/compare/v1.7.0...v1.7.1) (2026-07-23)


### Bug Fixes

* **core:** currentSampleRate() unifica los caminos y arregla 3 bugs del backend ([c1f822d](https://github.com/mati-araujo/watermelon-audio/commit/c1f822d19c0003ad46a8c3d44bf8cf1036145a5a))
* **core:** data race + use-after-free en el fade de stopWithFade (TSan) ([10e3548](https://github.com/mati-araujo/watermelon-audio/commit/10e35487588603929e4e6712b5289880f6cc3a6c))
* **engines:** mmap64/off64_t no existen en Darwin ([158d976](https://github.com/mati-araujo/watermelon-audio/commit/158d976dea5bf347772770113eb124fa64568d5d))

## [1.7.0](https://github.com/mati-araujo/watermelon-audio/compare/v1.6.0...v1.7.0) (2026-07-22)


### Features

* **kmp:** targets iOS y commonMain realmente multiplataforma (WA-0.2) ([9fbc8b7](https://github.com/mati-araujo/watermelon-audio/commit/9fbc8b78d609844876d41096536137dd603237ff))


### Bug Fixes

* **dsp:** elimina campos muertos de FDN que rompian el build con clang ([f109a9e](https://github.com/mati-araujo/watermelon-audio/commit/f109a9e0966cd71c7996c5aaaac277f34d6353d4))
* **dsp:** mSize sin smoothing en FDN::process() — click audible al mover size ([df4c5d4](https://github.com/mati-araujo/watermelon-audio/commit/df4c5d44309811c6759b8d5add052a9dbfbabb3e))
* **dsp:** mSize sin smoothing en FDN::process() — click audible al mover size ([50e5b9f](https://github.com/mati-araujo/watermelon-audio/commit/50e5b9fc8a56a88bcba311710b09ea5bb337a72e))
* **platform:** Logger.h no compilaba con clang — gnu_printf no existe ahi ([8880fea](https://github.com/mati-araujo/watermelon-audio/commit/8880fea29029ca380d90aeaf86ad5e42721f6c1d))
* portabilidad C++ con Apple clang — desbloquea el job macOS (WA-0.3) ([3fa5dee](https://github.com/mati-araujo/watermelon-audio/commit/3fa5dee4c37816cb84b74120e8703338b9f8d7fd))
* **scripts:** run-cpp-tests.sh nunca corrio en macOS — bash 3.2 y set -u ([949c119](https://github.com/mati-araujo/watermelon-audio/commit/949c119a177c0d5ceb6458e3703307d3f710ce55))

## [1.6.0](https://github.com/mati-araujo/watermelon-audio/compare/v1.5.0...v1.6.0) (2026-07-21)


### Features

* **usb:** App V library support — native log capture + RT-env probe ([0118f8d](https://github.com/mati-araujo/watermelon-audio/commit/0118f8d6d772a14702b00ebc7b30e7701f43cc60))
* **usb:** E4 RoundTripMeasurer — physical loopback latency (Fase 5) ([e204b66](https://github.com/mati-araujo/watermelon-audio/commit/e204b66af08d9ac6ec1756fe6fe85be45a3040bd))


### Bug Fixes

* **effects:** envelope-based noise gate in DistortionEffect ([3030eb6](https://github.com/mati-araujo/watermelon-audio/commit/3030eb675122e2d18912eb6c3ae3d5dcf7711e10))
* **effects:** FilterType con underlying type fijo — UB al castear fuera de rango ([ec029d7](https://github.com/mati-araujo/watermelon-audio/commit/ec029d75ee465fbb563abfde2172097384c132eb))
* **input:** noise gate off by default + hysteresis unit bug ([10650cb](https://github.com/mati-araujo/watermelon-audio/commit/10650cbebcb31c31cdf9ad561235ecc9cdba4fad))
* **usb/effects:** costuras E5 + fixes de auditoría, con los dos jobs de sanitizers en verde ([167fd34](https://github.com/mati-araujo/watermelon-audio/commit/167fd34196d2e5ef2c544dc091db975a079e80a8))
* **usb:** audit fixes F1-F4 for jitter-budget convergence (E1-E3) ([ade3771](https://github.com/mati-araujo/watermelon-audio/commit/ade37716d8dfa91d076aa45d64294aaf87f56073))
* **usb:** audit follow-ups — round-trip error via atomic, poll widened ([6d64322](https://github.com/mati-araujo/watermelon-audio/commit/6d643229c05205b88399fb3a8374f9534c33bf52))
* **usb:** data race real en ResizableRingBuffer — el lector veía el unique_ptr mutado ([471eeb4](https://github.com/mati-araujo/watermelon-audio/commit/471eeb475c57f4e9f2490ecef9f850aa27ca50e3))
* **usb:** guard normal_distribution ctor in roundtrip test harness ([c28dbb9](https://github.com/mati-araujo/watermelon-audio/commit/c28dbb901da274e1d4c9bf47d4f70af373d926bc))
* **usb:** latency profile no longer leaks across starts + nice-fallback seed ([5577abd](https://github.com/mati-araujo/watermelon-audio/commit/5577abd5dd3b3ac0ec82496ca8506040c5f04074))

## [1.5.0](https://github.com/mati-araujo/watermelon-audio/compare/v1.4.0...v1.5.0) (2026-07-06)


### Features

* local midis ([f5a1548](https://github.com/mati-araujo/watermelon-audio/commit/f5a15485def5c89070f7f84a8886c02354314a07))

## [1.4.0](https://github.com/mati-araujo/watermelon-audio/compare/v1.3.2...v1.4.0) (2026-07-05)


### Features

* looper evolution, midis and touch 2.0 ([0850791](https://github.com/mati-araujo/watermelon-audio/commit/0850791765023c17443a1a2469590bf88e03ee4d))
* **looper:** default to the paged buffer with budget-bounded pool RAM ([537986f](https://github.com/mati-araujo/watermelon-audio/commit/537986f761ce570bc98f3d827b4a2c6bf51b8e5f))
* **looper:** widen setTrackLoopRegion frame contract to int64 ([0fa4630](https://github.com/mati-araujo/watermelon-audio/commit/0fa4630051b658a19963b88b8b892ccbae4a1946))

## [1.3.2](https://github.com/mati-araujo/watermelon-audio/compare/v1.3.1...v1.3.2) (2026-06-02)


### Bug Fixes

* publish wf ([3a04f9f](https://github.com/mati-araujo/watermelon-audio/commit/3a04f9fe8e4d282e96a01224a768381c0616d5f0))
* publish wf ([2792f26](https://github.com/mati-araujo/watermelon-audio/commit/2792f263c2481bbcb9fceb8c0ed1a8c8f1ff3cc2))

## [1.3.1](https://github.com/mati-araujo/watermelon-audio/compare/v1.3.0...v1.3.1) (2026-06-01)


### Bug Fixes

* versoin and publish workflow ([9f64e68](https://github.com/mati-araujo/watermelon-audio/commit/9f64e68a4a05cd057fa3fd23ba8626d3c2236f84))
* versoin and publish workflow ([754dd6a](https://github.com/mati-araujo/watermelon-audio/commit/754dd6a19b2be1e0d8d458d278fd45bd5691a1ea))


### Performance Improvements

* **audit:** AUD-3 currentMidiNoteFlow + AUD-4 SoundFont preset cache ([bf27051](https://github.com/mati-araujo/watermelon-audio/commit/bf2705107351e4812363054e2322258a5adb5713))

## [1.3.0](https://github.com/mati-araujo/watermelon-audio/compare/v1.2.2...v1.3.0) (2026-06-01)


### Features

* **usb:** implement stage 2 discovery and directed selection ([6fb53df](https://github.com/mati-araujo/watermelon-audio/commit/6fb53df2f1643db8f767bcaf1e1cbd29311d2f8a))
* **usb:** populate UAC2 clock source rates via RANGE query (stage 3) ([09577ca](https://github.com/mati-araujo/watermelon-audio/commit/09577ca6da5d307c3dbf638e41f81ab408c956af))


### Bug Fixes

* **audio:** configure components before starting USB backend ([8a3f97c](https://github.com/mati-araujo/watermelon-audio/commit/8a3f97cbbf27a0fcfc3ca088ffb259385ce13c29))
* **audio:** reset effect chain state on chaos_pad→input_fx transition ([e4b727b](https://github.com/mati-araujo/watermelon-audio/commit/e4b727bf992e9538e00970bd373af86843539db3))
* **audio:** reset LookaheadLimiter state on stop/start to prevent first-playback distortion ([36f446d](https://github.com/mati-araujo/watermelon-audio/commit/36f446d9940b99b2fbd0ee200ea94500ea071d57))
* **usb:** avoid permission dialog race and add cold-start auto-connect ([ee3a30d](https://github.com/mati-araujo/watermelon-audio/commit/ee3a30d2311c0919be8bf0d3d8a219f45301daaf))
* **usb:** query native snapshot directly, drop cache reliance ([f8dbb5a](https://github.com/mati-araujo/watermelon-audio/commit/f8dbb5a47a2c9f7bc9e1605f762e992b698a8c88))
* **usb:** relax minChannels for capture selection ([9e871f5](https://github.com/mati-araujo/watermelon-audio/commit/9e871f54991f5a017a3320e30b6e14617fccda8d))
* **usb:** skip redundant SET_CUR and add output peak meter ([69f0809](https://github.com/mati-araujo/watermelon-audio/commit/69f080989a00718ae3326407230948c1d77e46a0))

## [1.2.2](https://github.com/mati-araujo/watermelon-audio/compare/v1.2.1...v1.2.2) (2026-04-11)


### Bug Fixes

* **usb:** decode input PCM using the input's own bit depth, not the output's ([5ad6fbc](https://github.com/mati-araujo/watermelon-audio/commit/5ad6fbc2673b163a0af368b3dacd9ca930ba80f0))

## [1.2.1](https://github.com/mati-araujo/watermelon-audio/compare/v1.2.0...v1.2.1) (2026-04-10)


### Bug Fixes

* **usb:** write output iso packets contiguously, not slot-strided ([82f64db](https://github.com/mati-araujo/watermelon-audio/commit/82f64db9f6695d770e4a3c2b6c3d467f161b3de5))

## [1.2.0](https://github.com/mati-araujo/watermelon-audio/compare/v1.1.2...v1.2.0) (2026-04-10)


### Features

* add release-please for automated version management ([b16ff76](https://github.com/mati-araujo/watermelon-audio/commit/b16ff766b6db79d1fc70fa56a407e71e6645c954))
* synchronize Maven and C API versions via CMake ([cca3729](https://github.com/mati-araujo/watermelon-audio/commit/cca37295b231b2dda852563922b1203a13c24355))
* **test:** add RATE_NEGOTIATION_SWEEP preset to UsbAudioTestRunner ([af9e4a0](https://github.com/mati-araujo/watermelon-audio/commit/af9e4a0d21abd81a7d672e5d8f3cea886d49809c))
* **usb:** stage 1 foundations — sample rate, feedback, event-driven DSP ([ef4e4a1](https://github.com/mati-araujo/watermelon-audio/commit/ef4e4a12100936c09a49f2ad47f626be9cc50f81))


### Bug Fixes

* add publish job to release-please workflow ([a3dfb65](https://github.com/mati-araujo/watermelon-audio/commit/a3dfb65d1329d705128ded7580bb31a87b5b55e1))
* harden CI workflows and fix stale comment ([17a6399](https://github.com/mati-araujo/watermelon-audio/commit/17a6399e001e1be1c936c8af03d7b4128576c49e))
* migrate release-please to non-deprecated action ([a9b8739](https://github.com/mati-araujo/watermelon-audio/commit/a9b8739c9bc72f7a1e077fd0376d1527f8e1dfb4))
* pin NDK version, fix manifest required feature, tighten ProGuard rules ([778dac7](https://github.com/mati-araujo/watermelon-audio/commit/778dac73f807c9ad35d8005b103a7be606f0e9bc))
* set executable permission on gradlew for Linux CI ([94c9280](https://github.com/mati-araujo/watermelon-audio/commit/94c92808b8f726eb4680ec1a4fa2cea413f1d829))
* **usb:** drain pending transfers before exiting the event loop on stop ([92772f1](https://github.com/mati-araujo/watermelon-audio/commit/92772f1358fbb637eb10b848e44abc5f3d03adba))
* **usb:** run SET_CUR after set_interface_alt_setting, claim control interface ([8e6f368](https://github.com/mati-araujo/watermelon-audio/commit/8e6f36869c8f2fe1370f916ce693b5f1794f335d))
* **usb:** size iso packet slots by endpoint wMaxPacketSize and clock margin ([7dabb3d](https://github.com/mati-araujo/watermelon-audio/commit/7dabb3d2333d35bee57e3043f009aa70e81b94d1))
* **usb:** size iso packets by USB speed and pick altsetting by bit depth ([893ed1e](https://github.com/mati-araujo/watermelon-audio/commit/893ed1eab9859ccad95cd31acb5395f8b0bff1b1))

## [1.1.2](https://github.com/mati-araujo/watermelon-audio/compare/v1.1.1...v1.1.2) (2026-04-10)


### Bug Fixes

* **usb:** drain pending transfers before exiting the event loop on stop ([92772f1](https://github.com/mati-araujo/watermelon-audio/commit/92772f1358fbb637eb10b848e44abc5f3d03adba))
* **usb:** size iso packets by USB speed and pick altsetting by bit depth ([893ed1e](https://github.com/mati-araujo/watermelon-audio/commit/893ed1eab9859ccad95cd31acb5395f8b0bff1b1))

## [1.1.1](https://github.com/mati-araujo/watermelon-audio/compare/v1.1.0...v1.1.1) (2026-04-10)


### Bug Fixes

* **usb:** run SET_CUR after set_interface_alt_setting, claim control interface ([8e6f368](https://github.com/mati-araujo/watermelon-audio/commit/8e6f36869c8f2fe1370f916ce693b5f1794f335d))

## [1.1.0](https://github.com/mati-araujo/watermelon-audio/compare/v1.0.0...v1.1.0) (2026-04-10)


### Features

* **test:** add RATE_NEGOTIATION_SWEEP preset to UsbAudioTestRunner ([af9e4a0](https://github.com/mati-araujo/watermelon-audio/commit/af9e4a0d21abd81a7d672e5d8f3cea886d49809c))
* **usb:** stage 1 foundations — sample rate, feedback, event-driven DSP ([ef4e4a1](https://github.com/mati-araujo/watermelon-audio/commit/ef4e4a12100936c09a49f2ad47f626be9cc50f81))

## 1.0.0 (2026-04-09)


### Features

* add release-please for automated version management ([b16ff76](https://github.com/mati-araujo/watermelon-audio/commit/b16ff766b6db79d1fc70fa56a407e71e6645c954))
* synchronize Maven and C API versions via CMake ([cca3729](https://github.com/mati-araujo/watermelon-audio/commit/cca37295b231b2dda852563922b1203a13c24355))


### Bug Fixes

* add publish job to release-please workflow ([a3dfb65](https://github.com/mati-araujo/watermelon-audio/commit/a3dfb65d1329d705128ded7580bb31a87b5b55e1))
* harden CI workflows and fix stale comment ([17a6399](https://github.com/mati-araujo/watermelon-audio/commit/17a6399e001e1be1c936c8af03d7b4128576c49e))
* migrate release-please to non-deprecated action ([a9b8739](https://github.com/mati-araujo/watermelon-audio/commit/a9b8739c9bc72f7a1e077fd0376d1527f8e1dfb4))
* pin NDK version, fix manifest required feature, tighten ProGuard rules ([778dac7](https://github.com/mati-araujo/watermelon-audio/commit/778dac73f807c9ad35d8005b103a7be606f0e9bc))
* set executable permission on gradlew for Linux CI ([94c9280](https://github.com/mati-araujo/watermelon-audio/commit/94c92808b8f726eb4680ec1a4fa2cea413f1d829))

## 1.0.0 (2026-04-09)


### Features

* add release-please for automated version management ([b16ff76](https://github.com/mati-araujo/watermelon-audio/commit/b16ff766b6db79d1fc70fa56a407e71e6645c954))
* synchronize Maven and C API versions via CMake ([cca3729](https://github.com/mati-araujo/watermelon-audio/commit/cca37295b231b2dda852563922b1203a13c24355))


### Bug Fixes

* harden CI workflows and fix stale comment ([17a6399](https://github.com/mati-araujo/watermelon-audio/commit/17a6399e001e1be1c936c8af03d7b4128576c49e))
* migrate release-please to non-deprecated action ([a9b8739](https://github.com/mati-araujo/watermelon-audio/commit/a9b8739c9bc72f7a1e077fd0376d1527f8e1dfb4))
* pin NDK version, fix manifest required feature, tighten ProGuard rules ([778dac7](https://github.com/mati-araujo/watermelon-audio/commit/778dac73f807c9ad35d8005b103a7be606f0e9bc))
* set executable permission on gradlew for Linux CI ([94c9280](https://github.com/mati-araujo/watermelon-audio/commit/94c92808b8f726eb4680ec1a4fa2cea413f1d829))

## [Unreleased]

### Changed
- Renamed native library from `libnoisypad.so` to `libwatermelon_audio.so`
- Renamed C++ namespace from `noisypad` to `watermelon_audio`
- Pinned NDK version to 28.2.13676358 for reproducible builds
- Fixed `android.hardware.audio.output` uses-feature to `required="false"` in library manifest
- Tightened ProGuard consumer rules to keep only native JNI methods

### Added
- CI workflow (build on push/PR)
- Publish workflow (deploy to GitHub Packages on `v*` tags)
- Version synchronization between Gradle artifact and C API header
- NOTICE file with third-party license attributions (libusb LGPL-2.1, TinySoundFont MIT)
- Dependabot configuration for Gradle and GitHub Actions

### Fixed
- Removed stale libusb `.git` file (dangling reference from NoisyPad extraction)
- CI no longer silently swallows `sdkmanager` installation errors
