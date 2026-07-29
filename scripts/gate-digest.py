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


def main():
    entries = tracked_entries()
    print(digest(entries))
    if "--verbose" in sys.argv:
        print(f"archivos: {len(entries)}", file=sys.stderr)
        for mode, sha, path in entries[:5]:
            print(f"  {mode} {sha[:8]} {path}", file=sys.stderr)
        print("  ...", file=sys.stderr)


if __name__ == "__main__":
    main()
