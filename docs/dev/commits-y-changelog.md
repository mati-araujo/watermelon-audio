# Commits de etapa y el CHANGELOG

Movido del `CLAUDE.md` raiz por MINI-044 (2026-10-09). La regla corta vive en el `CLAUDE.md` raiz, «Commits y merges»; aca esta la medicion que la sostiene.

**Master sólo acepta merge commits.** Desde el 2026-08-31 `allow_squash_merge` y
`allow_rebase_merge` están en `false` (invariante 1 de ADR-0011: *"main sólo avanza por merge
commit que GitHub crea sobre un PR con checks verdes"*). Antes de eso el squash se llevó los
commits de etapa **tres veces seguidas** — REQ-019, REQ-020 y REQ-021, las dos últimas rescatadas
con los tags `stages/REQ-020` y `stages/REQ-021`.

> 🔴 **Y eso cambió lo que significa el tipo de un commit de etapa.**
>
> Con squash, lo único público era el **título del PR**: los commits de la rama se colapsaban y su
> tipo daba igual. Con merge commits, **cada commit de etapa llega a `master` y release-please lo
> lee**. O sea que el tipo de un commit de etapa pasó de ser una nota interna a ser **una entrada
> del CHANGELOG público**.
>
> **Medido** (simulación sobre `v2.13.0..v2.14.0`, reconstruyendo los 274 commits de rama de los
> once PRs contra lo que quedó squasheado):
>
> | | con squash | con merge commits |
> |---|---|---|
> | Features | 1 | **2** |
> | Bug Fixes | 1 | **3** |
> | bump | `minor` | **`minor`** (idéntico) |
>
> ✅ **El bump NO cambia**, y no había ninguno escondido: cero `BREAKING CHANGE` y cero `!` en los
> 274. Ese era el riesgo que valía medir y quedó descartado.
>
> ❌ Lo que sí aparece es **ruido**: `fix(build): el dir de cobertura del arnés va como output
> declarado` y `feat(jni): que un callback opcional en null deje rastro` son plomería interna, y
> bajo el régimen nuevo se publican como Bug Fix y Feature.
>
> **La regla, entonces:** un commit de etapa que es **interno** se tipa `chore:`, `build:`, `ci:`,
> `test:`, `docs:` o `refactor:` — todos ocultos en el changelog. Los tipos **visibles**
> (`feat:`, `fix:`, `perf:`, `revert:`) se reservan para lo que un consumidor podría leer en el
> CHANGELOG y entender. Preguntá *"¿esto le cambia algo a NoisyPad?"* antes de escribir `feat` o
> `fix` en un commit de etapa.

**El título del PR sigue importando**, pero por otra razón: es lo que se lee en el historial y lo
que nombra el merge commit en la UI. Va con prefijo convencional igual.
