#!/bin/bash
# cores.sh -- core allocator for the fd_native_mt test harnesses.
#
# Physical-core view: the SMT siblings of a physical core count as ONE core,
# and a physical core is FREE only when ALL of its logical siblings are idle.
# NUMA grouping: every core handed to one task comes from ONE NUMA node --
# never mix cores from different CPUs in a task's group.
# Placement is done with taskset; the group is identified free in a single
# pass over all logicals, re-verified immediately before the launch, and
# re-verified again after the task finishes.
#
# Sourced, not executed: . "$(dirname "$0")/cores.sh"

NLOGICAL=$(nproc)
SMT=${SMT:-2}                                  # logical cores per physical core
NPHYS=$((NLOGICAL / SMT))
IDLE_PCT=${IDLE_PCT:-90}                       # a logical is free at >= this idle share
SAMPLE_S=${SAMPLE_S:-0.1}                      # idle-sample interval

init_core_map() {
    COREMAP=${COREMAP:-/dev/shm/fdncores.$$.map}
    : > "$COREMAP"
    local c sib rep
    for c in $(seq 0 $((NLOGICAL - 1))); do
        sib=$(cat /sys/devices/system/cpu/cpu$c/topology/thread_siblings_list 2>/dev/null | tr ',' ' ')
        rep=${sib%% *}
        echo "$c $rep" >> "$COREMAP"
    done
}

init_node_map() {
    NODEMAP=${NODEMAP:-/dev/shm/fdnnodes.$$.map}
    : > "$NODEMAP"
    local nf n r a b c p
    for nf in /sys/devices/system/node/node*/cpulist; do
        n=$(basename "$(dirname "$nf")" | sed 's/^node//')
        for r in $(tr ',' ' ' < "$nf"); do
            case $r in
              *-*) a=${r%-*}; b=${r#*-} ;;
              *)   a=$r; b=$r ;;
            esac
            for c in $(seq "$a" "$b"); do
                p=$(phys_of "$c")
                echo "$p $n" >> "$NODEMAP"
            done
        done
    done
    sort -u -o "$NODEMAP" "$NODEMAP"
}

phys_of() { awk -v c="$1" '$1==c{print $2; exit}' "$COREMAP"; }
node_of_phys() { awk -v p="$1" '$1==p{print $2; exit}' "$NODEMAP"; }
sibling_of() { [ "$1" -lt "$NPHYS" ] && echo $(( $1 + NPHYS )) || echo $(( $1 - NPHYS )); }

# idle_sample: "logical idle total" per online logical cpu
idle_sample() {
    awk '$1 ~ /^cpu[0-9]+$/ {t=0; for(i=2;i<=NF;i++) t+=$i; printf "%s %s %s\n", substr($1,4), $5, t}' /proc/stat
}

# idle_pct FILE -> "logical pct" lines over one sample interval (ALL logicals)
idle_pct() {
    idle_sample > "$1.a" 2>/dev/null
    sleep "$SAMPLE_S"
    idle_sample > "$1.b" 2>/dev/null
    awk 'NR==FNR {a[$1]=$2; ta[$1]=$3; next} ($1 in a) && ($3-ta[$1])>0 {printf "%s %d\n", $1, ($2-a[$1])*100/($3-ta[$1])}' "$1.a" "$1.b"
}

# snapshot_free TMP -> "phys free|busy" per physical, from ONE sample pass;
# a physical core is free iff every logical sibling is idle
snapshot_free() {
    idle_pct "$1" | awk -v nph="$NPHYS" -v thr="$IDLE_PCT" '
        { pct[$1] = $2 }
        END {
            for (p = 0; p < nph; p++) {
                s1 = p; s2 = (p < nph) ? (p + nph) : -1
                f = (pct[s1] >= thr)
                if (s2 >= 0 && !(pct[s2] >= thr)) f = 0
                printf "%s %s\n", p, (f ? "free" : "busy")
            }
        }'
}

# phys_all_free TMP PHYS... -> true iff every listed physical is free (one pass)
phys_all_free() {
    local tmp=$1; shift
    local snap; snap=$(snapshot_free "$tmp")
    local p
    for p in "$@"; do
        [ "$(awk -v p="$p" '$1==p{print $2}' <<< "$snap")" = "free" ] || return 1
    done
    return 0
}

# pick_free NODE SIZE TMP -> the free physical core ids on NODE (one per line);
# succeeds only if at least SIZE were found, from ONE sample pass
pick_free() {
    local node=$1 size=$2 tmp=$3 snap p n=0
    snap=$(snapshot_free "$tmp")
    for p in $(seq 0 $((NPHYS - 1))); do
        [ "$(node_of_phys "$p")" = "$node" ] || continue
        [ "$(awk -v p="$p" '$1==p{print $2}' <<< "$snap")" = "free" ] || continue
        echo "$p"
        n=$((n + 1))
        [ "$n" -ge "$size" ] && break
    done
    [ "$n" -ge "$size" ]
}

# run_on_group SIZE CMD... -- run CMD pinned to SIZE free physical cores of one
# NUMA node (one representative logical per physical); identifies the group in
# one pass, re-verifies it free just before the launch, and re-verifies again
# after the task finishes. FDN_THREADS defaults to SIZE unless already set.
run_on_group() {
    local size=$1; shift
    local cmd=("$@")
    local tmp node rc=0 logicals="" p
    tmp=$(mktemp /dev/shm/fdnrun.XXXXXX)
    local -a phys=()
    for node in $(ls /sys/devices/system/node/ | grep '^node[0-9]*$' | sed 's/node//'); do
        mapfile -t phys < <(pick_free "$node" "$size" "$tmp" 2>/dev/null)
        [ "${#phys[@]}" -eq "$size" ] && break
        phys=()
    done
    if [ "${#phys[@]}" -ne "$size" ]; then rm -f "$tmp"; return 2; fi
    phys_all_free "$tmp" "${phys[@]}" || echo "cores.sh: warning: a core went busy before launch, proceeding" >&2
    for p in "${phys[@]}"; do logicals="$logicals$p,"; done            # the representative IS the lower sibling
    logicals=${logicals%,}
    [ -n "${FDN_THREADS:-}" ] || export FDN_THREADS=$size
    taskset -c "$logicals" "${cmd[@]}"
    rc=$?
    phys_all_free "$tmp" "${phys[@]}" || echo "cores.sh: warning: group not free after the task" >&2
    rm -f "$tmp"
    return $rc
}
