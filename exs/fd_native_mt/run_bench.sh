#!/bin/sh
# run_bench.sh -- the scaled exs/cp benchmark suite for picat-fdn.
# Prints runtime/backtracks per benchmark on the stock path (FDN=0) and
# with the picat-fdn native solver (default). With PROFILE=1 also a perf
# flat profile (needs perf, kernel.perf_event_paranoid <= 1).
# Usage: exs/fd_native_mt/run_bench.sh [PICAT_BIN]   (build Picat-src/emu/picat first)
cd "$(dirname "$0")" || exit 1
P=${1:-../../emu/picat}
T=${TIMEOUT:-300}
run() {
  echo "=== $* (picat-fdn)"
  /usr/bin/time -f "  wall=%es" timeout "$T" $P "$@" 2>&1 | grep -E 'STAT|sols=|^(un)?sat|wall|rror'
  echo "=== $* (stock)"
  /usr/bin/time -f "  wall=%es" FDN=0 timeout "$T" $P "$@" 2>&1 | grep -E 'STAT|sols=|^(un)?sat|wall|rror'
}
run qall.pi 14            # all solutions, forward checking (#!=) + count_all
run qall.pi 15            # bigger enumeration: the parallel count_all
run qff.pi 600            # first solution, ff (search-heavy N)
run pigeon.pi 11          # 12 pigeons / 11 holes, 0/1 model, linear sums
run pigeon.pi 10          # 11 pigeons / 10 holes
run kakuroN.pi 1000       # kakuro x1000 (posting-dominated)
run qopt.pi 14 ff         # weighted N-queens branch and bound (native B&B)
