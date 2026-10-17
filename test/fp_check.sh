#!/bin/sh
# fp_check.sh -- run test/fp_check.pi (the fp module composition check).
#
#   sh test/fp_check.sh [PICAT_BIN]
#
# cd's to the repo root, removes any stale fp.qi compiled next to this
# script (a stale cache shadows lib2/fp.pi edits), and runs the check.
cd "$(dirname "$0")/.." || exit 1
P=${1:-emu/picat}
rm -f test/fp.qi
FDN=${FDN:-0} PICATPATH=.:./lib2 $P test/fp_check.pi
