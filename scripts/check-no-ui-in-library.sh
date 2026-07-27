#!/usr/bin/env bash
#
# WA-5.5 — guardrail: la UI del harness no puede entrar al artefacto publicado.
#
# Este repo publica `com.watermellonstudios:audio`, y buena parte de su valor es
# que NO arrastra UI. `:harness` existe para probar la libreria a mano en las dos
# plataformas, y trae Compose Multiplatform con todo su arbol.
#
# Mismo patron que WA-0.4 / check-cpp-portability.sh: una regla que dependia de
# que nadie se olvidara pasa a ser un comando que voltea el build.
#
# LAS TRES ASSERTIONS, y por que la segunda es la unica que gana su lugar:
#
#   1. Solo :audio publica. Toda task de publicacion del build tiene que
#      pertenecer a :audio.
#   2. El classpath resuelto de :audio no tiene UNA SOLA coordenada de Compose.
#   3. :audio no depende de :harness.
#
#   La 1 cubre un accidente que nadie comete: los dos workflows dicen
#   `:audio:publishAll...`, path-qualified, y el harness ni siquiera aplica
#   maven-publish. La 3 directamente NO PUEDE dispararse hoy —lo probo la
#   mutacion, ver la nota sobre ella mas abajo—.
#
#   Lo que pasa DE VERDAD es que alguien le agrega una dependencia de Compose a
#   :audio "para un helper de preview". Eso no lo ve ni la 1 ni la 3. La 2 si, y
#   por eso este script existe.
#
# NO es una assertion: que el catalogo de versiones tenga entradas de Compose.
# El catalogo declara versiones DISPONIBLES, no dependencias efectivas.
# Confundir las dos cosas lleva a gimnasia inutil; lo que decide es el classpath.
#
# Uso:  bash scripts/check-no-ui-in-library.sh

set -uo pipefail

cd "$(dirname "$0")/.."

readonly PUBLISHED_MODULE=":audio"
readonly HARNESS_MODULE=":harness"

# Coordenadas de UI que la libreria publicada no puede tener. Si el harness suma
# otro eje (Coil, Voyager, lo que sea), agregarlo aca — el gate vale lo que vale
# esta lista.
readonly FORBIDDEN='org\.jetbrains\.compose|androidx\.compose|androidx\.activity|androidx\.lifecycle:lifecycle-runtime-compose'

# Las lineas del arbol de `dependencies` empiezan con `+---` o `\---`, con
# barras de indentacion adelante. Filtrar por eso NO es cosmetico: sin ese
# filtro, el propio log de Gradle ("> Configure project :harness") dispara un
# falso positivo — pasó en el primer intento de escribir este script.
readonly TREE_LINE='^[| ]*[\+]--- |^[| ]*\\--- '

fail() { printf '\n\033[31mFAIL\033[0m — %s\n' "$1" >&2; exit 1; }
ok()   { printf '  \033[32mOK\033[0m — %s\n' "$1"; }

# Corre Gradle guardando stderr, para poder MOSTRAR su error en vez de tragarselo.
#
# No es cosmetico: la primera version mandaba stderr a /dev/null, y al mutar
# `:audio -> :harness` este script reportaba "no se encontro ninguna task de
# publicacion" mientras Gradle decia, textual, "Circular dependency between the
# following tasks". El gate acertaba y el mensaje mentia.
#
# El stderr va a un ARCHIVO y no a una variable global a proposito: run_gradle se
# llama dentro de `$( ... | grep | sort )`, o sea en un subshell, asi que una
# asignacion global adentro no vuelve al padre. La segunda version del script
# tenia exactamente ese bug y seguia imprimiendo "(sin detalle)" — lo agarro la
# misma mutacion, apuntando otra vez al harness y no a lo que se estaba probando.
GRADLE_ERR_FILE="$(mktemp)"
trap 'rm -f "$GRADLE_ERR_FILE"' EXIT

run_gradle() {
    ./gradlew "$@" --quiet 2>"$GRADLE_ERR_FILE"
}

gradle_error() {
    sed -n '/What went wrong/,$p' "$GRADLE_ERR_FILE" | head -20
}

printf 'WA-5.5 guardrail: la UI no entra al artefacto publicado\n\n'

# ---------------------------------------------------------------------------
# 1. Solo :audio publica.
# ---------------------------------------------------------------------------
publish_tasks="$(run_gradle publish --dry-run \
    | grep -oE '^:[A-Za-z0-9:_-]*:[A-Za-z0-9_]*[Pp]ublish[A-Za-z0-9_]*' \
    | sort -u)"

if [[ -z "$publish_tasks" ]]; then
    fail "no se encontro NINGUNA task de publicacion. O ${PUBLISHED_MODULE} dejo
       de publicar, o el build no configura. Lo que dijo Gradle:

$(gradle_error | sed 's/^/       /')"
fi

stray="$(printf '%s\n' "$publish_tasks" | grep -v "^${PUBLISHED_MODULE}:" || true)"
if [[ -n "$stray" ]]; then
    fail "hay tasks de publicacion fuera de ${PUBLISHED_MODULE}:

$(printf '%s\n' "$stray" | sed 's/^/       /')

       Solo ${PUBLISHED_MODULE} se publica. Si un modulo nuevo tiene que
       publicarse, es una decision de producto (versionado, consumidores,
       superficie publica) — no algo que se cuela por aplicar un plugin."
fi
ok "solo ${PUBLISHED_MODULE} publica ($(printf '%s\n' "$publish_tasks" | wc -l | tr -d ' ') tasks)"

# ---------------------------------------------------------------------------
# 2. El classpath resuelto de :audio no tiene UI. La que importa.
# ---------------------------------------------------------------------------
deps="$(run_gradle "${PUBLISHED_MODULE}:dependencies")"
if [[ -z "$deps" ]]; then
    fail "no se pudo resolver el arbol de dependencias de ${PUBLISHED_MODULE}.
       Lo que dijo Gradle:

$(gradle_error | sed 's/^/       /')"
fi

ui_hits="$(printf '%s\n' "$deps" | grep -E "$TREE_LINE" | grep -E "$FORBIDDEN" | sort -u || true)"
if [[ -n "$ui_hits" ]]; then
    fail "${PUBLISHED_MODULE} tiene UI en su classpath:

$(printf '%s\n' "$ui_hits" | sed 's/^/       /')

       El valor de esta libreria es que no arrastra UI: commonMain no tiene un
       solo import de android.*, y un consumidor no deberia heredar Compose por
       depender de un motor de audio. Si el codigo que lo trajo es un helper de
       preview o de debug, va en :harness."
fi
ok "${PUBLISHED_MODULE} no tiene coordenadas de UI en su classpath"

# ---------------------------------------------------------------------------
# 3. La dependencia va en una sola direccion.
#
# HONESTIDAD SOBRE ESTE CHECK: hoy no puede dispararse, y lo probo la mutacion.
# Como :harness depende de :audio, cualquier arista :audio -> :harness es un
# CICLO, y Gradle muere en la configuracion antes de que este check llegue a
# correr — el que lo agarra termina siendo el 1, via "Circular dependency".
#
# Se queda igual, por dos razones: deja de ser cierto en cuanto exista un segundo
# modulo de UI que no dependa de :audio (un design system, por ejemplo), y una
# assertion que documenta la direccion de la arista vale aunque hoy la sostenga
# otra capa. Lo que NO vale es dejar creer que este check es el que protege esa
# invariante — hoy la protege el grafo de tareas.
# ---------------------------------------------------------------------------
back_edge="$(printf '%s\n' "$deps" | grep -E "$TREE_LINE" | grep -F "project ${HARNESS_MODULE}" || true)"
if [[ -n "$back_edge" ]]; then
    fail "${PUBLISHED_MODULE} depende de ${HARNESS_MODULE}:

$(printf '%s\n' "$back_edge" | sed 's/^/       /')

       La dependencia va ${HARNESS_MODULE} -> ${PUBLISHED_MODULE} y nunca al
       reves. Al reves, el harness entero entra al artefacto."
fi
ok "${PUBLISHED_MODULE} no depende de ${HARNESS_MODULE}"

printf '\nOK — la UI del harness no puede entrar a lo que se publica.\n'
