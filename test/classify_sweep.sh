#!/bin/bash
# classify_sweep.sh -- classify OUTPUT-DIFFERS verdicts of a divergence_parallel.sh
# sweep into benign classes (fdn traces, mip artifacts, timing prints, heap-address
# noise, whitespace) vs a residue that needs manual inspection.
#
# usage: bash classify_sweep.sh <sweep.log> <out-dir> [residue-file]
#   <sweep.log>   the log written by test/divergence_parallel.sh
#   <out-dir>     the sweep's HAKAN_CHECK_OUT dir (printed in the log header)
#   <residue-file> where the residue (example + changed-line count) is written
#
# Per-example outputs are $out/<lab>.<base>.exp.out (merged build) and
# $out/<lab>.<base>.old.out (stock).  ppl examples are counted as the
# Monte-Carlo class separately (clock-seeded randoms).
LOG="$1"; OUT="$2"; RES="${3:-residue.txt}"
FILT='^Welcome to the CBC|^Version: 2\.|^Build Date:|^Done$|^glpk|^CBC|^result:|^Objective value|^Expanded'
clean() {
  grep -vE '^fdn:' "$1" 2>/dev/null |
  sed 's/fdn: .*$//; s/_0[0-9a-f]\{3,\}/_ADDR/g; s/Time: *[0-9]\+ *ms\?/Time: T/g' |
  grep -vE "$FILT" | cat -s
}
benign=0; mc=0; other=0; : > "$RES"
for f in $(grep -E "OUTPUT-DIFFERS" "$LOG" | awk '{print $2}'); do
  b="${f%.pi}"
  lab=$(grep " $f " "$LOG" | awk '{print $1}' | head -1)
  [ "$lab" = ppl ] && { mc=$((mc+1)); continue; }
  n=$(diff <(clean "$OUT/root.$b.exp.out") <(clean "$OUT/root.$b.old.out") 2>/dev/null | grep -cE "^[<>]")
  if [ "$n" -eq 0 ]; then benign=$((benign+1)); else other=$((other+1)); echo "$b $n" >> "$RES"; fi
done
echo "benign (fdn-trace/mip/timing/address/blank): $benign"
echo "ppl Monte-Carlo class: $mc"
echo "residue: $other  (see $RES)"
