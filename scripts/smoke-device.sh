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
#   2. lee la FICHA DE SETUP (scripts/smoke-setup.json, o --setup) y la valida antes de tocar nada;
#   3. construye e instala SOLO com.watermellonstudios.audio.harness (`install -r -g`: -g concede
#      los permisos de runtime del manifest, o sea RECORD_AUDIO; el permiso USB NO es de runtime y
#      no se concede asi — es el dialogo del sistema, un gesto humano);
#   4. verifica las precondiciones de HOST de la ficha para el plan pedido, por adb y en SOLO
#      LECTURA, antes de disparar el plan (REQ-053, AC-053.1);
#   5. dispara el plan con un extra de intent (MainActivity, build debug):
#        am start ... --es harness.smoke <plan> --es harness.smoke.run <id>
#   6. lee las lineas `HARNESS-SMOKE` de ESA corrida (filtra por run=<id>; el buffer de logcat no
#      se borra, es del device) hasta `panel=plan step=fin`, o hasta el techo;
#   7. cruza las precondiciones (las del host y las que emite la app) con la ficha y da un
#      veredicto por punto: PASS / FAIL / BLOQUEADO / HUMANO. Un paso con ok=false es FAIL; un paso
#      que falta es FAIL; lo que quedo detras de `step=esperando-humano` sin que el humano lo
#      resolviera es HUMANO, nunca PASS;
#   8. deja el JSON de la corrida al lado del log (harness-smoke.json);
#   9. lista aparte lo que requiere OIDO (ningun log dice si algo suena bien).
#
# REQ-053 — el modelo de precondiciones:
#   - La ficha declara, por plan, cada precondicion: quien la verifica (D4: `host` por adb, o `app`
#     con una linea `step=precondicion`), que pasos bloquea (`depende`) y la accion manual que la
#     arregla (`remedio`, D3). Es la UNICA fuente: este script no tiene ningun id escrito — nombra
#     CHEQUEOS de host (permiso-runtime, usb-interfaz-de-clase, paquetes-sin-proceso,
#     alsa-tarjeta-libre, usb-placa-no-reclamada) y la ficha elige uno por precondicion, con sus
#     parametros. Una precondicion con `depende: []` es una OBSERVACION (D14): se verifica y va al
#     JSON, pero no bloquea ningun paso.
#   - El script NUNCA ejecuta un remedio ni cambia nada del telefono (D3): lo imprime y lo deja en
#     el JSON. Una granja lo podra ejecutar despues; este script no.
#   - BLOQUEADO es un veredicto, no una cancelacion: la corrida se ejecuta igual y el JSON guarda lo
#     OBSERVADO de cada paso bloqueado. Un paso bloqueado no es PASS ni FAIL, aunque diga ok=true.
#   - Una precondicion que no se pudo verificar (adb fallo, salida que no se parsea o cortada, la
#     app no emitio su linea, la emitio quien no es su verificador) es `no-verificable`, y eso
#     bloquea igual que una incumplida: NUNCA cuenta como cumplida.
#   - D13: una precondicion de app con `ventana-humana` sin linea, con la espera humana del panel
#     pendiente, es `pendiente-humano`: lo que depende queda HUMANO (la ventana vencio), no BLOQUEADO.
#     Negada explicitamente, la app emite `cumplida=false` y es BLOQUEADO.
#
# Exit: 1 algun FAIL > 4 algun BLOQUEADO > 3 HUMANO pendiente > 0 todo PASS · 2 uso/infra (tambien
# una ficha invalida).
# Una fila de la suite USB NO-APLICA (linea con aplica=false: el device no ofrece su config, REQ-050
# S3) no mueve el exit: se lista aparte y no cuenta como cobertura. La vieja marca medido=false (D11
# de MINI-038: el runner ignoraba el rate de la fila) ya no existe y, si reaparece, es FAIL.
#
# El JSON (`<out>/harness-smoke.json`, D12): `run`, `plan`, `ficha`, `exit`, `resumen`,
# `precondiciones` ({id, plan, verificador, cumplida, estado, evidencia, remedio, depende}),
# `pasos` ({panel, paso, veredicto, detalle, observado, precondiciones}) y `sensor` (vacio: lo llena
# REQ-053 S3). `observado` es lo que emitio la app para ese paso (null si no lo emitio).
#
# El formato de las lineas vive en un solo lugar:
#   harness/src/commonMain/kotlin/com/watermellonstudios/audio/harness/smoke/HarnessSmoke.kt
# y lo fija HarnessSmokeTest. Los pasos que cada panel emite estan en SmokePlanRunner (salida,
# captura, sf2, sf3) y en UsbHarness (usb); EXPECTED, abajo, es la otra punta de ese contrato. Las
# precondiciones que emite la app estan en SmokePreconditions.
#
# Uso:
#   ANDROID_SERIAL=<serial> bash scripts/smoke-device.sh [--plan todo|salida,sf2,...]
#        [--usb-espera-s 120] [--techo-s N] [--out DIR] [--no-build] [--setup FICHA]
#   bash scripts/smoke-device.sh --self-test
#   bash scripts/smoke-device.sh --veredicto LOG RUN PLAN [--setup FICHA] [--json SALIDA] [--host-log LOG]
#        [--host-en-log]
#        # juzga un log ya grabado. LOG es el de la APP; las precondiciones del host van en
#        # --host-log (precondiciones-host.log). Una linea `verificador=host` en LOG se descarta.
#        # --host-en-log: SOLO para logs grabados antes de la separacion (S1), que traen las del
#        # host mezcladas; las toma del propio LOG y avisa que ese log no separa origenes. La
#        # corrida en device nunca lo pasa.

set -euo pipefail
cd "$(dirname "$0")/.."

readonly PKG="com.watermellonstudios.audio.harness"
readonly ACTIVITY="$PKG/.MainActivity"
readonly APK="harness/build/outputs/apk/debug/harness-debug.apk"
readonly SELFTEST_LOG="scripts/smoke-device-selftest.txt"
readonly SETUP_DEFAULT="scripts/smoke-setup.json"
readonly SELF="scripts/smoke-device.sh"

# ---------------------------------------------------------------------------
# La parte en Python: la ficha, el juez y los verificadores de host. Un solo programa con modos,
# para que los tres lean la ficha con el MISMO cargador:
#   veredicto LOG RUN PLAN FICHA [JSON]   el juez. Puro: log + run + plan + ficha adentro,
#                                         veredictos, exit code y (si se pide) el JSON afuera.
#                                         Es lo que prueba --self-test: no depende de adb.
#   host ADB SERIAL FICHA PLAN RUN PKG [DIR]
#                                         los verificadores de host: SOLO LECTURA, cada llamada
#                                         con `-s SERIAL`, y una linea `step=precondicion
#                                         verificador=host` por precondicion de host de la ficha.
#   validar FICHA                         exit 0 si la ficha es valida, 2 si no.
#   ids FICHA                             "plan id verificador", una por precondicion.
# ---------------------------------------------------------------------------
IFS= read -r -d '' SMOKE_PY <<'PY' || true
import fnmatch
import json
import os
import re
import select
import subprocess
import sys
import time

# --- El contrato con la app (ver la cabecera). El orden es el de emision. ---------------------
EXPECTED = {
    "salida": ["start", "stream", "frames"],
    "captura": ["start", "nivel", "stop"],
    # REQ-053 S3: `estimulo` y `control` son las dos ventanas de escucha (D5, D7), con el fixture
    # cargado: despues de la nota y antes de la descarga. Su orden lo fija la semilla.
    "sf2": ["fixture", "carga", "preset", "nota", "estimulo", "control", "descarga", "no-soundfont"],
    "sf3": ["fixture", "carga", "preset", "nota", "estimulo", "control", "descarga"],
    # REQ-050 S2: `motor-callback` va DESPUES de conectar (lo prepara la libreria dentro de
    # connectDevice, D6), asi que depende del permiso. `wake-lock` (D9): WAKE_LOCK llega por el
    # merge del manifest de :audio, porque el harness no lo declara. REQ-053 S3: el A4 del fixture
    # .sf2 se carga ANTES de parar el motor (fixture, carga, preset), suena por libusb despues de
    # parar el streaming (estimulo, control) y se descarga al final.
    "usb": ["fixture", "carga", "preset",
            "motor-parado", "dispositivos", "permiso", "conectar", "motor-callback", "capacidades", "descriptores", "backend",
            "wake-lock", "streaming-start", "streaming-stats", "reconectar-mismo", "conectar-otro",
            "suite", "streaming-stop", "estimulo", "control",
            "backend-restaurado", "desconectar", "descarga"],
}
ORDER = ["salida", "captura", "sf2", "sf3", "usb"]
# Pasos que se piden SOLO si hubo dialogo de permiso (REQ-050 S1): el broadcast falso que manda el
# script mientras el dialogo esta pendiente, y el juicio de la app sobre lo que paso.
WITH_DIALOG = {"usb": ["broadcast-falso", "permiso-falso"]}
# Lo del fixture en usb no depende del permiso: corre con o sin el humano.
USB_FONT = ("fixture", "carga", "preset", "descarga")
# Pasos del panel USB que dependen de que el humano haya dado el permiso.
AFTER_PERMISSION = [s for s in EXPECTED["usb"][EXPECTED["usb"].index("permiso"):] if s not in USB_FONT]
HUMAN = "esperando-humano"
# --- REQ-053 S3: el estimulo y el sensor ---------------------------------------------------------
# `escuchar` es el AVISO de una ventana (no es un veredicto, como esperando-humano) y es CIEGO: no
# dice si la ventana es estimulo o control. Al cerrarla la app emite `estimulo` o `control` con lo
# que el motor rindio. `sensor` es el juicio del sensor sobre una ventana: lo escribe el SCRIPT en
# su propio archivo, nunca la app (HarnessSmoke lo reserva y el juez descarta el que llegue por
# logcat).
LISTEN = "escuchar"
STIMULUS, CONTROL = "estimulo", "control"
WINDOW_STEPS = (STIMULUS, CONTROL)
SENSOR_STEP = "sensor"
AUDIBLE = ("sf2", "sf3", "usb")
# Lo que contesta un sensor sobre una ventana (D6): si detecto el estimulo, si no, o no sabe.
SENSOR_VERDICTS = ("presente", "ausente", "sin-dato")
NOT_DISCRIMINATING = "sensor-no-discrimina"
NOT_DISCRIMINATING_REMEDY = ("Repetir la corrida contestando SOLO lo que se oye: en una ventana de control no suena nada. "
                             "Si la ruta es la placa USB, escuchar con auriculares enchufados a la placa.")
# El margen entre la espera de la respuesta y la pausa que la app deja despues de cada ventana: la
# respuesta tiene que llegar ANTES de que la app avise la ventana siguiente (ver seguir()).
ANSWER_MARGIN_S = 3


def window_order(seed, panel):
    """El orden de las dos ventanas de un panel, de la semilla (AC-053.10). La MISMA cuenta que
    `WindowOrder.of` en el harness: FNV-1a de 32 bits sobre "<semilla>:<panel>", y el bit 0 de
    (h ^ (h >> 16)). Los dos lados fijan los mismos vectores en sus tests."""
    h = 2166136261
    for b in ("%d:%s" % (seed, panel)).encode("utf-8"):
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return [STIMULUS, CONTROL] if ((h >> 16) ^ h) & 1 == 0 else [CONTROL, STIMULUS]


def number(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def window_rendered(f):
    """AC-053.8: el motor rindio la ventana como se declaro. Un estimulo: ok, frames > 0 y pico > 0.
    Un control: ok (la app midio silencio) y frames > 0. El juez mira los NUMEROS, no solo el ok."""
    frames, peak = number(f.get("frames")), number(f.get("pico"))
    if f.get("ok") != "true" or frames is None or frames <= 0:
        return False
    if f.get("step") == STIMULUS:
        return peak is not None and peak > 0
    return True
# REQ-053 S1: la linea que trae una precondicion verificada (de la app, o del host con
# `verificador=host`). No es un paso: el juez la cruza con la ficha.
PRECOND = "precondicion"
KEY = re.compile(r"[a-z0-9-]+\Z")


def plan_panels(requested):
    """El plan PEDIDO, normalizado como lo normaliza SmokePlan (orden canonico, sin repetidos).
    None si nombra un panel que no existe."""
    if requested == "todo":
        return list(ORDER)
    ids = [x.strip() for x in requested.split(",")]
    if any(x not in ORDER for x in ids):
        return None
    return [p for p in ORDER if p in ids]


# --- La ficha de setup ---------------------------------------------------------------------------
# Los chequeos de host que el script sabe hacer, con los parametros que cada uno exige. La ficha
# elige uno por precondicion: el script nombra CHEQUEOS, nunca ids de precondicion.
CHECKS = {
    "permiso-runtime": {"permiso": str},
    "usb-interfaz-de-clase": {"clase": int},
    "paquetes-sin-proceso": {"paquetes": list},
    "alsa-tarjeta-libre": {"tarjeta": int},
    "usb-placa-no-reclamada": {"tarjeta": int, "clase": int},
}
COMMON_KEYS = {"id", "verificador", "depende", "remedio", "descripcion"}


class BadSetup(Exception):
    pass


def known_steps(panel):
    # `suite-1` representa a las filas `suite-N`, que no estan en EXPECTED.
    return EXPECTED[panel] + WITH_DIALOG.get(panel, []) + (["suite-1"] if panel == "usb" else [])


def valid_param(value, kind):
    if kind is int:
        return isinstance(value, int) and not isinstance(value, bool) and value >= 0
    if kind is str:
        return isinstance(value, str) and bool(value.strip())
    return isinstance(value, list) and bool(value) and all(isinstance(x, str) and x.strip() for x in value)


def load_setup(path):
    """La ficha, validada. Cualquier defecto es BadSetup: una ficha que no se entiende no puede
    decidir que bloquear, y adivinar es peor que no correr."""
    try:
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
    except OSError as e:
        raise BadSetup("no se puede leer %s: %s" % (path, e.strerror))
    except ValueError as e:
        raise BadSetup("%s no es JSON valido: %s" % (path, e))
    if not isinstance(doc, dict) or doc.get("formato") != 1:
        raise BadSetup('formato desconocido (se espera "formato": 1)')
    if set(doc) - {"formato", "descripcion", "planes"} or not isinstance(doc.get("planes"), dict):
        raise BadSetup('se espera {"formato", "descripcion", "planes"} y "planes" como objeto')
    seen, out = set(), {}
    for panel, body in doc["planes"].items():
        if panel not in EXPECTED:
            raise BadSetup("plan desconocido: '%s'" % panel)
        if (not isinstance(body, dict) or set(body) - {"precondiciones", "descripcion"}
                or not isinstance(body.get("precondiciones"), list)):
            raise BadSetup("%s: se espera {\"precondiciones\": [...]}" % panel)
        out[panel] = []
        for p in body["precondiciones"]:
            if not isinstance(p, dict):
                raise BadSetup("%s: una precondicion no es un objeto" % panel)
            pid = p.get("id")
            if not isinstance(pid, str) or not KEY.match(pid):
                raise BadSetup("%s: id invalido %r (se espera [a-z0-9-]+)" % (panel, pid))
            where = "%s/%s" % (panel, pid)
            if pid in seen:
                raise BadSetup("%s: id duplicado" % where)
            seen.add(pid)
            verifier = p.get("verificador")
            if verifier == "host":
                check = p.get("chequeo")
                if check not in CHECKS:
                    raise BadSetup("%s: chequeo de host desconocido %r (conocidos: %s)"
                                   % (where, check, ", ".join(sorted(CHECKS))))
                for name, kind in CHECKS[check].items():
                    if not valid_param(p.get(name), kind):
                        raise BadSetup("%s: el chequeo %s exige '%s' (%s, no vacio)" % (where, check, name, kind.__name__))
                allowed = COMMON_KEYS | {"chequeo"} | set(CHECKS[check])
            elif verifier == "app":
                if "ventana-humana" in p and not isinstance(p["ventana-humana"], bool):
                    raise BadSetup("%s: ventana-humana tiene que ser true o false" % where)
                allowed = COMMON_KEYS | {"ventana-humana"}
            else:
                raise BadSetup("%s: verificador desconocido %r (host o app)" % (where, verifier))
            unknown = set(p) - allowed
            if unknown:
                raise BadSetup("%s: clave desconocida %s" % (where, ", ".join(sorted(unknown))))
            dep = p.get("depende")
            # Vacia es valida (D14): una observacion se registra y no bloquea nada.
            if not isinstance(dep, list) or not all(isinstance(d, str) and d for d in dep):
                raise BadSetup("%s: 'depende' tiene que ser una lista de pasos (vacia: observacion)" % where)
            for pattern in dep:
                if not any(fnmatch.fnmatchcase(s, pattern) for s in known_steps(panel)):
                    raise BadSetup("%s: depende de '%s', que no es un paso de %s" % (where, pattern, panel))
            if not isinstance(p.get("remedio"), str) or not p["remedio"].strip():
                raise BadSetup("%s: sin remedio (D3: cada precondicion declara su accion manual)" % where)
            out[panel].append(p)
    # El atajo "sin placa => cumplida" de usb-placa-no-reclamada solo es honesto si la precondicion
    # que SI bloquea sin placa (usb-interfaz-de-clase, de la misma clase) esta en el mismo plan.
    for panel, pres in out.items():
        classes = {p["clase"] for p in pres if p.get("chequeo") == "usb-interfaz-de-clase"}
        for p in pres:
            if p.get("chequeo") == "usb-placa-no-reclamada" and p["clase"] not in classes:
                raise BadSetup("%s/%s: usb-placa-no-reclamada exige una precondicion usb-interfaz-de-clase "
                               "con la misma clase (%d) en el plan: sin ella, 'sin placa' no bloquea nada"
                               % (panel, p["id"], p["clase"]))
    return out


def value(v):
    """Como HarnessSmoke.value: sin blancos, nunca vacio."""
    s = re.sub(r"\s", "_", str(v))
    return s or "-"


# --- Los verificadores de host -------------------------------------------------------------------
class Unverifiable(Exception):
    """La precondicion no se pudo verificar. NUNCA cuenta como cumplida (AC-053.3)."""


class Shell:
    """`adb -s SERIAL shell CMD`, SOLO LECTURA. El exit del comando remoto viaja en una marca final:
    sin la marca, la salida no es de ese comando (adb fallo, el device se fue) y no se lee."""

    def __init__(self, adb, serial, pkg):
        self.adb, self.serial, self.pkg, self.log = adb, serial, pkg, []

    def run(self, cmd):
        argv = [self.adb, "-s", self.serial, "shell", "%s; r=$?; echo; echo wma-rc=$r" % cmd]
        try:
            p = subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                               encoding="utf-8", errors="replace", timeout=30)
        except (OSError, subprocess.SubprocessError) as e:
            self.log.append("$ %s\n[no corrio: %s]\n" % (cmd, e))
            raise Unverifiable("adb-no-corrio:%s" % type(e).__name__)
        out = p.stdout.replace("\r", "")
        self.log.append("$ %s\n[adb rc=%d]\n%s%s\n" % (cmd, p.returncode, out, p.stderr))
        m = re.search(r"(?:\A|\n)wma-rc=(\d+)\s*\Z", out)
        if not m:
            why = (p.stderr.strip() or out.strip() or "sin-salida").splitlines()[0][:80]
            raise Unverifiable("adb-sin-marca:rc=%d:%s" % (p.returncode, why))
        return int(m.group(1)), out[:m.start()]


def check_runtime_permission(p, sh):
    rc, out = sh.run("dumpsys package %s" % sh.pkg)
    if rc != 0:
        raise Unverifiable("dumpsys-package-rc=%d" % rc)
    # Con varios usuarios (perfil de trabajo, Secure Folder) cada uno tiene su bloque `User N:`;
    # el smoke corre en el usuario 0, y un perfil secundario sin el permiso no es este setup.
    users = re.split(r"^\s*User (\d+):", out, flags=re.M)
    if len(users) > 1:
        out = "".join(users[i + 1] for i in range(1, len(users), 2) if users[i] == "0")
    grants = re.findall(r"^\s*%s: granted=(true|false)" % re.escape(p["permiso"]), out, re.M)
    if not grants:
        raise Unverifiable("%s-no-figura-en-dumpsys-package" % p["permiso"].rsplit(".", 1)[-1])
    return ("true", "granted=true") if all(g == "true" for g in grants) else ("false", "granted=false")


def parse_dump(text):
    """El modo texto de dumpsys: `clave={` / `clave=[` abre un bloque, `{` suelto abre uno anonimo
    (clave "": los elementos de una lista), `}` / `]` lo cierran, `clave=valor` es hoja."""
    root, stack = [], []
    stack.append(root)
    for raw in text.splitlines():
        line = raw.strip()
        if line in ("}", "]"):
            if len(stack) > 1:
                stack.pop()
            continue
        m = re.match(r"([A-Za-z0-9_]*)=?[\{\[]$", line)
        if m:
            block = []
            stack[-1].append((m.group(1), block))
            stack.append(block)
            continue
        m = re.match(r"([A-Za-z0-9_]+)=(.*)$", line)
        if m:
            stack[-1].append((m.group(1), m.group(2)))
    return root


def blocks(nodes, key):
    out = []
    for k, v in nodes:
        if isinstance(v, list):
            if k == key:
                out.append(v)
            out.extend(blocks(v, key))
    return out


def leaf(nodes, key):
    for k, v in nodes:
        if k == key and isinstance(v, str):
            return v
    return None


def interfaces(device):
    """Las interfaces de un dispositivo: cada `interfaces={...}` suelto, o cada bloque anonimo de una
    lista `interfaces=[ {...} {...} ]` (el formato real del g42)."""
    out = []
    for b in blocks(device, "interfaces"):
        anon = [v for k, v in b if k == "" and isinstance(v, list)]
        out.extend(anon if anon else [b])
    return out


def check_usb_interface_class(p, sh):
    rc, out = sh.run("dumpsys usb")
    if rc != 0:
        raise Unverifiable("dumpsys-usb-rc=%d" % rc)
    hosts = blocks(parse_dump(out), "host_manager")
    if not hosts:
        # Sin el bloque del host no se sabe si la placa esta o si cambio el formato: no se adivina.
        raise Unverifiable("dumpsys-usb-sin-host_manager:formato-no-reconocido")
    devices = [d for h in hosts for k, d in h if k == "devices" and isinstance(d, list)]
    seen, hits = [], []
    for d in devices:
        vid, pid = leaf(d, "vendor_id") or "", leaf(d, "product_id") or ""
        tag = "%04x:%04x" % (int(vid), int(pid)) if vid.isdigit() and pid.isdigit() else "%s:%s" % (vid or "?", pid or "?")
        classes = sorted({leaf(i, "class") for i in interfaces(d)} - {None})
        seen.append("%s(clases:%s)" % (tag, ",".join(classes) or "-"))
        if str(p["clase"]) in classes:
            hits.append("%s:%s" % (tag, leaf(d, "product_name") or "-"))
    if hits:
        return "true", "interfaz-clase-%d:%s" % (p["clase"], ";".join(hits))
    return "false", "sin-interfaz-clase-%d:dispositivos=%s" % (p["clase"], ";".join(seen) or "ninguno")


def check_packages_without_process(p, sh):
    rc, out = sh.run("ps -A -o PID,NAME")
    if rc != 0:
        raise Unverifiable("ps-rc=%d" % rc)
    rows = [l.split() for l in out.splitlines() if l.strip()]
    if not rows or rows[0][:2] != ["PID", "NAME"]:
        raise Unverifiable("ps-salida-ilegible")
    procs = [(r[0], r[1]) for r in rows[1:] if len(r) >= 2 and r[0].isdigit()]
    if not any(name == "system_server" for _, name in procs):
        # Sin system_server este ps no ve procesos de otros UID: "no esta vivo" no se puede afirmar.
        raise Unverifiable("ps-no-ve-otros-uid:sin-system_server")
    alive = ["%s:pid=%s" % (name, pid) for pid, name in procs
             for pkg in p["paquetes"] if name == pkg or name.startswith(pkg + ":")]
    if alive:
        return "false", "vivos=" + ",".join(alive)
    return "true", "sin-proceso:" + ",".join(p["paquetes"])


def audioserver_threads(dump):
    """Los hilos VIVOS de `dumpsys media.audio_flinger`: columna 0 y `Output thread `, `Input thread `
    o `Mmap... thread `. Los que empiezan con `- ` son hilos ya cerrados y no cuentan. Devuelve
    (nombre, dispositivos, standby) con standby None si el hilo no tiene su linea de nivel de hilo
    (la de 2 espacios; la de `Hal stream dump` va mas adentro y no es la del hilo)."""
    threads, cur = [], None
    for line in dump.splitlines():
        if line and not line[0].isspace():
            m = re.match(r"(Output|Input|Mmap\S*) thread (?:\S+, name ([^\s,]+))?", line)
            cur = {"kind": m.group(1), "name": m.group(2) or "?", "devices": [], "seen": set(), "standby": None} if m else None
            if cur:
                threads.append(cur)
            continue
        if cur is None:
            continue
        m = re.match(r"  (Output devices|Input device): (.*)$", line)
        if m:
            cur["devices"].append(m.group(2))
            cur["seen"].add(m.group(1))
        m = re.match(r"  Standby: (yes|no)\s*$", line)
        if m and cur["standby"] is None:
            cur["standby"] = m.group(1)
    return threads


def check_alsa_card_free(p, sh):
    card = p["tarjeta"]
    rc, out = sh.run("ls /dev/snd")
    if rc != 0:
        raise Unverifiable("ls-dev-snd-rc=%d" % rc)
    entries = out.split()
    if not any(re.fullmatch(r"controlC\d+", e) for e in entries):
        raise Unverifiable("dev-snd-ilegible")
    if not any(re.fullmatch(r"pcmC%dD\d+[pc]" % card, e) for e in entries):
        return "true", "sin-pcmC%d" % card
    # La tarjeta esta. /proc/asound pide root; quien la tiene lo dice el audioserver, que shell lee.
    rc, dump = sh.run("dumpsys media.audio_flinger")
    if rc != 0:
        raise Unverifiable("dumpsys-audio_flinger-rc=%d" % rc)
    threads = audioserver_threads(dump)
    if not any(t["kind"] == "Output" for t in threads):
        raise Unverifiable("audio_flinger-sin-Output-thread:formato-no-reconocido")
    # Un hilo vivo sin la linea de dispositivos de SU lado (un Output thread trae tambien un
    # `Input device: 0`, que no lo cuenta) podria ser el de la placa: no se sabe.
    own = {"Output": "Output devices", "Input": "Input device"}
    nameless = [t["name"] for t in threads if (own[t["kind"]] not in t["seen"] if t["kind"] in own else not t["seen"])]
    if nameless:
        raise Unverifiable("%s:hilo-sin-linea-de-dispositivos" % ",".join(nameless))
    usb = [t for t in threads if any(re.search(r"AUDIO_DEVICE_(OUT|IN)_USB_", d) for d in t["devices"])]
    taken = [t for t in usb if t["standby"] == "no"]
    if taken:
        return "false", ";".join("%s:%s:standby=no" % (t["name"], " ".join(t["devices"])) for t in taken)
    blind = [t["name"] for t in usb if t["standby"] is None]
    if blind:
        raise Unverifiable("%s:hilo-usb-sin-Standby-legible" % ",".join(blind))
    if usb:
        return "true", ";".join("%s:standby=yes" % t["name"] for t in usb)
    return "true", "sin-hilo-usb"


def check_placa_no_reclamada(p, sh):
    """D14: un proceso que reclama la placa por usbfs hace que el kernel desligue el driver ALSA, y
    `controlC<n>` desaparece de /dev/snd hasta que la suelta. La placa se busca con el mismo parser
    de la precondicion de placa enumerada. Sin placa devuelve CUMPLIDA y no no-verificable: ya la bloquea
    esa otra, y una segunda precondicion bloqueando por lo mismo duplicaria el BLOQUEADO con
    un remedio que no es el de ese caso."""
    card = p["tarjeta"]
    present, _ = check_usb_interface_class({"clase": p["clase"]}, sh)
    if present != "true":
        return "true", "sin-placa:lo-cubre-placa-enumerada"
    rc, out = sh.run("ls /dev/snd")
    if rc != 0:
        raise Unverifiable("ls-dev-snd-rc=%d" % rc)
    entries = out.split()
    # Solo nombres de /dev/snd (controlC1, pcmC1D0p, comprC0D11, timer, seq...): si hay otra cosa,
    # no es un listado y la ausencia de controlC<n> no significa nada.
    if not entries or not all(re.fullmatch(r"controlC\d+|(pcm|hw|midi|compr)C\d+D\d+[pc]?|timer|seq", e) for e in entries):
        raise Unverifiable("dev-snd-ilegible")
    if "controlC%d" % card in entries:
        return "true", "controlC%d-presente" % card
    return "false", "placa-enumerada-sin-controlC%d:otro-proceso-la-reclama" % card


HOST_CHECKS = {
    "permiso-runtime": check_runtime_permission,
    "usb-interfaz-de-clase": check_usb_interface_class,
    "paquetes-sin-proceso": check_packages_without_process,
    "alsa-tarjeta-libre": check_alsa_card_free,
    "usb-placa-no-reclamada": check_placa_no_reclamada,
}
assert set(HOST_CHECKS) == set(CHECKS)


def host(adb, serial, setup_path, requested, run, pkg, evidence_dir):
    setup = load_setup(setup_path)
    for panel in plan_panels(requested) or []:
        for p in setup.get(panel, []):
            if p["verificador"] != "host":
                continue
            sh = Shell(adb, serial, pkg)
            try:
                met, evidence = HOST_CHECKS[p["chequeo"]](p, sh)
            except Unverifiable as e:
                met, evidence = "no-verificable", str(e)
            if evidence_dir:
                os.makedirs(evidence_dir, exist_ok=True)
                with open(os.path.join(evidence_dir, "%s.txt" % p["id"]), "w", encoding="utf-8") as f:
                    f.write("".join(sh.log))
            print("HARNESS-SMOKE v=1 run=%s panel=%s step=%s ok=%s id=%s cumplida=%s evidencia=%s verificador=host"
                  % (run, panel, PRECOND, "true" if met == "true" else "false", p["id"], met, value(evidence)))


# --- El juez -------------------------------------------------------------------------------------
def judge(log, run, requested, setup_path, json_out, host_log=None, host_in_log=False, sensor_log=None, cli_seed=None):
    setup = load_setup(setup_path)
    wanted = plan_panels(requested)
    rows = []        # {veredicto, panel, paso, detalle, observado, precondiciones}
    preconditions = []
    sensor_entries = []   # S3: un registro por ventana cerrada, con lo que dijo el sensor y su juicio
    seed, seed_origin, orders = None, None, {}

    def add(v, panel, step, detail="", observed=None, blockers=()):
        rows.append({"veredicto": v, "panel": panel, "paso": step, "detalle": detail,
                     "observado": observed, "precondiciones": [b if isinstance(b, str) else b["id"] for b in blockers]})
        return rows[-1]

    def extras(f):
        return " ".join("%s=%s" % (k, v) for k, v in f.items() if k not in ("v", "run", "panel", "step", "ok"))

    def observed(f):
        return {k: v for k, v in f.items() if k not in ("v", "run", "panel", "step")}

    def finish(code):
        counts = {k: sum(1 for r in rows if r["veredicto"] == k) for k in ("PASS", "FAIL", "BLOQUEADO", "HUMANO", "NO-APLICA")}
        if json_out:
            doc = {"formato": 1, "run": run, "plan": requested, "ficha": setup_path, "exit": code,
                   "resumen": counts, "lineas-host-descartadas": discarded, "host-en-log": taken_from_log,
                   "lineas-sensor-descartadas": discarded_sensor,
                   # S3 (AC-053.10): la semilla y el orden que da; con la semilla se reproduce.
                   "semilla": seed, "semilla-origen": seed_origin, "orden": orders,
                   "precondiciones": preconditions,
                   "pasos": [{k: v for k, v in r.items() if not k.startswith("_")} for r in rows],
                   # S3: cada ventana cerrada, con el juicio del sensor (o por que no se lo consulto).
                   "sensor": [{k: v for k, v in e.items() if not k.startswith("_")} for e in sensor_entries]}
            try:
                with open(json_out, "w", encoding="utf-8") as f:
                    json.dump(doc, f, ensure_ascii=False, indent=2)
                    f.write("\n")
            except OSError as e:
                print("ERROR — no se pudo escribir el JSON %s: %s" % (json_out, e.strerror))
                sys.exit(2)
        sys.exit(code)

    lines = []
    sensor_lines = []
    discarded = 0
    discarded_sensor = 0
    taken_from_log = 0

    def read_lines(path, from_host, from_sensor=False):
        # El log de la app (logcat) lo puede escribir cualquier app con el tag: una linea
        # `verificador=host` que llegue por ahi es una falsificacion y se DESCARTA (ni bloquea ni
        # cuenta). Las del host vienen SOLO de su propio archivo. S3: lo mismo con `step=sensor`, que
        # viene SOLO del registro del sensor, que escribe el script.
        nonlocal discarded, taken_from_log, discarded_sensor
        with open(path, encoding="utf-8", errors="replace") as f:
            for raw in f:
                fields = parse_line(raw)
                if fields is None or fields.get("run") != run:
                    continue
                if from_sensor:
                    if fields.get("step") == SENSOR_STEP:
                        sensor_lines.append(fields)
                    continue
                if fields.get("step") == SENSOR_STEP:
                    if not from_host:
                        discarded_sensor += 1
                    continue
                if from_host != (fields.get("verificador") == "host"):
                    # --host-en-log (solo logs grabados de S1, que mezclan origenes): las del host
                    # que trae el propio log valen como del host.
                    if not from_host and host_in_log:
                        taken_from_log += 1
                    else:
                        if not from_host:
                            discarded += 1
                        continue
                if fields.get("v") != "1":
                    add("FAIL", "formato", "version", "version desconocida: %s" % raw.strip())
                    print("FAIL  formato  version desconocida: %s" % raw.strip())
                    finish(1)
                lines.append(fields)

    if host_log:
        read_lines(host_log, True)
    read_lines(log, False)
    if sensor_log:
        read_lines(sensor_log, False, from_sensor=True)
    if host_in_log:
        print("AVISO — --host-en-log: %d linea(s) verificador=host tomadas del propio log; este log no separa origenes "
              "(cualquier app con el tag las puede escribir), asi que NO prueba que el host verifico\n" % taken_from_log)
    if discarded:
        print("AVISO — %d linea(s) verificador=host en el log de la app: descartadas (solo el host firma como host)\n" % discarded)
    if discarded_sensor:
        print("AVISO — %d linea(s) step=sensor en el log de la app: descartadas (los juicios del sensor los registra "
              "el script)\n" % discarded_sensor)

    inicio = [f for f in lines if f.get("panel") == "plan" and f.get("step") == "inicio"]
    panels = [p for p in inicio[0].get("plan", "").split(",") if p in EXPECTED] if inicio else []

    def human_wait(panel):
        mine = [f for f in lines if f.get("panel") == panel]
        waits = [f for f in mine if f.get("step") == HUMAN]
        # El humano actuo si el paso `permiso` lo dice, o si la precondicion con ventana humana de
        # la app se CUMPLIO (D13): con el permiso dado y connectDevice colgado no hay paso
        # `permiso`, y lo que sigue es un FAIL de la libreria, no una espera humana.
        window_ids = {p["id"] for p in setup.get(panel, []) if p["verificador"] == "app" and p.get("ventana-humana")}
        granted = any(f.get("step") == "permiso" and f.get("ok") == "true" for f in mine) or any(
            f.get("step") == PRECOND and f.get("id") in window_ids and "verificador" not in f
            and f.get("ok") == "true" and f.get("cumplida") == "true" for f in mine)
        denied = any(f.get("step") == "permiso" and f.get("ok") != "true" for f in mine)
        return waits, granted, denied

    # --- AC-053.1: las precondiciones, ANTES de juzgar cualquier paso. -----------------------
    def evaluate(p, panel, human_pending):
        mine = [f for f in lines if f.get("panel") == panel and f.get("step") == PRECOND and f.get("id") == p["id"]]
        # Una linea vale solo si la firmo el verificador que declara la ficha. La app no puede
        # escribir `verificador` (HarnessSmoke lo reserva): sin la clave, la linea es de la app.
        right = [f for f in mine if f.get("verificador", "app") == p["verificador"]]
        if not right:
            if mine:
                state, evidence = "no-verificable", "la-linea-no-es-del-verificador-%s" % p["verificador"]
            elif p["verificador"] == "app" and p.get("ventana-humana") and human_pending:
                # D13: la ventana humana vencio sin respuesta. No es un setup roto: lo que depende
                # queda HUMANO (la regla del permiso, abajo), nunca BLOQUEADO ni cumplido.
                state, evidence = "pendiente-humano", "sin-respuesta-humana-en-la-ventana"
            elif p["verificador"] == "app":
                state, evidence = "no-verificable", "la-app-no-emitio-su-linea"
            else:
                state, evidence = "no-verificable", "el-host-no-registro-su-verificacion"
        else:
            evidence = ";".join(f.get("evidencia", "-") for f in right)
            bad = [f for f in right if f.get("cumplida") not in ("true", "false") or f.get("ok") != f.get("cumplida")]
            if bad:
                # Incluye la de host que dice cumplida=no-verificable (adb fallo, salida ilegible).
                state = "no-verificable"
                if any(f.get("cumplida") != "no-verificable" for f in bad):
                    evidence = "linea-ilegible:" + evidence
            elif all(f.get("cumplida") == "true" for f in right):
                state = "cumplida"
            else:
                state = "incumplida"
                evidence = ";".join(f.get("evidencia", "-") for f in right if f.get("cumplida") == "false")
        return {"id": p["id"], "plan": panel, "verificador": p["verificador"], "cumplida": state == "cumplida",
                "estado": state, "evidencia": evidence, "remedio": p["remedio"], "depende": list(p["depende"])}

    judged = [p for p in ORDER if (wanted is not None and p in wanted) or p in panels]
    for panel in judged:
        waits, granted, _ = human_wait(panel)
        for p in setup.get(panel, []):
            preconditions.append(evaluate(p, panel, bool(waits) and not granted))
    blocking = [r for r in preconditions if r["estado"] in ("incumplida", "no-verificable")]

    def blockers(panel, step):
        return [r for r in blocking if r["plan"] == panel and any(fnmatch.fnmatchcase(step, d) for d in r["depende"])]

    def blocked(panel, step, bs, seen_text, observed_fields):
        detail = " | ".join("precondicion=%s estado=%s evidencia=%s remedio=%s"
                            % (b["id"], b["estado"], b["evidencia"], b["remedio"]) for b in bs)
        add("BLOQUEADO", panel, step, "%s · observado: %s" % (detail, seen_text), observed_fields, bs)

    # --- Los pasos. ------------------------------------------------------------------------
    if not lines:
        add("FAIL", "plan", "inicio", "ninguna linea HARNESS-SMOKE con run=%s" % run)
        print("FAIL  plan  ninguna linea HARNESS-SMOKE con run=%s" % run)
        finish(1)
    if not inicio:
        add("FAIL", "plan", "inicio", "falta")
    else:
        add("PASS" if inicio[0].get("ok") == "true" else "FAIL", "plan", "inicio", extras(inicio[0]), observed(inicio[0]))
        if wanted is None or panels != wanted:
            add("FAIL", "plan", "pedido", "se pidio '%s' y la app corrio '%s'" % (requested, ",".join(panels) or "-"))

    declared = {(r["plan"], r["id"]) for r in preconditions}
    for f in lines:
        if f.get("step") == PRECOND and (f.get("panel"), f.get("id")) not in declared:
            add("FAIL", f.get("panel", "-"), PRECOND,
                "id=%s no esta declarada en la ficha de setup para %s (o el plan no se pidio)" % (f.get("id", "-"), f.get("panel", "-")),
                observed(f))

    def judge_step(panel, step, f, pending):
        bs = blockers(panel, step)
        if bs and f.get("ok") != "true" and f.get("concluyente") == "true":
            # La app sabe que esta falla no la explica ninguna precondicion (p.ej. un grant que
            # UsbManager desmiente): un bloqueo no la puede tapar. Solo vale para ok=false:
            # `concluyente` nunca destapa un PASS.
            add("FAIL", panel, step, "concluyente, ninguna precondicion lo explica: " + extras(f), observed(f))
        elif bs:
            # AC-053.2: ni PASS ni FAIL, aunque la app haya dicho ok=true: lo que se observo
            # queda en el JSON (S2 lo necesita), pero no se juzga.
            blocked(panel, step, bs, extras(f) or "-", observed(f))
        elif pending and step in AFTER_PERMISSION:
            add("HUMANO", panel, step, "sin permiso: " + extras(f), observed(f))
        elif "medido" in f:
            # REQ-050 S3: el runner aplica el rate de cada fila, asi que NO-MEDIDO (D11 de MINI-038)
            # ya no existe. Una linea que lo trae es una regresion del harness o del runner: FAIL.
            add("FAIL", panel, step, "NO-MEDIDO ya no existe (REQ-050 S3): " + extras(f), observed(f))
        elif f.get("aplica") == "false" and panel == "usb" and re.fullmatch(r"suite-[0-9]+", step or ""):
            # REQ-050 S3 (D5): una fila que el device no ofrece. Ni PASS (aunque diga ok=true) ni FAIL,
            # y no cuenta como cobertura: se lista aparte. SOLO filas de la suite: cualquier otro paso
            # con aplica=false es un FAIL, o un paso podria desaparecer del veredicto.
            add("NO-APLICA", panel, step, extras(f), observed(f))
        elif f.get("aplica") == "false":
            add("FAIL", panel, step, "aplica=false fuera de una fila de la suite: " + extras(f), observed(f))
        elif step in WINDOW_STEPS and not window_rendered(f):
            # AC-053.8: el motor no rindio la ventana como se declaro. El juez mira los numeros
            # (frames, pico), no solo el ok: un ok=true con frames=0 tambien es FAIL.
            add("FAIL", panel, step, "%s no-rendido: %s" % (step, extras(f)), observed(f))
        elif f.get("ok") == "true":
            add("PASS", panel, step, extras(f), observed(f))
        else:
            add("FAIL", panel, step, extras(f), observed(f))

    def missing_steps(panel, seen, waits, pending):
        for step in EXPECTED[panel]:
            if step not in seen:
                bs = blockers(panel, step)
                if bs:
                    blocked(panel, step, bs, "no-emitido", None)
                elif pending and step in AFTER_PERMISSION:
                    add("HUMANO", panel, step, "no corrio: espera el permiso USB")
                else:
                    add("FAIL", panel, step, "FALTA: el panel no emitio este paso")
        # REQ-050 S1: con el dialogo pendiente, el script le manda a la app el broadcast de resultado
        # FALSO (broadcast-falso, lo escribe el script) y la app afirma que nada cambio (permiso-falso).
        # Sin dialogo (permiso ya concedido) no hay espera que falsear y no se piden.
        if waits:
            for step in WITH_DIALOG.get(panel, []):
                if step not in seen:
                    bs = blockers(panel, step)
                    if bs:
                        blocked(panel, step, bs, "no-emitido", None)
                    else:
                        add("FAIL", panel, step, "FALTA: con dialogo de permiso este paso es obligatorio (REQ-050 S1)")

    # --- S3 (AC-053.10): la semilla y el orden de las ventanas. ------------------------------
    windows = []   # (panel, linea de cierre, fila de su veredicto)
    window_lines = [f for f in lines if f.get("step") in WINDOW_STEPS and f.get("panel") in AUDIBLE]
    app_seed = inicio[0].get("semilla") if inicio else None
    if app_seed is not None and not re.fullmatch(r"[0-9]+", app_seed):
        add("FAIL", "plan", "semilla", "la app registro una semilla ilegible: %s" % app_seed)
    elif cli_seed is not None:
        seed, seed_origin = cli_seed, "script"
        if app_seed is None and window_lines:
            add("FAIL", "plan", "semilla", "el script mando semilla=%d y la app no registro ninguna" % cli_seed)
        elif app_seed is not None and int(app_seed) != cli_seed:
            add("FAIL", "plan", "semilla", "el script mando semilla=%d y la app corrio con semilla=%s" % (cli_seed, app_seed))
    elif app_seed is not None:
        seed, seed_origin = int(app_seed), inicio[0].get("semilla-origen", "app")
    elif window_lines:
        add("FAIL", "plan", "semilla", "sin semilla registrada: el orden de las ventanas no se puede reproducir")
    if seed is not None:
        orders.update({p: window_order(seed, p) for p in judged if p in AUDIBLE})
    for panel in [p for p in judged if p in AUDIBLE and p in orders]:
        mine_w = [f for f in window_lines if f.get("panel") == panel]
        ran = [(f.get("n"), f.get("step")) for f in mine_w]
        want = [(str(i + 1), k) for i, k in enumerate(orders[panel])]
        if mine_w and ran != want[:len(ran)]:
            add("FAIL", panel, "orden", "las ventanas no siguen la semilla %d: corrio %s, la semilla da %s"
                % (seed, ",".join("%s:%s" % x for x in ran), ",".join("%s:%s" % x for x in want)))

    # --- S3 (AC-053.8..10): el juicio del sensor sobre cada ventana cerrada. ------------------
    def judge_windows():
        by_key = {}
        for rec in sensor_lines:
            by_key.setdefault((rec.get("panel"), rec.get("n")), []).append(rec)
        used = set()
        not_discriminating = {}   # sensor -> controles en los que dijo "presente"
        listens = {(f.get("panel"), f.get("n")): f for f in lines if f.get("step") == LISTEN}
        for panel, f, wrow in windows:
            key = (panel, f.get("n"))
            used.add(key)
            recs = by_key.get(key, [])
            rec = recs[0] if len(recs) == 1 else {}
            kind = f.get("step")
            step = "sensor-" + kind
            entry = {"panel": panel, "n": int(f["n"]) if (f.get("n") or "").isdigit() else f.get("n"), "tipo": kind,
                     "estimulo": listens.get(key, {}).get("estimulo") or rec.get("estimulo"),
                     "ruta": f.get("ruta"), "sensor": rec.get("sensor"), "veredicto": rec.get("veredicto"),
                     "evidencia": rec.get("evidencia"), "consultado": bool(recs), "juicio": None}
            sensor_entries.append(entry)
            said = "sensor=%s veredicto=%s evidencia=%s ventana=%s/%s ruta=%s" % (
                rec.get("sensor", "-"), rec.get("veredicto", "-"), rec.get("evidencia", "-"), panel, f.get("n", "-"), f.get("ruta", "-"))
            v = wrow["veredicto"]
            if v in ("BLOQUEADO", "HUMANO"):
                # La ventana no se juzgo (precondicion, o el permiso USB pendiente): su juicio tampoco.
                entry["juicio"] = v
                bs = [b for b in blocking if b["plan"] == panel and b["id"] in wrow["precondiciones"]]
                if bs:
                    blocked(panel, step, bs, "la ventana salio BLOQUEADO; " + said, rec or None)
                else:
                    add(v, panel, step, "la ventana salio %s; %s" % (v, said), rec or None)
                continue
            if v != "PASS":
                # AC-053.8: el motor no rindio la ventana como se declaro, y el sensor NO se consulta.
                # Si hay juicio, el script lo pidio cuando no debia: eso es un FAIL, nunca un PASS.
                if recs:
                    entry["juicio"] = "FAIL"
                    add("FAIL", panel, step, "se consulto al sensor sobre una ventana no-rendida: " + said, rec or None)
                continue
            if not recs:
                entry["juicio"] = "HUMANO"
                add("HUMANO", panel, step, "sin-juicio: el sensor no registro nada para esta ventana", None)
                continue
            if len(recs) > 1:
                entry["juicio"] = "FAIL"
                add("FAIL", panel, step, "%d juicios para la misma ventana: el script consulto mas de una vez" % len(recs), None)
                continue
            heard = rec.get("veredicto")
            if heard not in SENSOR_VERDICTS or heard == "sin-dato":
                verdict = "HUMANO"
            elif kind == STIMULUS:
                verdict = "PASS" if heard == "presente" else "FAIL"
            elif heard == "ausente":
                verdict = "PASS"
            else:
                # D7: oyo el estimulo donde no sono. Invalida a ESE sensor en toda la corrida (abajo).
                verdict = "BLOQUEADO"
                not_discriminating.setdefault(rec.get("sensor"), []).append("%s/%s:control:presente" % key)
            entry["juicio"] = verdict
            row = add(verdict, panel, step, said, rec)
            row["_sensor"], row["_entry"] = rec.get("sensor"), entry
        for key in sorted(k for k in by_key if k not in used):
            for rec in by_key[key]:
                add("FAIL", key[0] or "-", "sensor-sin-ventana",
                    "juicio de sensor para la ventana %s/%s, que la app no emitio" % key, rec)
        # AC-053.10: un "si" en un control => TODO lo que juzgo ese sensor sale BLOQUEADO.
        for name, where in not_discriminating.items():
            for row in rows:
                if row.get("_sensor") == name:
                    old = "%s: %s" % (row["veredicto"], row["detalle"])
                    row.update(veredicto="BLOQUEADO", precondiciones=[NOT_DISCRIMINATING],
                               detalle="precondicion=%s estado=incumplida evidencia=%s remedio=%s · antes: %s"
                               % (NOT_DISCRIMINATING, ",".join(where), NOT_DISCRIMINATING_REMEDY, old))
                    row["_entry"]["juicio"] = "BLOQUEADO"

    human_pending = set()
    for panel in [p for p in ORDER if p in panels]:
        mine = [f for f in lines if f.get("panel") == panel]
        waits, granted, denied = human_wait(panel)
        for w in waits:
            state = "hecho — " if granted else ("DENEGADO por el humano — " if denied else "PENDIENTE — ")
            add("HUMANO", panel, HUMAN, state + extras(w), observed(w))
        pending = bool(waits) and not granted
        if pending:
            human_pending.add(panel)

        seen = set()
        for f in mine:
            step = f.get("step")
            if step in (HUMAN, PRECOND, LISTEN):
                continue
            seen.add(step)
            before = len(rows)
            judge_step(panel, step, f, pending)
            if step in WINDOW_STEPS and len(rows) > before:
                windows.append((panel, f, rows[-1]))
        missing_steps(panel, seen, waits, pending)
    judge_windows()

    fin = [f for f in lines if f.get("panel") == "plan" and f.get("step") == "fin"]
    if not fin:
        add("FAIL", "plan", "fin", "FALTA: la corrida no termino (o no llego al techo de espera)")
    else:
        ok = fin[0].get("ok") == "true"
        # `fin` agrega los paneles: si cada panel que la app dio por fallido lo explica una espera
        # humana pendiente o sus propios BLOQUEADO (sin ningun FAIL), el veredicto es ese, no un FAIL mas.
        failed = [p for p in fin[0].get("fallidos", "-").split(",") if p and p != "-"]
        with_fail = {r["panel"] for r in rows if r["veredicto"] == "FAIL"}
        with_block = {r["panel"] for r in rows if r["veredicto"] == "BLOQUEADO"} - with_fail
        explained = bool(failed) and all(p in human_pending or p in with_block for p in failed)
        if ok:
            add("PASS", "plan", "fin", extras(fin[0]), observed(fin[0]))
        elif explained and fin[0].get("motor-detenido") == "true":
            add("BLOQUEADO" if any(p in with_block for p in failed) else "HUMANO", "plan", "fin", extras(fin[0]), observed(fin[0]))
        else:
            add("FAIL", "plan", "fin", extras(fin[0]), observed(fin[0]))

    # --- La salida. --------------------------------------------------------------------------
    if preconditions:
        print("precondiciones (ficha %s):" % setup_path)
        for r in preconditions:
            print("  %-16s %s/%s (%s)  %s" % (r["estado"], r["plan"], r["id"], r["verificador"], r["evidencia"]))
            if r["estado"] in ("incumplida", "no-verificable"):
                print("  %-16s remedio: %s" % ("", r["remedio"]))
        print()
    width = max(len("%s/%s" % (r["panel"], r["paso"])) for r in rows)
    for r in rows:
        if r["veredicto"] != "NO-APLICA":
            print("%-9s  %-*s  %s" % (r["veredicto"], width, "%s/%s" % (r["panel"], r["paso"]), r["detalle"]))
    not_applicable = [r for r in rows if r["veredicto"] == "NO-APLICA"]
    if not_applicable:
        print("\nNO-APLICA (el device no ofrece la config de la fila: no es PASS ni FAIL y NO cuenta como cobertura):")
        for r in not_applicable:
            print("%-9s  %-*s  %s" % (r["veredicto"], width, "%s/%s" % (r["panel"], r["paso"]), r["detalle"]))

    n = {k: sum(1 for r in rows if r["veredicto"] == k) for k in ("PASS", "FAIL", "BLOQUEADO", "HUMANO", "NO-APLICA")}
    print("\nresumen: %d PASS · %d FAIL · %d BLOQUEADO · %d HUMANO · %d NO-APLICA"
          % (n["PASS"], n["FAIL"], n["BLOQUEADO"], n["HUMANO"], n["NO-APLICA"]))
    # S3: un juicio de sensor sin dato (?, sin respuesta, sin TTY, --sensor ninguno) es HUMANO pendiente.
    pending = [r for r in rows if r["veredicto"] == "HUMANO"
               and (r["panel"] in human_pending or r["panel"] == "plan" or r["paso"].startswith("sensor-"))]
    # AC-053.4: 1 FAIL > 4 BLOQUEADO > 3 HUMANO > 0.
    finish(1 if n["FAIL"] else (4 if n["BLOQUEADO"] else (3 if pending else 0)))


# --- REQ-053 S3: los sensores (D6) y el seguidor de ventanas ---------------------------------------
# Un SENSOR es una funcion con el contrato de D6: recibe {estimulo, ventana, ruta} (y cuanto puede
# esperar) y devuelve (veredicto, evidencia), con veredicto en SENSOR_VERDICTS. No sabe si la ventana
# es estimulo o control: se le pregunta lo MISMO en las dos (D7, controles ciegos). `consult` le
# agrega su nombre. REQ-051 enchufa el loopback agregando su funcion a SENSORS; el juez no cambia.
ROUTES = {
    "sistema": "la salida del sistema del telefono (Oboe: parlante o auriculares del telefono)",
    "libusb": "la placa USB, por libusb (en los auriculares enchufados a la placa)",
}
STIMULI = {"A4-440Hz": "un A4 (la de 440 Hz)"}


def sensor_oido_humano(estimulo, ventana, ruta, timeout_s):
    """El oido humano: una pregunta cerrada en la terminal de quien corre el script (s/n/?). Sin TTY,
    sin respuesta antes de `timeout_s` o con `?`, no sabe: sin-dato (=> HUMANO, nunca PASS)."""
    if not os.isatty(0):
        return "sin-dato", "sin-tty"
    try:
        import termios
        termios.tcflush(0, termios.TCIFLUSH)   # lo tipeado ANTES de la pregunta no la contesta
    except (ImportError, OSError):
        pass
    question = ">>> ¿Sonó %s por %s en la ventana %s? [s/n/?] " % (
        STIMULI.get(estimulo, estimulo), ROUTES.get(ruta, ruta), ventana)
    deadline = time.monotonic() + timeout_s
    while True:
        left = deadline - time.monotonic()
        if left <= 0:
            break
        sys.stdout.write(question)
        sys.stdout.flush()
        ready, _, _ = select.select([0], [], [], left)
        if not ready:
            break
        data = os.read(0, 4096)
        if not data:
            return "sin-dato", "eof"
        answer = data.decode("utf-8", "replace").strip().lower()
        if answer in ("s", "si", "sí"):
            return "presente", "respuesta:s"
        if answer in ("n", "no"):
            return "ausente", "respuesta:n"
        if answer == "?":
            return "sin-dato", "respuesta:?"
        print("    (contestá s, n o ?)")
    print()
    return "sin-dato", "sin-respuesta:%ds" % timeout_s


def sensor_ninguno(estimulo, ventana, ruta, timeout_s):
    """`--sensor ninguno`: corre sin nadie que juzgue. Todo queda HUMANO (D6)."""
    return "sin-dato", "--sensor-ninguno"


def sensor_guion(estimulo, ventana, ruta, timeout_s):
    """Solo del --self-test (el CLI no lo deja elegir): contesta en orden lo que dice el archivo
    WMA_SENSOR_GUION, anota cada consulta en WMA_SENSOR_LLAMADAS y, si se le pide, agrega una linea al
    log MIENTRAS contesta (WMA_SENSOR_AL_CONTESTAR en WMA_SENSOR_AL_CONTESTAR_EN): asi se prueba la
    respuesta que llega tarde sin relojes."""
    calls_path = os.environ["WMA_SENSOR_LLAMADAS"]
    with open(calls_path, encoding="utf-8") as f:
        done = sum(1 for line in f if line.strip())
    with open(calls_path, "a", encoding="utf-8") as f:
        f.write("estimulo=%s ventana=%s ruta=%s espera-s=%d\n" % (estimulo, ventana, ruta, timeout_s))
    late = os.environ.get("WMA_SENSOR_AL_CONTESTAR")
    if late:
        with open(os.environ["WMA_SENSOR_AL_CONTESTAR_EN"], "a", encoding="utf-8") as f:
            f.write(late + "\n")
    with open(os.environ["WMA_SENSOR_GUION"], encoding="utf-8") as f:
        answers = [line.strip() for line in f if line.strip()]
    return (answers[done] if done < len(answers) else "sin-dato"), "guion:%d" % (done + 1)


SENSORS = {"oido-humano": sensor_oido_humano, "ninguno": sensor_ninguno, "guion": sensor_guion}
# Los que se pueden elegir con --sensor en una corrida de device.
SENSORS_CLI = ("oido-humano", "ninguno")


def consult(name, estimulo, ventana, ruta, timeout_s):
    """{estimulo, ventana, ruta} -> {veredicto, sensor, evidencia} (D6). Un veredicto fuera del
    contrato es sin-dato: un sensor que dice algo que no se entiende no sabe."""
    verdict, evidence = SENSORS[name](estimulo, ventana, ruta, timeout_s)
    if verdict not in SENSOR_VERDICTS:
        verdict, evidence = "sin-dato", "veredicto-ilegible:%s" % value(verdict)
    return {"veredicto": verdict, "sensor": name, "evidencia": value(evidence)}


def parse_line(raw):
    """Los campos de una linea HARNESS-SMOKE, o None si no es una."""
    i = raw.find("HARNESS-SMOKE ")
    if i < 0:
        return None
    fields = {}
    for part in raw[i:].replace("\r", "").strip().split(" ")[1:]:
        if "=" in part:
            k, v = part.split("=", 1)
            fields[k] = v
    return fields


def app_events(path, run):
    """Las lineas de la APP de esta corrida, en el orden del log (sin las del host ni las del sensor)."""
    out = []
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for raw in f:
                fields = parse_line(raw)
                if (fields is not None and fields.get("run") == run and fields.get("verificador") != "host"
                        and fields.get("step") != SENSOR_STEP):
                    out.append(fields)
    except FileNotFoundError:
        pass
    return out


def follow(log, sensor_log, state_path, sensor, run):
    """El seguidor de ventanas, en vivo: lo llama el bucle de la corrida en cada vuelta, sobre la
    captura cruda. La APP anuncia cada ventana (`escuchar`) y la cierra (`estimulo`/`control`); el
    script la sigue — no hay relojes que sincronizar. Al ver un aviso, se lo dice al humano; al ver un
    cierre, consulta al sensor y anota el juicio en su propio registro.

    Lo que se decide por el ORDEN del log, sin relojes:
    - un cierre con el motor sin rendir la ventana NO se consulta (AC-053.8);
    - un aviso que se ve cuando su ventana ya cerro no dejo escuchar: no se consulta, sin-dato;
    - una respuesta que llega cuando la app ya aviso la ventana siguiente no vale: sin-dato.
    Cada ventana se procesa una vez (el estado vive en `state_path`), y se marca ANTES de consultar:
    si el seguidor se cae a mitad, esa ventana queda sin juicio (HUMANO), nunca con dos."""
    try:
        with open(state_path, encoding="utf-8") as f:
            state = json.load(f)
    except FileNotFoundError:
        state = {"anunciadas": [], "tarde": [], "procesadas": []}

    def save():
        with open(state_path, "w", encoding="utf-8") as f:
            json.dump(state, f)

    def record(f, res, estimulo, ruta):
        with open(sensor_log, "a", encoding="utf-8") as out:
            out.write("HARNESS-SMOKE v=1 run=%s panel=%s step=%s ok=true n=%s sensor=%s veredicto=%s evidencia=%s "
                      "estimulo=%s ruta=%s\n" % (run, f.get("panel"), SENSOR_STEP, f.get("n"), res["sensor"],
                                                 res["veredicto"], res["evidencia"], value(estimulo), value(ruta)))

    events = app_events(log, run)
    key = lambda f: "%s/%s" % (f.get("panel"), f.get("n"))
    closed = {key(f) for f in events if f.get("step") in WINDOW_STEPS}
    listens = {key(f): f for f in events if f.get("step") == LISTEN}
    for i, f in enumerate(events):
        k = key(f)
        if f.get("step") == LISTEN and k not in state["anunciadas"]:
            state["anunciadas"].append(k)
            if k in closed:
                state["tarde"].append(k)
                print("(ventana %s de %s, %s: el aviso llego cuando ya habia cerrado; no se pregunta)"
                      % (f.get("n"), f.get("de"), f.get("panel")))
            else:
                print(">>> ESCUCHÁ — ventana %s de %s (%s): arranca en ~%s s y dura %s s, por %s."
                      % (f.get("n"), f.get("de"), f.get("panel"), int(number(f.get("en-ms")) or 0) // 1000,
                         int(number(f.get("ventana-ms")) or 0) // 1000, ROUTES.get(f.get("ruta"), f.get("ruta"))))
            sys.stdout.flush()
            save()
        elif f.get("step") in WINDOW_STEPS and k not in state["procesadas"]:
            state["procesadas"].append(k)
            save()
            if not window_rendered(f):
                print("(ventana %s de %s: el motor no la rindio como se declaro; no se pregunta)" % (f.get("n"), f.get("panel")))
                continue
            listen = listens.get(k)
            if listen is None:
                record(f, {"veredicto": "sin-dato", "sensor": sensor, "evidencia": "sin-aviso"}, "-", f.get("ruta"))
                continue
            estimulo, ruta = listen.get("estimulo", "-"), listen.get("ruta", "-")
            if k in state["tarde"]:
                record(f, {"veredicto": "sin-dato", "sensor": sensor, "evidencia": "aviso-tarde"}, estimulo, ruta)
                continue
            pause_ms = number(f.get("pausa-ms")) or 0
            timeout_s = max(1, int(pause_ms // 1000) - ANSWER_MARGIN_S)
            ventana = "%s-%sde%s-%sms" % (f.get("panel"), f.get("n"), f.get("de"), f.get("ventana-ms"))
            res = consult(sensor, estimulo, ventana, ruta, timeout_s)
            # Releer: si mientras el sensor contestaba la app ya aviso OTRA ventana, la respuesta
            # pudo mezclar las dos. No vale.
            now = app_events(log, run)
            pos = next((j for j, g in enumerate(now) if g.get("step") in WINDOW_STEPS and key(g) == k), None)
            if pos is not None and any(g.get("step") == LISTEN for g in now[pos + 1:]):
                res = {"veredicto": "sin-dato", "sensor": res["sensor"],
                       "evidencia": "respuesta-tardia:%s" % res["veredicto"]}
            record(f, res, estimulo, ruta)


def veredicto_args(args):
    """veredicto LOG RUN PLAN FICHA [--json F] [--host-log F] [--host-en-log] [--sensor-log F] [--semilla N]"""
    pos, opt, flags = [], {}, set()
    it = iter(args)
    for a in it:
        if a in ("--json", "--host-log", "--sensor-log", "--semilla"):
            v = next(it, None)
            if v is None:
                raise ValueError("%s necesita un valor" % a)
            opt[a] = v
        elif a == "--host-en-log":
            flags.add(a)
        elif a.startswith("--"):
            raise ValueError("opcion desconocida: %s" % a)
        else:
            pos.append(a)
    if len(pos) != 4:
        raise ValueError("se esperan LOG RUN PLAN FICHA")
    seed = opt.get("--semilla")
    if seed is not None and not re.fullmatch(r"[0-9]+", seed):
        raise ValueError("--semilla tiene que ser un entero >= 0: %s" % seed)
    return pos, opt, flags, (int(seed) if seed is not None else None)


def main(argv):
    mode, args = (argv[0], argv[1:]) if argv else ("", [])
    try:
        if mode == "veredicto":
            try:
                pos, opt, flags, seed = veredicto_args(args)
            except ValueError as e:
                print("smoke_py veredicto: %s" % e, file=sys.stderr)
                sys.exit(2)
            judge(pos[0], pos[1], pos[2], pos[3], opt.get("--json") or None, opt.get("--host-log") or None,
                  host_in_log="--host-en-log" in flags, sensor_log=opt.get("--sensor-log") or None, cli_seed=seed)
        elif mode == "orden" and len(args) == 2 and re.fullmatch(r"[0-9]+", args[0]) and args[1] in AUDIBLE:
            print(" ".join(window_order(int(args[0]), args[1])))
        elif mode == "seguir" and len(args) == 5 and args[3] in SENSORS:
            follow(*args)
        elif mode == "sensor" and len(args) == 5 and re.fullmatch(r"[0-9]+", args[4]):
            if args[0] not in SENSORS_CLI:
                print("sensor desconocido: %s (conocidos: %s)" % (args[0], ", ".join(SENSORS_CLI)), file=sys.stderr)
                sys.exit(2)
            res = consult(args[0], args[1], args[2], args[3], int(args[4]))
            print("veredicto=%s sensor=%s evidencia=%s" % (res["veredicto"], res["sensor"], res["evidencia"]))
        elif mode == "host" and len(args) in (6, 7):
            host(*args[:6], evidence_dir=args[6] if len(args) == 7 else None)
        elif mode == "validar" and len(args) == 1:
            setup = load_setup(args[0])
            print("ficha valida: %d precondiciones en %d planes"
                  % (sum(len(v) for v in setup.values()), len(setup)))
        elif mode == "ids" and len(args) == 1:
            for panel, pres in load_setup(args[0]).items():
                for p in pres:
                    print("%s %s %s" % (panel, p["id"], p["verificador"]))
        else:
            print("smoke_py: modo o argumentos invalidos: %s" % " ".join(argv), file=sys.stderr)
            sys.exit(2)
    except BadSetup as e:
        print("ERROR — ficha de setup invalida: %s" % e)
        sys.exit(2)


main(sys.argv[1:])
PY
# `python3 -c` y no `python3 - <<PY`: con el heredoc, el stdin de python ERA el programa, y el sensor
# oido-humano (S3) necesita el stdin de quien llama, que es la TTY del humano.
smoke_py() {
    python3 -c "$SMOKE_PY" "$@"
}

# El juez. verdict LOG RUN PLAN [FICHA] [JSON] [LOG-DEL-HOST] [LOG-DEL-SENSOR] [opciones...] — ver
# smoke_py. LOG es el de la APP: una linea `verificador=host` o `step=sensor` que traiga se descarta;
# las del host vienen en LOG-DEL-HOST y los juicios del sensor (S3) en LOG-DEL-SENSOR. Las opciones
# (`--semilla N`, y `--host-en-log`, que es SOLO del CLI --veredicto) pasan tal cual.
verdict() {
    smoke_py veredicto "$1" "$2" "$3" "${4:-$SETUP_DEFAULT}" ${5:+--json "$5"} ${6:+--host-log "$6"} \
        ${7:+--sensor-log "$7"} "${@:8}"
}

# Un id por corrida que otra app no pueda adivinar: fecha, pid y 48 bits de /dev/urandom.
new_run_id() {
    local rnd
    rnd="$(od -An -N6 -tx1 /dev/urandom | tr -d ' \n')"
    [[ "$rnd" =~ ^[0-9a-f]{12}$ ]] || { echo "FAIL — no pude leer /dev/urandom" >&2; return 1; }
    echo "smoke-$(date +%Y%m%d-%H%M%S)-$$-$rnd"
}

# REQ-053 S3: la semilla del orden de las ventanas (AC-053.10). 31 bits de /dev/urandom: entra en
# un Int de Kotlin sin signo. Va por extra a la app y queda en el JSON; con ella el orden se reproduce.
new_seed() {
    local n
    n="$(od -An -N4 -tu4 /dev/urandom | tr -d ' \n')"
    [[ "$n" =~ ^[0-9]+$ ]] || { echo "FAIL — no pude leer /dev/urandom" >&2; return 1; }
    echo $(( n % 2147483648 ))
}

# Los sensores que se pueden elegir con --sensor (D6). El registro vive en smoke_py (SENSORS); estos
# son los que corren en device. REQ-051 agrega el suyo en los dos lugares.
sensor_valid() {
    case "$1" in oido-humano|ninguno) return 0 ;; *) return 1 ;; esac
}

# Cuantos paneles audibles (sf2, sf3, usb) tiene el plan: cada uno toca dos ventanas.
audible_panels() {
    local plan="$1" p n=0
    [[ "$plan" == todo ]] && plan="salida,captura,sf2,sf3,usb"
    local IFS=,
    for p in $plan; do
        case "$p" in sf2|sf3|usb) n=$((n + 1)) ;; esac
    done
    echo "$n"
}

# --plan viaja por el `sh` del telefono (am start --es): solo `todo` o paneles conocidos separados
# por comas, y nada mas.
plan_valid() {
    local plan="$1" p
    [[ "$plan" =~ ^[a-z0-9,]+$ ]] || return 1
    # El word-splitting de `for p in $plan` se come la coma final: un elemento vacio se rechaza aca.
    [[ "$plan" != ,* && "$plan" != *, && "$plan" != *,,* ]] || return 1
    [[ "$plan" == todo ]] && return 0
    local IFS=,
    for p in $plan; do
        case "$p" in salida|captura|sf2|sf3|usb) ;; *) return 1 ;; esac
    done
}

# uid_of_package <paquete> < salida de `pm list packages -U`: el uid del paquete EXACTO (el listado
# filtra por subcadena y trae paquetes vecinos).
uid_of_package() {
    tr -d '\r' | awk -v p="package:$1" '$1 == p && $2 ~ /^uid:[0-9]+$/ {sub(/^uid:/, "", $2); print $2; exit}'
}

# Los argumentos de `adb logcat` de la captura. Con uid, solo lo que escribio el harness.
logcat_capture_args() {
    local uid="${1:-}"
    echo "-v raw ${uid:+--uid=$uid }-s HARNESS-SMOKE:I"
}

# verdict_split LOG RUN PLAN FICHA [JSON]: LOG trae mezcladas las lineas de la app, las del host y
# los juicios del sensor (como los arman los casos del self-test); las separa para el juez.
verdict_split() {
    local d
    d="$(mktemp -d)"
    { grep -v -e 'verificador=host' -e ' step=sensor ' "$1" || true; } > "$d/app.log"
    { grep 'verificador=host' "$1" || true; } > "$d/host.log"
    { grep ' step=sensor ' "$1" || true; } > "$d/sensor.log"
    verdict "$d/app.log" "$2" "$3" "${4:-$SETUP_DEFAULT}" "${5:-}" "$d/host.log" "$d/sensor.log"
}

# REQ-053 S3 (AC-053.11): la vieja lista de OIDO ya no existe. Lo que tenia es un paso con estimulo y
# sensor (sf2/sf3/usb), o se borro con su razon (ver la cabecera). Lo unico que queda fuera del plan
# es un gesto de MANO, no de oido.
print_manual_checks() {
    cat <<'EOF'

Fuera del plan automatico (MANO, no oido):
  - "elegir archivo (fd)" en el panel SoundFont: el selector del sistema, que carga por
    loadSoundFontFromFd.
EOF
}

# ---------------------------------------------------------------------------
# --self-test: el juez tiene que poder decir que NO. Sobre un log grabado de una corrida real en
# device (scripts/smoke-device-selftest.txt), y sobre mutantes de ese log.
#
# REQ-053 S1: los casos de las precondiciones corren con una ficha DE PRUEBA (abajo), no con la
# real: la real la cambian S2-S4 y estos casos fijan las REGLAS del juez, no los datos. La real se
# valida aparte, se cruza con el script (ningun id escrito aca) y con el harness (cada id de app
# lo emite la app), y juzga el control.
# ---------------------------------------------------------------------------
self_test() {
    local tmp
    tmp="$(mktemp -d)"
    [[ -f "$SELFTEST_LOG" ]] || { echo "self-test: FAIL — falta $SELFTEST_LOG" >&2; return 1; }
    [[ -f "$SETUP_DEFAULT" ]] || { echo "self-test: FAIL — falta $SETUP_DEFAULT" >&2; return 1; }
    local run
    run="$(sed -nE 's/.*HARNESS-SMOKE v=1 run=([^ ]+) panel=plan step=inicio .*/\1/p' "$SELFTEST_LOG" | head -1)"
    [[ -n "$run" ]] || { echo "self-test: FAIL — el log grabado no tiene 'plan inicio'" >&2; return 1; }

    local real="$SETUP_DEFAULT" ficha="$tmp/ficha.json" vacia="$tmp/vacia.json"
    echo '{"formato": 1, "planes": {}}' > "$vacia"
    # La ficha de prueba: una precondicion de host por cada chequeo que conoce el script, y dos de
    # app (una con ventana humana, D13). Los remedios son marcas unicas para poder buscarlas.
    cat > "$ficha" <<'JSON'
{
  "formato": 1,
  "planes": {
    "captura": {"precondiciones": [
      {"id": "t-host-cap", "verificador": "host", "chequeo": "permiso-runtime",
       "permiso": "android.permission.RECORD_AUDIO", "depende": ["nivel"], "remedio": "REMEDIO-T-HOST-CAP"},
      {"id": "t-app-cap", "verificador": "app", "depende": ["start"], "remedio": "REMEDIO-T-APP-CAP"}
    ]},
    "usb": {"precondiciones": [
      {"id": "t-usb-clase", "verificador": "host", "chequeo": "usb-interfaz-de-clase", "clase": 1,
       "depende": ["dispositivos"], "remedio": "REMEDIO-T-USB-CLASE"},
      {"id": "t-host-usb", "verificador": "host", "chequeo": "paquetes-sin-proceso",
       "paquetes": ["com.example.ajena"],
       "depende": ["conectar", "motor-callback", "capacidades", "descriptores", "backend", "wake-lock",
                   "streaming-start", "streaming-stats", "reconectar-mismo", "conectar-otro", "suite*",
                   "streaming-stop", "estimulo", "control", "backend-restaurado", "desconectar"],
       "remedio": "REMEDIO-T-HOST-USB"},
      {"id": "t-alsa", "verificador": "host", "chequeo": "alsa-tarjeta-libre", "tarjeta": 1,
       "depende": ["streaming-start"], "remedio": "REMEDIO-T-ALSA"},
      {"id": "t-reclamada", "verificador": "host", "chequeo": "usb-placa-no-reclamada", "tarjeta": 1, "clase": 1,
       "depende": ["streaming-start"], "remedio": "REMEDIO-T-RECLAMADA"},
      {"id": "t-permiso", "verificador": "app", "ventana-humana": true,
       "depende": ["permiso", "permiso-falso", "conectar", "motor-callback", "capacidades", "descriptores",
                   "backend", "wake-lock", "streaming-start", "streaming-stats", "reconectar-mismo",
                   "conectar-otro", "suite*", "streaming-stop", "estimulo", "control", "backend-restaurado",
                   "desconectar"],
       "remedio": "REMEDIO-T-PERMISO"}
    ]}
  }
}
JSON

    local failures=0
    # judged <archivo> <plan> <ficha>: corre el juez y deja su salida en $tmp/out y su exit en
    # $tmp/out.rc. Cacheado por CONTENIDO (log + ficha + plan): varios casos miran la misma corrida.
    judged() {
        local key
        key="$({ cat "$1" "$3" 2>/dev/null; printf '|%s|%s' "$2" "$3"; } | shasum | cut -c1-16)"
        if [[ ! -f "$tmp/juez-$key.out" ]]; then
            local rc=0
            verdict_split "$1" "$run" "$2" "$3" > "$tmp/juez-$key.out" 2>&1 || rc=$?
            echo "$rc" > "$tmp/juez-$key.rc"
        fi
        cp "$tmp/juez-$key.out" "$tmp/out"
        cat "$tmp/juez-$key.rc"
    }
    expect() {  # expect <nombre> <exit esperado> <archivo> [plan pedido] [ficha]
        local name="$1" want="$2" file="$3" plan="${4:-todo}" setup="${5:-$ficha}" got
        got="$(judged "$file" "$plan" "$setup")"
        if [[ "$got" == "$want" ]]; then
            printf '  ok    %-58s exit %s\n' "$name" "$got"
        else
            printf '  MAL   %-58s exit %s, esperaba %s\n' "$name" "$got" "$want"
            sed 's/^/        /' "$tmp/out" | tail -8
            failures=$((failures + 1))
        fi
    }

    expect_line() {  # expect_line <nombre> <regex que TIENE que aparecer> <archivo> [plan] [ficha]
        local name="$1" re="$2" file="$3" plan="${4:-todo}" setup="${5:-$ficha}"
        judged "$file" "$plan" "$setup" > /dev/null
        if grep -Eq -- "$re" "$tmp/out"; then
            printf '  ok    %-58s\n' "$name"
        else
            printf '  MAL   %-58s falta /%s/\n' "$name" "$re"
            failures=$((failures + 1))
        fi
    }
    expect_no_line() {  # expect_no_line <nombre> <regex que NO puede aparecer> <archivo> [plan] [ficha]
        local name="$1" re="$2" file="$3" plan="${4:-todo}" setup="${5:-$ficha}"
        judged "$file" "$plan" "$setup" > /dev/null
        if grep -Eq -- "$re" "$tmp/out"; then
            printf '  MAL   %-58s aparece /%s/\n' "$name" "$re"
            failures=$((failures + 1))
        else
            printf '  ok    %-58s\n' "$name"
        fi
    }
    # expect_json <nombre> <archivo> <plan> <expresion python sobre j, el JSON de la corrida>
    expect_json() {  # el quinto argumento, opcional, es otra ficha
        local name="$1" file="$2" plan="$3" expr="$4" setup="${5:-$ficha}"
        rm -f "$tmp/corrida.json"
        verdict_split "$file" "$run" "$plan" "$setup" "$tmp/corrida.json" > "$tmp/out" 2>&1 || true
        if python3 -c 'import json,sys; j=json.load(open(sys.argv[1])); sys.exit(0 if eval(sys.argv[2]) else 1)' \
                "$tmp/corrida.json" "$expr" 2>"$tmp/json-err"; then
            printf '  ok    %-58s\n' "$name"
        else
            printf '  MAL   %-58s el JSON no cumple: %s\n' "$name" "$expr"
            sed 's/^/        /' "$tmp/json-err" | tail -3
            failures=$((failures + 1))
        fi
    }

    # pre <panel> <id> <cumplida> [host] — una linea de precondicion como la emite la app (o el
    # host, con el cuarto argumento: firma `verificador=host`).
    pre() {
        local ok=false
        [[ "$3" == true ]] && ok=true
        echo "HARNESS-SMOKE v=1 run=$run panel=$1 step=precondicion ok=$ok id=$2 cumplida=$3 evidencia=prueba-$2${4:+ verificador=host}"
    }
    # before_fin <origen> <destino>: agrega stdin al origen, antes de `plan fin`.
    before_fin() {
        { grep -v 'panel=plan step=fin ' "$1" || true; cat; grep 'panel=plan step=fin ' "$1" || true; } > "$2"
    }
    # set_pre <id> <cumplida>: el sed que cambia el resultado de esa precondicion.
    set_pre() {
        local ok=false
        [[ "$2" == true ]] && ok=true
        echo "/ id=$1 /s/ ok=[a-z]+ / ok=$ok /; / id=$1 /s/ cumplida=[^ ]+ / cumplida=$2 /"
    }
    met_captura() { pre captura t-host-cap true host; pre captura t-app-cap true; }
    met_usb_host() { pre usb t-usb-clase true host; pre usb t-host-usb true host; pre usb t-alsa true host; pre usb t-reclamada true host; }

    # --- REQ-053 S3: las ventanas de escucha y lo que registro el sensor. -----------------------
    # El control grabado (01/10) es ANTERIOR a S3: no tiene ventanas, ni semilla, ni sensor. Lo que
    # S3 agrega se INYECTA con estos helpers, como mutantes declarados: no es una corrida de device.
    # La semilla de prueba fija el orden de cada panel (ver los vectores de abajo): con 12345, sf2 va
    # estimulo-control y sf3 y usb van control-estimulo.
    local SEM=12345 ord_sf2 ord_sf3 ord_usb
    ord_sf2="$(smoke_py orden "$SEM" sf2)"; ord_sf3="$(smoke_py orden "$SEM" sf3)"; ord_usb="$(smoke_py orden "$SEM" usb)"
    # ord <panel> [semilla]: el orden de ese panel (cacheado para la semilla de prueba).
    ord() {
        if [[ "${2:-$SEM}" != "$SEM" ]]; then smoke_py orden "$2" "$1"; return; fi
        case "$1" in sf2) echo "$ord_sf2" ;; sf3) echo "$ord_sf3" ;; usb) echo "$ord_usb" ;; esac
    }
    # win <panel> [ruta] [semilla]: el aviso y el cierre de cada ventana, como los emite la app.
    win() {
        local panel="$1" ruta="${2:-sistema}" seed="${3:-$SEM}" k=0 kind backend=OBOE
        [[ "$ruta" == libusb ]] && backend=LIBUSB
        for kind in $(ord "$panel" "$seed"); do
            k=$((k + 1))
            echo "HARNESS-SMOKE v=1 run=$run panel=$panel step=escuchar ok=true n=$k de=2 estimulo=A4-440Hz ruta=$ruta en-ms=4000 ventana-ms=2000"
            if [[ "$kind" == estimulo ]]; then
                echo "HARNESS-SMOKE v=1 run=$run panel=$panel step=estimulo ok=true n=$k de=2 sono=A4 hz=440 nota=69 archivo=wma-fixture.sf2 preset=0 ruta=$ruta backend=$backend ventana-ms=2000 frames=96000 pico=0.2100 pausa-ms=12000"
            else
                echo "HARNESS-SMOKE v=1 run=$run panel=$panel step=control ok=true n=$k de=2 tipo=silencio ruta=$ruta backend=$backend ventana-ms=2000 frames=96000 pico=0.0000 pausa-ms=12000"
            fi
        done
    }
    # sens <panel> [veredicto en el estimulo] [veredicto en el control] [sensor]: el registro del
    # sensor (lo escribe el SCRIPT, en su propio archivo: verdict_split lo separa del log de la app).
    sens() {
        local panel="$1" ve="${2:-presente}" vc="${3:-ausente}" sensor="${4:-oido-humano}" k=0 kind v
        for kind in $(ord "$panel"); do
            k=$((k + 1)); v="$vc"; [[ "$kind" == estimulo ]] && v="$ve"
            echo "HARNESS-SMOKE v=1 run=$run panel=$panel step=sensor ok=true n=$k sensor=$sensor veredicto=$v evidencia=prueba-$panel-$k estimulo=A4-440Hz ruta=x"
        done
    }
    # n_of <panel> <estimulo|control>: el numero de esa ventana con la semilla de prueba.
    n_of() { ord "$1" | tr ' ' '\n' | grep -n -x "$2" | cut -d: -f1; }
    # El usb de S3 carga el fixture .sf2 antes de parar el motor y lo descarga al final.
    usb_font_pre() {
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=fixture ok=true archivo=wma-fixture.sf2"
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=carga ok=true archivo=wma-fixture.sf2 cargado=true presets=1"
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=preset ok=true indice=0"
    }
    usb_font_post() { echo "HARNESS-SMOKE v=1 run=$run panel=usb step=descarga ok=true queda-cargado=false"; }
    with_seed() { sed -E "s/(panel=plan step=inicio ok=true plan=[^ ]*)/\1 semilla=$SEM semilla-origen=script/"; }

    # El control: el log grabado tal cual, con una ficha SIN precondiciones. Es anterior a S3, asi
    # que el juez de S3 le pide las ventanas y no las encuentra: FALTA (exit 1). Con las ventanas,
    # la semilla y el sensor inyectados, vuelve al exit que se grabo (ver la cabecera del log).
    local want_base
    want_base="$(sed -nE 's/^# exit-esperado: ([0-9]+).*/\1/p' "$SELFTEST_LOG" | head -1)"
    [[ -n "$want_base" ]] || { echo "self-test: FAIL — el log grabado no declara '# exit-esperado:'" >&2; return 1; }
    expect "control: el log grabado (anterior a S3) sin ventanas = FALTA" 1 "$SELFTEST_LOG" todo "$vacia"
    expect_line "control: sf2/estimulo sale FALTA" '^FAIL +sf2/estimulo +FALTA' "$SELFTEST_LOG" todo "$vacia"
    expect_line "control: usb/control sale FALTA" '^FAIL +usb/control +FALTA' "$SELFTEST_LOG" todo "$vacia"
    {
        win sf2; win sf3; usb_font_pre; win usb libusb; usb_font_post
        sens sf2; sens sf3; sens usb
    } | before_fin "$SELFTEST_LOG" "$tmp/control-s30.log"
    with_seed < "$tmp/control-s30.log" > "$tmp/control-s3.log"
    expect "control + S3 inyectado, ficha sin precondiciones = el exit grabado" "$want_base" "$tmp/control-s3.log" todo "$vacia"

    # Una version del control donde todo lo automatico pasa y el humano ya actuo: exit 0. Se arma
    # quitando del log los pasos USB (queda pendiente) — ver abajo — asi que primero el caso verde
    # sin USB: se recorta el plan a los paneles automaticos. Lleva las ventanas de S3 con el sensor
    # contestando bien, y las precondiciones de captura CUMPLIDAS: es el gemelo de todos los casos
    # de BLOQUEADO (cumplida no bloquea nada) y de los del sensor.
    sed -E "/panel=usb /d; s/(panel=plan step=inicio ok=true plan=)[^ ]*/\1salida,captura,sf2,sf3/; \
            s/(panel=plan step=fin )ok=[a-z]+ fallidos=[^ ]*/\1ok=true fallidos=-/" \
        "$SELFTEST_LOG" | with_seed > "$tmp/verde00.log"
    { win sf2; win sf3; sens sf2; sens sf3; } | before_fin "$tmp/verde00.log" "$tmp/verde0.log"
    met_captura | before_fin "$tmp/verde0.log" "$tmp/verde.log"
    local auto=salida,captura,sf2,sf3
    expect "verde: sin usb, todo lo automatico" 0 "$tmp/verde.log" "$auto"

    # M1: UN paso con ok=false da rojo.
    sed -E 's/panel=sf3 step=nota ok=true/panel=sf3 step=nota ok=false/' "$tmp/verde.log" > "$tmp/m1.log"
    expect "M1: sf3/nota con ok=false" 1 "$tmp/m1.log" "$auto"

    # M2: UN paso faltante da rojo.
    grep -v 'panel=salida step=frames ' "$tmp/verde.log" > "$tmp/m2.log"
    expect "M2: falta salida/frames" 1 "$tmp/m2.log" "$auto"

    # M3: sin `fin` la corrida no termino: rojo, aunque todo lo demas pase.
    grep -v 'panel=plan step=fin ' "$tmp/verde.log" > "$tmp/m3.log"
    expect "M3: falta plan/fin" 1 "$tmp/m3.log" "$auto"

    # M4: el humano no contesto — el USB queda HUMANO, no PASS ni FAIL: exit 3. Las precondiciones
    # de host se cumplen; la del permiso (app, con ventana humana) NO tiene linea: D13.
    {
        grep -v 'panel=plan step=fin ' "$tmp/verde.log"
        met_usb_host
        usb_font_pre
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=motor-parado ok=true"
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=dispositivos ok=true cantidad=1"
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=motor-callback ok=true inicializado=true"
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=esperando-humano ok=false accion=aceptar_el_dialogo"
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=broadcast-falso ok=true origen=adb enviados=2"
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=permiso-falso ok=true resultado=sin-respuesta-humana"
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=conectar ok=false motivo=sin-respuesta-humana"
        usb_font_post
        echo "HARNESS-SMOKE v=1 run=$run panel=plan step=fin ok=false fallidos=usb motor-detenido=true"
    } | sed -E "s/(panel=plan step=inicio ok=true plan=)[^ ]*/\1salida,captura,sf2,sf3,usb/" > "$tmp/m4.log"
    expect "M4: usb esperando al humano" 3 "$tmp/m4.log"

    # M5: lineas de OTRA corrida no cuentan: con otro run id no hay nada que juzgar.
    sed -E "s/run=$run /run=otra-corrida /" "$tmp/verde.log" > "$tmp/m5.log"
    expect "M5: el run id es de otra corrida" 1 "$tmp/m5.log" "$auto"

    # M6: un paso con ok=false que NO esta en la lista esperada (p.ej. una excepcion) tambien es rojo.
    awk -v run="$run" '/panel=plan step=fin /{print "HARNESS-SMOKE v=1 run=" run " panel=sf2 step=excepcion ok=false error=boom"} {print}' \
        "$tmp/verde.log" > "$tmp/m6.log"
    expect "M6: un paso inesperado con ok=false" 1 "$tmp/m6.log" "$auto"

    # Un USB completo y sano, detras del permiso que el humano SI dio, con sus precondiciones.
    usb_ok() {
        local s
        met_usb_host
        usb_font_pre
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=motor-parado ok=true"
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=dispositivos ok=true cantidad=1"
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=motor-callback ok=true inicializado=true"
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=esperando-humano ok=false accion=aceptar_el_dialogo"
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=broadcast-falso ok=true origen=adb enviados=2"
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=permiso-falso ok=true granted=1 granted-sin-permiso-en-usbmanager=0"
        pre usb t-permiso true
        for s in permiso conectar capacidades descriptores backend wake-lock streaming-start streaming-stats \
                 reconectar-mismo conectar-otro \
                 suite-1 suite-2 suite-3 suite streaming-stop; do
            echo "HARNESS-SMOKE v=1 run=$run panel=usb step=$s ok=true"
        done
        # REQ-053 S3: las ventanas por libusb, despues de parar el streaming y antes de desconectar.
        win usb libusb
        for s in backend-restaurado desconectar; do
            echo "HARNESS-SMOKE v=1 run=$run panel=usb step=$s ok=true"
        done
        usb_font_post
        sens usb
    }
    with_usb() {  # with_usb <archivo destino> <sed sobre las lineas usb>
        { grep -v 'panel=plan step=fin ' "$tmp/verde.log"
          usb_ok | sed -E "$2"
          echo "HARNESS-SMOKE v=1 run=$run panel=plan step=fin ok=true fallidos=- motor-detenido=true"
        } | sed -E "s/(panel=plan step=inicio ok=true plan=)[^ ]*/\1salida,captura,sf2,sf3,usb/" > "$1"
    }
    # El `fin` de una corrida en la que el panel usb fallo.
    usb_failed() { sed -i.bak -E 's/(panel=plan step=fin )ok=true fallidos=-/\1ok=false fallidos=usb/' "$1"; }

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

    # REQ-050 S3 — M10: NO-MEDIDO ya no existe (el runner aplica el rate de cada fila). Una fila que
    # vuelve a traer medido=false es FAIL, tambien con ok=true: es la regresion a MINI-039.
    with_usb "$tmp/m10.log" 's/step=suite-2 ok=true/step=suite-2 ok=false medido=false motivo=rate-no-aplicado/; s/step=suite-3 ok=true/step=suite-3 ok=true medido=false motivo=rate-no-aplicado/'
    expect "M10: filas NO-MEDIDO, el resto sano" 1 "$tmp/m10.log"
    expect_line "M10: suite-2 NO-MEDIDO sale FAIL" '^FAIL +usb/suite-2 +NO-MEDIDO ya no existe' "$tmp/m10.log"
    expect_line "M10: suite-3 NO-MEDIDO con ok=true sale FAIL" '^FAIL +usb/suite-3 +NO-MEDIDO ya no existe' "$tmp/m10.log"

    # REQ-050 S3 — M20: una fila que el device no ofrece es NO-APLICA: no mueve el exit (el verde lo
    # deciden las demas) y NUNCA sale PASS, ni siquiera con ok=true.
    with_usb "$tmp/m20.log" 's/step=suite-2 ok=true/step=suite-2 ok=false aplica=false motivo=el-device-no-ofrece/; s/step=suite-3 ok=true/step=suite-3 ok=true aplica=false motivo=el-device-no-ofrece/'
    expect "M20: filas no aplicables, el resto sano" 0 "$tmp/m20.log"
    expect_line "M20: suite-2 sale NO-APLICA" '^NO-APLICA +usb/suite-2 ' "$tmp/m20.log"
    expect_line "M20: suite-3 con ok=true sale NO-APLICA" '^NO-APLICA +usb/suite-3 ' "$tmp/m20.log"
    expect_no_line "M20: ninguna fila no aplicable sale PASS" '^PASS +usb/suite-[23] ' "$tmp/m20.log"
    # M20b: aplica=false fuera de una fila de la suite no puede hacer desaparecer un paso: FAIL.
    with_usb "$tmp/m20b.log" 's/step=streaming-stats ok=true/step=streaming-stats ok=true aplica=false/'
    expect "M20b: aplica=false fuera de la suite" 1 "$tmp/m20b.log"

    # M11: una fila medida con ok=false sigue siendo FAIL.
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

    # REQ-050 S2 — M18: sin wake-lock (el permiso no llego por el merge, D9) es FAIL por ESE paso,
    # tanto si falta la linea como si dice ok=false. Mata el mutante que lo saca de EXPECTED.
    with_usb "$tmp/m18.log" '/step=wake-lock /d'
    expect "M18: falta usb/wake-lock" 1 "$tmp/m18.log"
    expect_line "M18: wake-lock sale FALTA" '^FAIL +usb/wake-lock +FALTA' "$tmp/m18.log"
    with_usb "$tmp/m18b.log" 's/step=wake-lock ok=true/step=wake-lock ok=false motivo=WAKE_LOCK-no-llego-por-el-merge/'
    expect "M18b: wake-lock ok=false" 1 "$tmp/m18b.log"

    # REQ-050 S2 — M19: AC-050.5 en device. Reconectar el mismo device con el stream vivo y pedir
    # otro device son pasos obligatorios: si faltan o dicen ok=false (re-init, fd cambiado, sin
    # DEVICE_BUSY), es FAIL por ESE paso.
    with_usb "$tmp/m19.log" '/step=reconectar-mismo /d'
    expect "M19: falta usb/reconectar-mismo" 1 "$tmp/m19.log"
    expect_line "M19: reconectar-mismo sale FALTA" '^FAIL +usb/reconectar-mismo +FALTA' "$tmp/m19.log"
    with_usb "$tmp/m19b.log" 's/step=reconectar-mismo ok=true/step=reconectar-mismo ok=false motivo=cambio-el-fd:re-init/'
    expect "M19b: reconectar-mismo ok=false" 1 "$tmp/m19b.log"
    with_usb "$tmp/m19c.log" 's/step=conectar-otro ok=true/step=conectar-otro ok=false motivo=no-dio-DEVICE_BUSY/'
    expect "M19c: conectar-otro ok=false" 1 "$tmp/m19c.log"
    expect_line "M19c: conectar-otro sale FAIL" '^FAIL +usb/conectar-otro ' "$tmp/m19c.log"

    # M12: `medido=false` en un paso que no es fila de la suite tambien es FAIL (la marca esta retirada).
    with_usb "$tmp/m12.log" 's/step=streaming-stats ok=true/step=streaming-stats ok=false medido=false motivo=x/'
    expect "M12: medido=false fuera de la suite" 1 "$tmp/m12.log"

    # REQ-050 S1 — M15/M16/M17: con dialogo, el broadcast falso y su juicio son obligatorios; un
    # grant falso es FAIL; y sin dialogo (permiso ya concedido, el log grabado) no se piden.
    with_usb "$tmp/m15.log" '/step=broadcast-falso /d'
    expect "M15: con dialogo y sin broadcast-falso" 1 "$tmp/m15.log"
    expect_line "M15: broadcast-falso sale FALTA" '^FAIL +usb/broadcast-falso +FALTA' "$tmp/m15.log"
    with_usb "$tmp/m15b.log" '/step=permiso-falso /d'
    expect_line "M15b: permiso-falso sale FALTA" '^FAIL +usb/permiso-falso +FALTA' "$tmp/m15b.log"
    with_usb "$tmp/m16.log" 's/step=permiso-falso ok=true granted=1 granted-sin-permiso-en-usbmanager=0/step=permiso-falso ok=false granted=1 granted-sin-permiso-en-usbmanager=1/'
    expect "M16: un grant con UsbManager diciendo que no" 1 "$tmp/m16.log"
    expect_line "M16: permiso-falso sale FAIL" '^FAIL +usb/permiso-falso ' "$tmp/m16.log"
    with_usb "$tmp/m16b.log" 's/step=broadcast-falso ok=true/step=broadcast-falso ok=false/'
    expect "M16b: el broadcast falso no se despacho" 1 "$tmp/m16b.log"
    expect_no_line "M17: sin dialogo no se piden los pasos del falso" '^FAIL +usb/(broadcast|permiso)-falso ' "$SELFTEST_LOG" todo "$vacia"

    # =====================================================================================
    # REQ-053 S1 — AC-053.6, una regla por bloque. Cada uno es un mutante del log + la ficha de
    # prueba, y cada uno tiene su gemelo: el verde de arriba (todo cumplido, exit 0).
    # =====================================================================================

    # (a) Una incumplida bloquea SUS dependientes y NO otros. t-host-cap bloquea captura/nivel; el
    # resto de captura y los demas paneles se juzgan igual.
    sed -E "$(set_pre t-host-cap false)" "$tmp/verde.log" > "$tmp/a1.log"
    expect "a1: incumplida (host) bloquea su dependiente" 4 "$tmp/a1.log" "$auto"
    expect_line "a1: captura/nivel sale BLOQUEADO por t-host-cap" '^BLOQUEADO +captura/nivel +precondicion=t-host-cap estado=incumplida' "$tmp/a1.log" "$auto"
    expect_line "a1: con la evidencia observada" '^BLOQUEADO +captura/nivel .*evidencia=prueba-t-host-cap' "$tmp/a1.log" "$auto"
    expect_line "a1: con la accion manual de la ficha" '^BLOQUEADO +captura/nivel .*remedio=REMEDIO-T-HOST-CAP' "$tmp/a1.log" "$auto"
    expect_line "a1: captura/start (no depende) se juzga: PASS" '^PASS +captura/start ' "$tmp/a1.log" "$auto"
    expect_line "a1: captura/stop (no depende) se juzga: PASS" '^PASS +captura/stop ' "$tmp/a1.log" "$auto"
    expect_no_line "a1: ningun otro paso sale BLOQUEADO" '^BLOQUEADO +(salida|sf2|sf3|captura/(start|stop))' "$tmp/a1.log" "$auto"
    # Y los no dependientes siguen pudiendo FALLAR: el bloqueo no tapa un rojo ajeno.
    sed -E 's/panel=captura step=stop ok=true/panel=captura step=stop ok=false/' "$tmp/a1.log" > "$tmp/a1b.log"
    expect_line "a1b: un no dependiente con ok=false sale FAIL" '^FAIL +captura/stop' "$tmp/a1b.log" "$auto"

    # (a) en usb, la forma del BUSY del 05/10 (GWT de S1): con otra app viva, streaming-start falla
    # y la suite no trafica. Lo que depende sale BLOQUEADO con el remedio; dispositivos, permiso y
    # los paneles automaticos se juzgan igual; `fin` no agrega un FAIL por el panel bloqueado.
    with_usb "$tmp/a2.log" "$(set_pre t-host-usb false); s/step=streaming-start ok=true/step=streaming-start ok=false error=STREAMING_ERROR/; s/step=(suite-[123]|suite|streaming-stats) ok=true/step=\1 ok=false motivo=sin-streaming/"
    usb_failed "$tmp/a2.log"
    expect "a2: otra app viva (BUSY) da BLOQUEADO, no FAIL" 4 "$tmp/a2.log"
    expect_line "a2: streaming-start sale BLOQUEADO" '^BLOQUEADO +usb/streaming-start +precondicion=t-host-usb estado=incumplida.*remedio=REMEDIO-T-HOST-USB' "$tmp/a2.log"
    expect_line "a2: usb/dispositivos (no depende) PASS" '^PASS +usb/dispositivos ' "$tmp/a2.log"
    expect_line "a2: salida/frames (otro panel) PASS" '^PASS +salida/frames ' "$tmp/a2.log"
    expect_line "a2: plan/fin explicado por el bloqueo" '^BLOQUEADO +plan/fin ' "$tmp/a2.log"
    expect_no_line "a2: ningun FAIL" '^FAIL ' "$tmp/a2.log"
    # Los pasos que dependen y NO se emitieron (conectar fallo y el panel corto) son BLOQUEADO,
    # no FAIL por FALTA.
    with_usb "$tmp/a3.log" "$(set_pre t-host-usb false); s/step=conectar ok=true/step=conectar ok=false error=DEVICE_BUSY/; /step=(motor-callback|capacidades|descriptores|backend|wake-lock|streaming-[a-z]+|reconectar-mismo|conectar-otro|suite[-0-9]*|escuchar|estimulo|control|sensor|backend-restaurado|desconectar) /d"
    usb_failed "$tmp/a3.log"
    expect "a3: los dependientes que no corrieron" 4 "$tmp/a3.log"
    expect_line "a3: desconectar (no emitido) sale BLOQUEADO" '^BLOQUEADO +usb/desconectar +precondicion=t-host-usb.*no-emitido' "$tmp/a3.log"
    expect_no_line "a3: ninguno sale FALTA" '^FAIL ' "$tmp/a3.log"

    # (b) Una no verificable bloquea, nunca cuenta como cumplida: el host no pudo (adb fallo), la
    # app no emitio su linea, la linea no se parsea, o la emitio el verificador equivocado.
    sed -E "$(set_pre t-host-cap no-verificable)" "$tmp/verde.log" > "$tmp/b1.log"
    expect "b1: el host la dio no-verificable" 4 "$tmp/b1.log" "$auto"
    expect_line "b1: captura/nivel sale BLOQUEADO no-verificable" '^BLOQUEADO +captura/nivel +precondicion=t-host-cap estado=no-verificable' "$tmp/b1.log" "$auto"
    grep -v ' id=t-app-cap ' "$tmp/verde.log" > "$tmp/b2.log"
    expect "b2: la app no emitio su linea" 4 "$tmp/b2.log" "$auto"
    expect_line "b2: captura/start sale BLOQUEADO no-verificable" '^BLOQUEADO +captura/start +precondicion=t-app-cap estado=no-verificable' "$tmp/b2.log" "$auto"
    expect_line "b2: salida/start (otro panel, mismo paso) se juzga" '^PASS +salida/start ' "$tmp/b2.log" "$auto"
    grep -v ' id=t-host-cap ' "$tmp/verde.log" > "$tmp/b3.log"
    expect "b3: el host no registro su verificacion" 4 "$tmp/b3.log" "$auto"
    sed -E '/ id=t-app-cap /s/ cumplida=true / cumplida=quizas /' "$tmp/verde.log" > "$tmp/b4.log"
    expect "b4: cumplida ilegible" 4 "$tmp/b4.log" "$auto"
    sed -E '/ id=t-app-cap /s/ ok=true / ok=false /' "$tmp/verde.log" > "$tmp/b5.log"
    expect "b5: ok y cumplida no coinciden" 4 "$tmp/b5.log" "$auto"
    sed -E '/ id=t-host-cap /s/ verificador=host//' "$tmp/verde.log" > "$tmp/b6.log"
    expect "b6: la de host la emitio la app" 4 "$tmp/b6.log" "$auto"
    expect_line "b6: sale no-verificable, no cumplida" '^BLOQUEADO +captura/nivel +precondicion=t-host-cap estado=no-verificable' "$tmp/b6.log" "$auto"
    { cat "$tmp/verde.log"; pre captura t-host-cap false host; } > "$tmp/b7.log"
    expect "b7: dos lineas que no coinciden: no cumplida" 4 "$tmp/b7.log" "$auto"

    # (c) Un BLOQUEADO nunca suma como PASS: a1 bloquea captura/nivel, que la app dio ok=true.
    expect_no_line "c1: el bloqueado con ok=true no sale PASS" '^PASS +captura/nivel ' "$tmp/a1.log" "$auto"
    expect_line "c1: y el resumen lo cuenta BLOQUEADO" '^resumen: [0-9]+ PASS · 0 FAIL · 1 BLOQUEADO ' "$tmp/a1.log" "$auto"
    local pass_verde pass_a1
    pass_verde="$(verdict_split "$tmp/verde.log" "$run" "$auto" "$ficha" 2>&1 | sed -nE 's/^resumen: ([0-9]+) PASS.*/\1/p' || true)"
    pass_a1="$(verdict_split "$tmp/a1.log" "$run" "$auto" "$ficha" 2>&1 | sed -nE 's/^resumen: ([0-9]+) PASS.*/\1/p' || true)"
    if [[ -n "$pass_verde" && "$pass_a1" == "$((pass_verde - 1))" ]]; then
        printf '  ok    %-58s\n' "c2: bloquear un paso le resta uno al PASS ($pass_verde -> $pass_a1)"
    else
        printf '  MAL   %-58s PASS verde=%s, con un bloqueo=%s\n' "c2: bloquear un paso le resta uno al PASS" "$pass_verde" "$pass_a1"
        failures=$((failures + 1))
    fi

    # (d) Precedencia del exit: 1 FAIL > 4 BLOQUEADO > 3 HUMANO > 0.
    sed -E 's/panel=sf3 step=nota ok=true/panel=sf3 step=nota ok=false/' "$tmp/a1.log" > "$tmp/d1.log"
    expect "d1: FAIL + BLOQUEADO = 1" 1 "$tmp/d1.log" "$auto"
    sed -E "$(set_pre t-host-cap false)" "$tmp/m4.log" > "$tmp/d2.log"
    expect "d2: BLOQUEADO + HUMANO = 4" 4 "$tmp/d2.log"
    expect "d3: HUMANO solo = 3 (M4)" 3 "$tmp/m4.log"
    expect "d4: nada de eso = 0 (verde)" 0 "$tmp/verde.log" "$auto"

    # D13 — el permiso USB con su ventana humana.
    # Negado explicitamente: la app emite t-permiso cumplida=false al cerrar la ventana. Los pasos
    # que dependen —incluido permiso-falso, que no puede distinguir al humano de un broadcast
    # ajeno— salen BLOQUEADO, no FAIL ni HUMANO.
    with_usb "$tmp/p1.log" "$(set_pre t-permiso false); s/step=permiso ok=true/step=permiso ok=false origen=dialogo concedido=false/; s/step=permiso-falso ok=true .*/step=permiso-falso ok=false motivo=negado:humano-o-broadcast-ajeno resultado=PERMISSION_DENIED/; s/step=conectar ok=true/step=conectar ok=false error=PERMISSION_DENIED/; /step=(motor-callback|capacidades|descriptores|backend|wake-lock|streaming-[a-z]+|reconectar-mismo|conectar-otro|suite[-0-9]*|escuchar|estimulo|control|sensor|backend-restaurado|desconectar) /d"
    usb_failed "$tmp/p1.log"
    expect "D13: permiso negado = BLOQUEADO (exit 4)" 4 "$tmp/p1.log"
    expect_line "D13: permiso-falso sale BLOQUEADO" '^BLOQUEADO +usb/permiso-falso +precondicion=t-permiso estado=incumplida' "$tmp/p1.log"
    expect_line "D13: conectar sale BLOQUEADO" '^BLOQUEADO +usb/conectar +precondicion=t-permiso' "$tmp/p1.log"
    # La ventana vencio sin respuesta (M4): sin linea de la app, HUMANO como hoy — nunca BLOQUEADO.
    expect_no_line "D13: ventana vencida no da BLOQUEADO (M4)" '^BLOQUEADO ' "$tmp/m4.log"
    # Gemelo: la exencion de la ventana humana vale SOLO con una espera humana pendiente. Sin
    # dialogo (permiso ya concedido) la app tiene que emitir su linea: sin ella, no-verificable.
    with_usb "$tmp/p2.log" '/step=(esperando-humano|broadcast-falso|permiso-falso) /d; / id=t-permiso /d'
    expect "D13: sin espera humana y sin linea = BLOQUEADO" 4 "$tmp/p2.log"
    expect_line "D13: ... por no-verificable" '^BLOQUEADO +usb/conectar +precondicion=t-permiso estado=no-verificable' "$tmp/p2.log"
    expect "D13: concedido = 0 (M7)" 0 "$tmp/m7.log"
    # Gemelo con dialogo: el humano dio el permiso (`permiso ok=true`) y la app no emitio la linea.
    # No hay espera pendiente que la exima: no-verificable.
    with_usb "$tmp/p3.log" '/ id=t-permiso /d'
    expect "D13: dialogo, permiso dado y sin linea = BLOQUEADO" 4 "$tmp/p3.log"
    # Negado y permiso-falso no emitido: un paso de WITH_DIALOG que depende tambien sale BLOQUEADO.
    grep -v ' step=permiso-falso ' "$tmp/p1.log" > "$tmp/p1b.log"
    expect_line "D13: permiso-falso no emitido sale BLOQUEADO" '^BLOQUEADO +usb/permiso-falso +precondicion=t-permiso .*no-emitido' "$tmp/p1b.log"
    # El permiso se CUMPLIO pero connectDevice no volvio (no hay `permiso`, conectar sin respuesta):
    # no es un humano pendiente — es un FAIL de la libreria.
    with_usb "$tmp/p4.log" "/step=permiso /d; s/step=conectar ok=true/step=conectar ok=false motivo=sin-respuesta-humana/; /step=(motor-callback|capacidades|descriptores|backend|wake-lock|streaming-[a-z]+|reconectar-mismo|conectar-otro|suite[-0-9]*|escuchar|estimulo|control|sensor|backend-restaurado|desconectar) /d"
    usb_failed "$tmp/p4.log"
    expect "D13: permiso cumplido y connect colgado = FAIL" 1 "$tmp/p4.log"
    expect_line "D13: ... conectar sale FAIL, no HUMANO" '^FAIL +usb/conectar ' "$tmp/p4.log"

    # Un paso CONCLUYENTE (`concluyente=true`: la app sabe que la falla no la explica ninguna
    # precondicion — p.ej. un grant que UsbManager desmiente) es FAIL aunque este bloqueado.
    sed -E 's/step=permiso-falso ok=false [^ ]+/step=permiso-falso ok=false concluyente=true granted-sin-permiso-en-usbmanager=1/' \
        "$tmp/p1.log" > "$tmp/k1.log"
    expect "concluyente: grant falso con el permiso negado = FAIL" 1 "$tmp/k1.log"
    expect_line "concluyente: permiso-falso sale FAIL" '^FAIL +usb/permiso-falso +concluyente' "$tmp/k1.log"
    # Gemelo: `concluyente` nunca destapa un PASS — con ok=true el paso sigue BLOQUEADO.
    sed -E 's/(panel=captura step=nivel ok=true)/\1 concluyente=true/' "$tmp/a1.log" > "$tmp/k2.log"
    expect "concluyente con ok=true sigue BLOQUEADO" 4 "$tmp/k2.log" "$auto"

    # Una linea de precondicion que la ficha no declara es FAIL: o la app y la ficha se
    # desincronizaron, o alguien emite precondiciones que nadie juzga.
    { cat "$tmp/verde.log"; pre captura t-no-declarada true; } > "$tmp/u1.log"
    expect "u1: precondicion no declarada en la ficha" 1 "$tmp/u1.log" "$auto"
    expect_line "u1: sale FAIL con su id" '^FAIL +captura/precondicion +.*t-no-declarada' "$tmp/u1.log" "$auto"

    # AC-053.5 / D12 — el JSON de la corrida: run id, cada paso con lo OBSERVADO (tambien los
    # bloqueados: S2 lee de ahi el claim_interface), cada precondicion y el sensor (vacio hasta S3).
    expect_json "json: run, exit y un juicio de sensor por ventana" "$tmp/a2.log" todo \
        "j['run'] == '$run' and j['exit'] == 4 and len(j['sensor']) == 6"
    expect_json "json: el bloqueado guarda lo observado" "$tmp/a2.log" todo \
        "[p for p in j['pasos'] if p['panel'] == 'usb' and p['paso'] == 'streaming-start' and p['veredicto'] == 'BLOQUEADO' and p['observado'].get('error') == 'STREAMING_ERROR' and p['precondiciones'] == ['t-host-usb']]"
    expect_json "json: el no emitido guarda observado nulo" "$tmp/a3.log" todo \
        "[p for p in j['pasos'] if p['paso'] == 'desconectar' and p['veredicto'] == 'BLOQUEADO' and p['observado'] is None]"
    expect_json "json: cada precondicion con sus campos" "$tmp/a2.log" todo \
        "[p for p in j['precondiciones'] if p['id'] == 't-host-usb' and p['verificador'] == 'host' and p['cumplida'] is False and p['estado'] == 'incumplida' and p['evidencia'] == 'prueba-t-host-usb' and p['remedio'] == 'REMEDIO-T-HOST-USB'] and [p for p in j['precondiciones'] if p['id'] == 't-permiso' and p['cumplida'] is True]"
    expect_json "json: una no verificable nunca es cumplida" "$tmp/b2.log" "$auto" \
        "[p for p in j['precondiciones'] if p['id'] == 't-app-cap' and p['cumplida'] is False and p['estado'] == 'no-verificable']"

    # La ficha: un error en ella es de uso (exit 2), nunca un juicio. Cada mutante rompe UNA regla.
    local v
    ficha_mutante() {  # ficha_mutante <nombre> <expresion python que muta f, la ficha de prueba>
        python3 -c 'import json,sys; f=json.load(open(sys.argv[1])); exec(sys.argv[2]); json.dump(f, open(sys.argv[3], "w"))' \
            "$ficha" "$2" "$tmp/fm.json"
        expect "ficha: $1" 2 "$tmp/verde.log" "$auto" "$tmp/fm.json"
    }
    ficha_mutante "depende de un paso que no existe" "f['planes']['captura']['precondiciones'][0]['depende'] = ['nivle']"
    ficha_mutante "comodin que no matchea ningun paso" "f['planes']['usb']['precondiciones'][1]['depende'] = ['suite-x*']"
    ficha_mutante "chequeo de host desconocido" "f['planes']['captura']['precondiciones'][0]['chequeo'] = 'adivinar'"
    ficha_mutante "sin remedio" "del f['planes']['captura']['precondiciones'][1]['remedio']"
    ficha_mutante "remedio vacio" "f['planes']['captura']['precondiciones'][1]['remedio'] = ' '"
    ficha_mutante "id duplicado" "f['planes']['usb']['precondiciones'][0]['id'] = 't-host-cap'"
    ficha_mutante "id con mayusculas" "f['planes']['captura']['precondiciones'][1]['id'] = 'T-App'"
    ficha_mutante "clave desconocida (typo)" "f['planes']['captura']['precondiciones'][1]['dependen'] = ['start']"
    ficha_mutante "verificador desconocido" "f['planes']['captura']['precondiciones'][1]['verificador'] = 'telefono'"
    ficha_mutante "plan desconocido" "f['planes']['sf4'] = {'precondiciones': []}"
    ficha_mutante "host sin el parametro de su chequeo" "del f['planes']['usb']['precondiciones'][1]['paquetes']"
    ficha_mutante "lista de paquetes vacia" "f['planes']['usb']['precondiciones'][1]['paquetes'] = []"
    ficha_mutante "ventana humana en una de host" "f['planes']['captura']['precondiciones'][0]['ventana-humana'] = True"
    ficha_mutante "formato desconocido" "f['formato'] = 2"
    # El atajo "sin placa => cumplida" del chequeo usb-placa-no-reclamada solo es honesto si la que SI bloquea
    # (usb-interfaz-de-clase, de la misma clase) esta en el mismo plan.
    ficha_mutante "usb-placa-no-reclamada sin usb-interfaz-de-clase en su plan" "del f['planes']['usb']['precondiciones'][0]"
    ficha_mutante "usb-placa-no-reclamada con otra clase que usb-interfaz-de-clase" "f['planes']['usb']['precondiciones'][0]['clase'] = 3"
    ficha_mutante "usb-placa-no-reclamada sin su clase" "del f['planes']['usb']['precondiciones'][3]['clase']"
    echo '{"formato": 1, "planes": {' > "$tmp/rota.json"
    expect "ficha: JSON roto" 2 "$tmp/verde.log" "$auto" "$tmp/rota.json"
    expect "ficha: no existe" 2 "$tmp/verde.log" "$auto" "$tmp/no-existe.json"

    # La ficha REAL: valida, la unica fuente de los ids (AC-053.5) y en sintonia con el harness.
    if smoke_py validar "$real" > "$tmp/out" 2>&1; then
        printf '  ok    %-58s\n' "ficha real: valida"
    else
        printf '  MAL   %-58s\n' "ficha real: valida"; sed 's/^/        /' "$tmp/out"; failures=$((failures + 1))
    fi
    local leaked
    leaked="$(ids_literal_in "$SELF" "$real")"
    if [[ -z "$leaked" ]]; then
        printf '  ok    %-58s\n' "1.4: ningun id de la ficha escrito en el script"
    else
        printf '  MAL   %-58s %s\n' "1.4: ningun id de la ficha escrito en el script" "$leaked"; failures=$((failures + 1))
    fi
    # ... y el chequeo ve un id copiado (su mutante): si no, el verde de arriba no prueba nada.
    local first_id
    first_id="$(smoke_py ids "$real" | awk 'NR == 1 {print $2}')"
    { cat "$SELF"; echo "    [[ \"\$x\" == $first_id ]]"; } > "$tmp/script-mutante.sh"
    if [[ -n "$first_id" && "$(ids_literal_in "$tmp/script-mutante.sh" "$real")" == *"$first_id"* ]]; then
        printf '  ok    %-58s\n' "1.4: el chequeo ve un id copiado al script ($first_id)"
    else
        printf '  MAL   %-58s\n' "1.4: el chequeo ve un id copiado al script"; failures=$((failures + 1))
    fi
    local missing_app
    missing_app="$(app_ids_not_emitted harness/src "$real")"
    if [[ -z "$missing_app" ]] && smoke_py ids "$real" | grep -q ' app$'; then
        printf '  ok    %-58s\n' "ficha real: cada id de app tiene su precondition() en el harness"
    else
        printf '  MAL   %-58s falta: %s\n' "ficha real: cada id de app tiene su precondition() en el harness" "${missing_app:-(no hay ids de app)}"
        failures=$((failures + 1))
    fi
    # Su mutante: el harness sin la llamada que emite mic... (la primera de app de la ficha).
    cp -R harness/src "$tmp/harness-src"
    grep -rl 'precondition(.*SmokePreconditions\.' "$tmp/harness-src" | head -1 | xargs sed -i.bak '/precondition(.*SmokePreconditions\./d'
    find "$tmp/harness-src" -name '*.bak' -delete
    if [[ -n "$(app_ids_not_emitted "$tmp/harness-src" "$real")" ]]; then
        printf '  ok    %-58s\n' "... y el chequeo ve una llamada borrada"
    else
        printf '  MAL   %-58s\n' "... y el chequeo ve una llamada borrada"; failures=$((failures + 1))
    fi

    # La ficha real sobre el control grabado (01/10, anterior a REQ-053, sin lineas de
    # precondicion): nada se verifico, asi que captura y usb salen BLOQUEADO — y con cada una
    # cumplida, el control vuelve a 0. El control NO se re-graba (1.9): la app nueva agrega lineas
    # pero no cambia ninguna de las que el control ya tiene.
    expect "control + ficha real: nada verificado = 4" 4 "$tmp/control-s3.log" todo "$real"
    expect_line "control + ficha real: usb/conectar no-verificable" '^BLOQUEADO +usb/conectar +precondicion=.* estado=no-verificable' "$tmp/control-s3.log" todo "$real"
    expect_line "control + ficha real: usb/estimulo no-verificable" '^BLOQUEADO +usb/estimulo +precondicion=.* estado=no-verificable' "$tmp/control-s3.log" todo "$real"
    expect_line "control + ficha real: salida/frames se juzga" '^PASS +salida/frames ' "$tmp/control-s3.log" todo "$real"
    smoke_py ids "$real" | while read -r panel id verifier; do
        if [[ "$verifier" == host ]]; then pre "$panel" "$id" true host; else pre "$panel" "$id" true; fi
    done | before_fin "$tmp/control-s3.log" "$tmp/control-cumplido.log"
    expect "control + ficha real, todo cumplido = el exit grabado" "$want_base" "$tmp/control-cumplido.log" todo "$real"

    # S-1 (security-auditor): logcat filtra por TAG, asi que cualquier app puede escribir una linea
    # `verificador=host`. El juez recibe las del host por SEPARADO y descarta las que lleguen por el
    # log de la app: no bloquean ni cuentan.
    # (S3: los juicios del sensor tambien van en su propio archivo, como los del host.)
    grep -v -e 'verificador=host' -e ' step=sensor ' "$tmp/verde.log" > "$tmp/app-verde.log" || true
    grep 'verificador=host' "$tmp/verde.log" > "$tmp/host-verde.log" || true
    grep ' step=sensor ' "$tmp/verde.log" > "$tmp/sensor-verde.log" || true
    local split_sensor="$tmp/sensor-verde.log"
    : > "$tmp/host-vacio.log"
    split_case() {  # split_case <nombre> <exit esperado> <log de la app> <log del host> [regex que TIENE que salir]
        local name="$1" want="$2" app="$3" hostf="$4" re="${5:-}" got=0
        verdict "$app" "$run" "$auto" "$ficha" "" "$hostf" "$split_sensor" > "$tmp/out" 2>&1 || got=$?
        if [[ "$got" == "$want" ]] && { [[ -z "$re" ]] || grep -Eq -- "$re" "$tmp/out"; }; then
            printf '  ok    %-58s exit %s\n' "$name" "$got"
        else
            printf '  MAL   %-58s exit %s, esperaba %s%s\n' "$name" "$got" "$want" "${re:+ y /$re/}"
            sed 's/^/        /' "$tmp/out" | tail -6
            failures=$((failures + 1))
        fi
    }
    sed -E 's/panel=captura step=nivel ok=true/panel=captura step=nivel ok=false/' "$tmp/app-verde.log" > "$tmp/app-nivel-mal.log"
    split_case "S-1 gemelo: captura/nivel mal, host cumplido" 1 "$tmp/app-nivel-mal.log" "$tmp/host-verde.log"
    { cat "$tmp/app-nivel-mal.log"; pre captura t-host-cap false host; } > "$tmp/app-forjada-mala.log"
    split_case "S-1: una linea host falsa de la app no tapa el FAIL" 1 "$tmp/app-forjada-mala.log" "$tmp/host-verde.log" \
        '^AVISO .* 1 linea\(s\) verificador=host'
    { cat "$tmp/app-verde.log"; pre captura t-host-cap true host; } > "$tmp/app-forjada-buena.log"
    split_case "S-1 gemelo: host cumplido, sin forjar" 0 "$tmp/app-verde.log" "$tmp/host-verde.log"
    split_case "S-1: una linea host falsa no suple la que el host no grabo" 4 "$tmp/app-forjada-buena.log" "$tmp/host-vacio.log" \
        'el-host-no-registro-su-verificacion'
    split_case "S-1: las lineas host del log de la app no se cuentan" 4 "$tmp/verde.log" "$tmp/host-vacio.log"
    # --host-en-log: los logs grabados de S1 traen las lineas del host MEZCLADAS con las de la app. El
    # flag es explicito, solo del CLI --veredicto, y avisa que ese log no separa origenes.
    cli_case() {  # cli_case <nombre> <exit esperado> <regex que TIENE que salir> <args de --veredicto...>
        local name="$1" want="$2" re="$3" got=0; shift 3
        bash "$0" --veredicto "$@" > "$tmp/out" 2>&1 || got=$?
        if [[ "$got" == "$want" ]] && grep -Eq -- "$re" "$tmp/out"; then
            printf '  ok    %-58s exit %s\n' "$name" "$got"
        else
            printf '  MAL   %-58s exit %s, esperaba %s y /%s/\n' "$name" "$got" "$want" "$re"
            sed 's/^/        /' "$tmp/out" | tail -6
            failures=$((failures + 1))
        fi
    }
    sed -E 's/ok=true id=t-host-cap cumplida=true/ok=false id=t-host-cap cumplida=false/' "$tmp/verde.log" > "$tmp/mezclado-incumplida.log"
    cli_case "host-en-log: un log mezclado se juzga con el flag" 0 'no separa origenes' \
        "$tmp/verde.log" "$run" "$auto" --setup "$ficha" --host-en-log --sensor-log "$tmp/sensor-verde.log"
    cli_case "host-en-log: ... y juzga las precondiciones del propio log" 4 '^BLOQUEADO +captura/nivel +precondicion=t-host-cap estado=incumplida' \
        "$tmp/mezclado-incumplida.log" "$run" "$auto" --setup "$ficha" --host-en-log --sensor-log "$tmp/sensor-verde.log"
    cli_case "host-en-log: sin el flag el mismo log es BLOQUEADO, con su AVISO" 4 '^AVISO .* [0-9]+ linea\(s\) verificador=host.*descartadas' \
        "$tmp/verde.log" "$run" "$auto" --setup "$ficha"
    cli_case "host-en-log: sin el flag no se toma ni una linea host" 4 'estado=no-verificable evidencia=el-host-no-registro-su-verificacion' \
        "$tmp/mezclado-incumplida.log" "$run" "$auto" --setup "$ficha"
    # La corrida en device nunca lo activa: el log de la app es de un tercero, el del host es el suyo.
    if [[ "$(awk '/^run_device\(\) \{/ {on = 1} on {print} on && /^\}/ {exit}' "$0" | grep -c 'host-en-log')" == 0 ]]; then
        printf '  ok    %-58s\n' "host-en-log: run_device no lo pasa"
    else
        printf '  MAL   %-58s\n' "host-en-log: run_device no lo pasa"; failures=$((failures + 1))
    fi
    # La captura del script: con uid, solo lo del harness; el uid sale del paquete EXACTO.
    local pm_fix="package:$PKG.test uid:10999
package:$PKG uid:10234
package:com.ajena uid:10001"
    if [[ "$(uid_of_package "$PKG" <<< "$pm_fix")" == 10234 && -z "$(uid_of_package "$PKG" <<< "package:$PKG.test uid:10999")" \
        && -z "$(uid_of_package "$PKG" <<< "package:$PKG uid:x1")" && -z "$(uid_of_package "$PKG" <<< "")" ]]; then
        printf '  ok    %-58s\n' "S-1: el uid es el del paquete exacto, o nada"
    else
        printf '  MAL   %-58s\n' "S-1: el uid es el del paquete exacto, o nada"; failures=$((failures + 1))
    fi
    if [[ "$(logcat_capture_args 10234)" == "-v raw --uid=10234 -s HARNESS-SMOKE:I" && "$(logcat_capture_args "")" == "-v raw -s HARNESS-SMOKE:I" ]]; then
        printf '  ok    %-58s\n' "S-1: la captura filtra por uid, y sin uid sigue como antes"
    else
        printf '  MAL   %-58s\n' "S-1: la captura filtra por uid, y sin uid sigue como antes"; failures=$((failures + 1))
    fi

    # S-2: el run id no se puede adivinar (fecha + pid + 48 bits de /dev/urandom), y el juez sigue
    # aceptando el viejo: el del control grabado ya se juzgo arriba con ese formato.
    local rid1 rid2
    rid1="$(new_run_id || true)"; rid2="$(new_run_id || true)"
    if [[ "$rid1" =~ ^smoke-[0-9]{8}-[0-9]{6}-[0-9]+-[0-9a-f]{12}$ && "$rid1" != "$rid2" ]]; then
        printf '  ok    %-58s\n' "S-2: el run id lleva 48 bits aleatorios y cambia"
    else
        printf '  MAL   %-58s %s %s\n' "S-2: el run id lleva 48 bits aleatorios y cambia" "$rid1" "$rid2"; failures=$((failures + 1))
    fi
    sed -E "s/run=$run /run=$rid1 /" "$tmp/verde.log" > "$tmp/verde-rid.log"
    grep -v -e 'verificador=host' -e ' step=sensor ' "$tmp/verde-rid.log" > "$tmp/app-rid.log" || true
    grep 'verificador=host' "$tmp/verde-rid.log" > "$tmp/host-rid.log" || true
    grep ' step=sensor ' "$tmp/verde-rid.log" > "$tmp/sensor-rid.log" || true
    local got=0
    verdict "$tmp/app-rid.log" "$rid1" "$auto" "$ficha" "" "$tmp/host-rid.log" "$tmp/sensor-rid.log" > /dev/null 2>&1 || got=$?
    if [[ "$got" == 0 ]]; then printf '  ok    %-58s exit 0\n' "S-2: el juez acepta un run id con el formato nuevo"
    else printf '  MAL   %-58s exit %s\n' "S-2: el juez acepta un run id con el formato nuevo" "$got"; failures=$((failures + 1)); fi
    if grep -qE '^    run="\$\(new_run_id\)" \|\| exit 2$' "$0"; then
        printf '  ok    %-58s\n' "S-2: la corrida en device saca su run id de new_run_id"
    else
        printf '  MAL   %-58s\n' "S-2: la corrida en device saca su run id de new_run_id"; failures=$((failures + 1))
    fi
    if [[ "$run" =~ ^smoke-[0-9]{8}-[0-9]{6}-[0-9]+$ ]]; then
        printf '  ok    %-58s\n' "S-2: el control grabado conserva el run id viejo"
    else
        printf '  MAL   %-58s %s\n' "S-2: el control grabado conserva el run id viejo" "$run"; failures=$((failures + 1))
    fi

    # S-3: --plan se interpola en `adb shell am start`, o sea que vuelve a pasar por el `sh` del
    # telefono. Un plan que no sea `todo` o paneles conocidos es exit 2 SIN llamar a adb.
    mkdir -p "$tmp/espia"
    cat > "$tmp/espia/adb" <<'SPY'
#!/usr/bin/env bash
echo "$*" >> "$SPY_LOG"
SPY
    chmod +x "$tmp/espia/adb"
    plan_case() {  # plan_case <nombre> <plan> <exit esperado> <llamo a adb: si|no>
        local name="$1" plan="$2" want="$3" calls="$4" got=0 called=no
        : > "$tmp/espia.log"
        PATH="$tmp/espia:$PATH" SPY_LOG="$tmp/espia.log" ANDROID_SERIAL=falso-123 \
            bash "$0" --plan "$plan" --no-build > "$tmp/out" 2>&1 || got=$?
        [[ -s "$tmp/espia.log" ]] && called=si
        if [[ "$got" == "$want" && "$called" == "$calls" ]]; then
            printf '  ok    %-58s exit %s, adb: %s\n' "$name" "$got" "$called"
        else
            printf '  MAL   %-58s exit %s (esperaba %s), adb: %s (esperaba %s)\n' "$name" "$got" "$want" "$called" "$calls"
            failures=$((failures + 1))
        fi
    }
    plan_case "S-3 control: un plan valido llega a adb (el espia anda)" salida 2 si
    plan_case "S-3: todo,usb no es un plan (todo va solo)" 'todo,usb' 2 no
    plan_case "S-3: --plan 'todo;id' es exit 2 sin llamar a adb" 'todo;id' 2 no
    plan_case "S-3: un panel desconocido es exit 2 sin adb" 'salida,nada' 2 no
    plan_case "S-3: mayusculas, exit 2 sin adb" 'Salida' 2 no
    plan_case "S-3: un plan con espacio o \$() es exit 2 sin adb" 'salida $(id)' 2 no
    plan_case "S-3: un plan vacio es exit 2 sin adb" '' 2 no
    plan_case "S-3: coma final (salida,) es exit 2 sin adb" 'salida,' 2 no
    plan_case "S-3: elemento vacio en el medio es exit 2 sin adb" 'salida,,sf2' 2 no
    plan_case "S-3: coma inicial es exit 2 sin adb" ',salida' 2 no

    # =====================================================================================
    # REQ-053 S3 — el estimulo y el sensor (AC-053.8..11). Cada regla tiene su gemelo (el verde, con
    # el sensor contestando bien) y un mutante del juez, del seguidor o del sensor que la mata (ver
    # la tabla de las Notas de la etapa).
    # =====================================================================================
    local ne nc
    ne="$(n_of sf2 estimulo)"; nc="$(n_of sf2 control)"
    is_eq() {  # is_eq <nombre> <obtenido> <esperado>
        if [[ "$2" == "$3" ]]; then
            printf '  ok    %-58s\n' "$1"
        else
            printf '  MAL   %-58s dio [%s], esperaba [%s]\n' "$1" "$2" "$3"; failures=$((failures + 1))
        fi
    }
    has_line() {  # has_line <nombre> <regex> <archivo>
        if grep -Eq -- "$2" "$3"; then
            printf '  ok    %-58s\n' "$1"
        else
            printf '  MAL   %-58s falta /%s/ en %s\n' "$1" "$2" "$(basename "$3")"
            sed 's/^/        /' "$3" | tail -6; failures=$((failures + 1))
        fi
    }

    # AC-053.10 — el orden sale de la semilla, con los MISMOS vectores que fija ListeningWindowsTest
    # (harness, commonTest): son dos implementaciones del mismo orden y el juez cruza una con otra.
    local vec v_seed v_panel v_a v_b bad_vec=""
    for vec in "0 sf2 estimulo control" "1 sf2 control estimulo" "42 usb estimulo control" \
               "12345 sf3 control estimulo" "2147483647 sf2 control estimulo" "123456789 usb estimulo control"; do
        read -r v_seed v_panel v_a v_b <<< "$vec"
        [[ "$(smoke_py orden "$v_seed" "$v_panel")" == "$v_a $v_b" ]] || bad_vec+="[$vec] "
    done
    is_eq "orden: los vectores compartidos con el harness" "${bad_vec:-ninguno}" ninguno

    # --- AC-053.8: el estimulo declarado, y lo que el motor rindio en la ventana. ---------------
    expect_line "S3 gemelo: sf2/estimulo rendido sale PASS" '^PASS +sf2/estimulo ' "$tmp/verde.log" "$auto"
    expect_line "S3 gemelo: sf2/control (silencio rendido) sale PASS" '^PASS +sf2/control ' "$tmp/verde.log" "$auto"
    expect_line "S3 gemelo: el sensor oyo el estimulo = PASS" '^PASS +sf2/sensor-estimulo +.*sensor=oido-humano veredicto=presente' "$tmp/verde.log" "$auto"
    expect_line "S3 gemelo: el sensor no oyo nada en el control = PASS" '^PASS +sf2/sensor-control +.*veredicto=ausente' "$tmp/verde.log" "$auto"
    # El juez mira los NUMEROS (frames > 0 y pico > 0), no solo el ok de la app.
    sed -E '/panel=sf2 step=estimulo /s/ frames=[0-9]+ / frames=0 /' "$tmp/verde.log" > "$tmp/e1.log"
    expect "e1: estimulo con frames=0 (aunque diga ok=true) = FAIL" 1 "$tmp/e1.log" "$auto"
    expect_line "e1: sf2/estimulo sale FAIL" '^FAIL +sf2/estimulo +.*no-rendido' "$tmp/e1.log" "$auto"
    sed -E '/panel=sf2 step=estimulo /s/ pico=[0-9.]+ / pico=0.0000 /' "$tmp/verde.log" > "$tmp/e2.log"
    expect "e2: estimulo con pico=0 (aunque diga ok=true) = FAIL" 1 "$tmp/e2.log" "$auto"
    expect_line "e2: sf2/estimulo sale FAIL" '^FAIL +sf2/estimulo ' "$tmp/e2.log" "$auto"
    sed -E '/panel=sf2 step=estimulo /s/ ok=true (.*)$/ ok=false \1 motivo=sin-senal/' "$tmp/verde.log" > "$tmp/e3.log"
    expect "e3: estimulo con ok=false = FAIL" 1 "$tmp/e3.log" "$auto"
    # ... y lo que el sensor haya dicho sobre esa ventana NO se juzga: si hay juicio, el script lo
    # pidio cuando no debia (FAIL); si no lo hay, no hay fila del sensor.
    expect_no_line "e1: el sensor no da PASS sobre un estimulo no rendido" '^PASS +sf2/sensor-estimulo ' "$tmp/e1.log" "$auto"
    expect_line "e1: un juicio sobre la ventana no rendida sale FAIL" '^FAIL +sf2/sensor-estimulo +.*no-rendida' "$tmp/e1.log" "$auto"
    grep -v " panel=sf2 step=sensor ok=true n=$ne " "$tmp/e1.log" > "$tmp/e1b.log"
    expect "e1b: sin consulta, sigue FAIL por el estimulo" 1 "$tmp/e1b.log" "$auto"
    expect_no_line "e1b: sin consulta no hay fila del sensor" 'sf2/sensor-estimulo ' "$tmp/e1b.log" "$auto"
    expect_json "e1b: el JSON dice que no se consulto" "$tmp/e1b.log" "$auto" \
        "[x for x in j['sensor'] if x['panel'] == 'sf2' and x['n'] == $ne and x['consultado'] is False and x['juicio'] is None]"
    # Un control que no fue silencio (la app lo dice) es FAIL, y su juicio tampoco se juzga.
    sed -E '/panel=sf2 step=control /s/ ok=true (.*) pico=0.0000 (.*)$/ ok=false \1 pico=0.2000 \2 motivo=el-control-sono/' "$tmp/verde.log" > "$tmp/e4.log"
    expect "e4: un control que sono = FAIL" 1 "$tmp/e4.log" "$auto"
    expect_line "e4: sf2/control sale FAIL" '^FAIL +sf2/control ' "$tmp/e4.log" "$auto"

    # --- El seguidor (en vivo): consulta al sensor al cerrar cada ventana, y SOLO si se rindio. ---
    # Corre con el sensor `guion` (contesta de un archivo y anota cada consulta), sin TTY.
    seguir_case() {  # seguir_case <log> [linea que "llega" mientras el sensor contesta]
        WMA_SENSOR_GUION="$tmp/guion" WMA_SENSOR_LLAMADAS="$tmp/seg.calls" WMA_SENSOR_AL_CONTESTAR="${2:-}" \
            WMA_SENSOR_AL_CONTESTAR_EN="$1" \
            smoke_py seguir "$1" "$tmp/seg.sensor" "$tmp/seg.state" guion "$run" > "$tmp/seg.out" 2>&1 < /dev/null \
            || echo "seguir: rc=$?" >> "$tmp/seg.out"
    }
    seguir_reset() { rm -f "$tmp/seg.state" "$tmp/seg.sensor"; : > "$tmp/seg.calls"; : > "$tmp/seg.sensor"; }
    calls() { grep -c . "$tmp/seg.calls" || true; }
    printf 'presente\nausente\npresente\nausente\n' > "$tmp/guion"
    win sf2 > "$tmp/f-ok.log"
    seguir_reset; head -1 "$tmp/f-ok.log" > "$tmp/f-part.log"; seguir_case "$tmp/f-part.log"
    head -2 "$tmp/f-ok.log" > "$tmp/f-part.log"; seguir_case "$tmp/f-part.log"
    head -3 "$tmp/f-ok.log" > "$tmp/f-part.log"; seguir_case "$tmp/f-part.log"
    seguir_case "$tmp/f-ok.log"
    is_eq "f1 gemelo: dos ventanas rendidas, dos consultas" "$(calls)" 2
    has_line "f1: el sensor recibe estimulo, ventana y ruta del aviso" \
        '^estimulo=A4-440Hz ventana=sf2-1de2-2000ms ruta=sistema espera-s=9$' "$tmp/seg.calls"
    has_line "f1: el registro lleva sensor, veredicto y evidencia" \
        "^HARNESS-SMOKE v=1 run=$run panel=sf2 step=sensor ok=true n=1 sensor=guion veredicto=presente evidencia=[^ ]+ estimulo=A4-440Hz ruta=sistema" "$tmp/seg.sensor"
    seguir_case "$tmp/f-ok.log"
    is_eq "f1: correr otra vez no vuelve a consultar" "$(calls)" 2
    # AC-053.8: un estimulo no rendido NO se consulta (frames=0, pico=0, ok=false), ni un control invalido.
    local mode_sed
    for mode_sed in 's/ frames=[0-9]+ / frames=0 /' 's/ pico=[0-9.]+ / pico=0.0000 /' 's/ ok=true / ok=false /'; do
        sed -E "/step=estimulo /$mode_sed" "$tmp/f-ok.log" > "$tmp/f-mal.log"
        seguir_reset; head -1 "$tmp/f-mal.log" > "$tmp/f-part.log"; seguir_case "$tmp/f-part.log"
        head -3 "$tmp/f-mal.log" > "$tmp/f-part.log"; seguir_case "$tmp/f-part.log"; seguir_case "$tmp/f-mal.log"
        is_eq "f2: estimulo no rendido ($mode_sed): solo se consulta el control" "$(calls)" 1
        is_eq "f2: ... y no queda juicio de esa ventana" "$(grep -c " n=$ne " "$tmp/seg.sensor" || true)" 0
    done
    sed -E '/step=control /s/ ok=true / ok=false /' "$tmp/f-ok.log" > "$tmp/f-mal.log"
    seguir_reset; head -1 "$tmp/f-mal.log" > "$tmp/f-part.log"; seguir_case "$tmp/f-part.log"
    head -3 "$tmp/f-mal.log" > "$tmp/f-part.log"; seguir_case "$tmp/f-part.log"; seguir_case "$tmp/f-mal.log"
    is_eq "f2: un control invalido no se consulta" "$(calls)" 1
    # Sin relojes: el ORDEN del log dice si la respuesta llego a tiempo. Si mientras el sensor
    # contestaba ya se anuncio la ventana siguiente, la respuesta no vale (sin-dato, nunca PASS).
    seguir_reset; head -1 "$tmp/f-ok.log" > "$tmp/f-part.log"; seguir_case "$tmp/f-part.log"
    head -2 "$tmp/f-ok.log" > "$tmp/f-part.log"
    seguir_case "$tmp/f-part.log" "$(sed -n 3p "$tmp/f-ok.log")"
    is_eq "f3: la respuesta tardia se consulto" "$(calls)" 1
    has_line "f3: ... pero queda sin-dato" ' n=1 sensor=guion veredicto=sin-dato evidencia=respuesta-tardia:presente ' "$tmp/seg.sensor"
    # Un aviso que llega cuando la ventana ya cerro no deja escuchar: no se consulta, sin-dato.
    seguir_reset; seguir_case "$tmp/f-ok.log"
    is_eq "f4: aviso tardio: no se consulta" "$(calls)" 0
    has_line "f4: ... y queda sin-dato aviso-tarde" ' n=1 sensor=guion veredicto=sin-dato evidencia=aviso-tarde' "$tmp/seg.sensor"
    # Un sensor que contesta fuera del contrato (presente/ausente/sin-dato) no sabe: sin-dato.
    printf 'si\n' > "$tmp/guion"
    seguir_reset; head -1 "$tmp/f-ok.log" > "$tmp/f-part.log"; seguir_case "$tmp/f-part.log"
    head -2 "$tmp/f-ok.log" > "$tmp/f-part.log"; seguir_case "$tmp/f-part.log"
    has_line "f5: un veredicto fuera del contrato queda sin-dato" ' n=1 sensor=guion veredicto=sin-dato evidencia=veredicto-ilegible:si ' "$tmp/seg.sensor"
    printf 'presente\nausente\npresente\nausente\n' > "$tmp/guion"
    # `--sensor ninguno` deja todo HUMANO: el seguidor registra sin-dato y el juez no da PASS.
    {
        sed -E '/ step=sensor /d' "$tmp/verde.log"
    } > "$tmp/ninguno-app.log"
    seguir_reset
    smoke_py seguir "$tmp/ninguno-app.log" "$tmp/seg.sensor" "$tmp/seg.state" ninguno "$run" > /dev/null 2>&1 < /dev/null || true
    is_eq "ninguno: 4 juicios sin-dato" "$(grep -c 'sensor=ninguno veredicto=sin-dato ' "$tmp/seg.sensor" || true)" 4
    cat "$tmp/ninguno-app.log" "$tmp/seg.sensor" > "$tmp/ninguno.log"
    expect "ninguno: todo HUMANO, exit 3" 3 "$tmp/ninguno.log" "$auto"
    expect_no_line "ninguno: ningun juicio del sensor sale PASS" '^PASS +[a-z0-9]+/sensor-' "$tmp/ninguno.log" "$auto"

    # El aviso de una ventana no es un veredicto (como esperando-humano): no tiene fila.
    expect_no_line "escuchar: el aviso no es un paso juzgado" '/escuchar ' "$tmp/verde.log" "$auto"
    # --- AC-053.9: s => PASS (el verde), n => FAIL, ? / sin respuesta / ilegible => HUMANO. -----
    sed -E "/panel=sf2 step=sensor ok=true n=$ne /s/veredicto=[a-z-]+/veredicto=ausente/" "$tmp/verde.log" > "$tmp/s-n.log"
    expect "s-n: el sensor no oyo el estimulo = FAIL" 1 "$tmp/s-n.log" "$auto"
    expect_line "s-n: sf2/sensor-estimulo sale FAIL" '^FAIL +sf2/sensor-estimulo ' "$tmp/s-n.log" "$auto"
    sed -E "/panel=sf2 step=sensor ok=true n=$ne /s/veredicto=[a-z-]+/veredicto=sin-dato/" "$tmp/verde.log" > "$tmp/s-q.log"
    expect "s-?: sin dato del sensor = HUMANO (exit 3)" 3 "$tmp/s-q.log" "$auto"
    expect_line "s-?: sf2/sensor-estimulo sale HUMANO" '^HUMANO +sf2/sensor-estimulo ' "$tmp/s-q.log" "$auto"
    grep -v " panel=sf2 step=sensor ok=true n=$ne " "$tmp/verde.log" > "$tmp/s-0.log"
    expect "s-0: sin juicio del sensor = HUMANO (exit 3)" 3 "$tmp/s-0.log" "$auto"
    expect_line "s-0: sf2/sensor-estimulo sale HUMANO" '^HUMANO +sf2/sensor-estimulo +.*sin-juicio' "$tmp/s-0.log" "$auto"
    sed -E "/panel=sf2 step=sensor ok=true n=$ne /s/veredicto=[a-z-]+/veredicto=si/" "$tmp/verde.log" > "$tmp/s-x.log"
    expect "s-x: un veredicto ilegible = HUMANO, nunca PASS" 3 "$tmp/s-x.log" "$auto"
    sed -E "/panel=sf2 step=sensor ok=true n=$nc /s/veredicto=[a-z-]+/veredicto=sin-dato/" "$tmp/verde.log" > "$tmp/s-qc.log"
    expect "s-?c: sin dato en el control = HUMANO (exit 3)" 3 "$tmp/s-qc.log" "$auto"
    # Dos juicios para la misma ventana: el script consulto dos veces. No se elige uno.
    { cat "$tmp/verde.log"; grep " panel=sf2 step=sensor ok=true n=$ne " "$tmp/verde.log" | sed -E 's/veredicto=[a-z-]+/veredicto=ausente/'; } > "$tmp/s-2.log"
    expect "s-2: dos juicios para una ventana = FAIL" 1 "$tmp/s-2.log" "$auto"
    # Un juicio para una ventana que la app no emitio.
    { cat "$tmp/verde.log"; echo "HARNESS-SMOKE v=1 run=$run panel=sf2 step=sensor ok=true n=7 sensor=oido-humano veredicto=presente evidencia=x estimulo=A4-440Hz ruta=x"; } > "$tmp/s-7.log"
    expect "s-7: juicio de una ventana que no existe = FAIL" 1 "$tmp/s-7.log" "$auto"
    # El sensor oido-humano por una TTY de verdad (pty): s/n/? y sin respuesta; y sin TTY.
    sensor_pty() {  # sensor_pty <respuestas separadas por |, o vacio> <espera-s>
        python3 - "$0" "$1" "$2" <<'PTY'
import os, pty, select, sys, time
script, answers, wait = sys.argv[1], sys.argv[2], sys.argv[3]
pid, fd = pty.fork()
if pid == 0:
    os.execvp("bash", ["bash", script, "--sensor-consulta", "oido-humano", "A4-440Hz", "sf2-1de2-2000ms", "sistema", wait])
buf = b""
def pump(prompts, limit):
    global buf
    end = time.time() + limit
    while time.time() < end and (prompts is None or buf.count(b"[s/n/?]") < prompts):
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                chunk = os.read(fd, 4096)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
for i, a in enumerate([x for x in answers.split("|") if x]):
    pump(i + 1, 20)
    os.write(fd, a.encode() + b"\n")
pump(None, 30)
os.waitpid(pid, 0)
print(next((l.strip() for l in buf.decode(errors="replace").splitlines() if l.strip().startswith("veredicto=")), "sin-linea"))
PTY
    }
    is_eq "sensor tty: s = presente" "$(sensor_pty s 20)" "veredicto=presente sensor=oido-humano evidencia=respuesta:s"
    is_eq "sensor tty: n = ausente" "$(sensor_pty n 20)" "veredicto=ausente sensor=oido-humano evidencia=respuesta:n"
    is_eq "sensor tty: ? = sin-dato" "$(sensor_pty '?' 20)" "veredicto=sin-dato sensor=oido-humano evidencia=respuesta:?"
    is_eq "sensor tty: algo que no es s/n/? se vuelve a preguntar" "$(sensor_pty 'x|s' 20)" "veredicto=presente sensor=oido-humano evidencia=respuesta:s"
    is_eq "sensor tty: sin respuesta = sin-dato" "$(sensor_pty '' 1)" "veredicto=sin-dato sensor=oido-humano evidencia=sin-respuesta:1s"
    is_eq "sensor: sin TTY = sin-dato (no pregunta)" \
        "$(bash "$0" --sensor-consulta oido-humano A4-440Hz sf2-1de2-2000ms sistema 20 < /dev/null 2>/dev/null)" \
        "veredicto=sin-dato sensor=oido-humano evidencia=sin-tty"
    is_eq "sensor: ninguno = sin-dato" \
        "$(bash "$0" --sensor-consulta ninguno A4-440Hz sf2-1de2-2000ms sistema 20 < /dev/null 2>/dev/null)" \
        "veredicto=sin-dato sensor=ninguno evidencia=--sensor-ninguno"
    local rc_s=0
    bash "$0" --sensor-consulta loopback A4-440Hz v sistema 5 < /dev/null > /dev/null 2>&1 || rc_s=$?
    is_eq "sensor: uno que no esta en el registro = exit 2" "$rc_s" 2

    # --- AC-053.10: un "si" en un control => todo lo que juzgo ESE sensor sale BLOQUEADO. --------
    sed -E "/panel=sf2 step=sensor ok=true n=$nc /s/veredicto=[a-z-]+/veredicto=presente/" "$tmp/verde.log" > "$tmp/nd.log"
    expect "nd: el sensor oyo algo en un control = BLOQUEADO (exit 4)" 4 "$tmp/nd.log" "$auto"
    expect_line "nd: el control sale BLOQUEADO sensor-no-discrimina" '^BLOQUEADO +sf2/sensor-control +precondicion=sensor-no-discrimina ' "$tmp/nd.log" "$auto"
    expect_line "nd: el estimulo del mismo panel tambien" '^BLOQUEADO +sf2/sensor-estimulo +precondicion=sensor-no-discrimina ' "$tmp/nd.log" "$auto"
    expect_line "nd: y los de OTRO panel juzgados por el mismo sensor" '^BLOQUEADO +sf3/sensor-estimulo +precondicion=sensor-no-discrimina ' "$tmp/nd.log" "$auto"
    expect_no_line "nd: ningun juicio de ese sensor sale PASS" '^PASS +[a-z0-9]+/sensor-' "$tmp/nd.log" "$auto"
    expect_line "nd: lo que rindio el motor se juzga igual" '^PASS +sf3/estimulo ' "$tmp/nd.log" "$auto"
    expect_json "nd: en el JSON, juicio BLOQUEADO para los 4" "$tmp/nd.log" "$auto" \
        "len([x for x in j['sensor'] if x['sensor'] == 'oido-humano' and x['juicio'] == 'BLOQUEADO']) == 4"
    # Otro sensor en la misma corrida no queda invalidado por el que no discrimina.
    sed -E '/panel=sf3 step=sensor /s/sensor=oido-humano/sensor=otro/' "$tmp/nd.log" > "$tmp/nd2.log"
    expect_line "nd2: el otro sensor sigue juzgando: PASS" '^PASS +sf3/sensor-estimulo +.*sensor=otro' "$tmp/nd2.log" "$auto"
    expect_line "nd2: ... y el que no discrimina sigue BLOQUEADO" '^BLOQUEADO +sf2/sensor-estimulo ' "$tmp/nd2.log" "$auto"
    # Un "si" en un control que NO fue silencio no prueba nada del sensor: no invalida.
    sed -E "/panel=sf2 step=sensor ok=true n=$nc /s/veredicto=[a-z-]+/veredicto=presente/" "$tmp/e4.log" > "$tmp/nd3.log"
    expect_no_line "nd3: un si en un control invalido no bloquea al sensor" '^BLOQUEADO ' "$tmp/nd3.log" "$auto"
    # Heredan el BLOQUEADO de su ventana: con la precondicion del USB incumplida (a2).
    expect_line "a2: usb/sensor-estimulo hereda el BLOQUEADO de su ventana" '^BLOQUEADO +usb/sensor-estimulo +precondicion=t-host-usb ' "$tmp/a2.log"

    # --- AC-053.10: la semilla queda en el JSON y reproduce el orden; el juez lo verifica. -------
    expect_json "semilla: queda en el JSON con el orden que da" "$tmp/verde.log" "$auto" \
        "j['semilla'] == $SEM and j['orden'] == {'sf2': '$ord_sf2'.split(), 'sf3': '$ord_sf3'.split()}"
    expect_json "json: cada juicio del sensor con su ventana, su respuesta y su juicio" "$tmp/verde.log" "$auto" \
        "[x for x in j['sensor'] if x['panel'] == 'sf2' and x['n'] == $ne and x['tipo'] == 'estimulo' and x['estimulo'] == 'A4-440Hz' and x['ruta'] == 'sistema' and x['sensor'] == 'oido-humano' and x['veredicto'] == 'presente' and x['evidencia'] == 'prueba-sf2-$ne' and x['consultado'] is True and x['juicio'] == 'PASS']"
    { win sf2 sistema 1; win sf3; sens sf2; sens sf3; } | before_fin "$tmp/verde00.log" "$tmp/o1-0.log"
    met_captura | before_fin "$tmp/o1-0.log" "$tmp/o1.log"
    expect "o1: la app corrio otro orden que el de la semilla = FAIL" 1 "$tmp/o1.log" "$auto"
    expect_line "o1: sf2/orden sale FAIL" '^FAIL +sf2/orden ' "$tmp/o1.log" "$auto"
    sed -E 's/ semilla=[0-9]+ semilla-origen=[a-z]+//' "$tmp/verde.log" > "$tmp/o2.log"
    expect "o2: sin semilla registrada el orden no se reproduce = FAIL" 1 "$tmp/o2.log" "$auto"
    expect_line "o2: plan/semilla sale FAIL" '^FAIL +plan/semilla ' "$tmp/o2.log" "$auto"
    grep -v -e 'verificador=host' -e ' step=sensor ' "$tmp/verde.log" > "$tmp/s3-app.log" || true
    grep 'verificador=host' "$tmp/verde.log" > "$tmp/s3-host.log" || true
    grep ' step=sensor ' "$tmp/verde.log" > "$tmp/s3-sensor.log" || true
    cli_case "o3 gemelo: --semilla igual a la de la app = 0" 0 'resumen:' \
        "$tmp/s3-app.log" "$run" "$auto" --setup "$ficha" --host-log "$tmp/s3-host.log" --sensor-log "$tmp/s3-sensor.log" --semilla "$SEM"
    cli_case "o3: --semilla distinta de la que corrio la app = FAIL" 1 '^FAIL +plan/semilla ' \
        "$tmp/s3-app.log" "$run" "$auto" --setup "$ficha" --host-log "$tmp/s3-host.log" --sensor-log "$tmp/s3-sensor.log" --semilla 999

    # D6: un juicio de sensor que llega por el log de la APP no cuenta (cualquier app escribe el tag).
    cat "$tmp/s3-app.log" "$tmp/s3-sensor.log" > "$tmp/s3-forjado.log"
    split_sensor=""
    split_case "sensor forjado por logcat: se descarta = HUMANO" 3 "$tmp/s3-forjado.log" "$tmp/s3-host.log" \
        '^AVISO .* 4 linea\(s\) step=sensor'

    # La corrida en device: la semilla sale de /dev/urandom (31 bits), va a la app por extra y al juez,
    # y el juez recibe el registro del sensor.
    local seed1 seed2
    seed1="$(new_seed || true)"; seed2="$(new_seed || true)"
    if [[ "$seed1" =~ ^[0-9]+$ && "$seed1" -le 2147483647 && "$seed1" != "$seed2" ]]; then
        printf '  ok    %-58s\n' "semilla: 31 bits aleatorios, cambia"
    else
        printf '  MAL   %-58s %s %s\n' "semilla: 31 bits aleatorios, cambia" "$seed1" "$seed2"; failures=$((failures + 1))
    fi
    local body
    body="$(awk '/^run_device\(\) \{/ {on = 1} on {print} on && /^\}/ {exit}' "$0")"
    is_eq "semilla: la corrida la saca de new_seed" "$(grep -cE '^    seed="\$\(new_seed\)" \|\| exit 2$' <<< "$body")" 1
    is_eq "semilla: va a la app por el extra harness.smoke.semilla" "$(grep -cE -- '--es harness\.smoke\.semilla "\$seed"' <<< "$body")" 1
    is_eq "semilla y sensor: el juez los recibe" \
        "$(grep -cE '^    verdict "\$app_log" .* "\$sensor_log" --semilla "\$seed"' <<< "$body")" 1
    is_eq "seguidor: corre en el bucle y una vez mas al final" "$(grep -cE '^ +smoke_py seguir ' <<< "$body")" 2
    # AC-053.11: ya no hay lista de OIDO: lo que habia es un paso con estimulo y sensor, o se borro.
    is_eq "AC-053.11: el script no lista chequeos de oido" \
        "$(grep -cE 'Requiere O[IÍ]DO|print_ear[_]checks' "$0" || true)" 0
    # El sensor se valida antes de tocar el telefono, como --plan.
    : > "$tmp/espia.log"
    rc_s=0
    PATH="$tmp/espia:$PATH" SPY_LOG="$tmp/espia.log" ANDROID_SERIAL=falso-123 \
        bash "$0" --plan salida --sensor loopback --no-build > /dev/null 2>&1 || rc_s=$?
    is_eq "--sensor desconocido: exit 2 sin llamar a adb" "$rc_s:$(grep -c . "$tmp/espia.log" || true)" "2:0"

    # Los verificadores de HOST, contra un adb FALSO: ninguno corre sin `-s <serial>`, y cada salida
    # que no se puede leer (adb falla, rc != 0, basura, formato desconocido, tarjeta sin dueno
    # visible) da no-verificable — nunca cumplida. Los dumpsys son RECORTES de la captura real del g42
    # (scripts/smoke-device-fixtures/); las variantes se derivan de ellos con sed, nunca a mano.
    host_selftest "$tmp" "$ficha" "$run" || failures=$((failures + $?))

    # D14: la ficha REAL (sin captura), con el adb falso. NoisyPad vivo y la salida USB tomada por el
    # audioserver ya NO bloquean: son observaciones (depende: []), y los pasos se juzgan. Lo que bloquea
    # es la placa reclamada, que se ve en /dev/snd. El log es el grabado, con las precondiciones del
    # host de cada modo en su archivo aparte (como en la corrida).
    python3 -c 'import json,sys; f=json.load(open(sys.argv[1])); del f["planes"]["captura"]; json.dump(f, open(sys.argv[2], "w"))' "$real" "$tmp/ficha-usb-real.json"
    d14_case() {  # d14_case <modo del adb falso> <archivo de salida>
        FAKE_MODE="$1" FAKE_SERIAL=falso-123 FAKE_DIR="$tmp/host" \
            smoke_py host "$tmp/host/adb" falso-123 "$tmp/ficha-usb-real.json" usb "$run" "$PKG" "$tmp/host/evidencia-d14-$1" > "$tmp/d14-$1.txt" 2>&1 || true
        with_usb "$tmp/d14-$1-pre.log" 's/^//'
        grep -v 'step=precondicion' "$tmp/d14-$1-pre.log" > "$tmp/d14-$1-base.log"
        { grep 'panel=usb' "$tmp/d14-$1.txt"; pre usb "$id_permiso" true; } | before_fin "$tmp/d14-$1-base.log" "$2"
    }
    local real_ficha="$tmp/ficha-usb-real.json" id_app id_alsa id_reclamada id_placa
    # Los ids salen de la ficha por su chequeo: el script no copia ninguno (AC-053.5).
    id_by_check() { python3 -c 'import json,sys; print([p["id"] for p in json.load(open(sys.argv[1]))["planes"]["usb"]["precondiciones"] if p.get("chequeo") == sys.argv[2]][0])' "$real_ficha" "$1"; }
    id_app="$(id_by_check paquetes-sin-proceso)"; id_alsa="$(id_by_check alsa-tarjeta-libre)"
    id_reclamada="$(id_by_check usb-placa-no-reclamada)"; id_placa="$(id_by_check usb-interfaz-de-clase)"
    local id_permiso
    id_permiso="$(python3 -c 'import json,sys; print([p["id"] for p in json.load(open(sys.argv[1]))["planes"]["usb"]["precondiciones"] if p.get("ventana-humana")][0])' "$real_ficha")"
    d14_case noisypad-vivo "$tmp/d14-vivo.log"
    expect "D14: NoisyPad vivo y la salida USB tomada, exit 0" 0 "$tmp/d14-vivo.log" todo "$real_ficha"
    expect_line "D14: con NoisyPad vivo streaming-start se juzga: PASS" '^PASS +usb/streaming-start ' "$tmp/d14-vivo.log" todo "$real_ficha"
    expect_no_line "D14: nada sale BLOQUEADO con NoisyPad vivo" '^BLOQUEADO ' "$tmp/d14-vivo.log" todo "$real_ficha"
    expect_json "D14: el JSON registra la observacion de NoisyPad" "$tmp/d14-vivo.log" todo \
        "[p for p in j['precondiciones'] if p['id'] == '$id_app' and p['estado'] == 'incumplida' and p['depende'] == [] and 'noisypad' in p['evidencia']]" "$real_ficha"
    expect_json "D14: ... y la de la tarjeta ALSA tomada" "$tmp/d14-vivo.log" todo \
        "[p for p in j['precondiciones'] if p['id'] == '$id_alsa' and p['estado'] == 'incumplida' and p['depende'] == [] and 'standby=no' in p['evidencia']]" "$real_ficha"
    expect_json "D14: ... y la placa no reclamada, cumplida, bloqueante" "$tmp/d14-vivo.log" todo \
        "[p for p in j['precondiciones'] if p['id'] == '$id_reclamada' and p['estado'] == 'cumplida' and p['depende']]" "$real_ficha"
    d14_case reclamada "$tmp/d14-reclamada.log"
    expect "D14: placa reclamada bloquea (exit 4)" 4 "$tmp/d14-reclamada.log" todo "$real_ficha"
    expect_line "D14: streaming-start BLOQUEADO por la placa reclamada, con su remedio" "^BLOQUEADO +usb/streaming-start +precondicion=$id_reclamada estado=incumplida evidencia=placa-enumerada-sin-controlC1:otro-proceso-la-reclama remedio=Cerrar la app que tenga la placa tomada.*El script no sabe quien la tiene" "$tmp/d14-reclamada.log" todo "$real_ficha"
    expect_line "D14: usb/dispositivos (no depende) se juzga" '^PASS +usb/dispositivos ' "$tmp/d14-reclamada.log" todo "$real_ficha"
    d14_case sin-placa "$tmp/d14-sin-placa.log"
    expect_line "D14: sin placa bloquea la de placa enumerada" "^BLOQUEADO +usb/streaming-start +precondicion=$id_placa " "$tmp/d14-sin-placa.log" todo "$real_ficha"
    expect_no_line "D14: ... y la de placa reclamada no duplica el bloqueo" "precondicion=$id_reclamada" "$tmp/d14-sin-placa.log" todo "$real_ficha"
    # El orden (la carrera): el harness reclama la placa al arrancar, asi que el host la mira ANTES.
    local host_at start_at
    host_at="$(grep -nE '^    host_lines="\$\(smoke_py host ' "$0" | head -1 | cut -d: -f1)"
    start_at="$(grep -nE '^    adb_ shell am start -n ' "$0" | head -1 | cut -d: -f1)"
    if [[ -n "$host_at" && -n "$start_at" ]] && (( host_at < start_at )); then
        printf '  ok    %-58s\n' "D14: la verificacion de host corre ANTES del am start"
    else
        printf '  MAL   %-58s host=%s am-start=%s\n' "D14: la verificacion de host corre ANTES del am start" "${host_at:-nada}" "${start_at:-nada}"
        failures=$((failures + 1))
    fi

    rm -rf "$tmp"
    if (( failures )); then
        echo "self-test: FAIL — $failures caso(s) con el veredicto equivocado" >&2
        return 1
    fi
    echo "self-test: OK — el juez distingue verde, ok=false, faltante, sin fin, humano pendiente, humano hecho, fallo con permiso, plan recortado, no aplicable, NO-MEDIDO retirado, otra corrida, y (REQ-053) precondicion incumplida, no verificable, BLOQUEADO que no suma PASS, precedencia del exit, permiso negado vs ventana vencida, ficha invalida, verificadores de host, (D14) placa reclamada vs observaciones y (S3) estimulo no rendido sin consulta, s/n/? del sensor, controles que invalidan al sensor y el orden de la semilla"
}

# El adb falso del self-test y sus casos. Devuelve la cantidad de casos MAL.
host_selftest() {
    local tmp="$1" ficha="$2" run="$3" serial="falso-123" bad=0 mode
    local evid="$tmp/host"
    mkdir -p "$evid"
    : > "$evid/sin-s.txt"
    cat > "$evid/adb" <<'FAKE'
#!/usr/bin/env bash
# adb FALSO del self-test de smoke-device.sh. Anota toda llamada que no empiece con -s <serial>.
if [[ "${1:-}" != "-s" || "${2:-}" != "$FAKE_SERIAL" ]]; then echo "$*" >> "$FAKE_DIR/sin-s.txt"; exit 97; fi
shift 2
[[ "${1:-}" == shell && $# -eq 2 ]] || { echo "no es 'shell <cmd>': $*" >> "$FAKE_DIR/sin-s.txt"; exit 98; }
cmd="$2"
case "$FAKE_MODE" in
    falla) echo "error: device '$FAKE_SERIAL' not found" >&2; exit 1 ;;
    basura) echo "lorem ipsum"; exit 0 ;;
    denegado) echo "/system/bin/sh: Permission denied"; echo "wma-rc=1"; exit 0 ;;
esac
# `usb-falla` / `snd-rc`: solo ese comando termina mal (snd-rc con un listado legible: el rc manda).
[[ "$FAKE_MODE" == usb-falla && "$cmd" == *"dumpsys usb"* ]] && { echo "dumpsys: boom"; echo; echo "wma-rc=1"; exit 0; }
[[ "$FAKE_MODE" == snd-rc && "$cmd" == *"ls /dev/snd"* ]] && { printf 'controlC0\ncontrolC1\n'; echo; echo "wma-rc=1"; exit 0; }
case "$cmd" in
    *"dumpsys package"*)
        g=true; [[ "$FAKE_MODE" == incumplida ]] && g=false
        # Dos usuarios: el 0 (el que corre el smoke) y un perfil secundario sin el permiso.
        printf 'Packages:\n  Package [x]\n    User 0: ceDataInode=1 installed=true\n      runtime permissions:\n        android.permission.RECORD_AUDIO: granted=%s, flags=[ USER_SENSITIVE ]\n    User 10: ceDataInode=0 installed=false\n      runtime permissions:\n        android.permission.RECORD_AUDIO: granted=false, flags=[ ]\n' "$g" ;;
    *"dumpsys usb"*)
        f=cm720
        [[ "$FAKE_MODE" == incumplida || "$FAKE_MODE" == sin-placa ]] && f=hid
        [[ "$FAKE_MODE" == sin-host ]] && f=sin-host
        [[ "$FAKE_MODE" == dos-dispositivos ]] && f=dos
        cat "$FAKE_DIR/dumpsys-usb-$f.txt" ;;
    *"dumpsys media.audio_flinger"*)
        f=af-standby
        [[ "$FAKE_MODE" == incumplida || "$FAKE_MODE" == noisypad-vivo ]] && f=af-tomada
        [[ "$FAKE_MODE" == af-* ]] && f="$FAKE_MODE"
        cat "$FAKE_DIR/$f.txt" ;;
    *"ps -A"*)
        printf '  PID NAME\n    1 init\n'
        # `ps-ciego`: un ps que no ve los procesos de otros UID (sin system_server).
        [[ "$FAKE_MODE" == ps-ciego ]] || printf ' 1500 system_server\n  812 com.android.systemui\n'
        [[ "$FAKE_MODE" == incumplida ]] && printf ' 4242 com.example.ajena:servicio\n'
        [[ "$FAKE_MODE" == noisypad-vivo ]] && printf ' 4343 com.watermellonstudios.noisypad\n' ;;
    *"ls /dev/snd"*)
        # `reclamada`: la placa esta enumerada y un proceso la reclama, asi que controlC1/pcmC1* no estan.
        case "$FAKE_MODE" in
            snd-ilegible) echo "lorem ipsum" ;;
            snd-vacio) : ;;
            # El listado REAL del g42 (una linea, separada por espacios): `ls` por adb sale uno por linea.
            reclamada|sin-placa) tr -s ' ' '\n' < "$FAKE_DIR/dev-snd-reclamada.txt" ;;
            *) tr -s ' ' '\n' < "$FAKE_DIR/dev-snd-cm720.txt" ;;
        esac ;;
    *) echo "comando no previsto: $cmd" >> "$FAKE_DIR/sin-s.txt" ;;
esac
# `cortado`: la salida de un comando sano, pero la conexion se cae antes de la marca final. Lo
# que llego se PARSEA bien (ps sin el proceso ajeno) y aun asi no se puede leer: no es completa.
[[ "$FAKE_MODE" == cortado ]] && { echo "error: closed" >&2; exit 255; }
# `rc-1`: la salida de un comando sano, pero el comando termino mal. Se parsearia bien; no se lee.
[[ "$FAKE_MODE" == rc-1 ]] && { echo "wma-rc=1"; exit 0; }
echo "wma-rc=0"
FAKE
    chmod +x "$evid/adb"
    local fx=scripts/smoke-device-fixtures
    cp "$fx/dumpsys-usb-cm720.txt" "$evid/dumpsys-usb-cm720.txt"
    # Sin interfaces de clase 1 (y otro vendor): la misma captura con la placa convertida en un HID.
    sed -E 's/^( *)class=1$/\1class=3/; s/vendor_id=11145/vendor_id=1133/' "$fx/dumpsys-usb-cm720.txt" > "$evid/dumpsys-usb-hid.txt"
    sed -E '/host_manager=\{/,$d' "$fx/dumpsys-usb-cm720.txt" > "$evid/dumpsys-usb-sin-host.txt"
    # Dos dispositivos bajo host_manager: un HID primero y la CM720 despues (el recorte, repetido).
    awk -v hid="$evid/dumpsys-usb-hid.txt" 'FNR == NR { if (/^    devices=\{$/) on = 1; if (on) d = d $0 "\n"; if (on && /^    \}$/) on = 0; next }
        /^    devices=\{$/ && !done { printf "%s", d; done = 1 } { print }' "$evid/dumpsys-usb-hid.txt" "$fx/dumpsys-usb-cm720.txt" > "$evid/dumpsys-usb-dos.txt"
    # audio_flinger: el hilo USB es AudioOut_15; su linea de nivel de hilo es la UNICA de 2 espacios.
    # /dev/snd real, sin su cabecera `#`; la placa reclamada es el MISMO listado sin controlC1 ni pcmC1*.
    grep -v '^#' "$fx/dev-snd-cm720.txt" > "$evid/dev-snd-cm720.txt"
    sed -E 's/(^| )(controlC1|pcmC1[^ ]*)//g' "$evid/dev-snd-cm720.txt" > "$evid/dev-snd-reclamada.txt"
    local af="$fx/audio-flinger-cm720.txt" usb_hilo='/^Output thread .*name AudioOut_15,/,/^Output thread .*name (AudioOut_D|AudioOut_25),|^Historical/'
    cp "$af" "$evid/af-standby.txt"
    sed -E "${usb_hilo}"'s/^  Standby: yes$/  Standby: no/' "$af" > "$evid/af-tomada.txt"
    sed -E 's/^-   Standby: yes$/-   Standby: no/; s/^-   Output devices: .*$/-   Output devices: 0x4000000 (AUDIO_DEVICE_OUT_USB_HEADSET)/' "$af" > "$evid/af-cerrado-no.txt"
    sed -E "${usb_hilo}"'s/^      Standby: yes$/      Standby: no/' "$af" > "$evid/af-hal-no.txt"
    : > "$evid/af-vacio.txt"
    # El formato que cambia: `Output devices:` pasa a singular y ningun hilo vivo dice sus dispositivos.
    sed -E 's/^  Output devices: /  Output device: /' "$af" > "$evid/af-sin-dispositivos.txt"
    sed -E '/^Output thread /,$d' "$af" > "$evid/af-sin-hilos.txt"
    sed -E "${usb_hilo}"'s/AUDIO_DEVICE_OUT_USB_HEADSET/AUDIO_DEVICE_OUT_SPEAKER/' "$af" > "$evid/af-sin-usb.txt"
    sed -E "${usb_hilo}"'{/^  Standby: /d;}' "$af" > "$evid/af-sin-standby.txt"
    # Un hilo de ENTRADA con la placa: el mismo hilo USB reescrito como Input, con su Standby en no.
    sed -E "${usb_hilo}"'{s/^Output thread /Input thread /; s/^  Output devices: .*$/  Output devices:  (Empty device types)/; s/^  Input device: 0 \(AUDIO_DEVICE_NONE\)/  Input device: 0x80000000 (AUDIO_DEVICE_IN_USB_DEVICE)/; s/^  Standby: yes$/  Standby: no/;}' "$af" > "$evid/af-entrada-tomada.txt"

    host_case() {  # host_case <modo> <id> <cumplida esperada>
        local m="$1" id="$2" want="$3" got
        got="$(sed -nE "s/.* id=$id cumplida=([^ ]+) .*/\1/p" "$evid/out-$m.txt" | head -1)"
        if [[ "$got" == "$want" ]]; then
            printf '  ok    %-58s\n' "host[$m]: $id = $want"
        else
            printf '  MAL   %-58s dio %s\n' "host[$m]: $id = $want" "${got:-nada}"
            sed 's/^/        /' "$evid/out-$m.txt" | tail -6
            bad=$((bad + 1))
        fi
    }
    for mode in ok incumplida falla basura denegado cortado rc-1 ps-ciego sin-host dos-dispositivos \
        af-standby af-tomada af-cerrado-no af-hal-no af-vacio af-sin-hilos af-sin-usb af-sin-standby af-sin-dispositivos af-entrada-tomada \
        reclamada sin-placa usb-falla snd-rc snd-ilegible snd-vacio; do
        FAKE_MODE="$mode" FAKE_SERIAL="$serial" FAKE_DIR="$evid" \
            smoke_py host "$evid/adb" "$serial" "$ficha" todo "$run" "$PKG" "$evid/evidencia-$mode" \
            > "$evid/out-$mode.txt" 2>&1 || true
    done
    host_case ok t-host-cap true;  host_case ok t-usb-clase true;  host_case ok t-host-usb true;  host_case ok t-alsa true
    host_case ok t-reclamada true
    host_case incumplida t-host-cap false; host_case incumplida t-usb-clase false
    host_case incumplida t-host-usb false; host_case incumplida t-alsa false
    for mode in falla basura denegado cortado rc-1; do
        for id in t-host-cap t-usb-clase t-host-usb t-alsa t-reclamada; do host_case "$mode" "$id" no-verificable; done
    done
    host_case sin-host t-usb-clase no-verificable
    host_case dos-dispositivos t-usb-clase true     # el HID primero no tapa a la CM720 que viene despues
    host_case ps-ciego t-host-usb no-verificable
    # D14, la placa reclamada. Una regla por caso, cada una con su mutante (ver el reporte).
    host_case reclamada t-reclamada false         # placa enumerada y sin controlC1: la reclama otro proceso
    host_case sin-placa t-reclamada true          # sin placa no bloquea esta: la bloquea la de placa enumerada
    host_case sin-placa t-usb-clase false         # ... y esa si da incumplida (no se duplica el bloqueo)
    host_case usb-falla t-reclamada no-verificable  # dumpsys usb falla (el resto responde sano)
    host_case snd-rc t-reclamada no-verificable     # ls /dev/snd termina mal
    host_case snd-ilegible t-reclamada no-verificable  # ls /dev/snd no es un listado
    host_case snd-vacio t-reclamada no-verificable     # ls vacio: la ausencia de controlC1 no significa nada
    # La clase de la placa sale de la entrada de la ficha, no de un 1 fijo: con la clase 3 (el HID de
    # `sin-placa`) en las dos precondiciones, la placa ESTA y su controlC1 no, o sea reclamada.
    python3 -c 'import json,sys; f=json.load(open(sys.argv[1])); [p.update(clase=3) for p in f["planes"]["usb"]["precondiciones"] if "clase" in p]; json.dump(f, open(sys.argv[2], "w"))' "$ficha" "$evid/ficha-clase3.json"
    FAKE_MODE=sin-placa FAKE_SERIAL="$serial" FAKE_DIR="$evid" \
        smoke_py host "$evid/adb" "$serial" "$evid/ficha-clase3.json" todo "$run" "$PKG" "$evid/evidencia-clase3" > "$evid/out-clase3.txt" 2>&1 || true
    host_case clase3 t-reclamada false            # la clase de su propia entrada (3), no un 1 fijo
    host_case af-tomada t-reclamada true          # el audioserver con la salida USB tomada NO la hace incumplida
    # La tarjeta ALSA contra audio_flinger real. Cada regla tiene su caso y su mutante (ver el reporte).
    host_case af-standby t-alsa true            # el hilo USB en standby: libre
    host_case af-tomada t-alsa false            # Standby: no en el hilo USB: tomada
    host_case af-cerrado-no t-alsa true         # un `Standby: no` en un hilo "- " cerrado no cuenta
    host_case af-hal-no t-alsa true             # el de `Hal stream dump` no es el del hilo
    host_case af-vacio t-alsa no-verificable    # salida vacia: nunca cumplida por defecto
    host_case af-sin-hilos t-alsa no-verificable
    host_case af-sin-usb t-alsa true            # hay tarjeta pero ningun hilo USB
    host_case af-sin-standby t-alsa no-verificable
    host_case af-sin-dispositivos t-alsa no-verificable  # un hilo vivo sin linea de dispositivos legible
    host_case af-entrada-tomada t-alsa false    # un Input thread con IN_USB_ tambien toma la tarjeta
    for mode in af-standby af-tomada af-cerrado-no af-hal-no af-vacio af-sin-hilos af-sin-usb af-sin-standby af-sin-dispositivos af-entrada-tomada; do
        host_case "$mode" t-usb-clase true      # la captura real: la CM720 tiene interfaces de clase 1
    done
    local check
    for check in "incumplida:t-host-usb:4242" "incumplida:t-alsa:AudioOut_15:0x4000000" "af-tomada:t-alsa:AudioOut_15:0x4000000" "af-entrada-tomada:t-alsa:IN_USB_DEVICE" "af-standby:t-usb-clase:2b89:64ec:UGREEN" "dos-dispositivos:t-usb-clase:2b89:64ec:UGREEN" "af-standby:t-alsa:AudioOut_15:standby=yes" "af-sin-usb:t-alsa:sin-hilo-usb" "incumplida:t-usb-clase:046d" "reclamada:t-reclamada:placa-enumerada-sin-controlC1:otro-proceso-la-reclama" "sin-placa:t-reclamada:sin-placa:lo-cubre-placa-enumerada" "ok:t-reclamada:controlC1-presente"; do
        IFS=: read -r mode id needle <<< "$check"
        if grep -E " id=$id .*evidencia=[^ ]*$needle" "$evid/out-$mode.txt" > /dev/null; then
            printf '  ok    %-58s\n' "host[$mode]: la evidencia de $id lleva $needle"
        else
            printf '  MAL   %-58s\n' "host[$mode]: la evidencia de $id lleva $needle"; bad=$((bad + 1))
        fi
    done
    if [[ ! -s "$evid/sin-s.txt" ]]; then
        printf '  ok    %-58s\n' "host: toda llamada a adb lleva -s <serial>"
    else
        printf '  MAL   %-58s\n' "host: toda llamada a adb lleva -s <serial>"; sed 's/^/        /' "$evid/sin-s.txt" | head -5
        bad=$((bad + 1))
    fi
    # Cada linea la firma el host y es de ESTA corrida; y lo que no se pidio no se verifica.
    if grep -c . "$evid/out-ok.txt" | grep -qx 5 \
        && [[ "$(grep -c " run=$run panel=[a-z]* step=precondicion .* verificador=host$" "$evid/out-ok.txt")" == 5 ]]; then
        printf '  ok    %-58s\n' "host: 5 lineas firmadas verificador=host, run de la corrida"
    else
        printf '  MAL   %-58s\n' "host: 5 lineas firmadas verificador=host, run de la corrida"; bad=$((bad + 1))
    fi
    FAKE_MODE=ok FAKE_SERIAL="$serial" FAKE_DIR="$evid" \
        smoke_py host "$evid/adb" "$serial" "$ficha" salida,sf2 "$run" "$PKG" "$evid/evidencia-x" > "$evid/out-nada.txt" 2>&1 || true
    if [[ ! -s "$evid/out-nada.txt" ]]; then
        printf '  ok    %-58s\n' "host: un plan sin precondiciones no consulta nada"
    else
        printf '  MAL   %-58s\n' "host: un plan sin precondiciones no consulta nada"; bad=$((bad + 1))
    fi
    # Y lo que escribe el host es lo que el juez lee: el log de captura + las lineas del host en
    # modo ok + la de la app da 0; con el adb caido, 4.
    local base="$tmp/verde0.log"
    { grep -v 'panel=plan step=fin ' "$base"; grep 'panel=captura' "$evid/out-ok.txt"; \
      echo "HARNESS-SMOKE v=1 run=$run panel=captura step=precondicion ok=true id=t-app-cap cumplida=true evidencia=x"; \
      grep 'panel=plan step=fin ' "$base"; } > "$evid/juez-ok.log"
    { grep -v 'panel=plan step=fin ' "$base"; grep 'panel=captura' "$evid/out-falla.txt"; \
      echo "HARNESS-SMOKE v=1 run=$run panel=captura step=precondicion ok=true id=t-app-cap cumplida=true evidencia=x"; \
      grep 'panel=plan step=fin ' "$base"; } > "$evid/juez-falla.log"
    local got=0
    verdict_split "$evid/juez-ok.log" "$run" salida,captura,sf2,sf3 "$ficha" > /dev/null 2>&1 || got=$?
    if [[ "$got" == 0 ]]; then printf '  ok    %-58s exit 0\n' "host+juez: lo que escribe el host se juzga (ok)"
    else printf '  MAL   %-58s exit %s\n' "host+juez: lo que escribe el host se juzga (ok)" "$got"; bad=$((bad + 1)); fi
    got=0
    verdict_split "$evid/juez-falla.log" "$run" salida,captura,sf2,sf3 "$ficha" > /dev/null 2>&1 || got=$?
    if [[ "$got" == 4 ]]; then printf '  ok    %-58s exit 4\n' "host+juez: adb caido = BLOQUEADO"
    else printf '  MAL   %-58s exit %s\n' "host+juez: adb caido = BLOQUEADO" "$got"; bad=$((bad + 1)); fi
    return "$bad"
}

# app_ids_not_emitted <raiz de harness/src> <ficha>: los ids de app de la ficha que el harness NO
# emite — sin la constante en SmokePreconditions, o sin una llamada `precondition(...)` que la use.
app_ids_not_emitted() {
    local root="$1" setup="$2" id name
    for id in $(smoke_py ids "$setup" | awk '$3 == "app" {print $2}'); do
        name="$(grep -rhE --include='*.kt' "const val [A-Z_]+: String = \"$id\"" "$root" | sed -E 's/.*const val ([A-Z_]+):.*/\1/' | head -1)"
        if [[ -z "$name" ]] || ! grep -rqE --include='*.kt' "precondition\(.*SmokePreconditions\.$name\b" "$root"; then
            printf '%s ' "$id"
        fi
    done
    return 0
}

# ids_literal_in <archivo> <ficha>: los ids de la ficha que aparecen escritos en el archivo, como
# palabra (AC-053.5: el script lee la ficha, no la copia).
ids_literal_in() {
    local file="$1" setup="$2" id
    for id in $(smoke_py ids "$setup" | awk '{print $2}'); do
        grep -Eq -- "(^|[^a-z0-9-])${id}([^a-z0-9-]|\$)" "$file" && printf '%s ' "$id"
    done
    return 0
}

# ---------------------------------------------------------------------------
# La corrida en device.
# ---------------------------------------------------------------------------
run_device() {
    local plan="todo" usb_wait=120 ceiling="" out="" build=1 setup="$SETUP_DEFAULT" sensor="oido-humano"
    while (( $# )); do
        case "$1" in
            --plan) plan="$2"; shift 2 ;;
            --sensor) sensor="$2"; shift 2 ;;
            --setup) setup="$2"; shift 2 ;;
            --usb-espera-s) usb_wait="$2"; shift 2 ;;
            --techo-s) ceiling="$2"; shift 2 ;;
            --out) out="$2"; shift 2 ;;
            --no-build) build=0; shift ;;
            *) echo "opcion desconocida: $1" >&2; exit 2 ;;
        esac
    done
    # --plan viaja al `sh` del telefono: se valida ANTES de cualquier llamada a adb.
    plan_valid "$plan" || { echo "FAIL — --plan invalido: '$plan' (todo, o paneles de salida,captura,sf2,sf3,usb separados por comas)" >&2; exit 2; }
    [[ "$usb_wait" =~ ^[1-9][0-9]*$ ]] || { echo "FAIL — --usb-espera-s tiene que ser un entero > 0: '$usb_wait'" >&2; exit 2; }
    [[ -z "$ceiling" || "$ceiling" =~ ^[1-9][0-9]*$ ]] || { echo "FAIL — --techo-s tiene que ser un entero > 0: '$ceiling'" >&2; exit 2; }
    # REQ-053 S3: el sensor tambien se valida antes de tocar el telefono.
    sensor_valid "$sensor" || { echo "FAIL — --sensor invalido: '$sensor' (oido-humano o ninguno)" >&2; exit 2; }
    # El techo cubre la espera humana, la suite USB (3 tests de 5 s), las ventanas de escucha (dos por
    # panel audible, ~20 s cada una con la pausa para contestar) y el resto con holgura.
    ceiling="${ceiling:-$((usb_wait + 120 + 50 * $(audible_panels "$plan")))}"
    # REQ-053 S1: una ficha que no se entiende no puede decidir que bloquear. Se valida antes de
    # tocar nada: es un error de uso (exit 2), no un juicio.
    smoke_py validar "$setup" || exit 2

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
    # TODA llamada a adb pasa por aca (o por smoke_py host, que hace lo mismo): nunca sin -s.
    adb_() { "$adb_bin" -s "$serial" "$@"; }

    local state
    state="$(adb_ get-state 2>/dev/null || true)"
    if [[ "$state" != "device" ]]; then
        echo "FAIL — '$serial' no esta disponible (get-state: '${state:-nada}')." >&2
        exit 2
    fi

    local run seed
    run="$(new_run_id)" || exit 2
    seed="$(new_seed)" || exit 2
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
    # Cualquier app puede escribir con el tag HARNESS-SMOKE: si el logcat sabe filtrar por uid
    # (--uid=), la captura es solo del harness. Si no, se sigue como antes y la corrida queda
    # marcada (el juez igual descarta lo que se haga pasar por el host).
    local uid="" uid_note
    uid="$(adb_ shell pm list packages -U "$PKG" 2>/dev/null | uid_of_package "$PKG" || true)"
    if [[ -n "$uid" ]] && adb_ logcat -d -t 1 --uid="$uid" -s HARNESS-SMOKE:I > /dev/null 2>&1; then
        uid_note="filtro-uid=$uid"
    else
        uid=""
        uid_note="SIN-filtro-uid: el logcat no filtra por uid (o no se pudo leer el uid); la captura puede traer lineas ajenas"
    fi
    echo "=== captura: $uid_note ==="
    echo "$uid_note" > "$out/captura.txt"
    local logcat_pid=""
    start_capture() {
        # shellcheck disable=SC2046
        adb_ logcat $(logcat_capture_args "$uid") >> "$raw" 2>/dev/null &
        logcat_pid=$!
    }
    start_capture
    trap 'kill "$logcat_pid" 2>/dev/null || true' EXIT

    # REQ-053 S1 (AC-053.1, D4): las precondiciones del HOST, antes de disparar el plan. Solo
    # lectura (D3: el script no cambia nada del telefono). Cada una deja su linea en la captura,
    # firmada `verificador=host`, y su salida cruda en $out/precondiciones-host/<id>.txt. Las de la
    # app las emite la app durante la corrida; el juez cruza las dos con la ficha al final.
    echo "=== precondiciones del host (ficha: $setup) ==="
    local host_lines
    host_lines="$(smoke_py host "$adb_bin" "$serial" "$setup" "$plan" "$run" "$PKG" "$out/precondiciones-host")" \
        || { echo "FAIL — no corrieron los verificadores de host" >&2; exit 2; }
    # En su PROPIO archivo, no en la captura: logcat escribe ahi en paralelo y una linea podria
    # intercalarse. Se juntan con la captura al armar el log.
    local host_log="$out/precondiciones-host.log"
    : > "$host_log"
    if [[ -n "$host_lines" ]]; then
        printf '%s\n' "$host_lines" > "$host_log"
        sed -E 's/.* panel=([^ ]+) .* id=([^ ]+) cumplida=([^ ]+) evidencia=([^ ]+) .*/  \3  \1\/\2  \4/' <<< "$host_lines"
    else
        echo "  (el plan '$plan' no tiene precondiciones de host)"
    fi

    # REQ-050 S1 (1.4): con el dialogo de permiso PENDIENTE, lo que haria otra app instalada —
    # mandarle a la libreria el resultado del permiso con el extra que quiera (MINI-040)—. Primero
    # `true` (marcaria el device como confiable) y despues `false` (abortaria la conexion). Se manda
    # ANTES de avisarle al humano, asi el falso llega primero. `am broadcast` vuelve cuando el
    # broadcast termino de despacharse: no hace falta esperar un tiempo.
    # El resultado lo escribe el script como su propio paso (origen=adb) en la captura cruda; lo
    # que cambio o no en la app lo afirma ella en `step=permiso-falso`.
    send_forged_permission() {
        local waiting="$1" sent=0 v outp late=false
        # Si la app ya juzgo el permiso (el humano acepto antes de que este loop viera la espera),
        # el falso llega sin espera pendiente y no prueba nada: se manda igual, pero no vale.
        # (grep -c y no -q: con pipefail, el SIGPIPE de un -q que corta temprano tapa el match)
        (( $(tr -d '\r' < "$raw" | grep -F "run=$run " | grep -c 'step=permiso-falso ' || true) > 0 )) && late=true
        for v in true false; do
            # </dev/null: el stdin de la corrida es la TTY del oido humano (S3), y adb shell lo reenvia.
            outp="$(adb_ shell am broadcast -a com.watermellonstudios.audio.USB_PERMISSION -p "$PKG" --ez permission "$v" 2>&1 < /dev/null | tr -d '\r' || true)"
            echo "$outp" >> "$out/broadcast-falso.txt"
            grep -q 'Broadcast completed' <<< "$outp" && sent=$((sent + 1))
        done
        local requested=false ok=false
        grep -q 'dialogo-pedido=true' <<< "$waiting" && requested=true
        [[ "$sent" == 2 && "$requested" == true && "$late" == false ]] && ok=true
        echo "HARNESS-SMOKE v=1 run=$run panel=usb step=broadcast-falso ok=$ok origen=adb enviados=$sent de=2 dialogo-pedido=$requested tarde=$late" >> "$raw"
        echo "=== broadcast falso: $sent de 2 despachados (dialogo pedido: $requested) — $out/broadcast-falso.txt ==="
    }
    # REQ-053 S3 (D6, D7): las ventanas de escucha. La app las ANUNCIA (`escuchar`) y las cierra
    # (`estimulo`/`control`); el seguidor (smoke_py seguir) las sigue en la captura y, al cerrar
    # cada una, le pregunta al sensor. El orden sale de la semilla, que va por extra y queda en el JSON.
    local sensor_log="$out/sensor.log" follow_state="$out/seguidor.json"
    : > "$sensor_log"; rm -f "$follow_state"
    echo "=== sensor: $sensor · semilla del orden de las ventanas: $seed ==="
    if [[ "$sensor" == oido-humano && "$(audible_panels "$plan")" -gt 0 ]]; then
        echo ">>> HUMANO: en sf2, sf3 y usb vas a escuchar dos ventanas por panel. Te aviso cuando arranca cada"
        echo "    una y al cerrarla te pregunto si sonó el A4 (s/n/?). Algunas son de CONTROL y no suena nada:"
        echo "    contestá solo lo que oíste. Si no sabés, '?'."
    fi
    adb_ shell am force-stop "$PKG"
    adb_ shell am start -n "$ACTIVITY" \
        --es harness.smoke "$plan" --es harness.smoke.run "$run" \
        --es harness.smoke.usb-espera-s "$usb_wait" --es harness.smoke.semilla "$seed" | tr -d '\r'

    # Mientras corre, el log para mirar (esperando-humano, fin) junta host y captura; el que se
    # JUZGA es el de la app solo, con el del host aparte.
    local log="$out/harness-smoke-polling.log" announced=0 started=$SECONDS
    while (( SECONDS - started < ceiling )); do
        # Por Wi-Fi, un corte mata el logcat en streaming: se relanza (y lo perdido lo recupera el
        # volcado final de abajo mientras siga en el buffer).
        if ! kill -0 "$logcat_pid" 2>/dev/null; then
            echo "(logcat se corto; se relanza)"
            start_capture
        fi
        cat "$host_log" "$raw" | tr -d '\r' | grep -F "run=$run " > "$log" || true
        if (( ! announced )) && grep -q 'step=esperando-humano' "$log"; then
            announced=1
            send_forged_permission "$(grep -m1 'step=esperando-humano' "$log")"
            echo ">>> HUMANO: $(grep -m1 'step=esperando-humano' "$log" | sed -E 's/.*accion=([^ ]+).*/\1/' | tr '_' ' ')"
        fi
        # S3: avisa las ventanas nuevas y pregunta por las que cerraron. Puede bloquear lo que dura
        # una respuesta (la pausa que la app deja despues de cada ventana): el techo cuenta en SECONDS.
        smoke_py seguir "$raw" "$sensor_log" "$follow_state" "$sensor" "$run" \
            || echo "(el seguidor de ventanas fallo: lo que no se pregunto queda HUMANO)"
        if grep -q 'panel=plan step=fin ' "$log"; then
            break
        fi
        sleep 1   # WAIT-OK: polling con techo explicito (ceiling), no una espera ciega
    done

    kill "$logcat_pid" 2>/dev/null || true
    # Un ultimo volcado del buffer, combinado con lo capturado y sin duplicados (el orden de emision
    # se conserva: primero lo capturado en vivo).
    local app_log="$out/harness-smoke.log"
    { cat "$raw"; adb_ logcat -d $(logcat_capture_args "$uid") 2>/dev/null || true; } \
        | tr -d '\r' | grep -F "run=$run " | awk '!seen[$0]++' > "$app_log" || true
    # La ultima ventana pudo cerrar entre la ultima vuelta y `fin`: una pasada mas, sobre el log final.
    smoke_py seguir "$app_log" "$sensor_log" "$follow_state" "$sensor" "$run" \
        || echo "(el seguidor de ventanas fallo: lo que no se pregunto queda HUMANO)"
    local pid
    pid="$(adb_ shell pidof "$PKG" 2>/dev/null | tr -d '\r' || true)"
    if [[ -n "$pid" ]]; then
        adb_ logcat -d --pid="$pid" > "$out/logcat-app.txt" 2>/dev/null || true
        grep -E 'LibusbBackend|UsbAudio|libusb' "$out/logcat-app.txt" > "$out/logcat-usb.txt" || true
        # Evidencia (no veredicto) para D4: cuantas veces el receiver vio el resultado del permiso.
        # Con RECEIVER_NOT_EXPORTED los dos falsos no llegan: se espera 1, el del dialogo.
        echo "=== receiver USB_PERMISSION: $(grep -c 'ACTION_USB_PERMISSION received' "$out/logcat-app.txt" || true) recepciones ==="
    fi
    echo "=== registro: $out ==="
    echo

    local rc=0
    verdict "$app_log" "$run" "$plan" "$setup" "$out/harness-smoke.json" "$host_log" "$sensor_log" --semilla "$seed" || rc=$?
    echo "=== JSON de la corrida: $out/harness-smoke.json ==="
    [[ -n "$uid" ]] || echo "AVISO — $uid_note"
    print_manual_checks
    exit "$rc"
}

case "${1:-}" in
    --self-test) self_test ;;
    --veredicto)
        [[ $# -ge 4 ]] || { echo "uso: $0 --veredicto LOG RUN PLAN [--setup FICHA] [--json SALIDA] [--host-log LOG] [--sensor-log LOG] [--semilla N] [--host-en-log]" >&2; exit 2; }
        log_="$2" run_="$3" plan_="$4" setup_="$SETUP_DEFAULT" json_="" hostlog_="" sensorlog_="" seed_="" mixed_=""
        shift 4
        while (( $# )); do
            case "$1" in
                --setup) setup_="${2:?--setup necesita un archivo}"; shift 2 ;;
                --json) json_="${2:?--json necesita un archivo}"; shift 2 ;;
                --host-log) hostlog_="${2:?--host-log necesita un archivo}"; shift 2 ;;
                --sensor-log) sensorlog_="${2:?--sensor-log necesita un archivo}"; shift 2 ;;
                --semilla) seed_="${2:?--semilla necesita un numero}"; shift 2 ;;
                --host-en-log) mixed_=1; shift ;;
                *) echo "opcion desconocida: $1" >&2; exit 2 ;;
            esac
        done
        verdict "$log_" "$run_" "$plan_" "$setup_" "$json_" "$hostlog_" "$sensorlog_" \
            ${seed_:+--semilla "$seed_"} ${mixed_:+--host-en-log} ;;
    --sensor-consulta)
        # El contrato de D6, suelto: SENSOR ESTIMULO VENTANA RUTA ESPERA-S -> "veredicto=.. sensor=.. evidencia=..".
        [[ $# -eq 6 ]] || { echo "uso: $0 --sensor-consulta SENSOR ESTIMULO VENTANA RUTA ESPERA-S" >&2; exit 2; }
        smoke_py sensor "$2" "$3" "$4" "$5" "$6" ;;
    -h|--help) awk 'NR > 1 && /^set -euo/ {exit} NR > 1 {print}' "$0" ;;
    *) run_device "$@" ;;
esac
