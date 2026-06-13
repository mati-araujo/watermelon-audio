#!/usr/bin/env bash
# ============================================================================
# Benchmark the agent edit -> build -> test loop for the host C++ test suite,
# so build CONFIGURATIONS can be compared over time. Linux/macOS/CI companion
# to bench-agent-loop.ps1.
#
#   cold_total   wipe build dir, configure + build + ctest   (1 sample)
#   incremental  touch one test .cpp, build + run dsp suite   (RUNS samples)
#   test_only    ctest                                        (RUNS samples)
#
# Each invocation = one config. Rows append to docs/agent-loop-bench.csv with
# a label, keyed so you re-run after a change and diff. The googletest .deps
# cache is kept warm (isolates generator/parallelism, not network).
#
# Usage:
#   scripts/bench-agent-loop.sh -l ninja-j8 -g Ninja -j 8
#   scripts/bench-agent-loop.sh -l make-j4  -g "Unix Makefiles" -j 4
#   scripts/bench-agent-loop.sh -l with-stress -s        # include 5s stress test
# ============================================================================
set -euo pipefail

GEN="Ninja"; JOBS=0; STRESS=0; RUNS=5; LABEL=""
while getopts "g:j:r:l:s" opt; do
  case $opt in
    g) GEN="$OPTARG" ;;
    j) JOBS="$OPTARG" ;;
    r) RUNS="$OPTARG" ;;
    l) LABEL="$OPTARG" ;;
    s) STRESS=1 ;;
    *) echo "usage: $0 [-g generator] [-j jobs] [-r runs] [-l label] [-s]"; exit 2 ;;
  esac
done

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="$REPO_ROOT/audio/src/main/cpp/tests"
BUILD_DIR="$SRC_DIR/.bench-build"
DEPS_DIR="$SRC_DIR/.deps"
CSV="$REPO_ROOT/docs/agent-loop-bench.csv"
TOUCH_FILE="$SRC_DIR/../dsp/tests/test_dsp_math.cpp"
STRESS_RE='ResizableRingBufferTest.StressWriterReaderAndRepeatedResize'
DSP_RE='Dsp|BiquadFilter|ParameterSmoother|SoftClipper|DelayLine|Lfo|DcBlocker|LockFreeRingBuffer'

CFG=(-S "$SRC_DIR" -B "$BUILD_DIR" -G "$GEN" -DCMAKE_BUILD_TYPE=Debug "-DFETCHCONTENT_BASE_DIR=$DEPS_DIR")
BUILD=(--build "$BUILD_DIR"); [[ "$JOBS" -gt 0 ]] && BUILD+=(-j "$JOBS")
CTEST=(--test-dir "$BUILD_DIR" --output-on-failure); [[ "$STRESS" -eq 0 ]] && CTEST+=(-E "$STRESS_RE")
COMPILER="$(${CXX:-g++} -dumpversion 2>/dev/null | head -1 | sed 's/^/gcc-/')"

now() { date +%s.%N; }
elapsed() { awk "BEGIN{printf \"%.3f\", $2-$1}"; }
median() { printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END{n=NR; if(n%2) printf "%.3f",a[int((n+1)/2)]; else printf "%.3f",(a[n/2]+a[n/2+1])/2}'; }
minv() { printf '%s\n' "$@" | sort -n | head -1; }
maxv() { printf '%s\n' "$@" | sort -n | tail -1; }

echo "Generator: $GEN  Compiler: $COMPILER  Jobs: $([[ $JOBS -gt 0 ]] && echo $JOBS || echo auto)  Stress: $STRESS  Runs: $RUNS"

# cold_total
rm -rf "$BUILD_DIR"
t0=$(now); cmake "${CFG[@]}" >/dev/null 2>&1; cmake "${BUILD[@]}" >/dev/null 2>&1; ctest "${CTEST[@]}" >/dev/null 2>&1 || true; t1=$(now)
COLD=$(elapsed "$t0" "$t1")
echo "cold_total : ${COLD}s"

# incremental
INC=()
for _ in $(seq 1 "$RUNS"); do
  touch "$TOUCH_FILE"
  a=$(now); cmake "${BUILD[@]}" >/dev/null 2>&1; ctest "${CTEST[@]}" -R "$DSP_RE" >/dev/null 2>&1 || true; b=$(now)
  INC+=("$(elapsed "$a" "$b")")
done
echo "incremental: $(median "${INC[@]}")s (min $(minv "${INC[@]}") / max $(maxv "${INC[@]}"))"

# test_only
TST=()
for _ in $(seq 1 "$RUNS"); do
  a=$(now); ctest "${CTEST[@]}" >/dev/null 2>&1 || true; b=$(now)
  TST+=("$(elapsed "$a" "$b")")
done
echo "test_only  : $(median "${TST[@]}")s (min $(minv "${TST[@]}") / max $(maxv "${TST[@]}"))"

# append CSV
GITREV="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo nogit)"
TS="$(date +%Y-%m-%dT%H:%M:%S)"
[[ -f "$CSV" ]] || echo "timestamp,git_rev,label,generator,compiler,jobs,stress,scenario,runs,median_s,min_s,max_s" > "$CSV"
{
  echo "$TS,$GITREV,$LABEL,$GEN,$COMPILER,$JOBS,$STRESS,cold_total,1,$COLD,$COLD,$COLD"
  echo "$TS,$GITREV,$LABEL,$GEN,$COMPILER,$JOBS,$STRESS,incremental,$RUNS,$(median "${INC[@]}"),$(minv "${INC[@]}"),$(maxv "${INC[@]}")"
  echo "$TS,$GITREV,$LABEL,$GEN,$COMPILER,$JOBS,$STRESS,test_only,$RUNS,$(median "${TST[@]}"),$(minv "${TST[@]}"),$(maxv "${TST[@]}")"
} >> "$CSV"
echo "Appended 3 rows to $CSV"
