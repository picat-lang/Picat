#!/bin/sh
# bench_speed.sh -- time a fixed list of CP-search programs on one Picat
# binary and report per-example wall times plus the geomean.  Two runs
# per example, the minimum is kept.
#
# usage: sh bench_speed.sh <picat-binary> [FDN=0|1]
#   FDN defaults to 1 (the fdn hook on -- the realistic config for the
#   experimental builds; the stock binary ignores it).
#
# The examples come from the hakan suite (HAKAN_DIR) and must all have a
# main/0.  Timing uses /usr/bin/time -o <scratch file> -f %e.

PICAT_BIN=${1:?usage: bench_speed.sh <picat-binary> [FDN]}
FDNV=${2:-1}
HAKAN_DIR=${HAKAN_DIR:-/home/jovyan/snapshotable/q38fokus/hakan_examples}
TFILE=${TFILE:-/home/jovyan/snapshotable/q38fokus/Picat-src/scratch/maincmp/timing/bench.txt}
mkdir -p "$(dirname "$TFILE")"

EXAMPLES="100_doors.pi 3_jugs.pi 24_game.pi binary_sudoku.pi map_coloring.pi \
perfect_numbers.pi send_more_money.pi alphametic.pi dqueens.pi 4_rings_problem.pi \
abc_endview.pi arrows.pi 12_pack_problem.pi 3_in_a_row.pi"

total=0; n=0; logsum=0
printf '%-24s %8s\n' example "sec(min2)"
for ex in $EXAMPLES; do
  f="$HAKAN_DIR/$ex"
  [ -e "$f" ] || { printf '%-24s %8s\n' "$ex" "n/a"; continue; }
  /usr/bin/time -o "$TFILE" -f %e env FDN=$FDNV timeout 60 "$PICAT_BIN" "$f" > /dev/null 2>/dev/null
  t1=$(cat "$TFILE" 2>/dev/null)
  /usr/bin/time -o "$TFILE" -f %e env FDN=$FDNV timeout 60 "$PICAT_BIN" "$f" > /dev/null 2>/dev/null
  t2=$(cat "$TFILE" 2>/dev/null)
  m=$(printf '%s\n%s\n' "$t1" "$t2" | sort -n | head -1)
  case "$m" in *[!0-9.]*) printf '%-24s %8s\n' "$ex" "fail"; continue ;; esac
  printf '%-24s %8s\n' "$ex" "$m"
  logsum=$(awk -v a="$logsum" -v b="$m" 'BEGIN{print a+log(b)}')
  n=$((n+1))
done
if [ "$n" -gt 0 ]; then
  awk -v s="$logsum" -v c="$n" 'BEGIN{printf "geomean over %d examples: %.3f s\n", c, exp(s/c)}'
fi
