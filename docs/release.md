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
6. The `Release Please` workflow publishes the tag to GitHub Packages.

The publish job fails before building if the tag version does not match
`gradle.properties`. There is also a separate `Publish` workflow for manual or
emergency re-runs against an existing tag.

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

If the tag already exists but the package was not published, run the `Publish`
workflow manually from GitHub Actions and choose the existing tag ref.

Avoid passing `-Pversion=...` for releases. It can make the artifact version
different from the checked-in version metadata.
