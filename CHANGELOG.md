# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
