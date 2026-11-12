#!/usr/bin/env bash
# ============================================================================
# WA-0.4 / REQ-061 AC-061.7 — portability guardrail for the C++ engine.
#
# Two rules over audio/src/main/cpp (production only: tests/ and thirdparty/
# are not scanned for rule 2):
#
# 1. WA-0.4: a source file may not pull in <jni.h> or <android/...> outside the
#    layers that are Android-only by design. Those two headers are the cheapest
#    possible proxy for "this file cannot cross-compile for iOS".
#
# 2. REQ-061: a source file may not use a POSIX-only or Windows-only API outside
#    the declared layer (platform/, the backend of its platform, usb/, jni/).
#    The API arrives by header (<sys/mman.h>, <windows.h>...) or by a bare
#    symbol that lives in headers MinGW does have (lstat, link(, O_NOFOLLOW...),
#    which a header lint cannot see. Comments and string literals are ignored.
#    The list is API_TABLE below; each entry carries its group (= allowed
#    layer) and why. Headers MinGW compiles (<unistd.h>, <pthread.h>...) are NOT
#    listed on purpose: forbidding them would be policy, not detection.
#
#    Rule 2 has a ratchet baseline, scripts/cpp-portability-baseline.txt
#    (`path|API`, no line numbers): it fails on a violation NOT listed AND on a
#    listed entry that no longer reproduces. Its diff is the review.
#
# The Kotlin half of this guardrail is not a script: it is the `ios` CI job
# compiling both iOS targets on every PR.
#
# Usage:
#   scripts/check-cpp-portability.sh              # exit 1 on violation
#   scripts/check-cpp-portability.sh --self-test  # the lint itself can fail
#
# To add an exception to rule 1, extend is_allowed() WITH A COMMENT saying why.
# To add an API to rule 2, add a line to API_TABLE and a case to self_test().
# An exception without a reason is a hole, not a rule.
# ============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CPP_ROOT="$REPO_ROOT/audio/src/main/cpp"
BASELINE="$REPO_ROOT/scripts/cpp-portability-baseline.txt"

# Paths are relative to $CPP_ROOT. Rule 1 only.
is_allowed() {
    case "$1" in
        # The JNI layer IS the Android bridge — jni.h is its whole reason to exist.
        jni/*) return 0 ;;
        # Oboe and libusb backends are Android-only by decision D4 / platform.
        backends/OboeBackend.*|backends/LibusbBackend.*) return 0 ;;
        # The USB audio driver is Android-only (D4): iOS has no generic USB access.
        usb/*) return 0 ;;
        # The Android implementation of the platform abstraction, by definition.
        platform/PlatformAndroid.cpp) return 0 ;;
        # Shared logger: the <android/log.h> include sits inside a
        # `#if defined(__ANDROID__)` block, so this file compiles for iOS too.
        # It is in the shipped iOS target — see ios/CMakeLists.txt.
        platform/Logger.cpp) return 0 ;;
        # REQ-045: las palancas de test del `.so` del ARNÉS de host, que sólo existe
        # para que un test de JVM ejecute las JNIEXPORT reales — o sea que `jni.h` es
        # su razón de ser, igual que en jni/. No entra a ningún build que shippee:
        # lo compila únicamente tests/hostjni/CMakeLists.txt, y ese proyecto no tiene
        # target de iOS. Ver el encabezado del archivo.
        tests/hostjni/*) return 0 ;;
        *) return 1 ;;
    esac
}

# ---- Rule 2: the API list ---------------------------------------------------
# group|kind|name|why
#   group  posix | windows  -> which layers may use it (layer_ok / layer_text)
#   kind   H header (matched on #include, case-insensitive)
#          W bare word      C call: name followed by `(`
# A W/C name is not matched after [A-Za-z0-9_.>], so unlink( symlink(
# create_hard_link( x.link( x->link( are not `link(`.
read -r -d '' API_TABLE <<'EOF' || true
# --- group posix: allowed in platform/, usb/, jni/ and the Oboe/Libusb/CoreAudio
# --- backends. Neither header exists in MinGW (measured: Windows inventory 2026-11-10).
posix|H|sys/mman.h|mmap/mprotect/mlock; Windows has VirtualAlloc/VirtualLock
posix|H|poll.h|poll(2); Windows has WSAPoll/WaitForMultipleObjects
posix|H|sys/resource.h|setpriority/getrlimit; Windows has SetThreadPriority
# Bare symbols: they live in headers MinGW DOES have (<sys/stat.h>, <fcntl.h>,
# <unistd.h>), so the header lint cannot see them; the inventory measured them.
posix|W|lstat|no symlink-aware stat in MinGW
posix|C|link|hard links via link(2); Windows needs CreateHardLink
posix|W|O_NOFOLLOW|no such open flag in MinGW
posix|W|O_CLOEXEC|no such open flag in MinGW
posix|C|sysconf|_SC_* queries; Windows needs GetSystemInfo
# --- group windows: allowed in platform/ and the Wasapi backend only.
windows|H|windows.h|Win32 only
windows|H|mmdeviceapi.h|WASAPI device enumeration, Win32 only
windows|H|audioclient.h|WASAPI client, Win32 only
windows|H|avrt.h|MMCSS (AvSetMmThreadCharacteristics), Win32 only
windows|H|objbase.h|COM, Win32 only
windows|H|combaseapi.h|COM, Win32 only
windows|H|winsock2.h|Winsock, Win32 only
EOF

# Allowed layer per group. platform/ = the Platform* abstraction itself.
layer_text() {
    case "$1" in
        posix)   echo "platform/, usb/, jni/, backends/*Oboe*, backends/*Libusb*, backends/*CoreAudio*" ;;
        windows) echo "platform/, backends/*Wasapi*" ;;
        *)       echo "?" ;;
    esac
}

layer_ok() { # group relpath
    case "$1" in
        posix)
            case "$2" in
                platform/*|usb/*|jni/*|backends/*Oboe*|backends/*Libusb*|backends/*CoreAudio*) return 0 ;;
            esac ;;
        windows)
            case "$2" in
                platform/*|backends/*Wasapi*) return 0 ;;
            esac ;;
    esac
    return 1
}

# Reads file names (relative to its cwd) on stdin, prints `file|line|id|group|why`
# for every hit, comments and string literals excluded.
read -r -d '' AWK_PROG <<'EOF' || true
function strip(s,   out, i, n, c, d, q) {
    out = ""; n = length(s); i = 1
    while (i <= n) {
        c = substr(s, i, 1); d = substr(s, i, 2)
        if (inblock) {
            if (d == "*/") { inblock = 0; i += 2 } else i++
            continue
        }
        if (d == "//") break
        if (d == "/*") { inblock = 1; out = out " "; i += 2; continue }
        if (c == "\"" || (c == "'" && substr(s, i - 1, 1) !~ /[A-Za-z0-9_]/)) {
            q = c; out = out c; i++
            while (i <= n) {
                c = substr(s, i, 1)
                if (c == "\\") { out = out c substr(s, i + 1, 1); i += 2; continue }
                out = out c; i++
                if (c == q) break
            }
            continue
        }
        out = out c; i++
    }
    return out
}
function blankstr(s,   out, i, n, c, q) {
    out = ""; n = length(s); i = 1
    while (i <= n) {
        c = substr(s, i, 1)
        if (c == "\"" || (c == "'" && substr(s, i - 1, 1) !~ /[A-Za-z0-9_]/)) {
            q = c; out = out c; i++
            while (i <= n) {
                c = substr(s, i, 1)
                if (c == "\\") { out = out "  "; i += 2; continue }
                if (c == q) { out = out c; i++; break }
                out = out " "; i++
            }
            continue
        }
        out = out c; i++
    }
    return out
}
BEGIN {
    n = split(ENVIRON["TBL"], lines, "\n"); nt = 0; quick = "include"
    for (i = 1; i <= n; i++) {
        if (lines[i] ~ /^[ \t]*(#|$)/) continue
        split(lines[i], f, "|")
        nt++; grp[nt] = f[1]; kind[nt] = f[2]; why[nt] = f[4]
        if (f[2] != "H") quick = quick "|" f[3]
        if (f[2] == "H") {
            e = tolower(f[3]); gsub(/[.]/, "[.]", e)
            re[nt] = "^[ \t]*#[ \t]*include[ \t]*[<\"]" e "[>\"]"; id[nt] = f[3]
        } else if (f[2] == "W") {
            re[nt] = "(^|[^A-Za-z0-9_.>])" f[3] "([^A-Za-z0-9_]|$)"; id[nt] = f[3]
        } else {
            re[nt] = "(^|[^A-Za-z0-9_.>])" f[3] "[ \t]*[(]"; id[nt] = f[3] "("
        }
    }
}
FNR == 1 { inblock = 0 }
{
    if (!inblock && index($0, "/") == 0 && index($0, "\"") == 0 && index($0, "'") == 0) {
        out = $0
        isinc = (out ~ /^[ \t]*#[ \t]*include/)
    } else {
        out = strip($0)
        isinc = (out ~ /^[ \t]*#[ \t]*include/)
        if (!isinc) out = blankstr(out)
    }
    if (out !~ quick) next
    low = tolower(out)
    for (t = 1; t <= nt; t++) {
        if (kind[t] == "H") { if (isinc && low ~ re[t]) print FILENAME "|" FNR "|" id[t] "|" grp[t] "|" why[t] }
        else if (!isinc && out ~ re[t]) print FILENAME "|" FNR "|" id[t] "|" grp[t] "|" why[t]
    }
}
EOF

# Files under ROOT, same pruning for both rules.
list_files() { # ROOT
    (cd "$1" && find . \
        \( -name thirdparty -o -name .deps -o -name build -o -name .cxx \) -prune -o \
        \( -name '*.h' -o -name '*.hpp' -o -name '*.inc' \
           -o -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.mm' \) -print \
        | sed 's|^\./||' | sort)
}

FORBIDDEN='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](jni\.h|android/)'

# Spawning processes is what costs on Windows (the self-test runs check_tree ~80
# times), so the bookkeeping below is plain bash and each rule is ONE grep/awk.

# Rule 1. Prints violations (file:line:text) on stdout.
rule1() { # ROOT  (files on stdin)
    local root="$1" files hit f
    files=$(cat)
    [[ -z "$files" ]] && return 0
    while IFS= read -r hit; do
        [[ -z "$hit" ]] && continue
        f="${hit%%:*}"
        if is_allowed "$f"; then continue; fi
        printf '%s\n' "$hit"
    done <<< "$(cd "$root" && printf '%s\n' "$files" | xargs grep -nHE "$FORBIDDEN" || true)"
}

# Rule 2. Prints `file|line|id|group|why` for violations outside their layer.
rule2() { # ROOT  (files on stdin, tests/ already excluded)
    local root="$1" files hits file line id grp why
    files=$(cat)
    [[ -z "$files" ]] && return 0
    hits=$(cd "$root" && printf '%s\n' "$files" | TBL="$API_TABLE" xargs awk "$AWK_PROG" || true)
    [[ -z "$hits" ]] && return 0
    while IFS='|' read -r file line id grp why; do
        [[ -z "$file" ]] && continue
        if layer_ok "$grp" "$file"; then continue; fi
        printf '%s|%s|%s|%s|%s\n' "$file" "$line" "$id" "$grp" "$why"
    done <<< "$hits"
}

# Whole check over a tree. Info on stdout, failures on stderr; exit 1 on any.
check_tree() { # ROOT BASELINE
    local root="$1" base="$2" files prod="" scanned=0 f v1 v2 keys="" bkeys="" new="" stale="" rc=0
    local file line id grp why k l
    files=$(list_files "$root")
    while IFS= read -r f; do
        [[ -z "$f" ]] && continue
        scanned=$((scanned + 1))
        case "$f" in tests/*|*/tests/*) ;; *) prod="${prod}${f}"$'\n' ;; esac
    done <<< "$files"
    echo "portability guardrail (WA-0.4 + REQ-061): scanned $scanned files under audio/src/main/cpp"

    v1=$(printf '%s\n' "$files" | rule1 "$root")
    if [[ -n "$v1" ]]; then
        rc=1
        {
            echo
            echo "FAIL — Android-only headers outside the Android-only layers:"
            echo
            printf '%s\n' "$v1" | sed 's/^/  /'
            echo
            echo "These files are part of the portable engine and must cross-compile for"
            echo "iOS. Either move the Android-specific code behind IAudioBackend /"
            echo "wma::platform, guard it with '#if defined(__ANDROID__)', or — if the file"
            echo "is genuinely Android-only — add it to is_allowed() in this script with a"
            echo "comment explaining why."
        } >&2
    fi

    v2=$(printf '%s' "$prod" | rule2 "$root")
    while IFS='|' read -r file line id grp why; do
        [[ -z "$file" ]] && continue
        k="$file|$id"
        case $'\n'"$keys" in *$'\n'"$k"$'\n'*) ;; *) keys="${keys}${k}"$'\n' ;; esac
    done <<< "$v2"
    if [[ -f "$base" ]]; then
        while IFS= read -r l; do
            l="${l%$'\r'}"
            case "$l" in ''|'#'*|[[:space:]]*'#'*) continue ;; esac
            case $'\n'"$bkeys" in *$'\n'"$l"$'\n'*) ;; *) bkeys="${bkeys}${l}"$'\n' ;; esac
        done < "$base"
    fi
    while IFS= read -r k; do
        [[ -z "$k" ]] && continue
        case $'\n'"$bkeys" in *$'\n'"$k"$'\n'*) ;; *) new="${new}${k}"$'\n' ;; esac
    done <<< "$keys"
    while IFS= read -r k; do
        [[ -z "$k" ]] && continue
        case $'\n'"$keys" in *$'\n'"$k"$'\n'*) ;; *) stale="${stale}${k}"$'\n' ;; esac
    done <<< "$bkeys"

    if [[ -n "$new" ]]; then
        rc=1
        {
            echo
            echo "FAIL — OS-specific API outside its allowed layer (not in the baseline):"
            echo
            while IFS='|' read -r file line id grp why; do
                [[ -z "$file" ]] && continue
                case $'\n'"$new" in
                    *$'\n'"$file|$id"$'\n'*)
                        printf '  %s:%s: %s — allowed only in %s (%s)\n' \
                            "$file" "$line" "$id" "$(layer_text "$grp")" "$why" ;;
                esac
            done <<< "$v2"
            echo
            echo "Move the call behind the allowed layer (wma::platform in platform/, or the"
            echo "backend of its platform). Do NOT add it to the baseline: it only shrinks."
        } >&2
    fi
    if [[ -n "$stale" ]]; then
        rc=1
        {
            echo
            echo "FAIL — baseline entries that no longer reproduce (the ratchet only turns one way):"
            echo
            printf '%s' "$stale" | sed 's/^/  /'
            echo
            echo "Remove them from scripts/cpp-portability-baseline.txt."
        } >&2
    fi
    if [[ $rc -eq 0 ]]; then
        echo "OK — no forbidden includes, no OS-specific API outside its layer."
    fi
    return $rc
}

# ---- --self-test: the lint itself can fail ----------------------------------
self_test() {
    local n=0 bad=0 id content c
    T=$(mktemp -d)
    trap 'rm -rf "$T"' EXIT
    : > "$T/empty"

    mk() { # relpath content  (fresh tree)
        rm -rf "$T/r"; mkdir -p "$T/r/${1%/*}"
        printf '%s\n' "$2" > "$T/r/$1"
    }
    run() { # baseline -> sets RC, OUT
        if OUT=$(check_tree "$T/r" "$1" 2>&1); then RC=0; else RC=$?; fi
    }
    has() { [[ "$OUT" == *"$1"* ]]; }
    ok() { n=$((n + 1)); }
    ko() { n=$((n + 1)); bad=$((bad + 1)); echo "  self-test FAIL: $1" >&2; }

    # [id|content] — one per header and per symbol of the list.
    local cases=(
        'sys/mman.h|#include <sys/mman.h>'
        'poll.h|#include <poll.h>'
        'sys/resource.h|#include <sys/resource.h>'
        'windows.h|#include <windows.h>'
        'mmdeviceapi.h|#include <mmdeviceapi.h>'
        'audioclient.h|#include <audioclient.h>'
        'avrt.h|#include <avrt.h>'
        'objbase.h|#include <objbase.h>'
        'combaseapi.h|#include <combaseapi.h>'
        'winsock2.h|#include <winsock2.h>'
        'lstat|int r = ::lstat(p, &st);'
        'link(|int r = ::link(a, b);'
        'O_NOFOLLOW|int fl = O_NOFOLLOW;'
        'O_CLOEXEC|int fl = O_CLOEXEC;'
        'sysconf(|long n = sysconf(_SC_PAGESIZE);'
    )
    # Coverage: every row of API_TABLE has a case (a new API without one fails here).
    local tid tkind tname found
    while IFS='|' read -r _ tkind tname _; do
        case "$tkind" in H|W) tid="$tname" ;; C) tid="$tname(" ;; *) continue ;; esac
        found=0
        for c in "${cases[@]}"; do [[ "${c%%|*}" == "$tid" ]] && found=1; done
        if [[ $found -eq 1 ]]; then ok; else ko "API_TABLE row '$tid' has no self-test case"; fi
    done <<< "$(printf '%s\n' "$API_TABLE" | grep -Ev '^[[:space:]]*(#|$)')"

    for c in "${cases[@]}"; do
        id="${c%%|*}"; content="${c#*|}"
        mk "engines/Case.cpp" "$content"; run "$T/empty"
        if [[ $RC -eq 1 ]] && has "engines/Case.cpp:1: $id"; then ok; else ko "$id in engines/ must fail naming file:line and API (rc=$RC)"; fi
        if has 'allowed only in platform/'; then ok; else ko "$id message must name the allowed layer"; fi
        mk "platform/Case.cpp" "$content"; run "$T/empty"
        if [[ $RC -eq 0 ]]; then ok; else ko "$id under platform/ must pass (rc=$RC)"; fi
    done

    # Include spacing: `#include<x>` and `#  include <x>`, one header per group (+ rule 1).
    for c in 'sys/mman.h' 'windows.h'; do
        mk "engines/Sp.cpp" "#include<$c>"; run "$T/empty"
        if [[ $RC -eq 1 ]] && has "engines/Sp.cpp:1: $c"; then ok; else ko "#include<$c> (no space) must fail"; fi
        mk "engines/Sp.cpp" "#  include <$c>"; run "$T/empty"
        if [[ $RC -eq 1 ]] && has "engines/Sp.cpp:1: $c"; then ok; else ko "#  include <$c> must fail"; fi
        mk "engines/Sp.cpp" $'\t#\tinclude\t<'"$c"'>'; run "$T/empty"
        if [[ $RC -eq 1 ]] && has "engines/Sp.cpp:1: $c"; then ok; else ko "tab-spaced include of $c must fail"; fi
    done
    mk "engines/Sp.cpp" '#include<jni.h>'; run "$T/empty"
    if [[ $RC -eq 1 ]]; then ok; else ko "#include<jni.h> (no space) must fail (rule 1)"; fi
    mk "engines/Sp.cpp" '#  include <android/log.h>'; run "$T/empty"
    if [[ $RC -eq 1 ]]; then ok; else ko "#  include <android/log.h> must fail (rule 1)"; fi

    # CRLF (core.autocrlf checkouts): sources and baseline.
    mk "engines/Case.cpp" $'int r = ::lstat(p, &st);\r'; run "$T/empty"
    if [[ $RC -eq 1 ]] && has 'engines/Case.cpp:1: lstat'; then ok; else ko "CRLF source: lstat must fail"; fi
    mk "engines/Case.cpp" $'#include <sys/mman.h>\r'; run "$T/empty"
    if [[ $RC -eq 1 ]] && has 'engines/Case.cpp:1: sys/mman.h'; then ok; else ko "CRLF source: include must fail"; fi
    mk "engines/Case.cpp" $'int r = ::link(a, b);\r\nint fl = O_CLOEXEC\r'; run "$T/empty"
    if [[ $RC -eq 1 ]] && has 'engines/Case.cpp:2: O_CLOEXEC'; then ok; else ko "CRLF source: symbol at end of line must fail"; fi
    mk "engines/Neg.cpp" $'/*\r\n::lstat(p, &st);\r\n*/\r\n// O_CLOEXEC\r\nint z;\r'; run "$T/empty"
    if [[ $RC -eq 0 ]]; then ok; else ko "CRLF source: comments must stay ignored"; fi
    mk "platform/Case.cpp" $'int r = ::lstat(p, &st);\r'; run "$T/empty"
    if [[ $RC -eq 0 ]]; then ok; else ko "CRLF source under platform/ must pass"; fi
    mk "engines/Case.cpp" $'int r = ::lstat(p, &st);\r'
    printf '# header\r\nengines/Case.cpp|lstat\r\n\r\n' > "$T/bcrlf"
    run "$T/bcrlf"
    if [[ $RC -eq 0 ]]; then ok; else ko "CRLF baseline must pass (rc=$RC)"; fi
    mk "engines/Case.cpp" 'int x = 1;'
    run "$T/bcrlf"
    if [[ $RC -eq 1 ]] && has 'engines/Case.cpp|lstat' && ! has $'lstat\r'; then ok; else ko "CRLF baseline, stale entry must fail cleanly"; fi

    # Layers: the backend of the OTHER platform is not a free pass.
    mk "backends/OboeBackend.cpp" '#include <sys/mman.h>'; run "$T/empty"
    if [[ $RC -eq 0 ]]; then ok; else ko "POSIX API in backends/Oboe must pass"; fi
    mk "backends/OboeBackend.cpp" '#include <windows.h>'; run "$T/empty"
    if [[ $RC -eq 1 ]]; then ok; else ko "Windows API in backends/Oboe must fail"; fi
    mk "backends/WasapiBackend.cpp" '#include <windows.h>'; run "$T/empty"
    if [[ $RC -eq 0 ]]; then ok; else ko "Windows API in backends/Wasapi must pass"; fi
    mk "backends/WasapiBackend.cpp" '#include <sys/mman.h>'; run "$T/empty"
    if [[ $RC -eq 1 ]]; then ok; else ko "POSIX API in backends/Wasapi must fail"; fi
    mk "usb/Dev.cpp" 'int r = ::lstat(p, &st);'; run "$T/empty"
    if [[ $RC -eq 0 ]]; then ok; else ko "POSIX API in usb/ must pass"; fi
    mk "tests/CaseTest.cpp" '#include <sys/mman.h>'; run "$T/empty"
    if [[ $RC -eq 0 ]]; then ok; else ko "tests/ is not scanned by rule 2"; fi
    mk "thirdparty/x/a.c" '#include <windows.h>'; run "$T/empty"
    if [[ $RC -eq 0 ]]; then ok; else ko "thirdparty/ is not scanned"; fi

    # Negatives: what must NOT trip.
    local neg=(
        'unlink(p);'
        '::symlink(a, b);'
        'std::filesystem::create_hard_link(a, b);'
        'node->link(other);'
        'node.link(other);'
        'int my_link(int x);'
        'size_t lstat_count = 0;'
        '// ::lstat(p, &st); O_CLOEXEC sysconf(1)'
        'int x = 1;  // #include <sys/mman.h>'
        '/* ::link(a, b); */ int y = 2;'
        'const char* s = "lstat sysconf( O_NOFOLLOW";'
        '#include <unistd.h>'
        '#include <pthread.h>'
        '#include <fcntl.h>'
        '#include <sys/stat.h>'
        'int d = 1'"'"'000; // not a char literal'
    )
    for c in "${neg[@]}"; do
        mk "engines/Neg.cpp" "$c"; run "$T/empty"
        if [[ $RC -eq 0 ]]; then ok; else ko "false positive on: $c"; fi
    done
    mk "engines/Neg.cpp" $'/*\n::lstat(p, &st);\n#include <sys/mman.h>\n*/\nint z;'; run "$T/empty"
    if [[ $RC -eq 0 ]]; then ok; else ko "false positive inside a multi-line block comment"; fi
    mk "engines/Neg.cpp" $'/* x */ int a;\n::lstat(p, &st);'; run "$T/empty"
    if [[ $RC -eq 1 ]] && has 'engines/Neg.cpp:2: lstat'; then ok; else ko "code after a closed block comment must still be scanned"; fi

    # Rule 1 still works.
    mk "engines/J.cpp" '#include <jni.h>'; run "$T/empty"
    if [[ $RC -eq 1 ]]; then ok; else ko "jni.h outside jni/ must fail"; fi
    mk "jni/J.cpp" '#include <jni.h>'; run "$T/empty"
    if [[ $RC -eq 0 ]]; then ok; else ko "jni.h under jni/ must pass"; fi

    # Ratchet, both directions.
    mk "engines/Case.cpp" 'int r = ::lstat(p, &st);'
    printf '# header\nengines/Case.cpp|lstat\n' > "$T/b1"
    run "$T/b1"
    if [[ $RC -eq 0 ]]; then ok; else ko "listed violation must pass (rc=$RC)"; fi
    run "$T/empty"
    if [[ $RC -eq 1 ]]; then ok; else ko "unlisted violation must fail"; fi
    printf 'engines/Case.cpp|lstat\nengines/Case.cpp|sysconf(\n' > "$T/b2"
    run "$T/b2"
    if [[ $RC -eq 1 ]] && has 'engines/Case.cpp|sysconf(' && has 'no longer reproduce'; then ok; else ko "stale baseline entry must fail naming it (rc=$RC)"; fi
    mk "engines/Case.cpp" 'int x = 1;'
    run "$T/b1"
    if [[ $RC -eq 1 ]]; then ok; else ko "baseline entry for a clean file must fail (stale)"; fi
    printf 'engines/Case.cpp|O_CLOEXEC\n' > "$T/b3"
    mk "engines/Case.cpp" 'int r = ::lstat(p, &st);'
    run "$T/b3"
    if [[ $RC -eq 1 ]] && has 'engines/Case.cpp:1: lstat' && has 'engines/Case.cpp|O_CLOEXEC'; then ok; else ko "wrong API in baseline: new AND stale must both fail"; fi

    if [[ $bad -gt 0 ]]; then
        echo "self-test FAILED: $bad of $n checks" >&2
        return 1
    fi
    echo "self-test OK ($n checks)"
}

main() {
    if [[ "${1:-}" == "--self-test" ]]; then
        self_test
        return
    fi
    if [[ ! -d "$CPP_ROOT" ]]; then
        echo "error: C++ root not found at $CPP_ROOT" >&2
        return 2
    fi
    check_tree "$CPP_ROOT" "$BASELINE"
}

main "$@"
