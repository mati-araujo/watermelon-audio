#!/usr/bin/env bash
# ============================================================================
# Bloquea hasta que la corrida de CI (ci.yml, evento `push`) para UN commit
# termine en `success`, y sale 0 sólo en ese caso.
#
#   scripts/wait-for-ci.sh <commit-sha>
#
# Existe para cerrar un agujero real: hasta el 2026-07-29 el job `publish` de
# release-please.yml corría en PARALELO con el CI sobre el mismo push, sin
# depender de él. Un CI rojo —o un flake de red como el handshake de CMake que
# se vio ese mismo dia— no impedia que el artefacto se publicara. Este script
# es lo que hace que "el CI paga su costo entero antes de un release" pase de
# coincidencia temporal a garantia.
#
# CONTRATO — fail-closed, sin ambiguedad:
#   exit 0  SOLO si la corrida de CI (push) para este SHA terminó en `success`.
#   exit 1  para TODO lo demas: terminó en fail/cancelled, no aparece a tiempo,
#           la API no contesta, el JSON viene roto, o se vence el techo. Ante la
#           duda NO se publica — no publicar es el default seguro; se re-dispara.
#
# En master el CI no honra atestaciones (el evento es push, no pull_request),
# asi que `success` de esa corrida significa los 7 jobs verdes de verdad. Y un
# commit de release toca gradle.properties (no-prosa), asi que el job `changes`
# siempre dispara el gate completo: siempre hay una corrida que esperar.
# ============================================================================
set -uo pipefail

SHA="${1:?uso: scripts/wait-for-ci.sh <commit-sha>}"
REPO="${GITHUB_REPOSITORY:-$(gh repo view --json nameWithOwner -q .nameWithOwner 2>/dev/null)}"
WORKFLOW="${CI_WORKFLOW_FILE:-ci.yml}"
CEILING="${WAIT_CEILING_SECONDS:-2700}"   # 45 min: el CI tarda ~17, con margen
INTERVAL="${WAIT_INTERVAL_SECONDS:-30}"    # 30 s: la API de Actions tiene rate limit

[ -n "$REPO" ] || { echo "no se pudo determinar el repo (GITHUB_REPOSITORY)" >&2; exit 1; }

echo "esperando a que CI ($WORKFLOW, push) termine verde sobre $SHA en $REPO"
deadline=$(( $(date +%s) + CEILING ))

while :; do
    # La corrida `push` de ci.yml para este SHA exacto. Puede no existir todavia
    # (el CI y release-please arrancan juntos; publish empieza a esperar mientras
    # el CI recien corre) — eso NO es un fallo, es "seguir esperando".
    json="$(gh api "repos/$REPO/actions/workflows/$WORKFLOW/runs?head_sha=$SHA&event=push&per_page=20" 2>/dev/null)" || json=""

    # Un token "STATUS CONCLUSION". Se parsea con expansiones de shell, sin
    # heredoc anidado: la version anterior usaba un f-string con comillas
    # escapadas dentro de un `read <<EOF` y se rompia en silencio, cayendo
    # siempre al caso "esperando" — un falso "todavia corriendo" sobre un CI ya
    # verde. Se descubrio probando el script, no leyendolo.
    parsed="$(printf '%s' "$json" | python3 -c '
import json, sys
try:
    runs = (json.load(sys.stdin) or {}).get("workflow_runs") or []
except Exception:
    print("BADJSON"); sys.exit()
if not runs:
    print("NORUN"); sys.exit()
r = sorted(runs, key=lambda x: x.get("run_number", 0))[-1]
print((r.get("status") or "UNKNOWN") + " " + (r.get("conclusion") or "none"))
' 2>/dev/null)"
    st="${parsed%% *}"
    conc="${parsed#* }"
    [ "$conc" = "$parsed" ] && conc=""   # sin espacio (NORUN/BADJSON): no hay conclusion

    case "$st" in
        completed)
            if [ "$conc" = "success" ]; then
                echo "CI verde sobre $SHA — se puede publicar"
                exit 0
            fi
            echo "CI terminó en '${conc:-?}' sobre $SHA — NO se publica" >&2
            exit 1
            ;;
        NORUN|BADJSON|"")
            echo "  ... todavia no hay corrida de CI legible para $SHA; esperando"
            ;;
        *)
            echo "  ... CI en estado '$st'; esperando"
            ;;
    esac

    if [ "$(date +%s)" -ge "$deadline" ]; then
        echo "venció el techo de ${CEILING}s esperando al CI de $SHA — NO se publica" >&2
        exit 1
    fi
    sleep "$INTERVAL"
done
