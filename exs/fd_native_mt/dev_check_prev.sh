#!/bin/bash
# dev_check_prev.sh -- the deviation/regression check against the experimental
# build BEFORE the changes that are to be validated: the baseline is the
# committed HEAD, built into a temporary git worktree (the uncommitted working
# tree stays untouched). Runs every exs sample of the given families twice on
# a dedicated group of free physical cores of one NUMA node: the current build
# and the baseline, both with whatever fdn environment the caller exported
# (e.g. FDN=0 or FDN_THREADS); compares the normalized outputs, expects BOTH
# sides to run and return 0, and reports the acceleration.
#
#   bash exs/fd_native_mt/dev_check_prev.sh [family ...]
#
# Families default to "cp debug euler fdn". GROUP overrides the per-task core
# count (default 16 physical cores). TIMEOUT caps each side (default 300 s).
# FDNBASE keeps the baseline binary across runs (default: rebuilt under
# /dev/shm/fdnbase when missing; FDNBASE_FORCE=1 rebuilds). The script exits
# 0 only when every sample ran on both sides (rc=0) and every output matched.

GROUP=${GROUP:-16}
TIMEOUT=${TIMEOUT:-300}
PICAT_SRC=${PICAT_SRC:-$(cd "$(dirname "$0")/../.." && pwd)}
BIN=${BIN:-$PICAT_SRC/emu/picat}
BASE=${FDNBASE:-/dev/shm/fdnbase}
: "${FDN_HOOK:=$PICAT_SRC/emu}"
export FDN_HOOK

# clean runs: no debug or statistics prints on stdout/stderr
unset SATEXT_PRT_STATS SATEXT_PRT_MIN SATEXT_VERBOSE SATEXT_SHIM_MIN FDN_VERBOSE FDN_TRACE FDN_DUMP FDN_DEBUG 2>/dev/null

. "$PICAT_SRC/exs/fd_native_mt/cores.sh"
init_core_map; init_node_map

# the baseline: the committed HEAD in a scratch worktree, built once
if [ ! -x "$BASE/emu/picat" ] || [ -n "${FDNBASE_FORCE:-}" ]; then
    rm -rf "$BASE" "$BASE.wt"
    git -C "$PICAT_SRC" worktree add --detach "$BASE.wt" HEAD > /dev/null 2>&1 || exit 1
    make -C "$BASE.wt/emu" -f Makefile.linux64 -j16 picat FDN_ARCH=${FDN_ARCH:--mpopcnt} > /dev/null 2>&1 || exit 1
    mkdir -p "$BASE"
    cp -r "$BASE.wt" "$BASE/tree"
    cp "$BASE.wt/emu/picat" "$BASE/emu/picat"
    git -C "$PICAT_SRC" worktree remove --force "$BASE.wt" > /dev/null 2>&1
fi
BASEBIN=$BASE/emu/picat
[ -x "$BASEBIN" ] || { echo "baseline build failed" >&2; exit 1; }

FAMILIES=${*:-"cp debug euler fd_native_mt"}
for fam in $FAMILIES; do
    [ -d "$PICAT_SRC/exs/$fam" ] || { echo "no family $fam" >&2; exit 1; }
done

norm() {
    grep -vE 'second|Cbc00|Clp00|Coin0|_wall|Wallclock|wallclock|CPU time|cpu time|Wall time|wall time|Stack\+Heap:|^Program:|^Trail:|^Table:|^Memory manager:|^[[:space:]]*(Stack in use|Heap in use|In use|Symbols|Subgoals|Answers|Terms|GC|Expansions):' "$1" 2>/dev/null \
      | sed -E 's/in [0-9]+ ms/in _ ms/g; s/starting at [0-9]+ ms/starting at _ ms/g; s/\([0-9]+ ms\)/(_ ms)/g; s/time [0-9.]+/time _/g; s/Presolve [0-9.]+/Presolve _/g; s/runtime_ms=[0-9]+/runtime_ms=_/g'
}

WORK=$(mktemp -d /dev/shm/fdnchkprev.XXXXXX)
prep() {   # fam name -> the sample dir
    local fam=$1 name=$2 d="$WORK/$fam.$name"
    mkdir -p "$d"
    find "$PICAT_SRC/exs/$fam" -maxdepth 1 -type f ! -name '*.qi' ! -name '*.o' \
        ! -name '*.pdf' ! -name '*.png' ! -name '*.svg' -exec cp {} "$d/" \;
    echo "$d"
}

fail=0; nrun=0; nmatch=0
check() {   # fam name args...
    local fam=$1 name=$2; shift 2
    local t0 t1 tc tb erc brc d
    local erc_exp=0
    case "$name" in pigeon_ad.pi) erc_exp=1 ;; esac   # expected: fails at posting
    local out_c="$WORK/$fam.$name.cur.out" out_b="$WORK/$fam.$name.base.out"
    d=$(prep "$fam" "$name")
    t0=$(date +%s.%N)
    ( cd "$d" && run_on_group "$GROUP" timeout "$TIMEOUT" "$BIN" "$name" "$@" ) > "$out_c" 2>&1
    erc=$?
    t1=$(date +%s.%N); tc=$(awk -v a=$t0 -v b=$t1 'BEGIN{printf "%.2f", b-a}')
    ( cd "$d" && run_on_group "$GROUP" timeout "$TIMEOUT" "$BASEBIN" "$name" "$@" ) > "$out_b" 2>&1
    brc=$?
    tb=$(awk -v a=$t1 -v b=$(date +%s.%N) 'BEGIN{printf "%.2f", b-a}')
    nrun=$((nrun + 1))
    if [ "$erc" -ne "$erc_exp" ] || [ "$brc" -ne "$erc_exp" ]; then
        printf '%-8s %-34s FAILED-TO-RUN (cur rc=%s, base rc=%s)\n' "$fam" "$name" "$erc" "$brc"
        fail=$((fail + 1)); return
    fi
    if ! diff -q <(norm "$out_c") <(norm "$out_b") > /dev/null 2>&1; then
        printf '%-8s %-34s OUTPUT-DIFFERS\n' "$fam" "$name"
        fail=$((fail + 1)); return
    fi
    nmatch=$((nmatch + 1))
    local acc=$(awk -v a="$tc" -v b="$tb" 'BEGIN{if (a>0) printf "%.1f", b/a; else print "-"}')
    printf '%-8s %-34s ok cur=%ss base=%ss (%sx)\n' "$fam" "$name" "$tc" "$tb" "$acc"
}

get_args() {
    case "$1" in
        php.pi|php_matrix.pi) echo "8 9" ;;
        kakuroN.pi) echo "1000" ;;
        zebraN.pi) echo "2000" ;;
        pigeon.pi|pigeon_ad.pi|pigeon_sat.pi) echo "10" ;;
        qall.pi) echo "8" ;;
        qff.pi|qff_sat.pi|qffsplit.pi|qpost.pi) echo "12" ;;
        knight.pi) echo "50" ;;
        knight_sat.pi) echo "20" ;;
        seq.pi) echo "40" ;;
        *) echo "" ;;
    esac
}

for fam in $FAMILIES; do
    for f in $(find "$PICAT_SRC/exs/$fam" -maxdepth 1 -name '*.pi' | sort); do
        name=$(basename "$f")
        args=$(get_args "$name")
        # shellcheck disable=SC2086
        check "$fam" "$name" $args
    done
done

echo "# dev_check_prev: $nrun samples, $nmatch matched, $fail failed"
[ "$fail" -eq 0 ] && res=0 || res=1
rm -rf "$WORK"
exit $res
