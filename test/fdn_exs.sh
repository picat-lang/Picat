#!/bin/bash
# fdn_exs.sh -- the fdn transparency sweep over all Picat-src/exs examples.
# Every example runs twice on the SAME binary: with the fdn hook (default)
# and with FDN=0 (exactly the stock behavior). The verdict compares the
# return codes and the normalized outputs. The pb and satext examples run
# with the pb environment (PICATPATH=lib2 + PBSOL); php.pi and
# php_matrix.pi get the arguments "8 9".
#
#   bash fdn_exs.sh [JOBS]
#
# The runs go through `parallel -j JOBS` (default 32), each job running its
# fdn-on and FDN=0 side back to back on a dedicated core. Two fresh writable
# copies of exs per side; every run compiles from source.

JOBS=${1:-32}
PICAT_SRC=${PICAT_SRC:-/home/jovyan/snapshotable/q38fokus/Picat-src}
BIN=${BIN:-$PICAT_SRC/emu/picat}
PBSOL_BIN=${PBSOL:-/home/jovyan/snapshotable/q38fokus/scratch/roundingsat-src/build_my/roundingsat}
TIMEOUT=${TIMEOUT:-30}
TIMEOUT_PB=${TIMEOUT_PB:-120}
OUT=${FDN_EXS_OUT:-$(mktemp -d "/dev/shm/fdnexs.XXXXXX")}
mkdir -p "$OUT"

unset SATEXT_SOLVER SATEXT_NO_FALLBACK SATEXT_PRT_MIN SATEXT_PRT_STATS
unset SATEXT_PRT_BUDGET_MS SATEXT_SHIM_MIN

echo "# fdn exs sweep: jobs=$JOBS timeout=$TIMEOUT bin=$BIN"
echo "# outputs in $OUT"

# fresh writable copies: one private directory per example per side (the
# mip/smt examples write __tmp.lp/__tmp.smt2 relative to the CWD, so
# examples sharing a directory would race on those files)
rm -rf "$OUT/on" "$OUT/off"
JOBLIST="$OUT/jobs.tsv"
: > "$JOBLIST"
for d in cp debug euler fdn mip sat satext smt planner pb nn; do
    while IFS= read -r f; do
        rel=${f#"$PICAT_SRC/exs/"}
        sub="${rel%/*}"
        name="$(basename "$f")"
        for side in on off; do
            mkdir -p "$OUT/$side/$sub.$name"
            find "$PICAT_SRC/exs/$sub" -maxdepth 1 -type f \
                ! -name '*.qi' ! -name '*.o' ! -name '*.pdf' ! -name '*.png' ! -name '*.svg' \
                -exec cp {} "$OUT/$side/$sub.$name/" \;
        done
        printf '%s\t%s\t%s\t%s\n' "${sub//\//_}.$name" "$sub" "$name" "$sub.$name" >> "$JOBLIST"
    done < <(find "$PICAT_SRC/exs/$d" -maxdepth 1 -name '*.pi' | sort)
done
for f in "$PICAT_SRC"/exs/nn/*/*.pi; do
    rel=${f#"$PICAT_SRC/exs/"}
    sub="${rel%/*}"
    name="$(basename "$f")"
    for side in on off; do
        mkdir -p "$OUT/$side/$sub.$name"
        find "$PICAT_SRC/exs/$sub" -maxdepth 1 -type f \
            ! -name '*.qi' ! -name '*.o' ! -name '*.pdf' ! -name '*.png' ! -name '*.svg' \
            -exec cp {} "$OUT/$side/$sub.$name/" \;
    done
    printf '%s\t%s\t%s\t%s\n' "${sub//\//_}.$name" "$sub" "$name" "$sub.$name" >> "$JOBLIST"
done
echo "# jobs: $(wc -l < "$JOBLIST")"

norm() {
    grep -vE 'second|Cbc00|Clp00|Coin0|_wall|Wallclock|wallclock|CPU time|cpu time|Wall time|wall time|Stack\+Heap:|^Program:|^Trail:|^Table:|^Memory manager:|^[[:space:]]*(Stack in use|Heap in use|In use|Symbols|Subgoals|Answers|Terms|GC|Expansions):' "$1" 2>/dev/null | sed -E 's/in [0-9]+ ms/in _ ms/g; s/starting at [0-9]+ ms/starting at _ ms/g; s/\([0-9]+ ms\)/(_ ms)/g; s/time [0-9.]+/time _/g; s/Presolve [0-9.]+/Presolve _/g'
}
export -f norm

run_job() {  # tag sub name dir
    local tag="$1" d="$2" name="$3" dir="$4" tmo
    local bin="$JOBS_BIN" out="$JOBS_OUT" t="$JOBS_TIMEOUT" tp="$JOBS_TIMEOUT_PB" pbsol="$JOBS_PBSOL"
    local src="$JOBS_SRC/exs/$d/$name"
    tmo=$t; [ "$d" = pb ] || [ "$d" = satext ] && tmo=$tp
    local args=""
    case "$name" in php.pi|php_matrix.pi) args="8 9" ;; esac
    local env_pre=(env -u PICATPATH)
    if [ "$d" = pb ] || [ "$d" = satext ]; then
        env_pre=(env -u PICATPATH PICATPATH="$(dirname "$bin")/../lib2" PBSOL="$pbsol")
    fi
    local cwd_on="$out/on/$dir" cwd_off="$out/off/$dir" relname="$name"
    if [ "$d" != pb ] && [ "$d" != satext ] && [ -f "$src" ] && grep -q "PICATPATH" "$src" 2>/dev/null; then
        # the example documents a PICATPATH invocation in its header ("PICATPATH=lib2",
        # "PICATPATH=<picat>/lib2", "PICATPATH=lib", all from the repo root or exs/):
        # run it from the repo root with PICATPATH=lib2 (empirically works for all of
        # them), otherwise it load-errors identically on both sides and the verdict
        # would be a vacuous "same (both rc=1)" while no test ever executed
        env_pre=(env PICATPATH="$(dirname "$bin")/../lib2")
        cwd_on="$JOBS_SRC"; cwd_off="$JOBS_SRC"; relname="exs/$d/$name"
    fi
    ( cd "$cwd_on" && "${env_pre[@]}" timeout "$tmo" "$bin" "$relname" $args ) > "$out/$tag.on.out" 2>&1
    local erc=$?
    ( cd "$cwd_off" && "${env_pre[@]}" FDN=0 timeout "$tmo" "$bin" "$relname" $args ) > "$out/$tag.off.out" 2>&1
    local orc=$?
    if [ "$erc" -eq 124 ] && [ "$orc" -eq 124 ]; then
        printf '%-10s %-40s same (both timeout)\n' "$d" "$name"
    elif [ "$erc" -ne "$orc" ]; then
        printf '%-10s %-40s RC-DIFFERS (on=%s, off=%s)\n' "$d" "$name" "$erc" "$orc"
    elif ! diff -q <(norm "$out/$tag.on.out") <(norm "$out/$tag.off.out") > /dev/null 2>&1; then
        printf '%-10s %-40s OUTPUT-DIFFERS\n' "$d" "$name"
    elif grep -qE 'existence_error|module_not_found' "$out/$tag.on.out" 2>/dev/null; then
        printf '%-10s %-40s NOT-EXERCISED (both rc=%s: load error)\n' "$d" "$name" "$erc"
    elif [ "$erc" -ne 0 ]; then
        printf '%-10s %-40s same (both rc=%s)\n' "$d" "$name" "$erc"
    else
        printf '%-10s %-40s same\n' "$d" "$name"
    fi
}
export -f run_job

JOBS_BIN="$(readlink -f "$BIN")" JOBS_OUT="$OUT" JOBS_TIMEOUT=$TIMEOUT \
JOBS_TIMEOUT_PB=$TIMEOUT_PB JOBS_PBSOL="$PBSOL_BIN" JOBS_SRC="$PICAT_SRC" \
parallel -j "$JOBS" --colsep '\t' run_job {1} {2} {3} {4} :::: "$JOBLIST" \
    > "$OUT/summary.log" 2> "$OUT/parallel.err"

echo "# ---- summary ----"
echo "jobs: $(wc -l < "$JOBLIST")  verdicts: $(grep -c . "$OUT/summary.log")"
for pat in "OUTPUT-DIFFERS" "RC-DIFFERS" "NOT-EXERCISED" "both timeout"; do
    printf '  %-16s %s\n' "$pat" "$(grep -cF "$pat" "$OUT/summary.log")"
done
echo "# ---- differences (if any) ----"
grep -E "OUTPUT-DIFFERS|RC-DIFFERS" "$OUT/summary.log" || echo "(none)"
echo "# ---- not exercised (vacuous verdicts: load errors on both sides) ----"
grep -E "NOT-EXERCISED" "$OUT/summary.log" || echo "(none)"
