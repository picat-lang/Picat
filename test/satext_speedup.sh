#!/bin/bash
# Speedup test: the exs/sat examples on the satext portfolio vs no ext
# solvers (the built-in).
#
#   Side A: SATEXT_SOLVER=<portfolio>, SATEXT_NO_FALLBACK=1 (the user's
#           combination by default)
#   Side B: SATEXT_SOLVER empty -- the plain built-in flow
#
# Each example is run REPEATS times per side; the MEDIAN wall time of
# the whole process is kept (C startup + stdlib load + program, so a
# row dominated by the ~25-30 ms startup floor is not a real speedup --
# same caveat as the webasm battery). The table prints the median and
# the ratio portfolio/builtin; <1 means the portfolio is faster. The
# geomean of the ratios is summarized at the end. Examples that time
# out or fail on either side are marked and excluded from the geomean.
#
# Env overrides:
#   PICAT     the picat binary      (default: ../emu/picat from here)
#   SAT_EXS   the exs/sat dir       (default: ../exs/sat)
#   PORTFOLIO the SATEXT_SOLVER spec for side A (the user's combination)
#   REPEATS   measurements per side (default 3)
#   TIMEOUT   per run, seconds      (default 120)

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
PICAT="${PICAT:-$HERE/../emu/picat}"
SAT_EXS="${SAT_EXS:-$HERE/../exs/sat}"
REPEATS="${REPEATS:-3}"
TIMEOUT="${TIMEOUT:-120}"
PORTFOLIO="${PORTFOLIO:-builtin|kissat --sat|kissat --unsat|cadical --sat|cadical --unsat|cryptominisat -t 16}"
export PICATPATH="$HERE/../lib2:$HERE/../lib"

median() { # median of a list of ints (one per arg)
    printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END {print (NR%2) ? a[(NR+1)/2] : int((a[NR/2]+a[NR/2+1])/2)}'
}

run_once() { # $1 = the example, $2 = "A"|"B"; prints rc ms
    local f="$1" side="$2" s rc
    if [ "$side" = A ]; then
        s=$(date +%s%N)
        env SATEXT_SOLVER="$PORTFOLIO" SATEXT_NO_FALLBACK=1 \
            timeout "$TIMEOUT" "$PICAT" "$f" >/dev/null 2>&1
        rc=$?
    else
        s=$(date +%s%N)
        env SATEXT_SOLVER= timeout "$TIMEOUT" "$PICAT" "$f" >/dev/null 2>&1
        rc=$?
    fi
    echo "$rc $((($(date +%s%N)-s)/1000000))"
}

printf '| %-26s | %10s | %10s | %7s |\n' example 'portfolio' 'builtin' 'ratio'
printf '|%-28s|%-12s|%-12s|%-9s|\n' "$(printf '%0.s-' {1..28})" "$(printf '%0.s-' {1..12})" "$(printf '%0.s-' {1..12})" "$(printf '%0.s-' {1..9})"
GEOM=1; N=0; TO=0; FAIL=0
for f in "$SAT_EXS"/*.pi; do
    name="$(basename "$f" .pi)"
    ra=(); rb=(); oka=1; okb=1
    for _ in $(seq 1 "$REPEATS"); do
        read -r rc ms <<<"$(run_once "$f" A)"; ra+=("$ms")
        [ "$rc" != 0 ] && oka=0
        read -r rc ms <<<"$(run_once "$f" B)"; rb+=("$ms")
        [ "$rc" != 0 ] && okb=0
    done
    if [ $oka = 0 ] && [ $okb = 0 ]; then
        printf '| %-26s | %10s | %10s | %7s |\n' "$name" FAIL FAIL '-'
        FAIL=$((FAIL+1)); continue
    fi
    if [ $oka = 0 ] || [ $okb = 0 ]; then
        printf '| %-26s | %10s | %10s | %7s |\n' "$name" \
            "$([ $oka = 1 ] && echo ok || echo FAIL)" \
            "$([ $okb = 1 ] && echo ok || echo FAIL)" '-'
        FAIL=$((FAIL+1)); continue
    fi
    ma=$(median "${ra[@]}"); mb=$(median "${rb[@]}")
    r=$(awk "BEGIN{printf \"%.2f\", $ma/$mb}")
    printf '| %-26s | %10s | %10s | %7s |\n' "$name" "$ma" "$mb" "$r"
    GEOM=$(awk "BEGIN{print $GEOM*$ma/$mb}"); N=$((N+1))
done
if [ "$N" -gt 0 ]; then
    awk "BEGIN{printf \"geomean ratio (portfolio/builtin): %.2f over %d examples\", $GEOM**(1.0/$N), $N}"
    [ "$FAIL" -gt 0 ] && printf ' (%d rows excluded)' "$FAIL"
    printf '\n'
fi
