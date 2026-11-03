#!/bin/bash
# compile_check.sh -- compile+lint (picat -c) every exs example, at
# high parallelism.  Verifies that the compiler mode still accepts all
# of the examples, including the native lambda syntax ($lambda(...)),
# after changes to the runtime or the compiler.
#
#   bash test/compile_check.sh [JOBS] [--bin PATH]
#
#   JOBS          parallelism (default 128)
#   --bin PATH    the binary under test (default emu/picat)
#
# picat -c is the CHECK MODE: it compiles and lints the file WITHOUT
# running it and without writing a .qi, so the sweep is read-only and
# needs no private copies (the mip/smt examples' temp files are
# pid-named since 4.0b12, and -c writes nothing anyway).  A clean file
# exits 0;  syntax/lint/unresolved-predicate errors go to stderr and
# exit nonzero.
#
# Environments (mirroring test/fdn_exs.sh):
#   pb/, satext/  -> PICATPATH=<bin dir>/../lib2, PBSOL
#   examples whose header documents PICATPATH -> compiled from the
#                  repo root with PICATPATH=<bin dir>/../lib2
#   the rest      -> env -u PICATPATH (the preloaded lib)
#
# Output: "PASS <tag>" per example, "FAIL <tag> (rc=N)" otherwise, the
# failing files' first errors in failures.detail.  Exit nonzero if any
# FAIL.  Logs: scratch/compile_check/ (one .err per example).
#
# Known failures (pre-existing lint false positives, NOT regressions;
# the examples run fine -- only their -c lint cannot resolve the calls
# through the file's own redefined constraint-operator shims;  verified
# identical on the pre-4.0b18 build 6051b61):
#   cp/udf_ops.pi and its mip/sat/smt variants:
#     unresolved_function(flatten):module(udf),
#     unresolved_predicate(#=):module(<solver>)

KNOWN_FAIL="cp.udf_ops.pi mip.udf_ops_mip.pi sat.udf_ops_sat.pi smt.udf_ops_smt.pi"

set -u

JOBS=128
BIN="${BIN:-emu/picat}"
while [ $# -gt 0 ]; do
    case "$1" in
        --bin) shift; BIN="$1" ;;
        --bin=*) BIN="${1#--bin=}" ;;
        [1-9]*) JOBS="$1" ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
PBSOL="${PBSOL:-/home/jovyan/snapshotable/q38fokus/scratch/roundingsat-src/build_my/roundingsat}"
OUT="scratch/compile_check"
mkdir -p "$OUT"
: > "$OUT/failures.detail"

echo "# compile check (picat -c): jobs=$JOBS bin=$BIN"
date "+# started: %F %T"

JOBLIST="$OUT/jobs.tsv"
: > "$JOBLIST"
# the root .pi files plus one bounded per-directory find (small dirs,
# NFS-safe) -- the same pattern as fdn_exs.sh
for f in exs/*.pi; do
    [ -f "$f" ] || continue
    name=$(basename "$f")
    printf 'root.%s\t.\t%s\n' "$name" "$name" >> "$JOBLIST"
done
for dd in exs/*/; do
    d=${dd#exs/}; d=${d%/}
    while IFS= read -r f; do
        name=$(basename "$f")
        printf '%s\t%s\t%s\n' "${d//\//_}.$name" "$d" "$name" >> "$JOBLIST"
    done < <(find "exs/$d" -maxdepth 1 -name '*.pi' 2>/dev/null | sort)
done
echo "# jobs: $(wc -l < "$JOBLIST")"

run_job() {  # tag dir name
    local tag="$1" d="$2" name="$3"
    local bin="$JOBS_BIN" out="$JOBS_OUT" pbsol="$JOBS_PBSOL" root="$JOBS_ROOT"
    local env_pre=(env -u PICATPATH)
    local cwd="$root/exs/$d" relname="$name"
    if [ "$d" = pb ] || [ "$d" = satext ]; then
        env_pre=(env -u PICATPATH PICATPATH="$(dirname "$bin")/../lib2" PBSOL="$pbsol")
    elif grep -q "PICATPATH" "$root/exs/$d/$name" 2>/dev/null; then
        env_pre=(env PICATPATH="$(dirname "$bin")/../lib2")
        cwd="$root"; relname="exs/$d/$name"
    fi
    ( cd "$cwd" && "${env_pre[@]}" timeout 120 "$bin" -c "$relname" ) \
        > "$out/$tag.err" 2>&1
    local rc=$?
    if [ "$rc" -eq 0 ]; then
        printf 'PASS %s\n' "$tag"
    elif echo " $JOBS_KNOWN_FAIL " | grep -q " $tag "; then
        printf 'KNOWN-FAIL %s (pre-existing lint false positive)\n' "$tag"
    else
        printf 'FAIL %s (rc=%s)\n' "$tag" "$rc"
        printf '== %s (rc=%s)\n' "$tag" "$rc" >> "$out/failures.detail"
        head -4 "$out/$tag.err" | sed 's/^/   /' >> "$out/failures.detail"
    fi
}
export -f run_job
export JOBS_BIN="$BIN" JOBS_OUT="$PWD/$OUT" JOBS_PBSOL="$PBSOL" JOBS_ROOT="$PWD"
export JOBS_KNOWN_FAIL="$KNOWN_FAIL"

parallel -j "$JOBS" --colsep '\t' run_job {1} {2} {3} :::: "$JOBLIST" \
    > "$OUT/summary.log" 2> "$OUT/parallel.err"

pass=$(grep -c "^PASS" "$OUT/summary.log" || true)
fail=$(grep -c "^FAIL" "$OUT/summary.log" || true)
known=$(grep -c "^KNOWN-FAIL" "$OUT/summary.log" || true)
date "+# finished: %F %T"
echo "# pass=$pass fail=$fail known-fail=$known"
if [ "$fail" -ne 0 ]; then
    echo "# failing examples (details in $OUT/failures.detail):"
    grep "^FAIL" "$OUT/summary.log" | head -20 | sed 's/^/  /'
    echo "COMPILE CHECK: FAILURES PRESENT (logs in $OUT)"
    exit 1
fi
echo "COMPILE CHECK: ALL PASS (logs in $OUT)"
