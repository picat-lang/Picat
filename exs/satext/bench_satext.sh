#!/bin/bash
# bench_satext.sh: build the satext pieces and run both demos.
#
#   cd $ROOT/emu && make -f Makefile.linux64 satext.o satshim picat
#   PICATPATH=$ROOT/lib2 picat exs/satext/satext_demo.pi
#   PICATPATH=$ROOT/lib2 picat exs/satext/swap_demo.pi
#
# Needs kissat on $HOME/bin or PATH; cryptominisat is exercised via
# the "@file" transfer mode if present.
#
# swap_demo runs in five solver-selection modes:
#   (built-in) SATEXT_SOLVER unset, (a) SATEXT_SOLVER=kissat,
#   (b) SATEXT_SOLVER=cryptominisat, (c) SATEXT_SOLVER="kissat -q"
#   (whitespace-separated extra solver args), (d)
#   SATEXT_SOLVER="kissat|cryptominisat" (first-wins portfolio)
#   -- plus in-program bp.c_satext_set_solver() calls which work in
#   every mode.

set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export PATH="$HOME/bin:$PATH"

cd "$ROOT/emu"
make -f Makefile.linux64 satext.o kissat_picat.o satshim picat

export PICATPATH="$ROOT/lib2"

echo "== satext_demo (low-level API, built-in solver) =="
"$ROOT/emu/picat" "$ROOT/exs/satext/satext_demo.pi"

echo
echo "== swap_demo (import sat flow, built-in) =="
"$ROOT/emu/picat" "$ROOT/exs/satext/swap_demo.pi"

echo
echo "== swap_demo (SATEXT_SOLVER=kissat) =="
SATEXT_SOLVER=kissat "$ROOT/emu/picat" "$ROOT/exs/satext/swap_demo.pi"

echo
echo "== swap_demo (SATEXT_SOLVER=cryptominisat) =="
SATEXT_SOLVER=cryptominisat "$ROOT/emu/picat" "$ROOT/exs/satext/swap_demo.pi"

echo
echo "== swap_demo (SATEXT_SOLVER='kissat -q': extra solver args) =="
SATEXT_SOLVER="kissat -q" "$ROOT/emu/picat" "$ROOT/exs/satext/swap_demo.pi"

echo
echo "== swap_demo (SATEXT_SOLVER='kissat|cryptominisat': portfolio) =="
SATEXT_SOLVER="kissat|cryptominisat" SATEXT_PRT_MIN=1 \
    "$ROOT/emu/picat" "$ROOT/exs/satext/swap_demo.pi"

echo
echo "ALL SATEXT BENCHES PASSED"
