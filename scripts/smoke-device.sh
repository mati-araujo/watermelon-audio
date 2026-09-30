#!/usr/bin/env bash
#
# MINI-038 — smoke del harness EN UN DEVICE, por adb, con veredicto por punto.
#
# 🔴 NO ENTRA AL CI, y no es un olvido: necesita un device fisico (y para USB, una interfaz
# enchufada y un humano que acepte el dialogo de permiso). Lo corre una persona, o un agente con
# un device asignado. Lo que SI entra al gate es `--self-test`, que no necesita device.
#
# 🔴 Para el panel USB, adb tiene que ir por WI-FI (`adb pair`/`adb connect`, ANDROID_SERIAL del
# estilo `adb-XXXX._adb-tls-connect._tcp`): un telefono con un solo USB-C y adb por cable esta en
# modo device y no puede ser host de la interfaz de audio.
#
# Que hace:
#   1. exige ANDROID_SERIAL y lo usa en CADA llamada a adb: nunca toca otro device;
#   2. construye e instala SOLO com.watermellonstudios.audio.harness (`install -r -g`: -g concede
#      los permisos de runtime del manifest, o sea RECORD_AUDIO; el permiso USB NO es de runtime y
#      no se concede asi — es el dialogo del sistema, un gesto humano);
#   3. dispara el plan con un extra de intent (MainActivity, build debug):
#        am start ... --es harness.smoke <plan> --es harness.smoke.run <id>
#   4. lee las lineas `HARNESS-SMOKE` de ESA corrida (filtra por run=<id>; el buffer de logcat no
#      se borra, es del device) hasta `panel=plan step=fin`, o hasta el techo;
#   5. da un veredicto por punto: PASS / FAIL / HUMANO. Un paso con ok=false es FAIL; un paso que
#      falta es FAIL; lo que quedo detras de `step=esperando-humano` sin que el humano lo
#      resolviera es HUMANO, nunca PASS;
#   6. lista aparte lo que requiere OIDO (ningun log dice si algo suena bien).
#
# Exit: 0 todo PASS · 1 algun FAIL · 3 sin FAIL pero con puntos HUMANO pendientes · 2 uso/infra.
# Un paso NO-MEDIDO (linea con medido=false, D11) no mueve el exit: se lista aparte y no cuenta
# como cobertura.
#
# El formato de las lineas vive en un solo lugar:
#   harness/src/commonMain/kotlin/com/watermellonstudios/audio/harness/smoke/HarnessSmoke.kt
# y lo fija HarnessSmokeTest. Los pasos que cada panel emite estan en SmokePlanRunner (salida,
# captura, sf2, sf3) y en UsbHarness (usb); EXPECTED, abajo, es la otra punta de ese contrato.
#
# Uso:
#   ANDROID_SERIAL=<serial> bash scripts/smoke-device.sh [--plan todo|salida,sf2,...]
#        [--usb-espera-s 120] [--techo-s N] [--out DIR] [--no-build]
#   bash scripts/smoke-device.sh --self-test
#   bash scripts/smoke-device.sh --veredicto LOG RUN PLAN   # juzga un log ya grabado

set -euo pipefail
cd "$(dirname "$0")/.."

readonly PKG="com.watermellonstudios.audio.harness"
readonly ACTIVITY="$PKG/.MainActivity"
readonly APK="harness/build/outputs/apk/debug/harness-debug.apk"
readonly SELFTEST_LOG="scripts/smoke-device-selftest.txt"

# ---------------------------------------------------------------------------
# El juez. Puro: un log y un run id adentro, veredictos y exit code afuera. Es lo que prueba
# --self-test, asi que no puede depender de adb.
# ---------------------------------------------------------------------------
verdict() {
    local log="$1" run="$2" requested="$3"
    python3 - "$log" "$run" "$requested" <<'PY'
import sys

import re

log, run, requested = sys.argv[1], sys.argv[2], sys.argv[3]

# La otra punta del contrato (ver la cabecera). El orden es el de emision.
EXPECTED = {
    "salida": ["start", "stream", "frames"],
    "captura": ["start", "nivel", "stop"],
    "sf2": ["fixture", "carga", "preset", "nota", "descarga", "no-soundfont"],
    "sf3": ["fixture", "carga", "preset", "nota", "descarga"],
    "usb": ["motor-parado", "dispositivos", "motor-callback", "permiso", "conectar", "capacidades", "descriptores", "backend",
            "streaming-start", "streaming-stats", "suite", "streaming-stop",
            "backend-restaurado", "desconectar"],
}
ORDER = ["salida", "captura", "sf2", "sf3", "usb"]
# El plan PEDIDO, normalizado como lo normaliza SmokePlan (orden canonico, sin repetidos). El juez
# lo compara contra el que la app dice haber corrido: si la app corre menos de lo pedido (una
# regresion en SmokePlan o en MainActivity), eso es FAIL, no "todo lo que corrio paso".
if requested == "todo":
    wanted = list(ORDER)
else:
    ids = [x.strip() for x in requested.split(",")]
    wanted = [p for p in ORDER if p in ids]
    if any(x not in ORDER for x in ids):
        wanted = None
# Pasos del panel USB que dependen de que el humano haya dado el permiso.
AFTER_PERMISSION = EXPECTED["usb"][EXPECTED["usb"].index("permiso"):]
HUMAN = "esperando-humano"

lines = []
with open(log, encoding="utf-8", errors="replace") as f:
    for raw in f:
        i = raw.find("HARNESS-SMOKE ")
        if i < 0:
            continue
        parts = raw[i:].strip().split(" ")
        fields = {}
        for p in parts[1:]:
            if "=" in p:
                k, v = p.split("=", 1)
                fields[k] = v
        if fields.get("run") != run:
            continue
        if fields.get("v") != "1":
            print("FAIL  formato  version desconocida: %s" % raw.strip())
            sys.exit(1)
        lines.append(fields)

if not lines:
    print("FAIL  plan  ninguna linea HARNESS-SMOKE con run=%s" % run)
    sys.exit(1)

rows = []   # (veredicto, panel, paso, detalle)
def add(v, panel, step, detail=""):
    rows.append((v, panel, step, detail))

def extras(f):
    return " ".join("%s=%s" % (k, v) for k, v in f.items() if k not in ("v", "run", "panel", "step", "ok"))

inicio = [f for f in lines if f.get("panel") == "plan" and f.get("step") == "inicio"]
fin = [f for f in lines if f.get("panel") == "plan" and f.get("step") == "fin"]
if not inicio:
    add("FAIL", "plan", "inicio", "falta")
    panels = []
else:
    add("PASS" if inicio[0].get("ok") == "true" else "FAIL", "plan", "inicio", extras(inicio[0]))
    panels = [p for p in inicio[0].get("plan", "").split(",") if p in EXPECTED]
    if wanted is None or panels != wanted:
        add("FAIL", "plan", "pedido", "se pidio '%s' y la app corrio '%s'" % (requested, ",".join(panels) or "-"))

human_pending = set()
for panel in [p for p in ORDER if p in panels]:
    mine = [f for f in lines if f.get("panel") == panel]
    waits = [f for f in mine if f.get("step") == HUMAN]
    granted = any(f.get("step") == "permiso" and f.get("ok") == "true" for f in mine)
    denied = any(f.get("step") == "permiso" and f.get("ok") != "true" for f in mine)
    for w in waits:
        state = "hecho — " if granted else ("DENEGADO por el humano — " if denied else "PENDIENTE — ")
        add("HUMANO", panel, HUMAN, state + extras(w))
    pending = bool(waits) and not granted
    if pending:
        human_pending.add(panel)

    seen = set()
    for f in mine:
        step = f.get("step")
        if step == HUMAN:
            continue
        seen.add(step)
        if pending and step in AFTER_PERMISSION:
            add("HUMANO", panel, step, "sin permiso: " + extras(f))
        elif f.get("medido") == "false" and panel == "usb" and re.fullmatch(r"suite-[0-9]+", step or ""):
            # D11: una fila de la suite cuyo rate el runner no aplica. Ni PASS (aunque diga ok=true)
            # ni FAIL, y no cuenta como cobertura: se lista aparte. SOLO filas de la suite: cualquier
            # otro paso con medido=false es un FAIL, o un paso podria desaparecer del veredicto.
            add("NO-MEDIDO", panel, step, extras(f))
        elif f.get("medido") == "false":
            add("FAIL", panel, step, "medido=false fuera de la suite (D11 no lo cubre): " + extras(f))
        elif f.get("ok") == "true":
            add("PASS", panel, step, extras(f))
        else:
            add("FAIL", panel, step, extras(f))
    for step in EXPECTED[panel]:
        if step not in seen:
            if pending and step in AFTER_PERMISSION:
                add("HUMANO", panel, step, "no corrio: espera el permiso USB")
            else:
                add("FAIL", panel, step, "FALTA: el panel no emitio este paso")

if not fin:
    add("FAIL", "plan", "fin", "FALTA: la corrida no termino (o no llego al techo de espera)")
else:
    ok = fin[0].get("ok") == "true"
    # `fin` agrega los paneles: si el unico motivo de su ok=false es un panel que espera al humano,
    # el veredicto es de ese panel (HUMANO), no un FAIL mas.
    failed = [p for p in fin[0].get("fallidos", "-").split(",") if p and p != "-"]
    if ok:
        add("PASS", "plan", "fin", extras(fin[0]))
    elif failed and set(failed) <= human_pending and fin[0].get("motor-detenido") == "true":
        add("HUMANO", "plan", "fin", extras(fin[0]))
    else:
        add("FAIL", "plan", "fin", extras(fin[0]))

width = max(len("%s/%s" % (p, s)) for _, p, s, _ in rows)
for v, p, s, d in rows:
    if v != "NO-MEDIDO":
        print("%-6s  %-*s  %s" % (v, width, "%s/%s" % (p, s), d))
unmeasured = [r for r in rows if r[0] == "NO-MEDIDO"]
if unmeasured:
    print("\nNO-MEDIDO (no es PASS ni FAIL y NO cuenta como cobertura):")
    for v, p, s, d in unmeasured:
        print("%-9s  %-*s  %s" % (v, width, "%s/%s" % (p, s), d))

n = {k: sum(1 for r in rows if r[0] == k) for k in ("PASS", "FAIL", "HUMANO", "NO-MEDIDO")}
print("\nresumen: %d PASS · %d FAIL · %d HUMANO · %d NO-MEDIDO"
      % (n["PASS"], n["FAIL"], n["HUMANO"], n["NO-MEDIDO"]))
pending = [r for r in rows if r[0] == "HUMANO" and (r[1] in human_pending or r[1] == "plan")]
sys.exit(1 if n["FAIL"] else (3 if pending else 0))
PY
}

print_ear_checks() {
    cat <<'EOF'

Requiere OIDO (ningun log lo decide; hacerlo con la app abierta):
  - sf2/sf3: el boton "tocar A4" del panel SoundFont suena como una senoide limpia de 440 Hz,
    sin clicks en el loop y sin distorsion (el .sf3 pasa por stb_vorbis).
  - usb: con auriculares en la interfaz USB, "start stream" saca el tono de prueba de 440 Hz por
    ESA salida (no por el parlante del telefono), y la suite no se corta.
  - captura: hablandole al microfono, la barra del panel Entrada se mueve (el plan solo afirma que
    hay medicion, no que haya voz).
Requiere MANO (fuera del plan automatico):
  - el dialogo de permiso USB (el plan lo deja en esperando-humano);
  - "elegir archivo (fd)" en el panel SoundFont: el selector del sistema, que carga por
    loadSoundFontFromFd.
EOF
}

# ---------------------------------------------------------------------------
# --self-test: el juez tiene que poder decir que NO. Sobre un log grabado de una corrida real en
# device (scripts/smoke-device-selftest.txt), y sobre mutantes de ese log.
# ---------------------------------------------------------------------------
self_test() {
    local tmp
    tmp="$(mktemp -d)"
    [[ -f "$SELFTEST_LOG" ]] || { echo "self-test: FAIL — falta $SELFTEST_LOG" >&2; return 1; }
    local run
    run="$(sed -nE 's/.*HARNESS-SMOKE v=1 run=([^ ]+) panel=plan step=inicio .*/\1/p' "$SELFTEST_LOG" | head -1)"
    [[ -n "$run" ]] || { echo "self-test: FAIL — el log grabado no tiene 'plan inicio'" >&2; return 1; }

    local failures=0
    expect() {  # expect <nombre> <exit esperado> <archivo> [plan pedido]
        local name="$1" want="$2" file="$3" plan="${4:-todo}" got=0
        verdict "$file" "$run" "$plan" > "$tmp/out" 2>&1 || got=$?
        if [[ "$got" == "$want" ]]; then
            printf '  ok    %-44s exit %s\n' "$name" "$got"
        else
            printf '  MAL   %-44s exit %s, esperaba %s\n' "$name" "$got" "$want"
            sed 's/^/        /' "$tmp/out" | tail -8
            failures=$((failures + 1))
        fi
    }

    expect_line() {  # expect_line <nombre> <regex que TIENE que aparecer> <archivo> [plan]
        local name="$1" re="$2" file="$3" plan="${4:-todo}"
        verdict "$file" "$run" "$plan" > "$tmp/out" 2>&1 || true
        if grep -Eq "$re" "$tmp/out"; then
            printf '  ok    %-44s\n' "$name"
        else
            printf '  MAL   %-44s falta /%s/\n' "$name" "$re"
            failures=$((failures + 1))
        fi
    }
    expect_no_line() {  # expect_no_line <nombre> <regex que NO puede aparecer> <archivo> [plan]
        local name="$1" re="$2" file="$3" plan="${4:-todo}"
        verdict "$file" "$run" "$plan" > "$tmp/out" 2>&1 || true
        if grep -Eq "$re" "$tmp/out"; then
            printf '  MAL   %-44s aparece /%s/\n' "$name" "$re"
            failures=$((failures + 1))
        else
            printf '  ok    %-44s\n' "$name"
        fi
    }

    # El control: el log grabado tal cual. Su exit es el que se grabo (ver la cabecera del log).
    local want_base
    want_base="$(sed -nE 's/^# exit-esperado: ([0-9]+).*/\1/p' "$SELFTEST_LOG" | head -1)"
    [[ -n "$want_base" ]] || { echo "self-test: FAIL — el log grabado no declara '# exit-esperado:'" >&2; return 1; }
    expect "control: el log grabado" "$want_base" "$SELFTEST_LOG"

    # Una version del control donde todo lo automatico pasa y el humano ya actuo: exit 0. Se arma
    # quitando del log los pasos USB (queda pendiente) — ver abajo — asi que primero el caso verde
    # sin USB: se recorta el plan a los paneles automaticos.
    sed -E "/panel=usb /d; s/(panel=plan step=inicio ok=true plan=)[^ ]*/\1salida,captura,sf2,sf3/; \
            s/(panel=plan step=fin )ok=[a-z]+ fallidos=[^ ]*/\1ok=true fallidos=-/" \
        "$SELFTEST_LOG" > "$tmp/verde.log"
    expect "verde: sin usb, todo lo automatico" 0 "$tmp/verde.log" salida,captura,sf2,sf3

    # M1: UN paso con ok=false da rojo.
    sed -E 's/panel=sf3 step=nota ok=true/panel=sf3 step=nota ok=false/' "$tmp/verde.log" > "$tmp/m1.log"
    expect "M1: sf3/nota con ok=false" 1 "$tmp/m1.log" salida,captura,sf2,sf3

    # M2: UN paso faltante da rojo.
    grep -v 'panel=salida step=frames ' "$tmp/verde.log" > "$tmp/m2.log"
    expect "M2: falta salida/frames" 1 "$tmp/m2.log" salida,captura,sf2,sf3

    # M3: sin `fin` la corrida no termino: rojo, aunque todo lo demas pase.
    grep -v 'panel=plan step=fin ' "$tmp/verde.log" > "$tmp/m3.log"
    expect "M3: falta plan/fin" 1 "$tmp/m3.log" salida,captura,sf2,sf3

    # M4: el humano no contesto — el USB queda HUMANO, no PASS ni FAIL: exit 3.
    {
        cat "$tmp/verde.log" | grep -v 'panel=plan step=fin '
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=motor-parado ok=true"
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=dispositivos ok=true cantidad=1"
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=motor-callback ok=true inicializado=true"
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=esperando-humano ok=false accion=aceptar_el_dialogo"
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=conectar ok=false motivo=sin-respuesta-humana"
        echo "HARNESS-SMOKE v=1 run=$run panel=plan step=fin ok=false fallidos=usb motor-detenido=true"
    } | sed -E "s/(panel=plan step=inicio ok=true plan=)[^ ]*/\1salida,captura,sf2,sf3,usb/" > "$tmp/m4.log"
    expect "M4: usb esperando al humano" 3 "$tmp/m4.log"

    # M5: lineas de OTRA corrida no cuentan: con otro run id no hay nada que juzgar.
    sed -E "s/run=$run /run=otra-corrida /" "$tmp/verde.log" > "$tmp/m5.log"
    expect "M5: el run id es de otra corrida" 1 "$tmp/m5.log" salida,captura,sf2,sf3

    # M6: un paso con ok=false que NO esta en la lista esperada (p.ej. una excepcion) tambien es rojo.
    awk -v run="$run" '/panel=plan step=fin /{print "HARNESS-SMOKE v=1 run=" run " panel=sf2 step=excepcion ok=false error=boom"} {print}' \
        "$tmp/verde.log" > "$tmp/m6.log"
    expect "M6: un paso inesperado con ok=false" 1 "$tmp/m6.log" salida,captura,sf2,sf3

    # Un USB completo y sano, detras del permiso que el humano SI dio.
    usb_ok() {
        local s
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=motor-parado ok=true"
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=dispositivos ok=true cantidad=1"
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=motor-callback ok=true inicializado=true"
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=esperando-humano ok=false accion=aceptar_el_dialogo"
        for s in permiso conectar capacidades descriptores backend streaming-start streaming-stats \
                 suite-1 suite-2 suite-3 suite streaming-stop backend-restaurado desconectar; do
            echo "HARNESS-SMOKE v=1 run=$run panel=usb step=$s ok=true"
        done
    }
    with_usb() {  # with_usb <archivo destino> <sed sobre las lineas usb>
        { grep -v 'panel=plan step=fin ' "$tmp/verde.log"
          usb_ok | sed -E "$2"
          echo "HARNESS-SMOKE v=1 run=$run panel=plan step=fin ok=true fallidos=- motor-detenido=true"
        } | sed -E "s/(panel=plan step=inicio ok=true plan=)[^ ]*/\1salida,captura,sf2,sf3,usb/" > "$1"
    }

    # M7: el humano dio el permiso y el USB paso entero: exit 0 (el HUMANO "hecho" no retiene).
    with_usb "$tmp/m7.log" 's/^//'
    expect "M7: humano hecho y usb sano" 0 "$tmp/m7.log"

    # M8: el humano dio el permiso y libusb FALLO: es FAIL, no HUMANO. Un fallo real de libusb con
    # el permiso dado no se puede esconder detras del gesto humano.
    with_usb "$tmp/m8.log" 's/step=conectar ok=true/step=conectar ok=false error=INITIALIZATION_FAILED/'
    expect "M8: permiso dado y conectar falla" 1 "$tmp/m8.log"

    # M9: se pidio `todo` y la app corrio menos (una regresion en el plan): FAIL aunque lo que
    # corrio haya pasado.
    expect "M9: la app corrio menos que el plan pedido" 1 "$tmp/verde.log" todo

    # D11 — M10: las filas cuyo rate el runner no aplica son NO-MEDIDO: no mueven el exit (el verde
    # lo deciden las demas) y NUNCA salen PASS, ni siquiera con ok=true.
    with_usb "$tmp/m10.log" 's/step=suite-2 ok=true/step=suite-2 ok=false medido=false motivo=rate-no-aplicado/; s/step=suite-3 ok=true/step=suite-3 ok=true medido=false motivo=rate-no-aplicado/'
    expect "M10: filas no medidas, el resto sano" 0 "$tmp/m10.log"
    expect_line "M10: suite-2 sale NO-MEDIDO" '^NO-MEDIDO +usb/suite-2 ' "$tmp/m10.log"
    expect_line "M10: suite-3 con ok=true sale NO-MEDIDO" '^NO-MEDIDO +usb/suite-3 ' "$tmp/m10.log"
    expect_no_line "M10: ninguna fila no medida sale PASS" '^PASS +usb/suite-[23] ' "$tmp/m10.log"

    # D11 — M11: una fila de 48 k (medida) con ok=false sigue siendo FAIL.
    with_usb "$tmp/m11.log" 's/step=suite-1 ok=true/step=suite-1 ok=false rate-config=48000 motivo=sin-trafico/'
    expect "M11: fila de 48 k con ok=false" 1 "$tmp/m11.log"
    expect_line "M11: suite-1 sale FAIL" '^FAIL +usb/suite-1 ' "$tmp/m11.log"

    # M13 (g42, 30/09): streaming-start FALLA. Rojo, y por los pasos correctos, en las dos formas:
    # (a) la vieja, sin streaming-stats (FALTA), y (b) la actual, con streaming-stats ok=false
    # motivo=sin-streaming. Ningun otro paso USB puede salir FAIL por eso.
    with_usb "$tmp/m13a.log" '/step=streaming-stats /d; s/step=streaming-start ok=true/step=streaming-start ok=false motivo=motor-sin-callback/'
    expect "M13a: streaming-start FAIL sin streaming-stats" 1 "$tmp/m13a.log"
    expect_line "M13a: streaming-start sale FAIL" '^FAIL +usb/streaming-start +motivo=motor-sin-callback' "$tmp/m13a.log"
    expect_line "M13a: streaming-stats sale FALTA" '^FAIL +usb/streaming-stats +FALTA' "$tmp/m13a.log"
    with_usb "$tmp/m13b.log" 's/step=streaming-start ok=true/step=streaming-start ok=false motivo=motor-sin-callback/; s/step=streaming-stats ok=true/step=streaming-stats ok=false motivo=sin-streaming/'
    expect "M13b: streaming-start y streaming-stats FAIL" 1 "$tmp/m13b.log"
    expect_line "M13b: streaming-stats sale FAIL sin-streaming" '^FAIL +usb/streaming-stats +motivo=sin-streaming' "$tmp/m13b.log"
    for m in m13a m13b; do
        expect_no_line "$m: ningun otro paso usb sale FAIL" '^FAIL +usb/(dispositivos|motor-callback|permiso|conectar|capacidades|descriptores|backend|suite|streaming-stop|desconectar) ' "$tmp/$m.log"
    done

    # M14: si falta motor-callback (el motor antes del device), es FAIL por ESE paso. Mata el
    # mutante que lo saca de EXPECTED.
    with_usb "$tmp/m14.log" '/step=motor-callback /d'
    expect "M14: falta usb/motor-callback" 1 "$tmp/m14.log"
    expect_line "M14: motor-callback sale FALTA" '^FAIL +usb/motor-callback +FALTA' "$tmp/m14.log"

    # M12: `medido=false` fuera de una fila de la suite no puede hacer desaparecer un paso: FAIL.
    with_usb "$tmp/m12.log" 's/step=streaming-stats ok=true/step=streaming-stats ok=false medido=false motivo=x/'
    expect "M12: medido=false fuera de la suite" 1 "$tmp/m12.log"

    rm -rf "$tmp"
    if (( failures )); then
        echo "self-test: FAIL — $failures caso(s) con el veredicto equivocado" >&2
        return 1
    fi
    echo "self-test: OK — el juez distingue verde, ok=false, faltante, sin fin, humano pendiente, humano hecho, fallo con permiso, plan recortado, no medido y otra corrida"
}

# ---------------------------------------------------------------------------
# La corrida en device.
# ---------------------------------------------------------------------------
run_device() {
    local plan="todo" usb_wait=120 ceiling="" out="" build=1
    while (( $# )); do
        case "$1" in
            --plan) plan="$2"; shift 2 ;;
            --usb-espera-s) usb_wait="$2"; shift 2 ;;
            --techo-s) ceiling="$2"; shift 2 ;;
            --out) out="$2"; shift 2 ;;
            --no-build) build=0; shift ;;
            *) echo "opcion desconocida: $1" >&2; exit 2 ;;
        esac
    done
    [[ "$usb_wait" =~ ^[1-9][0-9]*$ ]] || { echo "FAIL — --usb-espera-s tiene que ser un entero > 0: '$usb_wait'" >&2; exit 2; }
    [[ -z "$ceiling" || "$ceiling" =~ ^[1-9][0-9]*$ ]] || { echo "FAIL — --techo-s tiene que ser un entero > 0: '$ceiling'" >&2; exit 2; }
    # El techo cubre la espera humana, la suite USB (3 tests de 5 s) y el resto con holgura.
    ceiling="${ceiling:-$((usb_wait + 120))}"

    if [[ -z "${ANDROID_SERIAL:-}" ]]; then
        echo "FAIL — falta ANDROID_SERIAL. Este script no elige device: nunca toca uno que no le nombraron." >&2
        exit 2
    fi
    local serial="$ANDROID_SERIAL"
    # adb: el del PATH, o el del SDK que declara local.properties / ANDROID_HOME.
    local adb_bin
    adb_bin="$(command -v adb || true)"
    if [[ -z "$adb_bin" ]]; then
        local sdk="${ANDROID_HOME:-$(sed -nE 's/^sdk\.dir=(.*)/\1/p' local.properties 2>/dev/null | head -1)}"
        adb_bin="${sdk:+$sdk/platform-tools/adb}"
    fi
    [[ -x "${adb_bin:-}" ]] || { echo "FAIL — no encuentro adb (PATH, ANDROID_HOME o sdk.dir)." >&2; exit 2; }
    adb_() { "$adb_bin" -s "$serial" "$@"; }

    local state
    state="$(adb_ get-state 2>/dev/null || true)"
    if [[ "$state" != "device" ]]; then
        echo "FAIL — '$serial' no esta disponible (get-state: '${state:-nada}')." >&2
        exit 2
    fi

    local run="smoke-$(date +%Y%m%d-%H%M%S)-$$"
    out="${out:-harness/build/smoke-device/$run}"
    mkdir -p "$out"

    if (( build )); then
        echo "=== build: :harness:assembleDebug ==="
        # Sin -q: el WARNING de D10 (sin encoder Vorbis, el .sf3 no se empaqueta) es un logger.warn
        # que -q esconde. Se muestra aca, antes de que el veredicto diga FAIL en sf3.
        ./gradlew :harness:assembleDebug --console=plain > "$out/build.log" 2>&1 \
            || { tail -20 "$out/build.log" >&2; echo "FAIL — no construye el harness" >&2; exit 2; }
        grep -A3 'WARNING: fixture' "$out/build.log" || true
    fi
    [[ -f "$APK" ]] || { echo "FAIL — no hay APK en $APK" >&2; exit 2; }

    echo "=== device: $serial — $(adb_ shell getprop ro.product.model | tr -d '\r') ==="
    echo "=== USB (dumpsys usb, lo que ve el sistema) ==="
    adb_ shell dumpsys usb 2>/dev/null | grep -iE 'idVendor|idProduct|mProductName|manufacturer' | head -8 \
        | tee "$out/dumpsys-usb.txt" || true
    adb_ shell ls /dev/snd 2>/dev/null | tr -d '\r' | tr '\n' ' ' | tee "$out/dev-snd.txt" || true
    echo

    echo "=== install -r -g (sólo $PKG) ==="
    local installed
    installed="$(adb_ install -r -g "$APK" 2>&1 || true)"
    if ! grep -q '^Success' <<< "$installed"; then
        echo "FAIL — no se pudo instalar el harness:" >&2
        echo "$installed" | tail -3 >&2
        exit 2
    fi
    echo "Success"

    echo "=== plan '$plan', run=$run ==="
    # Las lineas se capturan en STREAMING desde antes del am start, y no releyendo el buffer: durante
    # la espera humana y la suite USB el buffer circular de logcat rota, y releerlo perdia `inicio`.
    local raw="$out/logcat-harness-smoke-raw.txt"
    : > "$raw"
    local logcat_pid=""
    start_capture() {
        adb_ logcat -v raw -s HARNESS-SMOKE:I >> "$raw" 2>/dev/null &
        logcat_pid=$!
    }
    start_capture
    trap 'kill "$logcat_pid" 2>/dev/null || true' EXIT
    adb_ shell am force-stop "$PKG"
    adb_ shell am start -n "$ACTIVITY" \
        --es harness.smoke "$plan" --es harness.smoke.run "$run" \
        --es harness.smoke.usb-espera-s "$usb_wait" | tr -d '\r'

    local log="$out/harness-smoke.log" waited=0 announced=0
    while (( waited < ceiling )); do
        # Por Wi-Fi, un corte mata el logcat en streaming: se relanza (y lo perdido lo recupera el
        # volcado final de abajo mientras siga en el buffer).
        if ! kill -0 "$logcat_pid" 2>/dev/null; then
            echo "(logcat se corto; se relanza)"
            start_capture
        fi
        tr -d '\r' < "$raw" | grep -F "run=$run " > "$log" || true
        if (( ! announced )) && grep -q 'step=esperando-humano' "$log"; then
            announced=1
            echo ">>> HUMANO: $(grep -m1 'step=esperando-humano' "$log" | sed -E 's/.*accion=([^ ]+).*/\1/' | tr '_' ' ')"
        fi
        if grep -q 'panel=plan step=fin ' "$log"; then
            break
        fi
        sleep 2   # WAIT-OK: polling con techo explicito (ceiling), no una espera ciega
        waited=$((waited + 2))
    done

    kill "$logcat_pid" 2>/dev/null || true
    # Un ultimo volcado del buffer, combinado con lo capturado y sin duplicados (el orden de emision
    # se conserva: primero lo capturado en vivo).
    { cat "$raw"; adb_ logcat -d -v raw -s HARNESS-SMOKE:I 2>/dev/null || true; } \
        | tr -d '\r' | grep -F "run=$run " | awk '!seen[$0]++' > "$log" || true
    local pid
    pid="$(adb_ shell pidof "$PKG" 2>/dev/null | tr -d '\r' || true)"
    if [[ -n "$pid" ]]; then
        adb_ logcat -d --pid="$pid" > "$out/logcat-app.txt" 2>/dev/null || true
        grep -E 'LibusbBackend|UsbAudio|libusb' "$out/logcat-app.txt" > "$out/logcat-usb.txt" || true
    fi
    echo "=== registro: $out ==="
    echo

    local rc=0
    verdict "$log" "$run" "$plan" || rc=$?
    print_ear_checks
    exit "$rc"
}

case "${1:-}" in
    --self-test) self_test ;;
    --veredicto)
        [[ $# -eq 4 ]] || { echo "uso: $0 --veredicto LOG RUN PLAN" >&2; exit 2; }
        verdict "$2" "$3" "$4" ;;
    -h|--help) sed -n '2,40p' "$0" ;;
    *) run_device "$@" ;;
esac
