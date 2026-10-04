#!/bin/sh
# run_bench.sh -- the scaled exs/cp benchmark suite for picat-fdn.
# Prints runtime/backtracks per benchmark on the stock path (FDN=0) and
# with the picat-fdn native solver (default). With PROFILE=1 also a perf
# flat profile (needs perf, kernel.perf_event_paranoid <= 1).
# Usage: exs/fdaccel/run_bench.sh [JOBS_BIN]   (build Picat-src/emu/picat first)
cd "$(dirname "$0")" || exit 1
P=${1:-../../emu/picat}
T=${TIMEOUT:-300}
run() {
  echo "=== $* (picat-fdn)"
  /usr/bin/time -f "  wall=%es" timeout "$T" $P "$@" 2>&1 | grep -E 'STAT|sols=|^(un)?sat|wall|rror'
  echo "=== $* (stock)"
  /usr/bin/time -f "  wall=%es" FDN=0 timeout "$T" $P "$@" 2>&1 | grep -E 'STAT|sols=|^(un)?sat|wall|rror'
}
run qall.pi 14            # all solutions, forward checking (#!=)
run qff.pi 600            # first solution, ff
run qffsplit.pi 600       # posting time vs search time (search-heavy N)
run qpost.pi 800          # posting only (cubic in N)
run pigeon.pi 11          # 12 pigeons / 11 holes, 0/1 model, linear sums
run pigeon.pi 10          # 11 pigeons / 10 holes
run pigeon_ad.pi 11       # same, all_distinct model: fails at posting
run pigeon_sat.pi 11      # same 0/1 model with import sat
run knight.pi 40          # circuit
run sudoku25d.pi          # 25x25, all_distinct
run sudoku25_sat.pi       # 25x25, sat
run sudoku25.pi           # 25x25, all_different: > 600 s (TIMEOUT)
run kakuroN.pi 1000       # kakuro x1000 (posting-dominated)
run zebraN.pi 2000        # zebra x2000 (posting-dominated)
run ppm2.pi
run seq.pi 13
