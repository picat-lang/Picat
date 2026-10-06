#!/bin/sh
# ext_bench.sh -- the external CP-SAT speed check for picat-fdn.
# Each benchmark runs three ways: the native fdn (FDN_THREADS=64), the
# external CP-SAT server with 1 worker and with 64 workers
# (FDN_EXTSOLVER=cp-sat FDN_EXTTHREADS=n), and, where short, the interpreted
# path (FDN=0).  Whole-program wall time, median of 3, 30 s timeout per run
# (timed-out configs are marked TIMEOUT).  Outputs are checked by
# ext_check.sh, not here.  Uses only this folder's examples.
# Usage: exs/fd_native_mt/ext_bench.sh [PICAT_BIN]
cd "$(dirname "$0")" || exit 1
P=${1:-../../emu/picat}
bench() {   # env file args...
  e=$1; shift; f=$1; shift
  ts=""
  for i in 1 2 3; do
    T0=$(date +%s%N)
    timeout 30 env $e $P "$f" "$@" > /dev/null 2>&1
    rc=$?
    T1=$(date +%s%N)
    ts="$ts $(( (T1-T0)/1000000 ))"
  done
  if [ "$rc" = 124 ]; then
    echo "$f $* | $e | TIMEOUT (>30s, last $ts)"
  else
    med=$(printf '%s\n' $ts | sort -n | sed -n 2p)
    echo "$f $* | $e | median ${med}ms ($ts)"
  fi
}
for spec in "FDN_THREADS=64|qall.pi 14" "FDN_EXTSOLVER=cp-sat FDN_EXTTHREADS=1|qall.pi 14" \
            "FDN_EXTSOLVER=cp-sat FDN_EXTTHREADS=64|qall.pi 14" \
            "FDN_THREADS=64|pigeon.pi 11" "FDN_EXTSOLVER=cp-sat FDN_EXTTHREADS=1|pigeon.pi 11" \
            "FDN_EXTSOLVER=cp-sat FDN_EXTTHREADS=64|pigeon.pi 11" \
            "FDN_THREADS=64|qopt.pi 13 ff" "FDN_EXTSOLVER=cp-sat FDN_EXTTHREADS=1|qopt.pi 13 ff" \
            "FDN_EXTSOLVER=cp-sat FDN_EXTTHREADS=64|qopt.pi 13 ff" \
            "FDN_THREADS=64|kakuroN.pi 1000" "FDN_EXTSOLVER=cp-sat FDN_EXTTHREADS=1|kakuroN.pi 1000" \
            "FDN_EXTSOLVER=cp-sat FDN_EXTTHREADS=64|kakuroN.pi 1000" \
            "FDN_THREADS=64|qff.pi 600" "FDN_EXTSOLVER=cp-sat FDN_EXTTHREADS=1|qff.pi 600" \
            "FDN_EXTSOLVER=cp-sat FDN_EXTTHREADS=64|qff.pi 600" \
            "FDN=0|qall.pi 14" "FDN=0|pigeon.pi 11" "FDN=0|qopt.pi 13 ff" \
            "FDN=0|kakuroN.pi 1000" "FDN=0|qff.pi 600"; do
  e=${spec%%|*}; a=${spec#*|}; f=${a%% *}; args=${a#* }
  bench "$e" "$f" $args
done
