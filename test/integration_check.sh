#!/bin/bash
# integration_check.sh -- the full fdn-integration verification of the
# experimental build, at high parallelism.  Each phase delegates to a
# dedicated script that itself parallelizes, so the whole check runs
# fast (a few minutes at the default parallelism on the Hakan tree).
#
#   bash test/integration_check.sh [JOBS] [flags]
#
#   JOBS           parallelism for the delegated sweeps (default 128)
#   --no-build     skip step 1 (use the already-built emu/picat)
#   --skip-hakan   skip step 5 (the Hakan divergence sweep, the slowest)
#   --skip-exs     skip step 4 (the fdn exs sweep)
#   --wasm         also rebuild the wasm and run the headless compare
#                  (step 6;  slow, ~35 min)
#   --outdir=DIR   where the logs go (default scratch/integration)
#
# Steps:
#   1. rebuild the fdn binary (make -C emu -f Makefile.linux64);  the
#      step FAILS on any warning or error from our files (the vendored
#      kis_/espresso/fann sources are exempt)
#   2. the quick checks in parallel, each its own script:
#        fp_check.sh          (the fp module composition check)
#        udf_check.sh         (the udf lambda check over all 4 solvers)
#        reif_globals_check.sh (the reified globals vs the plain ones)
#   3. the inline checks in parallel, each with a timeout:
#        the smoke test (writeln + halt)
#        the float formatting (0.1+0.2 must print 0.3)
#        coins_grid_bv        (the bv objective;  test/integration/)
#        planner_ricochet     (the planner family, reads stdin)
#   4. fdn_exs.sh JOBS -- the fdn transparency sweep over all
#      Picat-src/exs examples (fdn-on vs FDN=0 on the same binary)
#   5. divergence_parallel.sh light JOBS -- every Hakan example on this
#      build vs the stock release binary (return codes only)
#   6. optional --wasm: make -C webasm, then webasm/headless/compare.py
#   7. the summary;  exit nonzero if any step failed
#
# The build under test is emu/picat (override with PICAT=...).  The
# stock binary for the Hakan sweep is STOCK_PICAT (default
# /home/jovyan/bin/picat312, Picat 3.9#12).  The Hakan tree is TARGET
# (default /home/jovyan/snapshotable/q38fokus/hakan_examples).
#
# Verdict lines this script recognizes in the logs:
#   step 2: "ALL PASS" in each log
#   step 3: "PASS <name>" per inline check
#   step 4: zero "RC-DIFFERS" lines (fdn_exs.sh summary)
#   step 5: zero lines where the experimental side errors while stock
#           succeeds ("exp-fails" / exp rc!=0 with stock rc=0)

set -u

JOBS=128
NO_BUILD=0
SKIP_HAKAN=0
SKIP_EXS=0
WITH_WASM=0
OUTDIR="scratch/integration"

for arg in "$@"; do
    case "$arg" in
        --no-build)   NO_BUILD=1 ;;
        --skip-hakan) SKIP_HAKAN=1 ;;
        --skip-exs)   SKIP_EXS=1 ;;
        --wasm)       WITH_WASM=1 ;;
        --outdir=*)   OUTDIR="${arg#--outdir=}" ;;
        [1-9]*)       JOBS="$arg" ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1
BIN="${PICAT:-emu/picat}"
mkdir -p "$OUTDIR"

echo "# integration check: jobs=$JOBS bin=$BIN out=$OUTDIR"
date "+# started: %F %T"

# ---- env hygiene: the satext shim vars must not leak into the runs ----
unset SATEXT_SOLVER SATEXT_NO_FALLBACK SATEXT_PRT_MIN SATEXT_PRT_STATS
unset SATEXT_PRT_BUDGET_MS SATEXT_SHIM_MIN

FAILED=0

# ---- step 1: rebuild; zero warnings/errors from our files ----
if [ "$NO_BUILD" = 0 ]; then
    echo "== step 1: rebuild"
    if ! make -C emu -f Makefile.linux64 > "$OUTDIR/build.log" 2>&1; then
        echo "   FAIL: build error (see $OUTDIR/build.log)"
        FAILED=1
    else
        ourwarn=$(grep -E ": warning:|: error:" "$OUTDIR/build.log" \
                  | grep -vE "kis_|espresso|fann" | wc -l)
        if [ "$ourwarn" -ne 0 ]; then
            echo "   FAIL: $ourwarn warnings/errors from our files (see $OUTDIR/build.log)"
            grep -E ": warning:|: error:" "$OUTDIR/build.log" \
                | grep -vE "kis_|espresso|fann" | head -5 | sed 's/^/     /'
            FAILED=1
        else
            echo "   PASS (zero warnings/errors from our files)"
        fi
    fi
else
    echo "== step 1: skipped (--no-build)"
fi

# ---- step 2: the quick checks in parallel ----
echo "== step 2: quick checks (parallel)"
( sh test/fp_check.sh "$BIN"          > "$OUTDIR/fp.log"   2>&1; echo $? > "$OUTDIR/fp.rc"   ) &
( sh test/udf_check.sh "$BIN"         > "$OUTDIR/udf.log"  2>&1; echo $? > "$OUTDIR/udf.rc"  ) &
( sh test/reif_globals_check.sh "$BIN" > "$OUTDIR/reif.log" 2>&1; echo $? > "$OUTDIR/reif.rc" ) &
wait
for c in fp udf reif; do
    rc=$(cat "$OUTDIR/$c.rc" 2>/dev/null || echo 1)
    if [ "$rc" -ne 0 ] || ! grep -q "ALL PASS" "$OUTDIR/$c.log" 2>/dev/null; then
        echo "   FAIL: $c (rc=$rc, see $OUTDIR/$c.log)"
        FAILED=1
    else
        echo "   PASS: $c"
    fi
done

# ---- step 3: the inline checks in parallel ----
# Each check runs the binary with a timeout and PASSES if its expected
# output pattern appears in the log.  (coins_grid_bv prints its first
# solution and then keeps minimizing, so the verdict is on the output,
# not on the exit code.)
echo "== step 3: inline checks (parallel)"
TMO=${TIMEOUT:-120}
mkdir -p /home/jovyan/snapshotable/q38fokus/scratch
printf 'main => writeln(hello), halt.\n' > /home/jovyan/snapshotable/q38fokus/scratch/t18.pi
printf 'main => writeln(0.1+0.2), halt.\n' > /home/jovyan/snapshotable/q38fokus/scratch/floatfmt.pi
check_one() { # name, expected-egrep-pattern, args...
    local name="$1"; local pat="$2"; shift 2
    timeout "$TMO" "$BIN" "$@" > "$OUTDIR/ex.$name.log" 2>&1
    if grep -qE "$pat" "$OUTDIR/ex.$name.log" 2>/dev/null; then
        echo "PASS" > "$OUTDIR/ex.$name.verdict"
    else
        echo "FAIL" > "$OUTDIR/ex.$name.verdict"
    fi
}
check_one smoke    'hello'  /home/jovyan/snapshotable/q38fokus/scratch/t18.pi &
check_one floatfmt '^0\.3$' /home/jovyan/snapshotable/q38fokus/scratch/floatfmt.pi &
check_one coins_bv '^Sum: ' test/integration/coins_grid_bv.pi &
check_one planner_ricochet 'go\(' webasm/examples/planner_ricochet.pi &
wait
for name in smoke floatfmt coins_bv planner_ricochet; do
    v=$(cat "$OUTDIR/ex.$name.verdict" 2>/dev/null || echo FAIL)
    echo "   $v: $name"
    [ "$v" = PASS ] || FAILED=1
done

# ---- step 4: the fdn exs sweep ----
if [ "$SKIP_EXS" = 0 ]; then
    echo "== step 4: fdn exs sweep (jobs=$JOBS)"
    if bash test/fdn_exs.sh "$JOBS" > "$OUTDIR/fdnexs.log" 2>&1; then
        # only the verdict lines count;  the indented summary lines
        # ("  RC-DIFFERS       0") must not match
        rcd=$(grep "RC-DIFFERS" "$OUTDIR/fdnexs.log" | grep -cv "^  ")
        if [ "$rcd" -ne 0 ]; then
            echo "   FAIL: $rcd RC-DIFFERS verdicts (see $OUTDIR/fdnexs.log)"
            FAILED=1
        else
            echo "   PASS (no RC-DIFFERS;  details in $OUTDIR/fdnexs.log)"
        fi
    else
        echo "   FAIL: fdn_exs.sh rc!=0 (see $OUTDIR/fdnexs.log)"
        FAILED=1
    fi
else
    echo "== step 4: skipped (--skip-exs)"
fi

# ---- step 5: the Hakan divergence sweep ----
if [ "$SKIP_HAKAN" = 0 ]; then
    echo "== step 5: Hakan divergence sweep (light, jobs=$JOBS)"
    if bash test/divergence_parallel.sh light "$JOBS" > "$OUTDIR/hakan.log" 2>&1; then
        # the real-regression direction: this build errors (rc!=0, not a
        # timeout) while stock succeeds.  Verdict lines start at column 0;
        # the indented summary lines and the [timeout]-tagged rows must not
        # match.  Every candidate should be re-verified standalone (run it
        # on both binaries without the sweep load):  if both sides agree
        # standalone, the candidate was a load artifact.
        regr=$(grep "DIVERGES" "$OUTDIR/hakan.log" | grep -v "^  " \
               | grep -v "\[timeout\]" | grep -cE "exp rc=[1-9]")
        to=$(grep "DIVERGES" "$OUTDIR/hakan.log" | grep -c "\[timeout\]" || true)
        if [ "$regr" -ne 0 ]; then
            echo "   FAIL: $regr real-regression candidates (see $OUTDIR/hakan.log;"
            echo "         re-verify standalone before trusting them)"
            grep "DIVERGES" "$OUTDIR/hakan.log" | grep -v "^  " \
                | grep -v "\[timeout\]" | grep -E "exp rc=[1-9]" \
                | head -5 | sed 's/^/     /'
            FAILED=1
        else
            echo "   PASS (no real regressions;  $to timeout candidates to re-verify with"
            echo "          test/divergence_reverify.sh;  details in $OUTDIR/hakan.log)"
        fi
    else
        echo "   FAIL: divergence_parallel.sh rc!=0 (see $OUTDIR/hakan.log)"
        FAILED=1
    fi
else
    echo "== step 6: skipped (--skip-hakan)"
fi

# ---- step 6: optional wasm round ----
if [ "$WITH_WASM" = 1 ]; then
    echo "== step 6: wasm rebuild + headless compare (slow)"
    if make -C webasm > "$OUTDIR/wasmbuild.log" 2>&1 \
       && timeout 3600 python3 -u webasm/headless/compare.py \
            --workers "$JOBS" --timeout 120 \
            > "$OUTDIR/wasmcmp.log" 2>&1; then
        echo "   PASS (see $OUTDIR/wasmcmp.log for the statistics)"
        tail -5 "$OUTDIR/wasmcmp.log" | sed 's/^/     /'
    else
        echo "   FAIL: wasm build or compare (see $OUTDIR/wasmbuild.log / $OUTDIR/wasmcmp.log)"
        FAILED=1
    fi
else
    echo "== step 6: skipped (pass --wasm to include the wasm round)"
fi

# ---- summary ----
date "+# finished: %F %T"
if [ "$FAILED" = 0 ]; then
    echo "INTEGRATION CHECK: ALL PASS (logs in $OUTDIR)"
else
    echo "INTEGRATION CHECK: FAILURES PRESENT (logs in $OUTDIR)"
fi
exit "$FAILED"
