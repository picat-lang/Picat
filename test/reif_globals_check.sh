#!/bin/sh
# reif_globals_check.sh -- verify the reified versions of the globals
# added to lib/global_reif.pi (global_cardinality, assignment, neqs,
# notin, matrix_element, regular) against the un-reified constraints.
#
#   sh test/reif_globals_check.sh [PICAT_BIN]
#
# Runs test/reif_globals_check.pi (truth-value equivalence + faithfulness,
# see the .pi header) on the cp2 module, which includes lib/global_reif.pi.
# The plain run is explicit FDN=0; the second run is explicit FDN=1 (the
# fdn native path over the same checks -- the reified globals build bp-level
# reifications, and a transparent fdn fallback must give identical results).
cd "$(dirname "$0")/.." || exit 1
P=${1:-emu/picat}
# the stale-.qi guard:  an earlier probe run caches a compiled cp2 next
# to this script (test/cp2.qi), which shadows lib2/cp2.pi and hides any
# global_reif.pi edit -- remove it first
rm -f test/cp2.qi
rc=0
for fdn in 0 1; do
  echo "=== reif_globals_check (FDN=$fdn)"
  FDN=$fdn PICATPATH=.:./lib2 $P test/reif_globals_check.pi > /tmp/reif_globals_check.$$ 2>&1
  rc1=$?
  cat /tmp/reif_globals_check.$$
  if [ $rc1 -ne 0 ]; then rc=$rc1; fi
  rm -f /tmp/reif_globals_check.$$
done
exit $rc
