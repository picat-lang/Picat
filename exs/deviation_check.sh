#!/bin/bash
# exs/deviation_check.sh -- run every exs sample on this build and on a
# stock release binary, and report where the two diverge.
#
#   bash exs/deviation_check.sh [family ...]
#
# Families default to "cp sat mip smt planner euler debug".  The stock
# binary is taken from STOCK_PICAT (default /home/jovyan/bin/picat312,
# Picat 3.9#12); samples that use functionality the stock build lacks
# (pvm, parblock_pvm, satext, the cp2/sat2 reified libraries, udf) will
# be reported as stock-side errors -- that is expected, not a bug.
#
# What counts as a deviation:
#   DIVERGES (exp fails)      this build fails where the stock build
#                             succeeds -- the real regression class;
#   DIVERGES (stock fails)    the stock build fails where this build
#                             succeeds;
#   OUTPUT-DIFFERS            both succeed, output differs (different
#                             valid solutions or timing lines).
# Library modules (no main) are labelled and skipped:  running them
# directly fails in every build.
#
# The comparison neutralizes the SATEXT_* environment so both builds use
# their built-in solvers; run with SATEXT_COMPARE=1 to compare with the
# external-solver portfolio active on this build only.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
EXP=$ROOT/emu/picat
STOCK_PICAT=${STOCK_PICAT:-/home/jovyan/bin/picat312}
OUT=${DEVCHECK_OUT:-$(mktemp -d /tmp/devcheck.XXXXXX)}
mkdir -p "$OUT"

families="${*:-cp sat mip smt planner euler debug}"

# neutralize the external-solver environment (both sides, unless asked)
if [ "${SATEXT_COMPARE:-0}" != "1" ]; then
    unset SATEXT_SOLVER SATEXT_NO_FALLBACK SATEXT_PRT_MIN SATEXT_PRT_STATS
    unset SATEXT_PRT_BUDGET_MS SATEXT_SHIM_MIN
fi

picatpath_for() {  # file -> extra PICATPATH (each example documents its own)
    grep -q "PICATPATH=lib2:lib" "$1" 2>/dev/null && echo "lib2:lib" && return
    grep -q "PICATPATH=.*lib2" "$1" 2>/dev/null && echo "lib2" && return
    grep -q "PICATPATH=.*lib" "$1" 2>/dev/null && echo "lib" && return
    echo ""
}

is_library() {  # no main clause -> library module
    ! grep -qE '^[[:space:]]*main' "$1" 2>/dev/null
}

# strip solver timing/progress noise so the diff is about answers
norm() {  # file -> normalized on stdout
    grep -vE 'second|Cbc00|Clp00|Coin0|satext: portfolio|_wall|Wallclock' "$1" 2>/dev/null
}

run_one() {  # family file
    local fam="$1" f="$2" p name base
    name=$(basename "$f"); base=${name%.pi}
    if is_library "$f"; then
        printf "%-10s %-26s library (no main; not runnable)\n" "$fam" "$name"
        return
    fi
    p=$(picatpath_for "$f")
    local envs=""
    [ -n "$p" ] && envs="PICATPATH=$p"
    (cd $ROOT; taskset -c 0-89 env $envs \
        timeout 300 $EXP "exs/$fam/$name" > "$OUT/$fam.$base.exp.out" 2>&1)
    (cd $ROOT; taskset -c 0-89 env $envs \
        timeout 300 $STOCK_PICAT "exs/$fam/$name" > "$OUT/$fam.$base.old.out" 2>&1)
    local e o tag
    e=$(grep -cE '^\*\*\* (Error|error|Undefined|SYNTAX)|^\*\* Error' "$OUT/$fam.$base.exp.out" 2>/dev/null)
    o=$(grep -cE '^\*\*\* (Error|error|Undefined|SYNTAX)|^\*\* Error' "$OUT/$fam.$base.old.out" 2>/dev/null)
    if [ "$e" -gt 0 ] && [ "$o" -gt 0 ]; then tag="both-error"
    elif [ "$e" -gt 0 ]; then tag="DIVERGES (exp fails)"
    elif [ "$o" -gt 0 ]; then tag="DIVERGES (stock fails)"
    elif diff -q <(norm "$OUT/$fam.$base.exp.out") <(norm "$OUT/$fam.$base.old.out") > /dev/null 2>&1; then tag="same"
    else tag="OUTPUT-DIFFERS"; fi
    printf "%-10s %-26s %s\n" "$fam" "$name" "$tag"
}

echo "# outputs in $OUT"
for fam in $families; do
    for f in "$ROOT/exs/$fam"/*.pi; do
        [ -e "$f" ] || continue
        run_one "$fam" "$f"
    done
done
