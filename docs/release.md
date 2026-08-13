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
`gradle.properties`. Its **first** step is `scripts/wait-for-ci.sh`, which blocks
until the CI of that exact commit is green and is fail-closed: no green CI, no
publish. There is also a separate `Publish` workflow for manual or emergency
re-runs against an existing tag, which deliberately does **not** wait.

## Verificar el release CONTRA EL REGISTRO

**Un paso de publish en verde NO prueba que el paquete esté arriba.** En v1.8.0
quedaron el tag y el GitHub Release creados con **cero artefacto** en Packages, y
todos los pasos figuraban `success`. Un `BUILD SUCCESSFUL` de Gradle y una versión
en el registro no son la misma prueba.

### La credencial, que es la parte que confunde

**No sirve el token de `gh`.** El de `gh auth login` es un OAuth propio de la CLI
(`gho_…`) y **no** trae scope de packages, así que `gh api user/packages` devuelve
`403 You need at least read:packages scope`. Ese 403 se dio por permanente durante
cuatro sesiones —y anotado como tal en este repo— hasta que se preguntó de qué
credencial se estaba hablando.

El que sirve es el **PAT que ya usás para consumir Packages** (ver
`CONTRIBUTING.md` §Credentials). En esta máquina, hoy, está en
`NoisyPad/local.properties` como `gpr.key`, con scopes `repo, workflow,
write:packages` — alcanza para listar. Para encontrarlo sin imprimirlo:

```bash
grep -rl '^gpr\.key=' ~/.gradle/gradle.properties ~/Documents/GitHub/*/local.properties 2>/dev/null
```

### Los comandos

El token va **por entorno a un solo proceso**: no se imprime, no se exporta a la
sesión y no se escribe en ningún lado.

```bash
TOK="$(sed -n 's/^gpr\.key=//p' ~/Documents/GitHub/NoisyPad/local.properties | head -1 | tr -d '\r')"

# 1. Qué paquetes existen
GH_TOKEN="$TOK" gh api "user/packages?package_type=maven" \
  --jq '.[] | "\(.name)  versiones=\(.version_count)"'

# 2. Si la versión está en UNO de ellos (el nombre va URL-encodeado)
GH_TOKEN="$TOK" gh api "user/packages/maven/com.watermellonstudios.audio/versions?per_page=100" \
  --jq '[.[] | select(.name=="2.0.2")] | .[0] // "AUSENTE"'
```

### Los CUATRO artefactos, y hay que chequear los cuatro

Un release está completo sólo si la versión aparece en **todos**:

| paquete | qué es |
|---|---|
| `com.watermellonstudios.audio` | la metadata KMP — **es la coordenada que consume NoisyPad** |
| `com.watermellonstudios.audio-android` | el AAR |
| `com.watermellonstudios.audio-iosarm64` | el klib de device |
| `com.watermellonstudios.audio-iossimulatorarm64` | el klib de simulador |

Que aparezca sólo el `-android` es exactamente el modo de falla que dejaría a un
consumidor iOS sin nada, con el release y el changelog diciendo que salió.

> **Ojo con el grep del log**, si además querés cruzarlo contra la salida de
> Gradle: `publish[A-Za-z]+PublicationTo…` reporta **2 de 4**, porque `IosArm64` e
> `IosSimulatorArm64` llevan dígitos. Va `[A-Za-z0-9]+`. Un patrón que no matchea
> se lee igual que un release incompleto.

**2.0.2 fue el primero verificado así** (2026-08-13): presente en los cuatro.

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
