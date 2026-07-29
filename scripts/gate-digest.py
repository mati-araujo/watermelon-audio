#!/usr/bin/env python3
# ============================================================================
# El digest de contenido sobre el que se apoya la atestacion local (docs/ci/).
#
# UNA sola implementacion, usada por los DOS lados: scripts/gate.sh la emite y
# scripts/verify-attestation.sh la recomputa en el CI. Si hubiera dos, el dia
# que diverjan el CI aceptaria una atestacion que no corresponde y nadie se
# entera.
#
# Que cubre: los archivos trackeados que NO son prosa, o sea el complemento
# EXACTO del filtro que ya usa el job `changes` de ci.yml para decidir si vale
# la pena compilar algo. No es una lista enumerada a mano a proposito: una
# lista se puede quedar corta, y quedarse corta acá significa falso verde.
# Sobre-cubrir sólo cuesta re-correr un gate que ibas a correr igual.
#
# Se excluye .github/local-gate.json: la atestacion no puede estar dentro de su
# propio digest.
#
# Los hashes salen del INDICE de git (`git ls-files -s` da el blob sha1 de cada
# archivo), no de leer el disco. Tres razones: es exactamente el contenido que
# el CI va a checkoutear, no depende de que exista sha256sum o shasum en el
# runner, y no hace falta abrir 685 archivos.
#
# Usage:
#   gate-digest.py            # imprime el digest
#   gate-digest.py --verbose  # digest + cantidad de archivos + los primeros
# ============================================================================
import hashlib
import re
import subprocess
import sys

# El MISMO filtro que ci.yml: '(\.md$|^docs/|^\.gitignore$|^LICENSE$)'. Si uno
# de los dos cambia y el otro no, un cambio de prosa empieza a invalidar
# atestaciones (ruido) o un cambio de codigo deja de invalidarlas (falso verde).
PROSE = re.compile(r"(\.md$|^docs/|^\.gitignore$|^LICENSE$)")

# La atestacion misma. No puede cubrirse a si misma.
ATTESTATION = ".github/local-gate.json"


def tracked_entries():
    """(mode, blob_sha, path) de cada archivo trackeado no-prosa, ordenado."""
    out = subprocess.run(
        ["git", "ls-files", "-s", "-z"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout

    entries = []
    for record in out.split("\0"):
        if not record:
            continue
        # Formato: "<mode> <sha1> <stage>\t<path>". El path puede tener espacios,
        # de ahi el split por tab y no por espacio.
        meta, _, path = record.partition("\t")
        mode, sha, _stage = meta.split()
        if PROSE.search(path) or path == ATTESTATION:
            continue
        entries.append((mode, sha, path))

    # LC_ALL=C: el orden tiene que ser byte a byte, no dependiente de locale.
    entries.sort(key=lambda e: e[2].encode())
    return entries


def digest(entries):
    h = hashlib.sha256()
    for mode, sha, path in entries:
        # El modo entra: cambiar el bit de ejecutable de un script cambia lo que
        # el CI puede correr, aunque el contenido sea identico.
        h.update(f"{mode} {sha} {path}\n".encode())
    return h.hexdigest()


# El filtro de prosa vive en DOS lugares: el `PROSE` de acá y el `grep -qvE` del
# job `changes` de ci.yml. Tienen que ser idénticos, y la dirección peligrosa de
# la deriva no es simétrica: si el PROSE de acá se hace MÁS amplio que el de
# ci.yml, un cambio de código pasa a contarse como prosa, queda FUERA del digest,
# y el gate lo atesta sin cubrirlo — el falso verde exacto que este esquema
# existe para evitar. Nada estructural los mantiene sincronizados (es la deuda de
# "N copias del mapeo" que en NoisyPad mordió tres veces), así que lo chequea
# este modo, que corre en el job `cpp-tests`. Vive acá, en el archivo que OWNea
# `PROSE`, para que la fuente del patrón y su verificación no se separen.
CI_YML = ".github/workflows/ci.yml"


def check_sync():
    try:
        text = open(CI_YML, encoding="utf-8").read()
    except OSError as exc:
        print(f"no se pudo leer {CI_YML}: {exc}", file=sys.stderr)
        return 1

    matches = re.findall(r"grep -q[a-zA-Z]*E '([^']*)'", text)
    if len(matches) != 1:
        # 0 → el job `changes` cambió de forma y este chequeo quedó ciego;
        # >1 → ambiguo. En los dos casos: fail-closed, no adivinar.
        print(
            f"esperaba exactamente un `grep -q..E '...'` en {CI_YML}, hay {len(matches)}",
            file=sys.stderr,
        )
        return 1

    ci_pattern = matches[0]
    if ci_pattern != PROSE.pattern:
        print("el filtro de prosa DIVERGIÓ entre ci.yml y gate-digest.py:", file=sys.stderr)
        print(f"  ci.yml (job changes): {ci_pattern!r}", file=sys.stderr)
        print(f"  gate-digest.py PROSE: {PROSE.pattern!r}", file=sys.stderr)
        return 1

    print(f"filtro de prosa sincronizado: {PROSE.pattern}")
    return 0


def main():
    if "--check-sync" in sys.argv:
        sys.exit(check_sync())

    entries = tracked_entries()
    print(digest(entries))
    if "--verbose" in sys.argv:
        print(f"archivos: {len(entries)}", file=sys.stderr)
        for mode, sha, path in entries[:5]:
            print(f"  {mode} {sha[:8]} {path}", file=sys.stderr)
        print("  ...", file=sys.stderr)


if __name__ == "__main__":
    main()
