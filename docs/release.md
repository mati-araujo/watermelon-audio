# Release workflow

Watermelon Audio publishes Maven artifacts to GitHub Packages from tags named
`vX.Y.Z`.

## Source of truth

The library version lives in `gradle.properties`:

```properties
version=1.3.1
```

`audio/build.gradle.kts` reads that value for the Maven artifact version and
passes it to CMake so `wma_get_version()` reports the same value.

## Automated release

1. Merge feature and fix commits to `master` using Conventional Commit prefixes
   when possible: `feat:`, `fix:`, `perf:`, `build:`, `docs:`, `chore:`.
2. The `Release Please` workflow opens or updates a release PR.
3. Review that PR for:
   - `CHANGELOG.md`
   - `.release-please-manifest.json`
   - `gradle.properties`
4. Merge the release PR.
5. Release Please creates the GitHub release and `vX.Y.Z` tag.
6. The `Publish` workflow runs from the tag and publishes to GitHub Packages.

The publish workflow fails before building if the tag version does not match
`gradle.properties`.

## Manual publish checks

Use these before publishing a local or emergency release:

```bash
./gradlew :audio:assembleRelease
./gradlew :audio:publishToMavenLocal
```

For an emergency tag, update `gradle.properties`, commit it, then tag the exact
same version:

```bash
git tag v1.3.1
git push origin v1.3.1
```

Avoid passing `-Pversion=...` for releases. It can make the artifact version
different from the checked-in version metadata.
