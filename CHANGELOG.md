# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
