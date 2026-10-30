#!/bin/bash
# divergence_reverify.sh — re-verify the flaky candidates of a parallel
# divergence sweep sequentially, with a longer timeout and an output
# comparison (the documented follow-up procedure for timeout-based
# DIVERGES verdicts).
#
# Usage: test/divergence_reverify.sh TIMEOUT <example.pi> ...
#   TIMEOUT  per-run timeout in seconds (the sweep default 30 is too
#            tight for the heavy programs; use 120 as in the README)
#   the rest: example paths relative to the Hakan tree root, as printed
#            by the sweep (e.g. root/coins_grid.pi, ppl/ppl_hurricane.pi)
#
# Each example runs once per side (this build vs the stock binary) on a
# fresh writable copy with any .qi deleted first, in its own directory
# under the scratch workdir, so no run shares compiled bytecode and no
# sibling module is missing (the whole tree is copied once).  Verdicts:
#   same        both sides ran (rc 0) or both timed out
#   exp-slower  the stock finished, this build timed out
#   exp-fails   this build failed (rc not 0/124), the stock ran
#   stock-fails the stock failed, this build ran
# Env: EXP (this build, default emu/picat), STOCK_PICAT, HAKAN (the tree),
# WORK (the scratch workdir for the copies).

TIMEOUT=${1:?usage: divergence_reverify.sh TIMEOUT <example.pi> ...}
shift

EXP=${EXP:-$(cd "$(dirname "$0")/.." && pwd)/emu/picat}
STOCK_PICAT=${STOCK_PICAT:-/home/jovyan/bin/picat312}
HAKAN=${HAKAN:-/home/jovyan/snapshotable/q38fokus/hakan_examples}
WORK=${WORK:-/home/jovyan/snapshotable/q38fokus/scratch/hkverify.$$}
mkdir -p "$WORK"
trap 'chmod -R u+w "$WORK" 2>/dev/null; rm -rf "$WORK" 2>/dev/null' EXIT

# one clean tree copy per side; .qi removed so no run shares bytecode
chmod -R u+w "$WORK/exp" "$WORK/stock" 2>/dev/null
rm -rf "$WORK/exp" "$WORK/stock"
cp -r "$HAKAN" "$WORK/exp"
cp -r "$HAKAN" "$WORK/stock"
chmod -R u+w "$WORK/exp" "$WORK/stock"
find "$WORK/exp" "$WORK/stock" -name '*.qi' -delete

for rel in "$@"; do
    base=$(basename "$rel")
    case "$rel" in
        root/*) dex="$WORK/exp"; dst="$WORK/stock" ;;
        *) dex="$WORK/exp/$(dirname "$rel")"; dst="$WORK/stock/$(dirname "$rel")" ;;
    esac
    ( cd "$dex" || exit 1
      rm -f ./*.qi
      timeout "$TIMEOUT" "$EXP" "$base" > "$WORK/exp.$base.out" 2>&1 )
    erc=$?
    ( cd "$dst" || exit 1
      rm -f ./*.qi
      timeout "$TIMEOUT" "$STOCK_PICAT" "$base" > "$WORK/stock.$base.out" 2>&1 )
    src=$?
    if [ "$erc" = 124 ] && [ "$src" = 0 ]; then v="exp-slower"
    elif [ "$erc" = 124 ] && [ "$src" = 124 ]; then v="same (both timeout)"
    elif [ "$erc" = 0 ] && [ "$src" = 0 ]; then v="same"
    elif [ "$erc" != 0 ] && [ "$erc" != 124 ] && [ "$src" = 0 ]; then v="exp-fails (rc=$erc)"
    elif [ "$src" != 0 ] && [ "$src" != 124 ] && [ "$erc" = 0 ]; then v="stock-fails (rc=$src)"
    elif [ "$erc" = 0 ]; then v="stock-timeout (rc=$src, exp rc=0)"
    else v="both-fail (exp rc=$erc, stock rc=$src)"
    fi
    printf '%-45s %s\n' "$rel" "$v"
done
