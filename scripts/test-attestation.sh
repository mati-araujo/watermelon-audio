#!/usr/bin/env bash
# ============================================================================
# El test del verificador. Arma arboles mutados en un clon descartable y afirma
# que scripts/verify-attestation.sh los RECHAZA.
#
# Por que existe: el experimento manual de 5 pushes prueba el cableado HOY.
# Esto es lo que sigue vivo despues. El modo de falla que cubre es el peor de
# todos —un refactor convierte el verificador en un pasa-todo y nada se pone
# rojo— y no lo cubre ningun otro test del repo.
#
# Corre en segundos, sin toolchain: solo git y python3. Por eso vive en el job
# `cpp-tests` de ubuntu y no en ninguno de macOS.
#
#   bash scripts/test-attestation.sh
# ============================================================================
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d -t wma-attest-test)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0

# El verificador solo habla por GITHUB_OUTPUT, asi que el test lee de ahi.
# Corre siempre con GITHUB_EVENT_NAME=pull_request: si no, rechazaria todo por
# el motivo equivocado y los casos negativos pasarian por accidente.
# Los artefactos del test viven FUERA del arbol verificado, siempre. Cuando
# vivian adentro, el `git add -A` del caso siguiente los commiteaba, entraban al
# digest por ser archivos no-prosa, y tres casos negativos empezaron a rechazar
# por digest en vez de por la rama que querian probar. Pasaban igual — por eso
# `expect` afirma el MOTIVO y no solo el rechazo.
# La ruta del log es DETERMINISTICA a partir de (dir, gate) en vez de quedar en
# una variable: `verify` se llama dentro de $( ), o sea en un subshell, y
# cualquier asignacion suya muere ahi. Es el mismo tipo de error que el anterior
# —el test mintiendo sobre si mismo— y lo destapo el mismo aserto de motivo.
logpath() { printf '%s/log-%s-%s' "$WORK" "$(basename "$1")" "$2"; }

verify() {  # verify <dir> <gate> -> imprime true|false; log en $(logpath dir gate)
    local dir="$1" gate="$2"
    local tag; tag="$(basename "$dir")-$gate"
    local out="$WORK/out-$tag"
    local LOG; LOG="$(logpath "$dir" "$gate")"
    : > "$out"
    ( cd "$dir" && GITHUB_OUTPUT="$out" GITHUB_EVENT_NAME=pull_request \
        bash scripts/verify-attestation.sh "$gate" ) > "$LOG" 2>&1
    local rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "EXIT-$rc"   # el contrato dice que SIEMPRE sale 0
        return
    fi
    sed -n 's/^valid=//p' "$out" | tail -1
}

expect() {  # expect <esperado> <obtenido> <descripcion> <log> [motivo esperado]
    local want="$1" got="$2" desc="$3" log="$4" reason="${5:-}"
    if [ "$got" != "$want" ]; then
        printf '  FALLO %s — esperaba valid=%s, obtuvo %s\n' "$desc" "$want" "$got"
        printf '        (log: %s)\n' "$log"
        FAIL=$(( FAIL + 1 ))
        return
    fi
    # Rechazar no alcanza: tiene que rechazar POR EL MOTIVO CORRECTO. Sin esto,
    # un caso puede pasar por accidente —p.ej. mutando un archivo que ademas
    # esta dentro del digest— y creerse que ejercita una rama que nunca corrio.
    if [ -n "$reason" ] && ! grep -qF "$reason" "$log" 2>/dev/null; then
        printf '  FALLO %s — rechazo, pero por otro motivo (esperaba "%s")\n' "$desc" "$reason"
        printf '        %s\n' "$(grep -m1 'RECHAZADA' "$log" 2>/dev/null)"
        FAIL=$(( FAIL + 1 ))
        return
    fi
    printf '  ok    %-22s %s\n' "$desc" "$(grep -m1 'RECHAZADA' "$log" 2>/dev/null | sed 's/^ *RECHAZADA: //')"
    PASS=$(( PASS + 1 ))
}

# --- un arbol de referencia, con atestacion valida -------------------------

BASE="$WORK/base"
git clone --quiet --no-hardlinks --depth 1 "file://$REPO_ROOT" "$BASE" 2>/dev/null \
    || git clone --quiet --no-hardlinks "$REPO_ROOT" "$BASE"
cd "$BASE"

# La atestacion se fabrica aca a proposito: el test verifica el VERIFICADOR, no
# el gate. Se construye con el digest real del arbol clonado y con los pins
# reales, que es exactamente lo que gate.sh habria escrito tras un verde.
make_attestation() {
    local digest; digest="$(python3 scripts/gate-digest.py)"
    python3 - "$digest" <<'PY'
import json
pins = json.load(open(".github/toolchain-pins.json"))
versions = {k: v for k, v in pins.items() if not k.startswith("_")}
import sys
json.dump({
    "schema": 1,
    "digest": sys.argv[1],
    "gates": {g: {"status": "pass", "seconds": 1}
              for g in ("cpp-tests-macos", "ios", "build")},
    "versions": versions,
    "total_seconds": 1,
    "created_utc": "2026-01-01T00:00:00Z",
    "head": "0" * 40,
}, open(".github/local-gate.json", "w"), indent=2)
PY
}

make_attestation
git -c user.email=t@t -c user.name=t add -A >/dev/null
git -c user.email=t@t -c user.name=t commit -qm "atestacion de prueba" >/dev/null

printf '\n== caso positivo ==\n'
for gate in ios build cpp-tests-macos; do
    expect true "$(verify "$BASE" "$gate")" "atestacion valida, gate $gate" "$(logpath "$BASE" "$gate")"
done

# --- los negativos, que son los que importan -------------------------------

printf '\n== casos negativos (todos tienen que RECHAZAR) ==\n'

mutate() {  # mutate <nombre> <motivo esperado> <comandos...>
    local name="$1" reason="$2"; shift 2
    local dir="$WORK/$name"
    cp -R "$BASE" "$dir"
    ( cd "$dir" && "$@" >/dev/null 2>&1
      git -c user.email=t@t -c user.name=t add -A >/dev/null 2>&1
      git -c user.email=t@t -c user.name=t commit -qm "$name" >/dev/null 2>&1 )
    expect false "$(verify "$dir" ios)" "$name" "$(logpath "$dir" ios)" "$reason"
}

# 1. Un byte de un archivo cubierto. EL caso: es lo que pasa cuando editas algo
#    despues de correr el gate y te olvidas de re-correrlo.
mutate byte-mutado "el contenido cambio" \
    bash -c 'printf "\n// byte mutado\n" >> audio/src/main/cpp/core/AudioEngine.cpp'

# 2. Sin atestacion. Un PR normal de alguien que no corrio el gate.
mutate sin-atestacion "no hay .github/local-gate.json" rm -f .github/local-gate.json

# 3. Digest corrompido a mano.
mutate digest-corrompido "el contenido cambio" \
    python3 -c 'import json;p=".github/local-gate.json";d=json.load(open(p));d["digest"]="0"*64;json.dump(d,open(p,"w"))'

# 4. Un toolchain distinto del pineado: verde con otro Xcode no dice nada de
#    este. Se muta lo que la atestacion DECLARA haber usado, no el archivo de
#    pins: los pins estan dentro del digest, asi que tocarlos rechazaria por
#    digest y la comparacion de versiones no se ejercitaria nunca. Este caso
#    existe para probar ESA rama.
mutate toolchain-distinto "toolchain 'xcode'" \
    python3 -c 'import json;p=".github/local-gate.json";d=json.load(open(p));d["versions"]["xcode"]="99.0 (ZZZZ)";json.dump(d,open(p,"w"))'

# 4b. Y el bump del pin en si: tiene que invalidar, aunque sea por digest. Es la
#     propiedad que hace que un cambio de toolchain fuerce una corrida completa.
mutate pin-bumpeado "el contenido cambio" \
    python3 -c 'import json;p=".github/toolchain-pins.json";d=json.load(open(p));d["xcode"]="99.0 (ZZZZ)";json.dump(d,open(p,"w"))'

# 5. El gate figura fallado. Un gate rojo jamas puede habilitar el camino rapido.
mutate gate-en-rojo "figura como 'fail'" \
    python3 -c 'import json;p=".github/local-gate.json";d=json.load(open(p));d["gates"]["ios"]["status"]="fail";json.dump(d,open(p,"w"))'

# 6. JSON roto. Tiene que rechazar, no explotar.
mutate json-roto "JSON ilegible" bash -c 'printf "{ no soy json" > .github/local-gate.json'

# 7. Schema de otra version: una atestacion de un formato futuro o viejo.
mutate schema-distinto "schema 99" \
    python3 -c 'import json;p=".github/local-gate.json";d=json.load(open(p));d["schema"]=99;json.dump(d,open(p,"w"))'

# 8. El gate pedido no esta cubierto por la atestacion.
mutate gate-ausente "no cubre el gate 'ios'" \
    python3 -c 'import json;p=".github/local-gate.json";d=json.load(open(p));del d["gates"]["ios"];json.dump(d,open(p,"w"))'

# 9. Fuera de pull_request: en `push: master` el CI paga entero, siempre.
printf '  --- fuera de pull_request ---\n'
OUT="$WORK/out-push"; : > "$OUT"
( cd "$BASE" && GITHUB_OUTPUT="$OUT" GITHUB_EVENT_NAME=push \
    bash scripts/verify-attestation.sh ios ) > "$WORK/log-push" 2>&1
expect false "$(sed -n 's/^valid=//p' "$OUT" | tail -1)" "evento push" "$WORK/log-push" "no pull_request"

# 10. Gate inexistente: un typo en ci.yml no puede saltear nada.
expect false "$(verify "$BASE" no-existe)" "gate desconocido" "$(logpath "$BASE" no-existe)" "gate desconocido"

printf '\n== %s ok, %s fallos ==\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
