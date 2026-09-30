# Inventario de lo vendorizado

Este archivo **se mantiene a mano**. Un bump de cualquier cosa vendorizada actualiza su fila
**en el mismo PR**. Todo lo de abajo se midio el 2026-09-30 con el comando que figura al lado;
`libusb.cmake`, `tinysoundfont.cmake`, `tinymidiloader.cmake`, `tsf_ext.h` y `tsf_impl.cpp`
son nuestros (como se compila / extensiones), no parte de lo vendorizado.

## libusb

| | |
|---|---|
| Version | 1.0.30 (tag `v1.0.30`, lightweight: apunta directo al commit) |
| Upstream | https://github.com/libusb/libusb |
| Commit | `87a55632db62c9bdc58cd31d3ccfa673f1bb017f` (2026-05-17) |
| Como se verifico | `gh api repos/libusb/libusb/git/ref/tags/v1.0.30 --jq .object` y `git clone --depth 1 --branch v1.0.30` + `diff -r --strip-trailing-cr -x .git <clone> audio/src/main/cpp/thirdparty/libusb` |
| Modificado localmente | ninguno, byte a byte igual al commit (incluye dotfiles; el tarball de GitHub los omite por `export-ignore`, usar clone) |
| Entro por | `bf24397` (REQ-047 S1). Antes: snapshot de master `102ab657` que decia 1.0.29. Arregla CVE-2026-47104 y CVE-2026-23679 |
| CVEs | `curl -s 'https://services.nvd.nist.gov/rest/json/cves/2.0?keywordSearch=libusb'` (9 resultados hoy) |

## TinySoundFont (`tinysoundfont/tsf.h`) — FORK

| | |
|---|---|
| Version | base v0.9 + cambios propios |
| Upstream | https://github.com/schellingb/TinySoundFont |
| Commit base | `790a219810cb0fca5defa8cdbd88e2487e5efc7a` (2025-06-05), el ultimo que toco `tsf.h` upstream (`gh api 'repos/schellingb/TinySoundFont/commits?path=tsf.h&per_page=1'`) |
| Como se verifico | la extraccion inicial (`d66ac4d`) es identica al commit base: `git show d66ac4d:<ruta>/tsf.h` vs `curl .../790a219.../tsf.h`, `diff` = 0 lineas |
| Modificado localmente | `diff --strip-trailing-cr` upstream vs local: **37 lineas quitadas, 184 agregadas (221 cambiadas)**, en 25 hunks. Commits: `git log --oneline --follow -- audio/src/main/cpp/thirdparty/tinysoundfont/tsf.h` (9 tras la extraccion: `21570f1`, `2ea53d7`, `2d79d1f`, `352f9f7`, `f096719`, `d3a7e5a`, `0c96096`, `3d3929d`, `2ca6065`) |
| `tsf_ext.h`, `tsf_impl.cpp` | nuestros: no existen en el arbol upstream del commit base |
| Politica | **NO se actualiza desde upstream** (decision tomada; upstream sin cambios en `tsf.h` desde 2025-06-05, su ultimo commit en master es de 2026-07-19). Los arreglos se hacen aca |
| Abierto | upstream issue #123 (heap over-read por indices del hydra SF2 en `tsf_load_presets`, abierto hoy): lo cubre REQ-048 (validacion del hydra en `tsf_load`) |
| CVEs | `curl -s 'https://services.nvd.nist.gov/rest/json/cves/2.0?keywordSearch=TinySoundFont'` da 0 hoy; la fuente real son los issues: `gh api 'repos/schellingb/TinySoundFont/issues?state=open'` |

## stb_vorbis (`tinysoundfont/stb_vorbis.c`)

| | |
|---|---|
| Version | 1.22 |
| Upstream | https://github.com/nothings/stb |
| Commit | `1ee679ca2ef753a528db5ba6801e1067b40481b8` (2021-07-12, "update version numbers"), el ultimo que toco el archivo (`gh api 'repos/nothings/stb/commits?path=stb_vorbis.c&per_page=1'`) |
| Como se verifico | `curl .../stb/1ee679c.../stb_vorbis.c` + `diff --strip-trailing-cr` contra el local |
| Modificado localmente | ninguno, byte a byte igual al commit |
| CVEs abiertos sin fix upstream | los 12 existen en NVD (`curl -s 'https://services.nvd.nist.gov/rest/json/cves/2.0?cveId=<id>'`) y afectan a stb_vorbis <= 1.22: CVE-2023-45675..45682, CVE-2023-47212, CVE-2026-5316 (`setup_free`), CVE-2026-5317 (`start_decoder`), CVE-2026-89266 (`start_decoder`, truncado size_t a int; NVD lo marca `Deferred`) |
| Alcanzables | el consumidor (NoisyPad) importa y descarga `.sf3` (SoundFont con muestras Vorbis) |
| Plan | REQ-048 lo reemplaza por el fork de SDL_mixer (commit `df66ae8`) con parches propios: **esta fila se reescribe ahi y los parches se listan aca (AC-048.2)** |

🔴 OSV **no conoce** CVE-2026-5316, CVE-2026-5317 ni CVE-2026-89266 (`/v1/vulns/<id>` devuelve 404); los 9 de 2023 si. Para stb_vorbis, NVD es la fuente, OSV sola da un falso limpio.

## TinyMidiLoader (`tinymidiloader/tml.h`)

| | |
|---|---|
| Version | 0.7 |
| Upstream | https://github.com/schellingb/TinySoundFont (mismo repo) |
| Commit | `472abcff8be97ff23f8196412041624fc3e34ce4` (2021-11-14), el ultimo que toco `tml.h` |
| Como se verifico | `curl .../472abcf.../tml.h` + `diff --strip-trailing-cr` contra el local |
| Modificado localmente | ninguno, byte a byte igual al commit |
| Uso | **solo tests** (`core/tests/CMakeLists.txt`, `MidiSpecHarness.h`, `tml_impl.cpp`); `git grep -n 'tml.h\|tinymidiloader' -- . ':!audio/src/main/cpp/thirdparty'`. No entra al artefacto publicado |
| CVEs | `keywordSearch=TinySoundFont` en NVD (0 hoy, y 0 con `keywordSearch=TinyMidiLoader`) |

## googletest (no vendorizado)

Lo baja CMake con `FetchContent` para la suite de host. Version **v1.18.0**, commit
`063de7e9578f82b369302001269680b4b1553359` (2026-08-10; `gh api repos/google/googletest/git/ref/tags/v1.18.0 --jq .object`,
el tag es lightweight: `type` es `commit`). El pin vive en un solo lugar, `thirdparty/googletest.cmake`, fijado por ese SHA (`GIT_TAG <sha>  # v1.18.0`), y
`scripts/check-dep-pins.py` falla si aparece otro. Un bump se verifica con el diff de nombres de `ctest -N`
antes y despues (tiene que dar vacio: REQ-047 S4 lo midio con 1465 nombres, 1.15.2 -> 1.18.0).
Solo corre en tests, no entra al artefacto.

## Lo que no es vendorizado pero Dependabot tampoco ve

Cada uno tiene UNA fuente; cambiarlo en otro lado es un bug:

- **NDK y CMake de AGP**: `gradle/libs.versions.toml`; los workflows lo leen con `scripts/android-sdk-packages.py`.
- **Xcode del CI**: la clave `xcode` de `.github/toolchain-pins.json`; los jobs de macOS la aplican con `scripts/ci-select-xcode.sh`.
- **googletest**: `audio/src/main/cpp/thirdparty/googletest.cmake` (vigilado por `scripts/check-dep-pins.py`).

Oboe no va aca: es una dependencia Maven y Dependabot la ve.

## Procedimiento

1. **Cuando**: antes de cada release y cuando Dependabot abre su PR mensual.
2. Por cada dependencia de arriba, correr su consulta de NVD (`keywordSearch=<nombre>` o `cveId=<id>`). NVD rate-limitea sin API key: dejar ~6 s entre requests (con esa pausa no hubo rechazos).
3. Consultar tambien OSV por commit: `curl -s https://api.osv.dev/v1/query -d '{"commit":"<sha>"}'`. Hoy devuelve `{}` para libusb, TinySoundFont, tml.h y googletest; para el commit de stb devuelve un ruido de `stb_image` (OSV-2020-1372), no de vorbis. **`{}` no es "limpio"**: OSV no conoce todo (ver 🔴 arriba).
4. Si aparece un CVE alcanzable: abrir un REQ o MINI **con un test de reproduccion** (un `.sf2`/`.sf3` hostil, un descriptor USB); el arreglo va con su test rojo primero.
5. Si el arreglo es un bump upstream: re-vendorizar desde el tag/commit, recorrer el `diff` de esta tabla y actualizar la fila (version, commit, fecha, resultado del `diff`) en el mismo PR.
6. Si upstream no arregla (stb_vorbis, TinySoundFont): el parche es nuestro y se lista en la fila con su commit.
