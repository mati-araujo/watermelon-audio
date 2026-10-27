#!/usr/bin/env bash
# REQ-055 S3 — firma (hardened runtime), DMG, notarización y staple del .app de la espiga.
#
#   bash studio-spike/packaging/sign-notarize.sh \
#       --app studio-spike/build/compose/binaries/main/app/NoisyPadStudioSpike.app \
#       --identity "Developer ID Application: <Nombre> (<TEAM>)" \
#       --keychain-profile noisypad-notary \
#       --out <dir>
#
# --identity -            firma AD-HOC (sin team): sirve para verificar estructura y
#                         hardened runtime; Gatekeeper la rechaza y no se notariza.
# --identity "Apple Development: …"   mismo team en todo el bundle: prueba library
#                         validation; Gatekeeper la rechaza (no es Developer ID).
# --keychain-profile P    perfil de `xcrun notarytool store-credentials` (lo crea el humano).
#                         Sin él no se notariza ni se abrocha (staple).
# --entitlements F        default: packaging/entitlements.plist (mínimos, sin
#                         disable-library-validation).
#
# Qué firma, de adentro hacia afuera (codesign --deep no reemplaza esto: no firma lo que
# vive dentro de un .jar, y la notarización sí lo inspecciona):
#   1. los Mach-O dentro de los .jar (skiko trae dylibs), re-empaquetados en su jar;
#   2. todo Mach-O suelto del bundle (runtime, skiko, el motor y el shim) salvo el ejecutable;
#   3. el ejecutable principal y el bundle, con los entitlements.
# No escribe contraseñas ni crea certificados ni perfiles.
set -euo pipefail

APP="" IDENTITY="" PROFILE="" OUT="" ENT=""
HERE="$(cd "$(dirname "$0")" && pwd)"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --app) APP="$2"; shift 2 ;;
        --identity) IDENTITY="$2"; shift 2 ;;
        --keychain-profile) PROFILE="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --entitlements) ENT="$2"; shift 2 ;;
        *) echo "argumento desconocido: $1" >&2; exit 2 ;;
    esac
done
[[ -n "$APP" && -d "$APP" ]] || { echo "falta --app <.app existente>" >&2; exit 2; }
[[ -n "$IDENTITY" ]] || { echo "falta --identity (o '-' para ad-hoc)" >&2; exit 2; }
[[ -n "$OUT" ]] || { echo "falta --out <dir>" >&2; exit 2; }
ENT="${ENT:-$HERE/entitlements.plist}"
[[ -f "$ENT" ]] || { echo "no existe $ENT" >&2; exit 2; }
if [[ -n "$PROFILE" && "$IDENTITY" == "-" ]]; then
    echo "una firma ad-hoc no se puede notarizar: sacá --keychain-profile o pasá un Developer ID" >&2
    exit 2
fi

NAME="$(basename "$APP" .app)"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"   # absoluto: el paso 1 entra con cd a un temporal
SIGNED="$OUT/$NAME.app"
rm -rf "$SIGNED"
ditto "$APP" "$SIGNED"

# Developer ID lleva sello de tiempo seguro (la notarizacion lo exige); ad-hoc no lo
# admite, y una identidad de desarrollo (prueba de library validation) no lo necesita.
TS=(--timestamp=none)
[[ "$IDENTITY" == "Developer ID Application"* ]] && TS=(--timestamp)
sign() { codesign --force "${TS[@]}" --options runtime --sign "$IDENTITY" "$@"; }

is_macho() { file -b "$1" | grep -q 'Mach-O'; }

echo "== 1. Mach-O dentro de los .jar"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
while IFS= read -r -d '' jar; do
    entries="$(unzip -Z1 "$jar" | grep -E '\.(dylib|jnilib)$' || true)"
    [[ -z "$entries" ]] && continue
    work="$TMP/$(basename "$jar").d"
    mkdir -p "$work"
    (cd "$work" && unzip -q -o "$jar" $entries)
    while IFS= read -r e; do
        sign "$work/$e"
        echo "   firmado $(basename "$jar")!/$e"
    done <<< "$entries"
    (cd "$work" && zip -q "$jar" $entries)
done < <(find "$SIGNED/Contents" -name '*.jar' -print0)

echo "== 2. Mach-O sueltos (de adentro hacia afuera)"
MAIN="$SIGNED/Contents/MacOS/$NAME"
while IFS= read -r -d '' f; do
    [[ "$f" == "$MAIN" ]] && continue
    if is_macho "$f"; then
        sign "$f"
        echo "   firmado ${f#$SIGNED/}"
    fi
done < <(find "$SIGNED/Contents" -type f -print0 | sort -z -r)
# El runtime embebido es un bundle en sí (Contents/runtime/Contents/Info.plist).
if [[ -d "$SIGNED/Contents/runtime" ]]; then
    sign "$SIGNED/Contents/runtime"
fi

echo "== 3. ejecutable y bundle, con $(basename "$ENT")"
sign --entitlements "$ENT" "$MAIN"
sign --entitlements "$ENT" "$SIGNED"

echo "== verificación"
codesign --verify --deep --strict --verbose=2 "$SIGNED"
codesign -dvv "$SIGNED" 2>&1 | grep -E 'Authority|TeamIdentifier|flags|Signature' || true
spctl -a -vv "$SIGNED" || echo "   (spctl: rechazado — ver arriba)"

echo "== DMG"
DMG="$OUT/$NAME.dmg"
rm -f "$DMG"
hdiutil create -quiet -volname "$NAME" -srcfolder "$SIGNED" -ov -format UDZO "$DMG"
[[ "$IDENTITY" != "-" ]] && codesign --force "${TS[@]}" --sign "$IDENTITY" "$DMG"
echo "   $DMG"

if [[ -z "$PROFILE" ]]; then
    echo "== notarización: SALTEADA (sin --keychain-profile)"
    exit 0
fi
echo "== notarización ($PROFILE)"
xcrun notarytool submit "$DMG" --keychain-profile "$PROFILE" --wait
xcrun stapler staple "$SIGNED"
xcrun stapler staple "$DMG"
spctl -a -vv "$SIGNED"
spctl -a -vv -t open --context context:primary-signature "$DMG"
