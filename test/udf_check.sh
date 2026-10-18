#!/bin/sh
# udf_check.sh -- run test/udf_check.pi (the udf module check with the cp
# solver) and, additionally, generated sat/mip/smt variants of the key
# constraint checks, so the udf lambda machinery is verified against all
# four solvers.
#
#   sh test/udf_check.sh [PICAT_BIN]
#
# cd's to the repo root, removes stale udf.qi compiled caches (a stale
# cache next to the main file shadows lib2/udf.pi edits), and runs the
# checks.
cd "$(dirname "$0")/.." || exit 1
P=${1:-emu/picat}
rm -f test/udf.qi lib2/udf.qi

SCRATCH=test/udf_check_tmp

# ---- 1. the main check (cp) ----
PICATPATH=.:./lib2 $P test/udf_check.pi || exit 1

# ---- 2. per-solver variants of the constraint checks ----
# Each variant is generated into $SCRATCH with the solver import and the
# shims swapped; it re-runs the lambda-in-constraint checks against the
# given solver.
mkdir -p $SCRATCH

for S in sat mip smt; do
    case $S in
      sat) DELEGATE=sat ;;
      mip) DELEGATE=mip ;;
      smt) DELEGATE=smt ;;
    esac
    cat > $SCRATCH/udf_check_$S.pi <<EOF
import udf.
import $S.

'#='(L,R)  => $S.'#='(udf.flatten(L), udf.flatten(R)).
'#=<'(L,R) => $S.'#=<'(udf.flatten(L), udf.flatten(R)).
'#!='(L,R) => $S.'#!='(udf.flatten(L), udf.flatten(R)).

main =>
    % Route A: lambda body in a constraint
    udf.define(\$sq(_), \$lambda([Y2], Y2*Y2)),
    I :: 1..20, Z :: 1..10,
    I #= \$sq(Z) + 1,
    solve([I, Z]),
    check(I = Z*Z + 1, "$S: lambda body in a constraint"),
    % closure over a solver variable
    K :: 1..10,
    udf.define(\$gsum(_), \$lambda([Y5], Y5 + K)),
    L :: 1..20, Z3 :: 1..10,
    L #= \$gsum(Z3),
    solve([L, Z3, K]),
    check(L = Z3 + K, "$S: closure over a solver variable (shared)"),
    % Route B: literal apply and higher-order apply
    M :: 1..20, Z4 :: 1..4,
    M #= \$apply(\$lambda([Y6], Y6*Y6), Z4) + 1,
    solve([M, Z4]),
    check(M = Z4*Z4 + 1, "$S: literal apply in a constraint"),
    udf.define(\$add(N), \$lambda([Y7], Y7 + N)),
    N1 :: 1..20, Z5 :: 1..10,
    N1 #= \$apply(\$add(5), Z5),
    solve([N1, Z5]),
    check(N1 = Z5 + 5, "$S: higher-order apply"),
    println("ALL PASS ($S)").

check(Result, Name), Result => println("ok: " ++ Name).
check(_Result, Name) => throw(\$udf_check_failed("$S:" ++ Name)).
EOF
    rm -f $SCRATCH/udf.qi
    echo "--- solver variant: $S ---"
    PICATPATH=.:./lib2:$SCRATCH $P $SCRATCH/udf_check_$S.pi || exit 1
done

rm -rf $SCRATCH
echo "udf_check.sh: all solver variants pass"
