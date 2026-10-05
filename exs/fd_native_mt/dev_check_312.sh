#!/bin/bash
# dev_check_312.sh -- the deviation/regression check against the stock release
# binary (Picat 3.9#12). Runs every exs sample of the given families twice,
# each side pinned to a dedicated group of free physical cores of one NUMA
# node: the experimental build (with whatever fdn environment the caller
# exported, e.g. FDN=0 or FDN_THREADS) and the stock binary; compares the
# normalized outputs, expects BOTH sides to run and return 0, and reports
# the acceleration.
#
#   bash exs/fd_native_mt/dev_check_312.sh [family ...]
#
# Families default to "cp debug euler fd_native_mt" (the pure-FD families,
# where the outputs must match; mip/smt/sat/pb have base-version noise).
# STOCK_PICAT overrides the stock binary (default /home/jovyan/bin/picat312).
# GROUP overrides the per-task core count (default 16 physical cores).
# TIMEOUT caps each side (default 300 s). The script exits 0 only when every
# sample ran on both sides (rc=0) and every output matched.

GROUP=${GROUP:-16}
TIMEOUT=${TIMEOUT:-300}
STOCK_PICAT=${STOCK_PICAT:-/home/jovyan/bin/picat312}
PICAT_SRC=${PICAT_SRC:-$(cd "$(dirname "$0")/../.." && pwd)}
BIN=${BIN:-$PICAT_SRC/emu/picat}

# clean runs: no debug or statistics prints on stdout/stderr
unset SATEXT_PRT_STATS SATEXT_PRT_MIN SATEXT_VERBOSE SATEXT_SHIM_MIN FDN_VERBOSE FDN_TRACE FDN_DUMP FDN_DEBUG 2>/dev/null

. "$PICAT_SRC/exs/fd_native_mt/cores.sh"
init_core_map; init_node_map

FAMILIES=${*:-"cp debug euler fd_native_mt"}
for fam in $FAMILIES; do
    [ -d "$PICAT_SRC/exs/$fam" ] || { echo "no family $fam" >&2; exit 1; }
done

norm() {
    grep -vE 'second|Cbc00|Clp00|Coin0|_wall|Wallclock|wallclock|CPU time|cpu time|Wall time|wall time|cores\.sh: warning|Stack\+Heap:|^Program:|^Trail:|^Table:|^Memory manager:|^[[:space:]]*(Stack in use|Heap in use|In use|Symbols|Subgoals|Answers|Terms|GC|Expansions):' "$1" 2>/dev/null \
      | sed -E 's/in [0-9]+ ms/in _ ms/g; s/starting at [0-9]+ ms/starting at _ ms/g; s/\([0-9]+ ms\)/(_ ms)/g; s/time [0-9.]+/time _/g; s/Presolve [0-9.]+/Presolve _/g; s/runtime_ms=[0-9]+/runtime_ms=_/g'
}

# fresh writable copy per sample: samples write __tmp.* relative to CWD
WORK=$(mktemp -d /dev/shm/fdnchk312.XXXXXX)
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
    local d out_e out_s t0 t1 te ts erc src
    local erc_exp=0
    d=$(prep "$fam" "$name")
    out_e="$WORK/$fam.$name.exp.out"; out_s="$WORK/$fam.$name.stock.out"
    t0=$(date +%s.%N)
    ( cd "$d" && run_on_group "$GROUP" timeout "$TIMEOUT" "$BIN" "$name" "$@" ) > "$out_e" 2>&1
    erc=$?
    t1=$(date +%s.%N); te=$(awk -v a=$t0 -v b=$t1 'BEGIN{printf "%.2f", b-a}')
    ( cd "$d" && run_on_group "$GROUP" timeout "$TIMEOUT" "$STOCK_PICAT" "$name" "$@" ) > "$out_s" 2>&1
    src=$?
    ts=$(awk -v a=$t1 -v b=$(date +%s.%N) 'BEGIN{printf "%.2f", b-a}')
    nrun=$((nrun + 1))
    if [ "$erc" -eq 124 ] && [ "$src" -eq 124 ]; then
        printf '%-14s %-34s BOTH-TIMEOUT (%ss)\n' "$fam" "$name" "$TIMEOUT"
        return
    fi
    if [ "$erc" -ne "$erc_exp" ] || [ "$src" -ne "$erc_exp" ]; then
        printf '%-14s %-34s FAILED-TO-RUN (exp rc=%s, stock rc=%s)\n' "$fam" "$name" "$erc" "$src"
        fail=$((fail + 1)); return
    fi
    if ! diff <(norm "$out_e") <(norm "$out_s") > "$WORK/$fam.$name.diff" 2>&1; then
        printf '%-14s %-34s OUTPUT-DIFFERS\n' "$fam" "$name"
        head -8 "$WORK/$fam.$name.diff" | sed 's/^/    /'
        fail=$((fail + 1)); return
    fi
    nmatch=$((nmatch + 1))
    local acc=$(awk -v a="$te" -v b="$ts" 'BEGIN{if (a>0) printf "%.1f", b/a; else print "-"}')
    printf '%-14s %-34s ok exp=%ss stock=%ss (%sx)\n' "$fam" "$name" "$te" "$ts" "$acc"
}

get_args() {
    case "$1" in
        php.pi|php_matrix.pi) echo "8 9" ;;
        kakuroN.pi) echo "1000" ;;
        pigeon.pi) echo "10" ;;
        qall.pi) echo "8" ;;
        qff.pi) echo "12" ;;
        *) echo "" ;;
    esac
}

for fam in $FAMILIES; do
    for f in $(find "$PICAT_SRC/exs/$fam" -maxdepth 1 -name '*.pi' | sort); do
        name=$(basename "$f")
        case "$name" in stat.pi) continue ;; esac   # helper module, no main
        args=$(get_args "$name")
        # shellcheck disable=SC2086
        check "$fam" "$name" $args
    done
done

echo "# dev_check_312: $nrun samples, $nmatch matched, $fail failed"
res=$?
[ "$fail" -eq 0 ] && res=0 || res=1
echo "# work dir kept: $WORK"
exit $res
