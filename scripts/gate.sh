#!/usr/bin/env bash
# ============================================================================
# EL punto de entrada del gate local. Corre en esta maquina exactamente lo que
# el CI iba a correr en los jobs `ios`, `build` y `cpp-tests-macos`, y si todo
# da verde deja una atestacion verificable en .github/local-gate.json para que
# el CI no repita el trabajo.
#
#   bash scripts/gate.sh                     # el gate completo + atestacion
#   bash scripts/gate.sh --only ios          # un solo gate, SIN atestar (iterar)
#   bash scripts/gate.sh --with-sanitizers   # + ASan/UBSan local (opt-in)
#   bash scripts/gate.sh --no-amend          # escribe la atestacion, no commitea
#
# LO QUE NO CORRE, y por que: los tres jobs de ubuntu (`cpp-tests`,
# `cpp-tests-asan`, `cpp-tests-tsan`) NUNCA se atestan y siempre corren en el
# CI. Suman 695 s de runner, corren en paralelo y jamas estan en el camino
# critico, asi que correrlos aca no ahorra un segundo. Y el TSan local esta
# PROBADO mas debil que el de Linux: una carrera sobrevivio 15 corridas locales
# y otra dio 0/60, y las dos fueron rojas a la primera en el CI. El TSan de
# Linux es la unica autoridad sobre carreras. Ver docs/ci/local_first.md §2.
#
# EL ARCHIVO DE ATESTACION LO ESCRIBE ESTE SCRIPT Y NADIE MAS. Editarlo a mano
# —o hacer que un agente lo regenere para acallar un CI rojo— no es un atajo:
# es fabricar una prueba de que se corrio algo que no se corrio. Si el gate no
# pasa, se arregla el codigo.
# ============================================================================
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

ATTESTATION=".github/local-gate.json"
PINS=".github/toolchain-pins.json"

# Timeouts. La calibracion NO se derivo de medianas por paso a proposito: un
# timeout es un detector de cuelgues, no un presupuesto de performance, y
# calibrarlo sobre una sola medicion invita falsos rojos en una maquina cargada.
#
# El riesgo de cuelgue esta CONCENTRADO en simctl: ninja, gradle, xcodebuild y
# clang terminan o fallan, pero CoreSimulator se traba. Medido el 2026-07-29 en
# esta maquina: `bootstatus` colgado 8 min, `terminate` 37 min, `install` 46 min,
# los tres sobre un simulador que ya estaba booteado. Con CoreSimulator sano el
# boot entero tarda 15,8 s.
SIMCTL_TIMEOUT=90        # holgadisimo: el boot sano tarda 16 s
GLOBAL_TIMEOUT=2700      # 45 min al gate entero, para que nada corra toda la noche
HEARTBEAT=30             # lo que dolio no fue la duracion, fue el silencio

# Ademas del timeout sobre las llamadas PROPIAS a simctl, hay uno POR PASO para
# los pasos que dependen del simulador. No es redundante: build-harness.sh hace
# sus propios `simctl terminate/install/launch` por dentro, y a esos no los
# alcanza el wrapper de aca. Se descubrio de la peor manera — la primera corrida
# de este script se colgo 29 min en `simctl launch`, con el timeout de 90 s
# puesto y sin efecto, exactamente en el paso donde ya habia fallado el dia
# anterior.
#
# Los valores son holgados a proposito (§5: un timeout es un detector de
# cuelgues, no un presupuesto): el harness sano tarda ~60 s y los tests de
# simulador ~60 s.
SIM_STEP_TIMEOUT=600     # 10 min a cada paso que toca el simulador

DO_SANITIZERS=0
DO_AMEND=1
ONLY=""

while [ $# -gt 0 ]; do
    case "$1" in
        --only)            ONLY="${2:-}"; shift 2 ;;
        --with-sanitizers) DO_SANITIZERS=1; shift ;;
        --no-amend)        DO_AMEND=0; shift ;;
        -h|--help)         sed -n '2,30p' "$0"; exit 0 ;;
        *) printf 'uso: %s [--only ios|build|cpp-tests-macos] [--with-sanitizers] [--no-amend]\n' "$0" >&2; exit 2 ;;
    esac
done

case "$ONLY" in
    ""|ios|build|cpp-tests-macos) ;;
    *) printf 'gate desconocido: %s (ios | build | cpp-tests-macos)\n' "$ONLY" >&2; exit 2 ;;
esac

# --- infraestructura -------------------------------------------------------

START_EPOCH="$(date +%s)"
STEP_FILE="$(mktemp "${TMPDIR:-/tmp}/wma-gate.XXXXXX")"
HEARTBEAT_PID=""
SIM_UDID=""
FAILED_STEP=""

# Un archivo por gate con "label<TAB>segundos<TAB>rc".
RESULTS="$(mktemp "${TMPDIR:-/tmp}/wma-gate-results.XXXXXX")"

cleanup() {
    [ -n "$HEARTBEAT_PID" ] && kill "$HEARTBEAT_PID" 2>/dev/null
    rm -f "$STEP_FILE" "$RESULTS"
}
trap cleanup EXIT

say() { printf '\n\033[1m=== %s\033[0m\n' "$*"; }

# Dormir en tajadas de 1 s, y NO en un `sleep` largo. La diferencia importa
# porque estos watchers se matan cuando el gate termina:
#
#   `kill` sobre la subshell NO mata a su `sleep` hijo. El hijo queda huerfano
#   y ademas HEREDA EL STDOUT del script, asi que sigue sosteniendo el pipe
#   hasta que vence. Con `sleep 2700` eso son 45 minutos DESPUES de que el gate
#   ya salio verde: medido, un `--only ios` que termino en 2m37s con los 6 pasos
#   en rc=0 dejo colgado el pipeline 45m02s exactos — o sea GLOBAL_TIMEOUT.
#   Redirigir a archivo lo esquivaba; cualquiera que pipeara `gate.sh` se comia
#   los 45 min creyendo que el gate seguia corriendo.
#
# Durmiendo de a 1 s el huerfano vive <= 1 s. Es la misma forma que ya usaba
# `run_with_timeout` mas abajo, que por eso nunca tuvo el problema.
nap() {
    local left="$1"
    while [ "$left" -gt 0 ]; do
        sleep 1
        left=$(( left - 1 ))
    done
}

# El heartbeat: imprime cada 30 s en que paso esta y hace cuanto. Sin esto, un
# cuelgue es 37 minutos de pantalla vacia.
start_heartbeat() {
    (
        while :; do
            nap "$HEARTBEAT"
            [ -f "$STEP_FILE" ] || exit 0
            printf '    ... [%s] %s\n' "$(fmt_elapsed $(( $(date +%s) - START_EPOCH )))" "$(cat "$STEP_FILE" 2>/dev/null)"
        done
    ) &
    HEARTBEAT_PID=$!
}

fmt_elapsed() { printf '%dm%02ds' $(( $1 / 60 )) $(( $1 % 60 )); }

# `timeout(1)` no existe en macOS y `gtimeout` requiere coreutils, asi que se
# implementa a mano. Devuelve 124 al vencer, como GNU timeout.
run_with_timeout() {
    local secs="$1"; shift
    "$@" &
    local pid=$!
    (
        local waited=0
        while [ "$waited" -lt "$secs" ]; do
            kill -0 "$pid" 2>/dev/null || exit 0
            sleep 1
            waited=$(( waited + 1 ))
        done
        kill -9 "$pid" 2>/dev/null
    ) &
    local watcher=$!
    wait "$pid" 2>/dev/null
    local rc=$?
    kill "$watcher" 2>/dev/null
    [ "$rc" -ge 128 ] && rc=124
    return "$rc"
}

simctl() { run_with_timeout "$SIMCTL_TIMEOUT" xcrun simctl "$@"; }

# Corre un paso, lo cronometra y lo anota bajo su gate. El primer fallo aborta
# el gate: sin verde en todo, no hay atestacion, asi que seguir es tiempo tirado.
step() {
    local gate="$1" label="$2"; shift 2
    printf '%s / %s' "$gate" "$label" > "$STEP_FILE"
    printf '  → %-24s ' "$label"
    local t0 t1 rc
    t0="$(date +%s)"
    if [ -n "${GATE_VERBOSE:-}" ]; then
        "$@"; rc=$?
    else
        "$@" > "/tmp/wma-gate-${gate}-${label}.log" 2>&1; rc=$?
    fi
    t1="$(date +%s)"
    printf '%s\t%s\t%s\n' "$gate:$label" "$(( t1 - t0 ))" "$rc" >> "$RESULTS"
    if [ "$rc" -eq 0 ]; then
        printf 'ok  %ss\n' "$(( t1 - t0 ))"
    else
        printf 'FALLO (rc=%s, %ss)\n' "$rc" "$(( t1 - t0 ))"
        printf '     log: /tmp/wma-gate-%s-%s.log\n' "$gate" "$label"
        [ "$rc" -eq 124 ] && printf '     (venció el timeout de %ss)\n' "$SIMCTL_TIMEOUT"
        FAILED_STEP="$gate:$label"
        return 1
    fi
}

# --- precondiciones --------------------------------------------------------

say "precondiciones"

if [ -z "$ONLY" ] && [ -n "$(git status --porcelain --untracked-files=normal)" ]; then
    cat >&2 <<'EOF'
El arbol de trabajo no esta limpio.

El digest de la atestacion sale del INDICE de git, o sea del contenido exacto
que el CI va a checkoutear. Correr el gate sobre un arbol sucio atestaria algo
distinto de lo que se pushea: commitea (o stashea) y volve a correr.

Para iterar sin atestar: bash scripts/gate.sh --only <gate>
EOF
    git status --short >&2
    exit 1
fi

# El digest de ARRANQUE. Se captura en el mismo instante que la precondicion de
# arriba —y por eso vive pegado a ella— porque es contra esto que se re-chequea
# al final, antes de atestar. El porque entero esta en el bloque "atestacion".
DIGEST_T0=""
[ -z "$ONLY" ] && DIGEST_T0="$(python3 scripts/gate-digest.py 2>/dev/null)"

# Versiones de herramienta observadas. Se comparan contra los pins ANTES de
# gastar un minuto: si el toolchain derivo, el gate corre igual pero no atesta,
# y es mejor saberlo ahora que en 6 minutos.
observe_versions() {
    XCODE_V="$(xcodebuild -version 2>/dev/null | tr '\n' ' ' | sed -E 's/Xcode ([^ ]+) Build version ([^ ]+).*/\1 (\2)/')"
    CLANG_V="$(xcrun clang --version 2>/dev/null | head -1)"
    IOS_SDK_V="$(xcrun --sdk iphoneos --show-sdk-version 2>/dev/null)"
    CMAKE_V="$(cmake --version 2>/dev/null | head -1 | sed -E 's/cmake version //')"
    NINJA_V="$(ninja --version 2>/dev/null)"
    JVM_V="$(./gradlew --version 2>/dev/null | sed -nE 's/^Launcher JVM: +([0-9.]+).*/\1/p')"
    SIM_RUNTIME_V="$(xcrun simctl list runtimes -j 2>/dev/null | python3 -c '
import json,sys
rs=[r["version"] for r in json.load(sys.stdin)["runtimes"] if r["platform"]=="iOS" and r["isAvailable"]]
print(sorted(rs, key=lambda v: [int(x) for x in v.split(".")])[-1] if rs else "")' 2>/dev/null)"
}

check_pins() {
    python3 - "$PINS" "$XCODE_V" "$CLANG_V" "$IOS_SDK_V" "$SIM_RUNTIME_V" "$CMAKE_V" "$NINJA_V" "$JVM_V" <<'PY'
import json, sys
pins = json.load(open(sys.argv[1]))
keys = ["xcode", "clang", "ios_sdk", "simulator_runtime", "cmake", "ninja", "launcher_jvm"]
bad = []
for key, got in zip(keys, sys.argv[2:]):
    want = pins.get(key, "")
    if got != want:
        bad.append(f"  {key}:\n    pin      = {want!r}\n    observado= {got!r}")
if bad:
    print("El toolchain local no coincide con " + sys.argv[1] + ":", file=sys.stderr)
    print("\n".join(bad), file=sys.stderr)
    sys.exit(1)
PY
}

observe_versions
if check_pins; then
    PINS_OK=1
    printf '  toolchain: coincide con %s\n' "$PINS"
else
    PINS_OK=0
    cat >&2 <<'EOF'

  El gate va a correr igual, pero NO va a emitir atestacion: una prueba que
  dice "verificado" sin decir con que, no prueba nada. Si el toolchain nuevo es
  el que corresponde, actualiza .github/toolchain-pins.json — eso cambia el
  digest y fuerza una corrida completa en CI, que es lo correcto.
EOF
fi

# Higiene de simulador. CoreSimulator acumula estado ENTRE SESIONES en una
# maquina de desarrollo, cosa que un runner del CI nunca sufre porque arranca
# limpio siempre. Sin esto, tres operaciones de simctl seguidas se colgaron.
sim_hygiene() {
    say "higiene de simulador"
    pkill -f 'simctl spawn' 2>/dev/null
    simctl shutdown all >/dev/null 2>&1
    printf '  simuladores apagados\n'
}

boot_simulator() {
    SIM_UDID="$(xcrun simctl list devices available -j 2>/dev/null | python3 -c '
import json,sys
d=json.load(sys.stdin)["devices"]
print(next((x["udid"] for v in d.values() for x in v if "iPhone" in x["name"]), ""))')"
    if [ -z "$SIM_UDID" ]; then
        printf '  no hay ningun iPhone disponible en este Xcode\n' >&2
        return 1
    fi
    simctl boot "$SIM_UDID" >/dev/null 2>&1
    simctl bootstatus "$SIM_UDID" -b >/dev/null 2>&1 || return 1
    export SIM_UDID
    printf '  simulador listo: %s\n' "$SIM_UDID"
}

# --- los gates -------------------------------------------------------------

gate_guardrails() {
    say "guardrails (segundos, y fallan rapido)"
    step guardrails portability   bash scripts/check-cpp-portability.sh || return 1
    step guardrails no-ui         bash scripts/check-no-ui-in-library.sh || return 1
    # WD-1.1 — el --self-test va PRIMERO y no es ceremonia: si el parser se
    # rompe, el lint queda en verde permanente, que es el peor estado posible
    # para un guardrail. Verificarlo cuesta 2 s.
    step guardrails rt-safety-self python3 scripts/check-rt-safety.py --self-test || return 1
    step guardrails rt-safety     python3 scripts/check-rt-safety.py || return 1
    # REQ-002 · S4 — misma disciplina que arriba: el self-test PRIMERO.
    # Este lint no busca "esperas sospechosas" por su forma —eso se evade sin
    # querer— sino que toda espera cruda en un test este CLASIFICADA. De 28, la
    # mayoria NO eran defectos: 13 polling y 8 estimulo. Clasificar era el trabajo.
    step guardrails waits-self    python3 scripts/check-test-waits.py --self-test || return 1
    step guardrails waits         python3 scripts/check-test-waits.py || return 1

    # REQ-006.4 — que un subsistema no vuelva a prepararse con un rate LITERAL.
    # No persigue el numero: persigue las LLAMADAS a preparar, que es donde el
    # rate real casi siempre esta disponible. El self-test va primero por la
    # misma razon que los otros dos: si el parser se rompe, el lint queda verde
    # para siempre.
    step guardrails rate-self     python3 scripts/check-literal-rate.py --self-test || return 1
    step guardrails literal-rate  python3 scripts/check-literal-rate.py || return 1

    # MINI-004 — que el contrato de ITuner ejerza TODA implementacion del modulo.
    # Kotlin/Native no tiene reflection, asi que las implementaciones se descubren
    # LEYENDO EL FUENTE y se comparan contra la lista de TunerSubjects.kt en las dos
    # direcciones. Sin esto la parametrizacion seria cosmetica: el tercer implementador
    # entra, nadie lo suma a la lista, y el contrato vuelve a probar dos — verde, y
    # todavia llamandose "contrato".
    step guardrails ituner-self   python3 scripts/check-ituner-implementations.py --self-test || return 1
    step guardrails ituner-impls  python3 scripts/check-ituner-implementations.py || return 1
    # REQ-013 — "?quien LLAMA a esto?". REQ-012 entrego un mecanismo completo,
    # verificado con TSan y mutacion, que nadie llamaba en produccion: en un
    # telefono el DSP de entrada seguia sin seguir al rate, con la suite entera
    # en verde. Lo destapo un `grep` al terminar S3.
    #
    # El self-test PRIMERO, igual que los cuatro de arriba y por lo mismo: este
    # lint tiene una degradacion silenciosa propia —si `DEF_RE` deja de matchear,
    # el conjunto detectado queda vacio, coincide con "no hay deuda nueva" y el
    # lint queda VERDE PARA SIEMPRE revisando nada.
    step guardrails callers-self  python3 scripts/check-mechanism-callers.py --self-test || return 1
    step guardrails callers       python3 scripts/check-mechanism-callers.py || return 1
    step guardrails doc-self      python3 scripts/check-doc-counts.py --self-test || return 1
    step guardrails doc-counts    python3 scripts/check-doc-counts.py || return 1
}

gate_cpp_tests_macos() {
    say "cpp-tests-macos — la suite bajo Apple clang + libc++"
    step cpp-tests-macos suite    bash scripts/run-cpp-tests.sh || return 1
}

gate_ios() {
    say "ios"
    # ESTE PASO ES OBLIGATORIO Y VA PRIMERO. No es redundante con Gradle, aunque
    # lo parezca: los inputs declarados de `buildIosNativeLib` son SOLO el arbol
    # de C++ (build-logic/.../KmpNativeConventionPlugin.kt), no este script. Un
    # cambio en scripts/build-ios.sh sin cambios en C++ dejaria la task
    # UP-TO-DATE y el .a viejo sobreviviria. Corriendolo suelto, CMake reconstruye
    # y, como cinterop declara el CONTENIDO del .a como input, la invalidacion
    # cascadea. Medido el 2026-07-29: con el .a cambiado, 7 de 12 tasks se
    # re-ejecutan y iosSimulatorArm64Test pasa de 2,4 s (UP-TO-DATE) a 58,6 s.
    step ios build-ios            bash scripts/build-ios.sh || return 1

    step ios compile-kotlin       ./gradlew :audio:compileKotlinIosArm64 :audio:compileKotlinIosSimulatorArm64 || return 1

    boot_simulator || return 1
    sim_step ios sim-test         ./gradlew :audio:iosSimulatorArm64Test || return 1

    # EL XCFRAMEWORK NO VA ACA, y es deliberado (2026-08-05). En `ci.yml` sus dos
    # pasos corren SOLO fuera de `pull_request`, asi que el gate tampoco los
    # corre: el contrato de este script es reproducir lo que el CI iba a correr
    # en el PR, no mas.
    #
    # Por que se lo saco del camino caliente, medido:
    #   - CERO consumidores. Ni NoisyPad (consume la coordenada Gradle KMP), ni
    #     :harness (embebe su propio HarnessKit.framework), ni nada afuera. Y no
    #     se distribuye: los releases no llevan assets y a Packages sube el
    #     artefacto KMP, no el XCFramework.
    #   - Su justificacion unica —"es lo unico que prueba que el .a embebido en
    #     el klib resuelve al linkear un binario"— ya la cubre el harness:
    #     HarnessKit.framework lleva los MISMOS 253 simbolos wma_*, y ademas
    #     arranca la app.
    #   - Costaba 138 s de los 637 s del job `ios`, y aca era el paso que hizo
    #     saltar DOS VECES el techo global de 45 min: 2 s con el build caliente,
    #     772 s tras un bump de toolchain, 2097 s tras tocar AudioEngine.h.
    #
    # Lo que se pierde pre-merge es UNA cosa y esta acotada: el link del
    # framework para el slice de DEVICE (ios-arm64). El simulador lo cubre el
    # harness y el .a de device lo cubre build-ios.sh; una rotura de ese link
    # ahora aparece como master rojo, no como PR rojo. Cada merge a master lo
    # sigue verificando, asi que nunca se convierte en sorpresa de release.
    #
    # `scripts/test-attestation.sh` tiene el guardian de paridad: si el paso
    # vuelve al camino de PR de ci.yml sin volver aca, falla.
    sim_step ios harness          bash scripts/build-harness.sh --ios-only || return 1
}

# Un paso que toca el simulador: con techo de tiempo, y con UN reintento despues
# de resetear CoreSimulator. El reintento no es optimismo — es la unica cura
# medida: con CoreSimulator podrido las operaciones cuelgan indefinidamente, y
# despues de `shutdown all` + matar el servicio el boot entero pasa a tardar
# 15,8 s. Un solo reintento: si vuelve a colgarse, el gate es rojo y el CI corre
# entero, que es el default seguro.
sim_step() {
    local gate="$1" label="$2"; shift 2
    if step "$gate" "$label" run_with_timeout "$SIM_STEP_TIMEOUT" "$@"; then
        return 0
    fi
    printf '     el paso toca el simulador y no cerro — resetenado CoreSimulator y reintentando UNA vez\n'
    pkill -f 'simctl (launch|install|terminate|spawn)' 2>/dev/null
    simctl shutdown all >/dev/null 2>&1
    killall -9 com.apple.CoreSimulator.CoreSimulatorService >/dev/null 2>&1
    sleep 5
    boot_simulator || return 1
    step "$gate" "$label-retry" run_with_timeout "$SIM_STEP_TIMEOUT" "$@"
}

gate_build() {
    say "build — la mitad Android"
    step build kotlin-unit        ./gradlew :audio:testDebugUnitTest || return 1
    step build assemble-debug     ./gradlew :audio:assembleDebug || return 1
    step build harness-android    bash scripts/build-harness.sh --android-only || return 1
    step build assemble-release   ./gradlew :audio:assembleRelease || return 1

    # MINI-001 — toda `external fun` del bridge tiene su simbolo JNI en el .so.
    #
    # VA ACA Y NO EN guardrails, y no es acomodo: necesita el .so, que recien
    # existe despues de assemble-release. Los guardrails corren en segundos y
    # ANTES de construir nada.
    #
    # El self-test va PRIMERO, con la misma disciplina que rt-safety y waits: un
    # chequeo que no sabe fallar da verde para siempre y se ve identico a uno que
    # anda. Y el chequeo real FALLA si no pudo chequear (sin .so, sin nm, cero
    # declaraciones): "no pude mirar" nunca es un pase.
    step build jni-symbols-self   python3 scripts/check-jni-symbols.py --self-test || return 1
    step build jni-symbols        python3 scripts/check-jni-symbols.py || return 1
}

gate_sanitizers() {
    say "sanitizers locales (opt-in — NO se atestan, el CI es la autoridad)"
    step sanitizers asan-ubsan \
        env SANITIZE=address,undefined \
            ASAN_OPTIONS=abort_on_error=1 \
            UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            bash scripts/run-cpp-tests.sh || return 1
}

# --- corrida ---------------------------------------------------------------

start_heartbeat

# El techo global. Si el gate entero se pasa de 45 min, algo se colgo de una
# forma que los timeouts de simctl no cubren.
#
# `nap` y no `sleep`, por lo explicado alla arriba: era ESTE watcher el que
# dejaba el huerfano de 45 min sosteniendo el stdout del gate. Y `>/dev/null`
# porque no escribe una sola linea a stdout —su aviso va a stderr—, asi que
# ademas le sacamos la posibilidad estructural de retener el pipe.
( nap "$GLOBAL_TIMEOUT"; printf '\n!! el gate supero el techo global de %ss — abortando\n' "$GLOBAL_TIMEOUT" >&2; kill -9 $$ 2>/dev/null ) >/dev/null &
GLOBAL_WATCHER=$!

sim_hygiene

RC=0
if [ -n "$ONLY" ]; then
    case "$ONLY" in
        ios)             gate_ios || RC=1 ;;
        build)           gate_build || RC=1 ;;
        cpp-tests-macos) gate_cpp_tests_macos || RC=1 ;;
    esac
else
    # Orden fail-fast: lo que cuesta segundos y falla seguido, primero.
    gate_guardrails       || RC=1
    [ "$RC" -eq 0 ] && { gate_cpp_tests_macos || RC=1; }
    [ "$RC" -eq 0 ] && { gate_ios   || RC=1; }
    [ "$RC" -eq 0 ] && { gate_build || RC=1; }
    [ "$RC" -eq 0 ] && [ "$DO_SANITIZERS" -eq 1 ] && { gate_sanitizers || RC=1; }
fi

kill "$GLOBAL_WATCHER" 2>/dev/null
kill "$HEARTBEAT_PID" 2>/dev/null; HEARTBEAT_PID=""

TOTAL=$(( $(date +%s) - START_EPOCH ))

say "resumen  ($(fmt_elapsed "$TOTAL"))"
awk -F'\t' '{ printf "  %-34s %5ss  rc=%s\n", $1, $2, $3 }' "$RESULTS"

if [ "$RC" -ne 0 ]; then
    printf '\n\033[1mGATE ROJO\033[0m — fallo %s. No se emite atestacion; el CI va a correr todo.\n' "$FAILED_STEP" >&2
    exit 1
fi

if [ -n "$ONLY" ]; then
    printf '\nOK (--only %s). NO se emite atestacion: sólo la corrida completa atesta.\n' "$ONLY"
    exit 0
fi

if [ "$PINS_OK" -ne 1 ]; then
    printf '\nGATE VERDE, pero SIN atestacion: el toolchain no coincide con %s.\n' "$PINS" >&2
    exit 0
fi

# --- la atestacion ---------------------------------------------------------

say "atestacion"

# LA PRECONDICION DE ARBOL LIMPIO VALE PARA UN INSTANTE: el de su propia
# ejecucion. Los gates tardan entre 2 y 25 minutos y el digest se computa ACA,
# al final — asi que sin re-chequear, cualquier cosa que entre al indice en el
# medio (vos que seguis trabajando, otra terminal, un agente que aplica un fix)
# queda atestada sin que ningun gate la haya visto. Y el CI la honra igual: el
# digest atestado y el real coinciden, porque los dos son el contenido NUEVO.
# Es un falso verde que no deja rastro en ningun log.
#
# Auditado el 2026-08-05 sobre los 12 PRs de 405474c..702dac1: nunca se disparo.
# El agujero era latente, no observado — pero es reproducible en tres comandos.
# Ver docs/ci/local_first.md §7.12.
#
# Se re-chequean las DOS cosas que la precondicion garantizaba, porque prueban
# cosas distintas: el DIGEST es lo que el CI compara, y el ARBOL LIMPIO es lo
# que hace que los gates hayan corrido sobre el indice y no sobre otra cosa. Un
# cambio de prosa mueve el segundo y no el primero, y se rechaza igual: enumerar
# que prosa puede o no influir en un gate es probar una ausencia, y sale mas
# caro que la corrida de CI que cuesta equivocarse.
#
# Va ANTES de escribir la atestacion, y eso es load-bearing: medido el
# 2026-08-05, una corrida completa deja el arbol con EXACTAMENTE un archivo
# modificado —.github/local-gate.json— y lo escribe este script unas lineas mas
# abajo. Aca todavia esta limpio; ningun gate toca un archivo trackeado.
#
# Falla cualquiera de los dos → GATE VERDE, SIN atestacion, igual que con los
# pins. Fail-closed sobre la PRUEBA, nunca sobre el gate: el unico costo es que
# el CI corra los tres enteros, que es exactamente lo que pasaba antes de que
# existiera el camino rapido.
DIGEST="$(python3 scripts/gate-digest.py)"
DIRT="$(git status --porcelain --untracked-files=normal)"

DRIFT=""
if [ -z "$DIGEST_T0" ]; then
    DRIFT="no se pudo computar el digest al arrancar"
elif [ -z "$DIGEST" ]; then
    DRIFT="no se pudo recomputar el digest"
elif [ "$DIGEST" != "$DIGEST_T0" ]; then
    DRIFT="el contenido cambio MIENTRAS corrian los gates"
elif [ -n "$DIRT" ]; then
    DRIFT="el arbol de trabajo se ensucio MIENTRAS corrian los gates"
fi

if [ -n "$DRIFT" ]; then
    printf '\n\033[1mGATE VERDE, pero SIN atestacion\033[0m: %s.\n' "$DRIFT" >&2
    if [ -n "$DIGEST_T0" ] && [ -n "$DIGEST" ] && [ "$DIGEST" != "$DIGEST_T0" ]; then
        printf '  al arrancar : %s\n' "$DIGEST_T0" >&2
        printf '  ahora       : %s\n' "$DIGEST" >&2
    fi
    [ -n "$DIRT" ] && git status --short >&2
    cat >&2 <<'EOF'

Los gates corrieron sobre el contenido que habia al arrancar, asi que atestar el
de ahora seria firmar algo que no se verifico. El CI va a correr los tres gates
enteros. Si el cambio es intencional, volve a correr el gate.
EOF
    exit 0
fi

python3 - "$ATTESTATION" "$DIGEST" "$RESULTS" "$TOTAL" \
         "$XCODE_V" "$CLANG_V" "$IOS_SDK_V" "$SIM_RUNTIME_V" "$CMAKE_V" "$NINJA_V" "$JVM_V" <<'PY'
import json, subprocess, sys, time

out, digest, results, total = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
xcode, clang, ios_sdk, sim_rt, cmake, ninja, jvm = sys.argv[5:12]

durations = {}
for line in open(results):
    label, secs, rc = line.rstrip("\n").split("\t")
    durations.setdefault(label.split(":", 1)[0], 0)
    durations[label.split(":", 1)[0]] += int(secs)

doc = {
    "_comment": (
        "Prueba de que el gate local corrio sobre ESTE contenido. La escribe "
        "scripts/gate.sh y nadie mas; el CI la verifica con "
        "scripts/verify-attestation.sh. Editarla a mano fabrica una prueba falsa."
    ),
    "schema": 1,
    "digest": digest,
    "gates": {
        # Los tres gates que el CI puede saltear. Los guardrails y los sanitizers
        # NO figuran: los primeros los reproduce el job `cpp-tests` de ubuntu y
        # los segundos jamas se atestan.
        name: {"status": "pass", "seconds": durations.get(name, 0)}
        for name in ("cpp-tests-macos", "ios", "build")
    },
    "versions": {
        "xcode": xcode, "clang": clang, "ios_sdk": ios_sdk,
        "simulator_runtime": sim_rt, "cmake": cmake, "ninja": ninja,
        "launcher_jvm": jvm,
    },
    "total_seconds": total,
    "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "head": subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True,
                           text=True).stdout.strip(),
}
with open(out, "w") as fh:
    json.dump(doc, fh, indent=2, ensure_ascii=False)
    fh.write("\n")
print(f"  {out}")
print(f"  digest {digest}")
PY

if [ "$DO_AMEND" -eq 1 ]; then
    git add "$ATTESTATION"
    # El amend sólo agrega el archivo de atestacion, que esta EXCLUIDO del
    # digest, asi que el digest que acaba de calcularse sigue siendo correcto
    # despues del amend.
    git commit --amend --no-edit --quiet
    printf '  commiteada en %s\n' "$(git rev-parse --short HEAD)"
else
    printf '  (--no-amend: el archivo quedo sin commitear)\n'
fi

printf '\n\033[1mGATE VERDE\033[0m en %s. El CI va a saltear ios, build y cpp-tests-macos en el PR.\n' "$(fmt_elapsed "$TOTAL")"
