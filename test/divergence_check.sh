#!/bin/bash
# divergence_check.sh -- run every Hakan example on the experimental build
# and on a stock release binary (Picat 3.9#12), and report where the two
# diverge.
#
#   bash divergence_check.sh [light|detailed]
#
#   light (default)  run both builds, compare return codes only
#   detailed         also compare the outputs (timing/solver noise stripped)
#
# TARGET is the Hakan examples tree (default
# /home/jovyan/snapshotable/q38fokus/hakan_examples;  that directory is
# read-only,  which is why this script may live anywhere).  The build under
# test is PICAT (default ../Picat-src/emu/picat, relative to TARGET);  the
# stock binary is STOCK_PICAT (default /home/jovyan/bin/picat312, Picat
# 3.9#12).  TIMEOUT (default 30s) bounds each run;  a run that hits it on
# both sides counts as the same verdict -- re-run with a larger TIMEOUT to
# resolve those.  Output directory: HAKAN_CHECK_OUT (default a fresh temp
# directory);  FILTER (an egrep pattern) restricts the sweep to matching
# example names.
#
# Each build drops .qi bytecode next to the sources it compiles,  and the
# two builds must never share compiled bytecode:  the script therefore
# works on two fresh writable copies of TARGET (exp/ and stock/ under the
# output directory),  so the original tree is never touched and no .qi can
# leak from one build into the other.  Any .qi in the copies is removed
# first:  every run compiles from source.
#
# Verdicts:
#   DIVERGES (exp rc=N, stock rc=0)    this build fails where the stock
#                                      succeeds -- the regression class;
#   DIVERGES (stock rc=N, exp rc=0)    the stock fails where this build
#                                      succeeds;
#   RC-DIFFERS (exp=N, stock=M)        both fail, different return codes;
#   OUTPUT-DIFFERS                     both fail or succeed, normalized
#                                      outputs differ (detailed only);
#   same                               identical return codes (and, in
#                                      detailed mode, identical normalized
#                                      outputs);  annotated (both rc=N)
#                                      when both fail, (both timeout) when
#                                      both hit TIMEOUT.
# Library modules (no main clause) are labelled and skipped:  running them
# directly fails in every build.
#
# The external-solver environment (SATEXT_*) is neutralized so both sides
# use their built-in solvers.

TARGET=${TARGET:-/home/jovyan/snapshotable/q38fokus/hakan_examples}
EXP=${PICAT:-$TARGET/../Picat-src/emu/picat}
STOCK_PICAT=${STOCK_PICAT:-/home/jovyan/bin/picat312}
TIMEOUT=${TIMEOUT:-30}
FILTER=${FILTER:-}
MODE=${1:-light}
case "$MODE" in
    light|detailed) ;;
    *) echo "usage: bash divergence_check.sh [light|detailed]" >&2; exit 2 ;;
esac
OUT=${HAKAN_CHECK_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/hakanchk.XXXXXX")}
mkdir -p "$OUT"
LOG="$OUT/summary.log"
: > "$LOG"

# neutralize the external-solver environment (both sides)
unset SATEXT_SOLVER SATEXT_NO_FALLBACK SATEXT_PRT_MIN SATEXT_PRT_STATS
unset SATEXT_PRT_BUDGET_MS SATEXT_SHIM_MIN

# two fresh writable copies, one per build:  no shared bytecode,  the
# original tree untouched
COPY_EXP="$OUT/exp"; COPY_STOCK="$OUT/stock"
echo "# making writable copies under $OUT ..."
rm -rf "$COPY_EXP" "$COPY_STOCK"
cp -r "$TARGET" "$COPY_EXP"; chmod -R u+w "$COPY_EXP"
cp -r "$TARGET" "$COPY_STOCK"; chmod -R u+w "$COPY_STOCK"
find "$COPY_EXP" "$COPY_STOCK" -name '*.qi' -delete

is_library() {  # no main clause -> library module
    ! grep -qE '^[[:space:]]*main' "$1" 2>/dev/null
}

# strip solver timing/progress noise so the diff is about answers
norm() {  # file -> normalized on stdout
    grep -vE 'second|Cbc00|Clp00|Coin0|_wall|Wallclock|wallclock|CPU time|cpu time|Wall time|wall time|Stack\+Heap|Stack in use|Heap in use|Symbols:' "$1" 2>/dev/null
}

emit() { printf '%s\n' "$1" | tee -a "$LOG"; }

run_one() {  # label reldir name
    local lab="$1" d="$2" name="$3" base tag erc orc
    base=${name%.pi}
    if is_library "$TARGET/$d/$name"; then
        emit "$(printf '%-10s %-46s library (no main; not runnable)' "$lab" "$name")"
        return
    fi
    if [ -n "$FILTER" ] && ! printf '%s\n' "$name" | grep -qE "$FILTER"; then
        return
    fi
    ( cd "$COPY_EXP/$d" && taskset -c 0-89 env -u PICATPATH \
        timeout "$TIMEOUT" "$EXP" "$name" ) > "$OUT/$lab.$base.exp.out" 2>&1
    erc=$?
    ( cd "$COPY_STOCK/$d" && taskset -c 0-89 env -u PICATPATH \
        timeout "$TIMEOUT" "$STOCK_PICAT" "$name" ) > "$OUT/$lab.$base.old.out" 2>&1
    orc=$?
    if [ "$erc" -eq 124 ] && [ "$orc" -eq 124 ]; then
        tag="same (both timeout)"
    elif [ "$erc" -ne "$orc" ]; then
        if [ "$orc" -eq 0 ]; then tag="DIVERGES (exp rc=$erc, stock rc=0)"
        elif [ "$erc" -eq 0 ]; then tag="DIVERGES (stock rc=$orc, exp rc=0)"
        else tag="RC-DIFFERS (exp=$erc, stock=$orc)"; fi
        if [ "$erc" -eq 124 ] || [ "$orc" -eq 124 ]; then tag="$tag [timeout]"; fi
    elif [ "$MODE" = detailed ] && ! diff -q <(norm "$OUT/$lab.$base.exp.out") \
            <(norm "$OUT/$lab.$base.old.out") > /dev/null 2>&1; then
        tag="OUTPUT-DIFFERS"
    elif [ "$erc" -ne 0 ]; then
        tag="same (both rc=$erc)"
    else
        tag="same"
    fi
    emit "$(printf '%-10s %-46s %s' "$lab" "$name" "$tag")"
}

echo "# outputs in $OUT"
for d in . ppl nn_hakank; do
    lab=$d; [ "$d" = . ] && lab=root
    n=0
    for f in "$TARGET/$d"/*.pi; do
        [ -e "$f" ] || continue
        run_one "$lab" "$d" "$(basename "$f")"
        n=$((n + 1))
    done
    echo "# $lab: $n files"
done

echo "# ---- summary ----"
echo "runs: $(grep -c . "$LOG")"
for pat in "DIVERGES" "RC-DIFFERS" "OUTPUT-DIFFERS" "both timeout" "library (no main"; do
    printf '  %-20s %s\n' "$pat" "$(grep -c "$pat" "$LOG")"
done
echo "# ---- divergences (if any) ----"
grep -E "DIVERGES|RC-DIFFERS|OUTPUT-DIFFERS" "$LOG" || echo "(none)"
