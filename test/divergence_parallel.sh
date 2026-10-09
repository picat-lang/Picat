#!/bin/bash
# divergence_parallel.sh -- the divergence sweep, run through GNU parallel.
# Every example runs on the experimental build and on the stock release
# binary (Picat 3.9#12); the verdicts match divergence_check.sh.  The
# runs go through `parallel -j 32` (32 examples in flight, each job
# running its exp and stock side back to back on a dedicated core).
#
#   bash divergence_parallel.sh [light|detailed] [JOBS]
#
#   light (default)  compare return codes only
#   detailed         also compare the outputs (timing/solver noise stripped)
#   JOBS             parallel jobs (default 32)
#
# The pb examples (Picat-src/exs/pb) are swept too, single-sided on the
# experimental build with the pb environment (PBSOL + PICATPATH): the
# stock 3.9#12 has no pb module, so a stock comparison would be trivial.
# They run with a larger timeout (the benchmarks are slow).
#
# Two fresh writable copies per tree (no shared bytecode, the original
# trees untouched); every run compiles from source.

MODE=${1:-light}
JOBS=${2:-32}
case "$MODE" in light|detailed) ;; *) echo "usage: bash divergence_parallel.sh [light|detailed] [JOBS]" >&2; exit 2 ;; esac

HAKAN=${HAKAN:-/home/jovyan/snapshotable/q38fokus/hakan_examples}
PB_SRC=${PB_SRC:-/home/jovyan/snapshotable/q38fokus/Picat-src/exs/pb}
EXP=${PICAT:-/home/jovyan/snapshotable/q38fokus/Picat-src/emu/picat}
STOCK_PICAT=${STOCK_PICAT:-/home/jovyan/bin/picat312}
TIMEOUT=${TIMEOUT:-30}
TIMEOUT_PB=${TIMEOUT_PB:-300}
PBSOL_BIN=${PBSOL:-/home/jovyan/snapshotable/q38fokus/scratch/roundingsat-src/build_my/roundingsat}
FILTER=${FILTER:-}
OUT=${HAKAN_CHECK_OUT:-$(mktemp -d "/dev/shm/hakanchk.XXXXXX")}
mkdir -p "$OUT"

# neutralize the external-solver environment for the hakan sides
unset SATEXT_SOLVER SATEXT_NO_FALLBACK SATEXT_PRT_MIN SATEXT_PRT_STATS
unset SATEXT_PRT_BUDGET_MS SATEXT_SHIM_MIN

echo "# parallel divergence sweep: mode=$MODE jobs=$JOBS timeout=$TIMEOUT"
echo "# outputs in $OUT"

# fresh writable copies: hakan (one per build) and the pb examples (exp only)
rm -rf "$OUT/exp" "$OUT/stock" "$OUT/pb"
cp -r "$HAKAN" "$OUT/exp";  chmod -R u+w "$OUT/exp"
cp -r "$HAKAN" "$OUT/stock"; chmod -R u+w "$OUT/stock"
cp -r "$PB_SRC" "$OUT/pb";  chmod -R u+w "$OUT/pb"
find "$OUT/exp" "$OUT/stock" "$OUT/pb" -name '*.qi' -delete
rm -f "$OUT/pb/README.md" "$OUT/pb/pb.qi"

# the job list: lab<TAB>dir<TAB>name
JOBLIST="$OUT/jobs.tsv"
: > "$JOBLIST"
for f in "$HAKAN"/*.pi;      do [ -e "$f" ] && printf 'root\t.\t%s\n'  "$(basename "$f")" >> "$JOBLIST"; done
for f in "$HAKAN"/ppl/*.pi;  do [ -e "$f" ] && printf 'ppl\tppl\t%s\n'     "$(basename "$f")" >> "$JOBLIST"; done
for f in "$HAKAN"/nn_hakank/*.pi; do [ -e "$f" ] && printf 'nn\tnn_hakank\t%s\n' "$(basename "$f")" >> "$JOBLIST"; done
for f in "$PB_SRC"/*.pi;     do [ -e "$f" ] && printf 'pb\t.\t%s\n'        "$(basename "$f")" >> "$JOBLIST"; done

norm() {
    grep -vE 'second|Cbc00|Clp00|Coin0|_wall|Wallclock|wallclock|CPU time|cpu time|Wall time|wall time|Stack\+Heap|Stack in use|Heap in use|Symbols:' "$1" 2>/dev/null
}
export -f norm

run_job() {  # lab dir name
    local lab="$1" d="$2" name="$3" mode="$JOBS_MODE" \
          timeout_h="$JOBS_TIMEOUT" timeout_pb="$JOBS_TIMEOUT_PB" \
          exp="$JOBS_EXP" stock="$JOBS_STOCK" out="$JOBS_OUT" pbsol="$JOBS_PBSOL" \
          filter="$JOBS_FILTER"
    local base tag erc orc tmo
        ERR=$(grep -qE '^[[:space:]]*main' "$out/$d/$name" 2>&1); RC=$?
    base=${name%.pi}
    if [ -n "$filter" ] && ! printf '%s\n' "$name" | grep -qE "$filter"; then
        return
    fi
    tmo=$timeout_h; [ "$lab" = pb ] && tmo=$timeout_pb
    CHK="$out/exp/$d/$name"; [ "$lab" = pb ] && CHK="$out/pb/$name"
    if ! grep -qE '^[[:space:]]*main' "$CHK" 2>/dev/null; then
        printf '%-10s %-46s library (no main; not runnable)\n' "$lab" "$name"
        return
    fi
    if [ "$lab" = pb ]; then
        ARGS=""
        case "$name" in php.pi|php_matrix.pi) ARGS="8 9" ;; esac
        ( cd "$out/pb" && PICATPATH="$(dirname "$exp")/../lib2" PBSOL="$pbsol" \
          timeout "$tmo" "$exp" "$name" $ARGS ) > "$out/$lab.$base.exp.out" 2>&1
        erc=$?
        if [ "$erc" -eq 0 ]; then tag="PASS"
        elif [ "$erc" -eq 124 ]; then tag="FAIL (timeout)"
        else tag="FAIL (rc=$erc)"; fi
        printf '%-10s %-46s %s\n' "$lab" "$name" "$tag"
        return
    fi
    ( cd "$out/exp/$d" && taskset -c 0-89 env -u PICATPATH \
        timeout "$tmo" "$exp" "$name" ) > "$out/$lab.$base.exp.out" 2>&1
    erc=$?
    ( cd "$out/stock/$d" && taskset -c 0-89 env -u PICATPATH \
        timeout "$tmo" "$stock" "$name" ) > "$out/$lab.$base.old.out" 2>&1
    orc=$?
    if [ "$erc" -eq 124 ] && [ "$orc" -eq 124 ]; then
        tag="same (both timeout)"
    elif [ "$erc" -ne "$orc" ]; then
        if [ "$orc" -eq 0 ]; then tag="DIVERGES (exp rc=$erc, stock rc=0)"
        elif [ "$erc" -eq 0 ]; then tag="DIVERGES (stock rc=$orc, exp rc=0)"
        else tag="RC-DIFFERS (exp=$erc, stock=$orc)"; fi
        if [ "$erc" -eq 124 ] || [ "$orc" -eq 124 ]; then tag="$tag [timeout]"; fi
    elif [ "$mode" = detailed ] && ! diff -q <(norm "$out/$lab.$base.exp.out") \
            <(norm "$out/$lab.$base.old.out") > /dev/null 2>&1; then
        tag="OUTPUT-DIFFERS"
    elif [ "$erc" -ne 0 ]; then
        tag="same (both rc=$erc)"
    else
        tag="same"
    fi
    printf '%-10s %-46s %s\n' "$lab" "$name" "$tag"
}
export -f run_job

JOBS_MODE=$MODE JOBS_TIMEOUT=$TIMEOUT JOBS_TIMEOUT_PB=$TIMEOUT_PB \
JOBS_EXP="$(readlink -f "$EXP")" JOBS_STOCK="$(readlink -f "$STOCK_PICAT")" \
JOBS_OUT="$OUT" JOBS_PBSOL="$PBSOL_BIN" JOBS_FILTER="$FILTER" \
parallel -j "$JOBS" --colsep '\t' run_job {1} {2} {3} :::: "$JOBLIST" \
    > "$OUT/summary.log" 2> "$OUT/parallel.err"

echo "# ---- summary ----"
echo "jobs: $(wc -l < "$JOBLIST")  verdicts: $(grep -c . "$OUT/summary.log")"
for pat in "DIVERGES" "RC-DIFFERS" "OUTPUT-DIFFERS" "both timeout" "library (no main" "^pb .*FAIL"; do
    printf '  %-20s %s\n' "$pat" "$(grep -cF "$pat" "$OUT/summary.log")"
done
pbpass=$(grep -cE '^pb .*PASS' "$OUT/summary.log")
echo "  pb: $pbpass PASS"
echo "# ---- divergences (if any) ----"
grep -E "DIVERGES|RC-DIFFERS|OUTPUT-DIFFERS" "$OUT/summary.log" || echo "(none)"
grep -E '^pb ' "$OUT/summary.log"
