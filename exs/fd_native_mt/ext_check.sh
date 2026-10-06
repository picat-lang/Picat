#!/bin/sh
# ext_check.sh -- the external CP-SAT check for picat-fdn (FDN_EXTSOLVER=cp-sat).
# Every program below must print the same output with and without the external
# server.  The external search is CP-SAT's own, so the solution ORDER and the
# backtracks are not Picat's: the comparison is semantic -- lines sorted, run
# times and backtracks masked.  ext_models.pi prints sorted solutions and
# objective values only, so it is compared byte for byte.
# Uses only this folder's examples (no scratch/ dependencies).
# Usage: exs/fd_native_mt/ext_check.sh [PICAT_BIN]
cd "$(dirname "$0")" || exit 1
P=${1:-../../emu/picat}
T=${TIMEOUT:-120}; fail=0; n=0
mask() { sed -E 's/runtime_ms=[0-9]+/runtime_ms=_/; s/(CPU time|time) *[=:]? *[0-9.]+ *(seconds|s|ms)?/\1 _/g; s/backtracks *= *[0-9]+/backtracks = _/'; }
run() {   # masklevel: plain (byte-identical) | sorted
  ml=$1; shift
  case $ml in
  plain)  a=$(timeout "$T" $P "$@" 2>/dev/null | mask)
          b=$(FDN_EXTSOLVER=cp-sat timeout "$T" $P "$@" 2>/dev/null | mask) ;;
  sorted) a=$(timeout "$T" $P "$@" 2>/dev/null | mask | sort)
          b=$(FDN_EXTSOLVER=cp-sat timeout "$T" $P "$@" 2>/dev/null | mask | sort) ;;
  esac
  n=$((n+1))
  if [ "$a" = "$b" ]; then echo "same  $*"
  else echo "DIFF  $*"; fail=$((fail+1)); fi
}
runext() {  # the ext side only; the output must contain unsat
  b=$(FDN_EXTSOLVER=cp-sat timeout "$T" $P "$@" 2>/dev/null)
  n=$((n+1))
  case "$b" in *unsat*) echo "ok    $* (ext proves unsat)" ;;
  *) echo "DIFF  $* (ext: no unsat proof)"; fail=$((fail+1));; esac
}
run plain ext_models.pi                 # every propagator family, sorted solutions
                                        # (its ent case also covers the extended-label-list
                                        # path: helper variables outside the label list)
run plain qall.pi 11                    # count_all counted by the server (2680 solutions)
run plain pigeon.pi 11                  # unsat, alone in the store: the server proves it
runext pigeon.pi 20                     # unsat with compiler-generated helper variables
                                        # (sums of > 20 terms): the ext side only -- the
                                        # native proof takes minutes, so no baseline here
run plain kakuroN.pi 20                 # 20 repeated solves over one server
echo "$((n-fail))/$n identical"
[ $fail = 0 ]
