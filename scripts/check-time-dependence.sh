#!/usr/bin/env bash
#
# check-time-dependence.sh — que test cambia de veredicto cuando se le saca el tiempo.
#
# REQ-002 · S1. Tres de cinco pushes a master fallaron el 2026-08-20 por tests
# que sincronizan con el reloj de pared: verdes en una maquina ociosa, rojos en
# un runner con siete jobs. Este script los hace reproducibles a voluntad.
#
# COMO, Y POR QUE NO ES "CARGAR LA MAQUINA"
# ------------------------------------------
# Colapsa las esperas CIEGAS (`wma_test::sleepFixed`) a cero y deja intactas las
# esperas POR CONDICION (`wma_test::waitUntil`). Un test que cambia de veredicto
# bajo eso depende del reloj; uno que no, no.
#
# Cargar la maquina NO sirve, y esta medido en S1:
#     40 quemadores sobre 10 nucleos ...............  0/10
#     taskpolicy -c background + 20 quemadores .....  1/10, y ese uno fue TIMEOUT
#     colapsar la espera ...........................  10/10, en 2 segundos
# Un `sleep_for(120ms)` es tiempo ABSOLUTO: ralentizar el mundo no achica esa
# ventana, y encima le da mas margen al worker porque el render tambien tarda mas.
#
# LO QUE ESTE INSTRUMENTO NO VE, DICHO ACA PARA QUE NO SE LEA DE MAS
# ------------------------------------------------------------------
# Cubre una clase: la espera ciega insuficiente. NO cubre la otra que REQ-002
# encontro — una espera POR CONDICION cuya condicion puede volverse inalcanzable
# (`TunerApiTest.SwitchingSourceWhileTheAnalysisIntegratesDoesNotRace` espera un
# snapshot sin alimentar audio). Medido: 0/10 en las dos escalas. Un verde de
# este script NO dice que la suite sea independiente del tiempo; dice que no
# quedan esperas ciegas insuficientes.
#
# EL SELF-TEST CORRE PRIMERO Y NO ES OPCIONAL
# --------------------------------------------
# Si el instrumento deja de colapsar las esperas no reporta nada, y eso se lee
# como "no hay defectos". Verde por ceguera — este repo ya lo pago dos veces (el
# parser del arnes de mutacion, la cobertura del lint de RT). Por eso los
# controles corren ANTES y un fallo suyo ABORTA sin decir una palabra del resto.
#
# QUE RE-CORRE (MINI-057)
# -----------------------
# Hasta su primera llamada a `sleepFixed`, un test ejecuta lo mismo a escala 1 que
# a escala 0 (nada mas lee la escala, y cada test es su proceso). Asi que solo los
# tests que LLEGARON a `sleepFixed` pueden cambiar de veredicto. `run-cpp-tests.sh`
# deja esa traza en `$BUILD/wait-trace/` (un archivo por test, ver TestWait.h) y su
# MANIFEST; este script re-corre en paralelo SOLO esos, elegidos por NUMERO de ctest.
#
#   (default)  traza valida  -> solo los de la traza
#   --full                   -> la suite entera (push a master)
#   sin MANIFEST / otro arbol / corrida filtrada / nombre que ctest no registra
#                            -> la suite entera, y dice por que. Falla cerrado.
#   MANIFEST valido pero sin el probe positivo en la traza
#                            -> exit 3, "traza ciega": nunca se cae a la suite entera, porque
#                               eso taparia justo la ceguera que el probe existe para delatar.
#
# UN NOMBRE DE LA TRAZA PUEDE SER VARIOS TESTS DE CTEST: el mismo test compilado en mas de un
# binario (looper_tests y looper_tests_dense) se registra dos veces con el mismo
# --gtest_filter, y la traza lo ve una sola vez (un archivo por nombre). Se corren TODOS los
# numeros que le corresponden. La afirmacion es "tests corridos == numeros elegidos", no
# "== nombres"; ambas cantidades se imprimen.
#
# INVARIANTE (D4, escrito para que alguien lo rompa a sabiendas): HOY las llamadas a
# `sleepFixed` son incondicionales en su camino. La traza solo ve un test que llego a
# `sleepFixed` a escala 1; si una llamada pasara a depender de un resultado previo, un
# test podria llegar a ella solo a escala 0 y la traza no lo veria. Por eso en
# `push: master` corre todo (`--full`): la traza es el atajo del PR, no la red.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${WMA_TEST_BUILD:-$ROOT/audio/src/main/cpp/tests/build}"
CORE="$BUILD/core_tests/core_tests"
PROBE_POS='TimeDependenceProbe.ABlindWaitIsDetectedByTheInstrument'
PROBE_NEG='TimeDependenceProbe.AConditionWaitSurvivesTheInstrument'

# El self-test y el control del probe corren SIN la traza: si escribieran en ella, el
# chequeo "el probe positivo esta en la traza" seria una tautologia.
unset WMA_TEST_WAIT_TRACE

only_self_test=0
full=0
for arg in "$@"; do
    case "$arg" in
        --self-test) only_self_test=1 ;;
        --full)      full=1 ;;
        *) echo "check-time-dependence: argumento desconocido: $arg" >&2; exit 2 ;;
    esac
done

# Igual que run-cpp-tests.sh.
JOBS="${CTEST_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
TRACE_DIR="$BUILD/wait-trace"

# Sello del arbol de trabajo: tree-id de HEAD + cambios + archivos nuevos no
# ignorados, via un indice temporal (no toca el indice real). Cambia si cambia
# cualquier fuente. IDENTICA a la de run-cpp-tests.sh; si divergen, el SHA no coincide
# y el detector corre todo (falla cerrado).
tree_stamp() {
    local d idx
    d="$(mktemp -d)" || return 0
    idx="$d/index"
    ( cd "$ROOT" && export GIT_INDEX_FILE="$idx" \
        && git read-tree HEAD >/dev/null 2>&1 \
        && git add -A . >/dev/null 2>&1 \
        && git write-tree 2>/dev/null ) || true
    rm -rf "$d"
}

if [ ! -x "$CORE" ]; then
    echo "check-time-dependence: no encuentro $CORE" >&2
    echo "  construi la suite primero:  bash scripts/run-cpp-tests.sh" >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# El extractor de bloques de fallo (MINI-022).
#
# `--output-on-failure` YA produce la salida de cada test fallado; el problema era
# que este script la mandaba a un temporal y sólo rescataba los NOMBRES. Con eso
# el rojo llegaba sin causa, y de paso anulaba el entregable de MINI-008: la guarda
# `kSnapDroppedFrames == 0` nombra sola por qué falló
# `ChangingTheTargetRestartsTheIntegration`, y ese mensaje se tiraba.
#
# Medido el 2026-09-10 (PR #275): ese rojo costó un worktree en master, dos
# corridas de suite y veinte del test aislado, para llegar a lo que la guarda decía
# en una línea.
#
# El formato que se recorta, verificado contra la salida real de ctest:
#
#     1/1 Test #1028: NOMBRE ...***Failed    0.04 sec      <- abre el bloque
#     <la salida del test>                                 <- esto es lo que interesa
#     0% tests passed, ...                                 <- lo cierra
#
# Se imprimen SÓLO los bloques de los fallados, nunca el log entero: un volcado de
# 1274 tests esconde la causa igual de bien que no imprimir nada.
bloques_de_fallo() {
    awk '
        /^ *[0-9]+\/[0-9]+ Test +#[0-9]+:/ {
            dentro = ($0 ~ /\*\*\*/) ? 1 : 0
            if (dentro) print
            next
        }
        /^ *Start +[0-9]+:/            { dentro = 0; next }
        /^[0-9]+% tests passed/        { dentro = 0 }
        /^The following tests FAILED:/ { dentro = 0 }
        /^Total Test time/             { dentro = 0 }
        dentro { print }
    ' "$1"
}

# ---------------------------------------------------------------------------
# 1. Self-test. Sin esto, nada de lo de abajo vale.
# ---------------------------------------------------------------------------
echo "== self-test del instrumento =="

if ! "$CORE" --gtest_filter="$PROBE_POS:$PROBE_NEG" >/dev/null 2>&1; then
    echo "  ROTO: con la escala normal los dos controles tienen que pasar, y no pasan." >&2
    echo "  El problema no es el instrumento: los controles mismos estan rotos." >&2
    exit 3
fi
echo "  escala normal: los dos controles pasan                    OK"

if WMA_TEST_WAIT_SCALE=0 "$CORE" --gtest_filter="$PROBE_POS" >/dev/null 2>&1; then
    echo "  ROTO: el control POSITIVO paso con la escala en 0." >&2
    echo "  El instrumento no esta colapsando las esperas, asi que un reporte" >&2
    echo "  vacio sobre la suite significaria 'no mire', no 'no hay defectos'." >&2
    exit 3
fi
echo "  escala 0: el control positivo FALLA, que es lo que debe    OK"

if ! WMA_TEST_WAIT_SCALE=0 "$CORE" --gtest_filter="$PROBE_NEG" >/dev/null 2>&1; then
    echo "  ROTO: el control NEGATIVO fallo con la escala en 0." >&2
    echo "  El instrumento esta achicando tambien los TECHOS de las esperas por" >&2
    echo "  condicion, asi que tiñe de rojo a los tests correctos y no discrimina." >&2
    exit 3
fi
echo "  escala 0: el control negativo sobrevive                    OK"

# ---------------------------------------------------------------------------
# Cuarto control (MINI-022): el EXTRACTOR, contra un fallo de verdad.
#
# Los tres de arriba verifican que el instrumento COLAPSA las esperas. Este
# verifica que además sabe CONTAR lo que encontró — que es otra cosa, y es la que
# se rompió en silencio: `--output-on-failure` producía la salida y el script la
# tiraba.
#
# 🔴 Por qué va acá y no en un test aparte: sin este control, un extractor que deja
# de encontrar los bloques (formato de ctest distinto, salida en otro orden)
# imprimiría vacío y NADIE se enteraría hasta el próximo rojo real — que es raro
# por definición, porque este script casi siempre pasa. El control positivo falla a
# PROPÓSITO con la escala en 0, así que sirve de fallo real sin depender de que
# haya un defecto en la suite.
probe_log="$(mktemp)"
WMA_TEST_WAIT_SCALE=0 ctest --test-dir "$BUILD" --output-on-failure --no-tests=error \
    -R "$PROBE_POS" >"$probe_log" 2>&1 || true
probe_bloque="$(bloques_de_fallo "$probe_log")"
rm -f "$probe_log"
if ! printf '%s' "$probe_bloque" | grep -q "$PROBE_POS"; then
    echo "  ROTO: el extractor no pudo sacar la salida de un fallo REAL." >&2
    echo "  El script seguiria reportando NOMBRES sin causa, que es el defecto que" >&2
    echo "  MINI-022 arreglo — y que ya habia anulado una vez el diagnostico de" >&2
    echo "  MINI-008. Revisa bloques_de_fallo() contra el formato de ctest." >&2
    exit 3
fi

# Y que sea ACOTADO, no el log entero.
#
# 🔴 Este chequeo lo trajo un MUTANTE QUE SOBREVIVIO: con `bloques_de_fallo`
# reemplazada por un `cat` del log, el control de arriba seguia en verde —el nombre
# aparece igual— y el script habria pasado a volcar 1274 tests por cada rojo. Un
# volcado esconde la causa igual de bien que no imprimir nada, que es justo lo que
# este MINI vino a arreglar.
#
# El discriminador es el RESUMEN de ctest: son las lineas que sobran, y un
# extractor correcto no las incluye nunca.
if printf '%s' "$probe_bloque" | grep -qE '^[0-9]+% tests passed|^Total Test time|^The following tests FAILED:'; then
    echo "  ROTO: el extractor esta trayendo el resumen de ctest, no solo el bloque." >&2
    echo "  Si arrastra eso, arrastra tambien la salida de los tests que PASARON: un" >&2
    echo "  volcado entero esconde la causa igual de bien que no imprimir nada." >&2
    exit 3
fi
echo "  el extractor saca la causa de un fallo real, y acotada     OK"

if [ $only_self_test -eq 1 ]; then
    echo "self-test OK."
    exit 0
fi

# ---------------------------------------------------------------------------
# 2. Que se re-corre: la traza si es valida, y si no la suite entera.
# ---------------------------------------------------------------------------
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
log="$work/ctest.log"
select_file="$work/select"
mode=trace; why=""
nsel=0; nnames=0; nmulti=0; sel_names=""

if [ $full -eq 1 ]; then
    mode=full; why="--full: suite entera (push a master, D4)"
elif [ ! -f "$TRACE_DIR/MANIFEST" ]; then
    mode=full; why="no hay $TRACE_DIR/MANIFEST (la suite no corrio entera con run-cpp-tests.sh)"
else
    stamp="$(tree_stamp)"
    man_tree="$(sed -n 's/^tree=//p' "$TRACE_DIR/MANIFEST")"
    man_traced="$(sed -n 's/^traced=//p' "$TRACE_DIR/MANIFEST")"
    have_traced="$(find "$TRACE_DIR" -maxdepth 1 -type f ! -name MANIFEST | wc -l | tr -d ' ')"
    if [ -z "$stamp" ]; then
        mode=full; why="no pude sellar el arbol con git"
    elif [ "$man_tree" != "$stamp" ]; then
        mode=full; why="el MANIFEST es de otro arbol ($man_tree) y no del actual ($stamp)"
    elif [ "$man_traced" != "$have_traced" ]; then
        mode=full; why="el MANIFEST dice $man_traced trazas y el directorio tiene $have_traced"
    elif [ ! -e "$TRACE_DIR/$PROBE_POS" ]; then
        # El MANIFEST es valido (mismo sello, misma cuenta) y aun asi falta el probe
        # positivo, que llama a sleepFixed a escala 1 en la corrida entera: algo dejo de
        # escribir la traza. Caer a la suite entera aca taparia la ceguera (AC-057.3).
        echo "  ROTO: el MANIFEST es valido pero el probe positivo ($PROBE_POS)" >&2
        echo "  no esta en la traza ($man_traced trazas): sleepFixed no la escribio, traza ciega." >&2
        echo "  Un verde de aca seria ceguera, no ausencia de defectos." >&2
        exit 3
    fi
fi

if [ "$mode" = trace ] && ! command -v python3 >/dev/null 2>&1; then
    mode=full; why="no hay python3 para leer el json de ctest"
fi

if [ "$mode" = trace ]; then
    ctest --test-dir "$BUILD" --show-only=json-v1 >"$work/tests.json" 2>"$work/json.err" \
        || { mode=full; why="ctest --show-only=json-v1 fallo: $(head -n 1 "$work/json.err")"; }
    ctest --test-dir "$BUILD" -N >"$work/tests.txt" 2>&1 || true
fi

# Mapea cada test de ctest por el `--gtest_filter=` de su comando: el nombre que
# registra ctest es otro (pretty, sin DISABLED_, con #GetParam() en los generadores
# propios). El numero de `-I` es la posicion en la lista: json-v1 no trae indice, asi
# que se contrasta contra `ctest -N`. Y NO se combina con -R/-E, que renumeran.
cat >"$work/map.py" <<'PY'
import json, os, re, sys
from urllib.parse import unquote

json_path, list_path, trace_dir, select_path, probe_prefix = sys.argv[1:6]

def bail(msg):
    print(msg)
    sys.exit(10)

tests = json.load(open(json_path, encoding="utf-8"))["tests"]
listed = {}
for line in open(list_path, encoding="utf-8", errors="replace"):
    m = re.match(r"\s*Test\s+#(\d+):\s(.*?)\s*$", line)
    if m:
        listed[int(m.group(1))] = m.group(2)
if len(listed) != len(tests) or any(listed.get(i) != t["name"] for i, t in enumerate(tests, 1)):
    bail("la numeracion de `ctest -N` no coincide con la posicion en json-v1")

by_filter = {}
for i, t in enumerate(tests, 1):
    for a in t.get("command", []):
        if a.startswith("--gtest_filter="):
            by_filter.setdefault(a[len("--gtest_filter="):], []).append(i)

names = sorted(unquote(f) for f in os.listdir(trace_dir) if f != "MANIFEST")
names = [n for n in names if not n.startswith(probe_prefix)]
absent = [n for n in names if n not in by_filter]
if absent:
    bail("la traza nombra tests que ctest no registra: " + ", ".join(absent[:5]))
if not names:
    bail("la traza no tiene ningun test aparte del probe")
# Un nombre mapea a UNO O MAS numeros (el mismo test en varios binarios): se eligen todos.
nums = sorted({i for n in names for i in by_filter[n]})
multi = [n for n in names if len(by_filter[n]) > 1]

with open(select_path, "w") as f:
    f.write("0,0,0," + ",".join(str(n) for n in nums) + "\n")
print(len(nums))
print(len(names))
print(len(multi))
for n in names:
    ids = by_filter[n]
    print(n if len(ids) == 1 else "%s  (%d tests de ctest: %s)" % (
        n, len(ids), ", ".join("#%d" % i for i in ids)))
PY

if [ "$mode" = trace ]; then
    py_rc=0
    sel_out="$(python3 -I "$work/map.py" "$work/tests.json" "$work/tests.txt" "$TRACE_DIR" \
        "$select_file" "${PROBE_POS%%.*}." 2>&1)" || py_rc=$?
    if [ $py_rc -eq 0 ]; then
        nsel="$(printf '%s\n' "$sel_out" | sed -n 1p)"
        nnames="$(printf '%s\n' "$sel_out" | sed -n 2p)"
        nmulti="$(printf '%s\n' "$sel_out" | sed -n 3p)"
        sel_names="$(printf '%s\n' "$sel_out" | sed 1,3d)"
    else
        mode=full; why="$(printf '%s\n' "$sel_out" | tail -n 1)"
        [ $py_rc -eq 10 ] || why="el mapeo de la traza fallo (python3 rc=$py_rc): $why"
    fi
fi

echo
if [ "$mode" = trace ]; then
    echo "== suite con las esperas ciegas colapsadas: $nnames nombres en la traza -> $nsel tests de ctest =="
    echo "  (un nombre puede ser varios tests de ctest: el mismo test en mas de un binario."
    echo "   Se corren todos; $nmulti nombre(s) mapean a mas de uno. Se elige por numero, con -I.)"
    printf '%s\n' "$sel_names" | sed 's/^/   /'
else
    echo "== suite ENTERA con las esperas ciegas colapsadas =="
    echo "  motivo: $why"
fi
set +e
if [ "$mode" = trace ]; then
    WMA_TEST_WAIT_SCALE=0 ctest --test-dir "$BUILD" \
        --output-on-failure --no-tests=error -I "$select_file" -j "$JOBS" >"$log" 2>&1
else
    WMA_TEST_WAIT_SCALE=0 ctest --test-dir "$BUILD" \
        --output-on-failure --no-tests=error \
        -E "TimeDependenceProbe" -j "$JOBS" >"$log" 2>&1
fi
rc=$?
set -e

# El resumen de ctest ("N% tests passed, F tests failed out of M") es lo unico que dice
# cuantos tests corrieron. Sin el, un log truncado (OOM, rc 137) no distingue "todo verde"
# de "no se que paso": en cualquier modo es exit 3.
ran="$(sed -n -E 's/^[0-9]+% tests passed, [0-9]+ tests? failed out of ([0-9]+).*/\1/p' "$log" | tail -n 1)"
if [ -z "$ran" ]; then
    echo "  ROTO: ctest termino (rc=$rc) sin imprimir su resumen 'tests passed ... out of N'." >&2
    echo "  El log esta truncado o ctest murio (OOM = rc 137): no se que corrio. Ultimas lineas:" >&2
    tail -n 15 "$log" | sed 's/^/   /' >&2
    exit 3
fi
if [ "$mode" = trace ]; then
    if [ "$ran" != "$nsel" ]; then
        echo "  ROTO: se eligieron $nsel tests de ctest ($nnames nombres de la traza) y ctest corrio $ran." >&2
        echo "  La seleccion por numero no hizo lo que dice: un verde seria ceguera." >&2
        exit 3
    fi
    echo "  corridos: $ran de $nsel elegidos ($nnames nombres en la traza)"
else
    echo "  corridos: $ran"
fi

failed="$(sed -n '/The following tests FAILED:/,$p' "$log" | grep -E '^\s+[0-9]+ - ' || true)"
if [ "$rc" -ne 0 ] && [ -z "$failed" ]; then
    echo "  ROTO: ctest salio con rc=$rc pero no pude extraer ningun test fallado del log." >&2
    echo "  No se puede declarar verde algo que ctest dio por rojo. Ultimas lineas:" >&2
    tail -n 15 "$log" | sed 's/^/   /' >&2
    exit 3
fi

# ---------------------------------------------------------------------------
# El falso verde que este bloque existe para impedir.
#
# El instrumento solo ve lo que pasa por `wma_test::sleepFixed`. Un
# `std::this_thread::sleep_for` crudo le es INVISIBLE, asi que "ningun test
# depende del reloj" y "ningun test migrado depende del reloj" se imprimen
# igual — y son cosas muy distintas mientras quede algo sin migrar.
# ---------------------------------------------------------------------------
raw_files="$(grep -rl 'this_thread::sleep_for' "$ROOT/audio/src/main/cpp" \
        --include='*.cpp' --include='*.h' 2>/dev/null \
        | grep -v '/build' | grep -E '/tests?/' \
        | grep -v 'support/TestWait.h' || true)"
raw="$(printf '%s' "$raw_files" | grep -c . || true)"
raw_n=0
if [ -n "$raw_files" ]; then
    raw_n="$(printf '%s\n' "$raw_files" | tr '\n' '\0' \
             | xargs -0 grep -h 'this_thread::sleep_for' | grep -c . || true)"
fi

if [ "${raw:-0}" -gt 0 ]; then
    echo
    echo "  AVISO — cobertura parcial: quedan $raw_n esperas crudas en $raw archivos de test."
    echo "  Este instrumento no las ve: solo alcanza lo que pasa por wma_test::sleepFixed."
    echo "  Hasta que esten migradas, un verde de aca significa 'nada MIGRADO depende del"
    echo "  reloj', no 'nada depende del reloj'. Que el camino crudo deje de existir es S4."
fi

if [ -z "$failed" ]; then
    echo "  ningun test MIGRADO depende de una espera ciega."
    exit 0
fi

echo "  estos tests cambian de veredicto cuando se les saca el tiempo:"
echo "$failed" | sed 's/^/   /'

# La CAUSA, no sólo el nombre (MINI-022). Sin esto el rojo obliga a reproducir a
# mano lo que la salida del test ya decía — y en el caso testigo, lo que una guarda
# de MINI-008 decía en una línea.
bloques="$(bloques_de_fallo "$log")"
echo
if [ -n "$bloques" ]; then
    echo "  --- lo que dijeron, que es la causa ---"
    printf '%s\n' "$bloques" | sed 's/^/   /'
    echo "  --- fin ---"
else
    # Un extractor mudo que falla es indistinguible de un test sin salida, y esa
    # ambiguedad es justo la que este bloque viene a borrar: se DICE.
    echo "  🔴 NO PUDE EXTRAER la salida de esos tests del log de ctest."
    echo "  El formato de ctest debe haber cambiado: revisá bloques_de_fallo()."
    echo "  Esto NO es 'el test no dijo nada' — es 'el instrumento no supo leerlo'."
fi
echo
echo "  Cada uno sincroniza con una duracion y afirma despues. Eso da verde en"
echo "  una maquina ociosa y rojo en un runner cargado, que es el defecto que"
echo "  REQ-002 persigue. La salida NO es agrandar la espera: es esperar por la"
echo "  condicion con techo (wma_test::waitUntil)."
exit "${rc:-1}"
