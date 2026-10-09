#!/bin/bash
# fdn_sweep.sh -- the fdn transparency sweep over all hakan examples.
# Every example runs twice on the SAME binary: with the fdn hook (default)
# and with FDN=0 (exactly the stock behavior). The verdict compares the
# return codes and the normalized outputs. The pb examples run with the pb
# environment (PICATPATH=lib2 + PBSOL); php.pi and php_matrix.pi get the
# arguments "8 9".
#
#   bash fdn_sweep.sh [JOBS]
#
# The runs go through `parallel -j JOBS` (default 32), each job running its
# fdn-on and FDN=0 side back to back on a dedicated core set. Two fresh
# writable copies per tree; every run compiles from source.

JOBS=${1:-32}
HAKAN=${HAKAN:-/home/jovyan/snapshotable/q38fokus/hakan_examples}
PB_SRC=${PB_SRC:-/home/jovyan/snapshotable/q38fokus/Picat-src/exs/pb}
BIN=${BIN:-/home/jovyan/snapshotable/q38fokus/Picat-src/emu/picat}
PBSOL_BIN=${PBSOL:-/home/jovyan/snapshotable/q38fokus/scratch/roundingsat-src/build_my/roundingsat}
TIMEOUT=${TIMEOUT:-30}
TIMEOUT_PB=${TIMEOUT_PB:-300}
OUT=${FDN_SWEEP_OUT:-$(mktemp -d "/dev/shm/fdnsweep.XXXXXX")}
mkdir -p "$OUT"

unset SATEXT_SOLVER SATEXT_NO_FALLBACK SATEXT_PRT_MIN SATEXT_PRT_STATS
unset SATEXT_PRT_BUDGET_MS SATEXT_SHIM_MIN

echo "# fdn hakan sweep: jobs=$JOBS timeout=$TIMEOUT bin=$BIN"
echo "# outputs in $OUT"

rm -rf "$OUT/on" "$OUT/off" "$OUT/pb"
cp -r "$HAKAN" "$OUT/on";  chmod -R u+w "$OUT/on"
cp -r "$HAKAN" "$OUT/off"; chmod -R u+w "$OUT/off"
cp -r "$PB_SRC" "$OUT/pb"; chmod -R u+w "$OUT/pb"
find "$OUT/on" "$OUT/off" "$OUT/pb" -name '*.qi' -delete
rm -f "$OUT/pb/README.md" "$OUT/pb/pb.qi"

JOBLIST="$OUT/jobs.tsv"
: > "$JOBLIST"
for f in "$HAKAN"/*.pi;            do [ -e "$f" ] && printf 'root\t.\t%s\n'        "$(basename "$f")" >> "$JOBLIST"; done
for f in "$HAKAN"/ppl/*.pi;        do [ -e "$f" ] && printf 'ppl\tppl\t%s\n'       "$(basename "$f")" >> "$JOBLIST"; done
for f in "$HAKAN"/nn_hakank/*.pi;  do [ -e "$f" ] && printf 'nn\tnn_hakank\t%s\n'  "$(basename "$f")" >> "$JOBLIST"; done
for f in "$PB_SRC"/*.pi;           do [ -e "$f" ] && printf 'pb\t.\t%s\n'          "$(basename "$f")" >> "$JOBLIST"; done
echo "# jobs: $(wc -l < "$JOBLIST")"

norm() {
    grep -vE 'second|Cbc00|Clp00|Coin0|_wall|Wallclock|wallclock|CPU time|cpu time|Wall time|wall time|Stack\+Heap:|^Program:|^Trail:|^Table:|^Memory manager:|^[[:space:]]*(Stack in use|Heap in use|In use|Symbols|Subgoals|Answers|Terms|GC|Expansions):' "$1" 2>/dev/null | sed -E 's/in [0-9]+ ms/in _ ms/g'
}
export -f norm

run_job() {  # lab dir name
    local lab="$1" d="$2" name="$3" tmo args envp erc orc
    local bin="$JOBS_BIN" out="$JOBS_OUT" t="$JOBS_TIMEOUT" tp="$JOBS_TIMEOUT_PB" pbsol="$JOBS_PBSOL"
    tmo=$t
    envp=(env -u PICATPATH)
    if [ "$lab" = pb ]; then
        tmo=$tp
        envp=(env -u PICATPATH PICATPATH="$(dirname "$bin")/../lib2" PBSOL="$pbsol")
        case "$name" in php.pi|php_matrix.pi) args="8 9" ;; esac
        ( cd "$out/pb" && "${envp[@]}" timeout "$tmo" "$bin" "$name" $args ) > "$out/$lab.$name.on.out" 2>&1
        erc=$?
        ( cd "$out/pb" && "${envp[@]}" FDN=0 timeout "$tmo" "$bin" "$name" $args ) > "$out/$lab.$name.off.out" 2>&1
        orc=$?
    else
        ( cd "$out/on/$d" && taskset -c 0-89 "${envp[@]}" timeout "$tmo" "$bin" "$name" ) > "$out/$lab.$name.on.out" 2>&1
        erc=$?
        ( cd "$out/off/$d" && taskset -c 0-89 "${envp[@]}" FDN=0 timeout "$tmo" "$bin" "$name" ) > "$out/$lab.$name.off.out" 2>&1
        orc=$?
    fi
    if [ "$erc" -eq 124 ] && [ "$orc" -eq 124 ]; then
        printf '%-6s %-46s same (both timeout)\n' "$lab" "$name"
    elif [ "$erc" -ne "$orc" ]; then
        printf '%-6s %-46s RC-DIFFERS (on=%s, off=%s)\n' "$lab" "$name" "$erc" "$orc"
    elif ! diff -q <(norm "$out/$lab.$name.on.out") <(norm "$out/$lab.$name.off.out") > /dev/null 2>&1; then
        printf '%-6s %-46s OUTPUT-DIFFERS\n' "$lab" "$name"
    elif [ "$erc" -ne 0 ]; then
        printf '%-6s %-46s same (both rc=%s)\n' "$lab" "$name" "$erc"
    else
        printf '%-6s %-46s same\n' "$lab" "$name"
    fi
}
export -f run_job

JOBS_BIN="$(readlink -f "$BIN")" JOBS_OUT="$OUT" JOBS_TIMEOUT=$TIMEOUT \
JOBS_TIMEOUT_PB=$TIMEOUT_PB JOBS_PBSOL="$PBSOL_BIN" \
parallel -j "$JOBS" --colsep '\t' run_job {1} {2} {3} :::: "$JOBLIST" \
    > "$OUT/summary.log" 2> "$OUT/parallel.err"

echo "# ---- summary ----"
echo "jobs: $(wc -l < "$JOBLIST")  verdicts: $(grep -c . "$OUT/summary.log")"
for pat in "OUTPUT-DIFFERS" "RC-DIFFERS" "both timeout"; do
    printf '  %-16s %s\n' "$pat" "$(grep -cF "$pat" "$OUT/summary.log")"
done
echo "# ---- differences (if any) ----"
grep -E "OUTPUT-DIFFERS|RC-DIFFERS" "$OUT/summary.log" || echo "(none)"
