#!/usr/bin/env bash
# ============================================================================
# El lado CI de la atestacion local. Contesta UNA pregunta:
#
#   ¿Existe una prueba, sobre ESTE contenido exacto, de que <gate> ya corrio
#   verde en la maquina del desarrollador?
#
#   bash scripts/verify-attestation.sh <ios|build|cpp-tests-macos>
#
# CONTRATO, y es lo unico que hay que tener en la cabeza al tocarlo:
#
#   1. SIEMPRE termina con exit 0. Nunca hace fallar al job. Un verificador que
#      puede morir convierte un gate en un check colgado, y un check requerido
#      que no reporta bloquea el PR PARA SIEMPRE (es lo que paso con
#      `paths-ignore` y por lo que existe el job `changes`).
#
#   2. Emite `valid=true` UNICAMENTE si todo cerro. Cualquier otra cosa —falta
#      el archivo, el digest no coincide, un pin no matchea, el JSON esta roto,
#      python no esta, el evento no es un pull_request, o algo que ni previmos—
#      emite `valid=false` y el gate corre entero. La atestacion es un camino
#      rapido, no una excepcion: ante la duda se gasta de mas, nunca se saltea.
#
#   3. `false` es el default, seteado ANTES de mirar nada. Todo camino que no
#      llegue explicitamente al final deja `false` puesto.
#
# Notese que el punto 1 y el "ponerlo rojo ante lo inesperado" no se contradicen:
# ante lo inesperado el gate CORRE, y si el codigo esta mal el gate lo pone rojo.
# El rojo lo pone el trabajo real, no el verificador.
# ============================================================================
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

GATE="${1:-}"
ATTESTATION=".github/local-gate.json"
PINS=".github/toolchain-pins.json"
SCHEMA=1

# UNA sola escritura del output, hecha por el trap de EXIT. Deliberadamente NO
# se escribe `false` al principio y `true` al final: eso dependeria de que
# Actions se quede con la ultima escritura de una misma clave, y todo el
# esquema cuelga de este valor. Con el trap hay exactamente una escritura, y el
# default es el seguro: cualquier salida —incluido un `set -u` que explote o
# una señal— deja `false`.
VALID=false
on_exit() {
    [ -n "${GITHUB_OUTPUT:-}" ] && printf 'valid=%s\n' "$VALID" >> "$GITHUB_OUTPUT"
    return 0
}
trap on_exit EXIT

reject() { printf '  RECHAZADA: %s\n' "$*"; printf '  → el gate corre entero.\n'; exit 0; }

printf '== atestacion local, gate "%s" ==\n' "$GATE"

case "$GATE" in
    ios|build|cpp-tests-macos) ;;
    *) reject "gate desconocido: '$GATE'" ;;
esac

# La atestacion vale SOLO en pull_request. En `push: master` el CI paga su costo
# entero siempre, sin excepcion: es el commit que se tagea y se publica, y es lo
# unico que verifica el resultado del squash —que ningun job vio— con el mismo
# toolchain que despues construye el artefacto.
EVENT="${GITHUB_EVENT_NAME:-}"
if [ "$EVENT" != "pull_request" ]; then
    reject "el evento es '${EVENT:-<vacio>}', no pull_request"
fi

[ -f "$ATTESTATION" ] || reject "no hay $ATTESTATION"
[ -f "$PINS" ]        || reject "no hay $PINS"
command -v python3 >/dev/null 2>&1 || reject "no hay python3 para recomputar el digest"

# El digest se recomputa con la MISMA implementacion que lo emitio
# (scripts/gate-digest.py). Y como ese script esta entre los archivos que el
# digest cubre, cambiar el algoritmo invalida automaticamente toda atestacion
# vieja: no hace falta versionar nada a mano.
ACTUAL="$(python3 scripts/gate-digest.py 2>/dev/null)" || reject "no se pudo recomputar el digest"
[ -n "$ACTUAL" ] || reject "el digest recomputado salio vacio"

python3 - "$ATTESTATION" "$PINS" "$GATE" "$ACTUAL" "$SCHEMA" <<'PY'
import json, sys

att_path, pins_path, gate, actual, schema = sys.argv[1:6]

def reject(msg):
    print(f"  RECHAZADA: {msg}")
    raise SystemExit(1)

try:
    att = json.load(open(att_path))
    pins = json.load(open(pins_path))
except Exception as exc:
    reject(f"JSON ilegible: {exc}")

if att.get("schema") != int(schema):
    reject(f"schema {att.get('schema')!r}, se esperaba {schema}")

claimed = att.get("digest")
if not isinstance(claimed, str) or not claimed:
    reject("no trae campo 'digest'")
if claimed != actual:
    print(f"    atestado : {claimed}")
    print(f"    real     : {actual}")
    reject("el contenido cambio desde que se corrio el gate")

entry = (att.get("gates") or {}).get(gate)
if not isinstance(entry, dict):
    reject(f"la atestacion no cubre el gate '{gate}'")
if entry.get("status") != "pass":
    reject(f"el gate '{gate}' figura como {entry.get('status')!r}")

# Los pins: un verde con Xcode 26.6 no dice nada sobre Xcode 27. Se comparan
# TODAS las claves del archivo de pins, asi que agregar una nueva endurece el
# chequeo sin tocar este script.
versions = att.get("versions") or {}
for key, want in pins.items():
    if key.startswith("_"):
        continue
    got = versions.get(key)
    if got != want:
        reject(f"toolchain '{key}': atestado {got!r}, pin {want!r}")

print(f"    digest  {actual}")
print(f"    gate    {gate}: pass ({entry.get('seconds')}s en local)")
print(f"    creada  {att.get('created_utc')} sobre {str(att.get('head'))[:8]}")
PY

if [ $? -ne 0 ]; then
    printf '  → el gate corre entero.\n'
    exit 0
fi

printf '  VALIDA — el gate "%s" ya corrio verde sobre este contenido.\n' "$GATE"
VALID=true
exit 0
