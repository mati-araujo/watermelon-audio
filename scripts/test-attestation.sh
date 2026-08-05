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
# `mktemp -d -t NOMBRE` es un macOS-ismo: GNU coreutils exige un template con
# al menos tres X y falla. Este script corre en ubuntu, asi que va la forma que
# entienden los dos. Y se verifica: sin esta guarda, un $WORK vacio manda todo
# a la raiz y el test reporta 14 fallos por el motivo equivocado.
WORK="$(mktemp -d "${TMPDIR:-/tmp}/wma-attest-test.XXXXXX")"
[ -n "$WORK" ] && [ -d "$WORK" ] || { echo "no se pudo crear el directorio de trabajo" >&2; exit 1; }
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

# El filtro de prosa vive en dos lugares (el PROSE de gate-digest.py y el
# `grep -qvE` del job `changes` de ci.yml). Si divergen en la dirección
# peligrosa, un cambio de código queda fuera del digest y el gate lo atesta sin
# cubrirlo — el falso verde que todo esto existe para evitar. Nada estructural
# los sincroniza, así que se chequea acá, donde ya corre la maquinaria.
printf '\n== sincronía del filtro de prosa (ci.yml ⇄ gate-digest.py) ==\n'
if python3 scripts/gate-digest.py --check-sync > "$WORK/sync.log" 2>&1; then
    printf '  ok    %s\n' "$(cat "$WORK/sync.log")"
    PASS=$(( PASS + 1 ))
else
    printf '  FALLO filtro de prosa divergió\n'
    sed 's/^/        /' "$WORK/sync.log"
    FAIL=$(( FAIL + 1 ))
fi

# El otro lado del mismo trato. La atestacion es segura porque `push: master`
# corre el gate entero — es la segunda linea que respalda al camino rapido. Pero
# eso sólo vale si el CI de master no se puede cancelar, y con el grupo de
# concurrencia derivado del ref se cancelaba el 21% de las veces (ver el
# comentario del bloque `concurrency:` en ci.yml, que trae la medición).
#
# ALCANCE, y hay que leerlo literal: esto detecta la DERIVA —que alguien
# simplifique el grupo y le saque el SHA— y NADA MAS. No dice que la expresión
# resuelva bien: eso no es verificable desde un job, sólo conductualmente (dos
# runs de grupos distintos que tienen que sobrevivir juntos). Un verde acá NO es
# "el esquema anda"; es "nadie borró la pieza".
printf '\n== el CI de master no puede cancelarse (ci.yml) ==\n'
if python3 - <<'PY' > "$WORK/concurrency.log" 2>&1
import re, sys

text = open(".github/workflows/ci.yml", encoding="utf-8").read()

# El bloque de nivel superior, no el de un job. Fail-closed si no hay
# exactamente uno: 0 → el bloque se movió y este chequeo quedó ciego; >1 →
# ambiguo. En los dos casos se falla, no se adivina.
blocks = re.findall(r"^concurrency:\n((?:[ \t]+\S.*\n|[ \t]*\n)+)", text, re.M)
if len(blocks) != 1:
    sys.exit(f"esperaba exactamente un bloque `concurrency:` de nivel superior, hay {len(blocks)}")

body = blocks[0]
group = re.search(r"^\s*group:\s*(.+?)\s*$", body, re.M)
cancel = re.search(r"^\s*cancel-in-progress:\s*(.+?)\s*$", body, re.M)
if not group or not cancel:
    sys.exit(f"el bloque `concurrency:` no trae group/cancel-in-progress:\n{body}")

g = group.group(1)
faltan = [t for t in ("github.event_name == 'push'", "github.sha") if t not in g]
if faltan:
    sys.exit(
        "el grupo de concurrencia ya no deriva del SHA en `push`; falta "
        + ", ".join(repr(t) for t in faltan)
        + f"\n  group: {g}\n"
        "  Con el mismo grupo para todo master, cada merge cancela el CI del\n"
        "  merge anterior — y desde local-first ese es el unico lugar donde\n"
        "  ios/build/cpp-tests-macos se ejecutan de verdad."
    )

# Si esto dejara de ser `true`, los PRs pararian de cancelar sus runs viejos: no
# es inseguro, pero es un cambio de comportamiento que nadie pidio y que este
# archivo es el unico lugar donde se afirma.
if cancel.group(1) != "true":
    sys.exit(f"cancel-in-progress dejo de ser `true` (es {cancel.group(1)!r}): los PRs ya no cancelan runs viejos")

print(f"grupo por SHA en push: {g}")
PY
then
    printf '  ok    %s\n' "$(cat "$WORK/concurrency.log")"
    PASS=$(( PASS + 1 ))
else
    printf '  FALLO el grupo de concurrencia derivó\n'
    sed 's/^/        /' "$WORK/concurrency.log"
    FAIL=$(( FAIL + 1 ))
fi

# La tercera pieza del mismo trato, y la que cubre el lado EMISOR. El verificador
# de abajo es incapaz de ver este modo de falla: compara el digest atestado
# contra el arbol pusheado y los dos coinciden. Lo que no coincide es lo que los
# gates realmente ejecutaron.
#
# `gate.sh` chequea que el arbol este limpio ANTES de correr los gates y computa
# el digest DESPUES. Entre las dos cosas hay entre 2 y 25 minutos, y todo lo que
# entre al indice en esa ventana se atesta sin haber corrido por ningun gate.
# Auditado el 2026-08-05 sobre 12 PRs: nunca se disparo. El arreglo es
# re-chequear digest y arbol antes de emitir la atestacion.
#
# ALCANCE, literal, igual que el guardian de arriba: esto detecta que alguien
# BORRE el re-chequeo, y nada mas. No prueba que la ventana este cerrada —eso
# solo se ve corriendo el gate con una mutacion en el medio—. Un verde aca es
# "nadie saco la pieza", no "el esquema anda".
printf '\n== gate.sh no atesta lo que los gates no corrieron ==\n'
if python3 - <<'PY' > "$WORK/toctou.log" 2>&1
import sys

text = open("scripts/gate.sh", encoding="utf-8").read()
lines = text.splitlines()

def first(pred, desc):
    for i, ln in enumerate(lines):
        if pred(ln):
            return i
    sys.exit(f"no se encontro {desc} en scripts/gate.sh")

# El digest de arranque tiene que existir y salir de gate-digest.py: sin el, no
# hay contra que comparar y el re-chequeo de abajo seria decorativo.
t0 = first(
    lambda ln: "DIGEST_T0=" in ln and "gate-digest.py" in ln,
    "la captura del digest de arranque (`DIGEST_T0=...gate-digest.py`)",
)

# El punto de no retorno: la linea que escribe la atestacion. Todo chequeo tiene
# que estar ANTES; uno posterior no evita nada.
write = first(
    lambda ln: 'python3 - "$ATTESTATION"' in ln,
    "la escritura de la atestacion (`python3 - \"$ATTESTATION\"`)",
)

cmp_ = [i for i, ln in enumerate(lines) if '"$DIGEST_T0"' in ln and '"$DIGEST"' in ln]
if not cmp_:
    sys.exit(
        "gate.sh ya no compara el digest de arranque contra el final.\n"
        "  Sin esa comparacion, un `git add` durante los gates queda atestado sin\n"
        "  que ningun gate lo haya visto, y el CI lo honra: el digest atestado y el\n"
        "  real coinciden, porque los dos son el contenido nuevo."
    )
if min(cmp_) > write:
    sys.exit("la comparacion de digests quedo DESPUES de escribir la atestacion: no evita nada")

# Y el arbol: un cambio de prosa no mueve el digest, asi que el digest solo no
# alcanza para afirmar que los gates corrieron sobre lo que hay ahora.
dirt = [i for i, ln in enumerate(lines) if "git status --porcelain" in ln]
if len(dirt) < 2:
    sys.exit("gate.sh dejo de re-chequear el arbol antes de atestar (falta el segundo `git status --porcelain`)")
if not any(t0 < i < write for i in dirt):
    sys.exit("el re-chequeo del arbol no esta entre la captura del digest y la escritura de la atestacion")

print(f"re-chequeo presente: digest en linea {min(cmp_) + 1}, arbol antes de la linea {write + 1}")
PY
then
    printf '  ok    %s\n' "$(cat "$WORK/toctou.log")"
    PASS=$(( PASS + 1 ))
else
    printf '  FALLO gate.sh puede atestar contenido que no corrio\n'
    sed 's/^/        /' "$WORK/toctou.log"
    FAIL=$(( FAIL + 1 ))
fi

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
