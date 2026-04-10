# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
