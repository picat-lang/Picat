#!/usr/bin/env bash
# test_race.sh - regression tests for the parblock race family
# (race_bug:  mode-1 choice-point fork re-ran only the first clause;
#  first-wins mode 2 -- bp.pvm_fork_rw -- makes the candidates run
#  concurrently, the first completer win, and the losers be killed).
#
# Requires the rebuilt engine (emu/picat, with bp.pvm_fork_rw) and
# PICATPATH=lib2.  Run from the repo root:
#   bash exs/parallel/test/test_race.sh
set -u
ROOT="$(cd "$(dirname "$0")/../../../" && pwd)"
PICAT="$ROOT/emu/picat"
export PICATPATH="$ROOT/lib2"
T="$ROOT/exs/parallel/test"

if [ ! -x "$PICAT" ]; then
    echo "engine not found: $PICAT"
    echo "build it with:  cd $ROOT/emu && make -f Makefile.linux64 picat"
    exit 1
fi

fail=0
chk() { # chk <name> <rc> <out>
    local name=$1 rc=$2 out=$3
    if [ "$rc" -ne 0 ] || ! printf '%s' "$out" | grep -q "PASS"; then
        echo "FAIL: $name (rc=$rc): $(printf '%s' "$out" | grep -E 'FAIL|uncaught' | tail -1)"
        fail=1
    else
        echo "ok:   $name -> $(printf '%s' "$out" | grep PASS | tail -1)"
    fi
}

# 1. block race (race_begin/race_cl/race_end): fast must win at NT>=2
for nt in 0 1 2 3 4; do
    out=$(timeout 40 "$PICAT" "$T/race_par_block.pi" $nt 2>&1)
    chk "race_par_block NT=$nt" "$?" "$out"
done

# 2. functional race_res
for nt in 0 1 2 4; do
    out=$(timeout 40 "$PICAT" "$T/race_par_res.pi" $nt 2>&1)
    chk "race_par_res NT=$nt" "$?" "$out"
done

# 3. hang / early-kill: a never-finishing first candidate must not
#    block; quick wins and the loser is killed (20 s timeout guards).
out=$(timeout 20 "$PICAT" "$T/race_early_kill.pi" 2>&1)
chk "race_early_kill (no hang, loser killed)" "$?" "$out"

# 4. count-all mode 2 (bp.pvm_fork(NT,2,M)) is untouched by first-wins
out=$(timeout 60 "$PICAT" "$ROOT/exs/parallel/pvm/queens_count.pi" 10 4 2>&1)
if [ "$?" -ne 0 ] || ! printf '%s' "$out" | grep -q "724"; then
    echo "FAIL: count-all queens_count -> expected 724, got: $(printf '%s' "$out" | tail -1)"
    fail=1
else
    echo "ok:   count-all queens_count = 724 (mode 2 untouched)"
fi

if [ "$fail" -eq 0 ]; then
    echo
    echo "ALL RACE TESTS PASSED"
else
    echo
    echo "RACE TESTS FAILED"
    exit 1
fi
