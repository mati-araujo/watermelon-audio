# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.9.1](https://github.com/mati-araujo/watermelon-audio/compare/v1.9.0...v1.9.1) (2026-07-27)


### Performance Improvements

* **ci:** −33% el job de iOS, y una ronda de deuda técnica, unificación y limpieza ([#61](https://github.com/mati-araujo/watermelon-audio/issues/61)) ([efea45f](https://github.com/mati-araujo/watermelon-audio/commit/efea45f99d3d8b7905c11a370147ba0a465f39c8))

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
