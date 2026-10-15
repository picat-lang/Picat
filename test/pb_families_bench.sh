#!/bin/sh
# pb_families_bench.sh -- the five pb-advantage families (the pigeon-hole
# principle in the cardinality form, perfect matching on an odd vertex
# count, the mutilated chessboard, the even colouring, subset
# cardinality), each program run under the three solvers.  The program
# bodies are solver-agnostic: only the import line decides the solver
# (import pb / import cp / import sat), so the script swaps the import
# into a temp copy per solver.  pb sends the linear/cardinality form to
# the external pseudo-Boolean solver (roundingsat, env PBSOL); cp and
# sat are built in.  All five families are UNSAT by a counting
# argument: the pseudo-Boolean form IS the proof, while the clausal
# and FD-search paths need exponential resolution / weak propagation,
# so expect the pb runs in ms and cp/sat growing fast or timing out.
#
# Env:
#   PBSOL      the roundingsat binary
#              (default /home/jovyan/snapshotable/q38fokus/scratch/
#              roundingsat-src/build_my/roundingsat)
#   BOOST_LIB  the dir with the libboost_iostreams.so.1.85.0 that PBSOL
#              was linked against, exported as LD_LIBRARY_PATH
#              (default /home/jovyan/snapshotable/q38fokus/scratch/boost185,
#              a symlink farm onto the system boost 1.83)
#   FDN        the fdn engine flag (default 0)
#   TIMEOUT    per run, seconds (default 60)
#   PICAT      the picat executable (default emu/picat)
#   WORK       the scratch dir for the swapped copies (default
#              scratch/pbfam, relative to the Picat tree)
#
# Usage (from the Picat tree root):
#   sh test/pb_families_bench.sh
# Output lines:  family size solver wall_ms result

PICAT=${PICAT:-emu/picat}
PBSOL=${PBSOL:-/home/jovyan/snapshotable/q38fokus/scratch/roundingsat-src/build_my/roundingsat}
BOOST_LIB=${BOOST_LIB:-/home/jovyan/snapshotable/q38fokus/scratch/boost185}
FDN=${FDN:-0}
TIMEOUT=${TIMEOUT:-60}
WORK=${WORK:-scratch/pbfam}

# the stale-.qi guard: a compiled lib2/pb.qi shadows an edited
# lib2/pb.pi (the loader does not refresh it), so start clean
rm -f lib2/pb.qi

mkdir -p "$WORK"

# family: sizes
PHP="8 10 12"
PMODD="7 8 9"
MCHESS="10 14 16"
EVENCOL="20 40 60"
SUBSETCARD="10 20 30"

run_one() {
    fam=$1; sz=$2; solver=$3; ex=$4
    case $solver in
        pb) envs="FDN=$FDN PICATPATH=lib2 PBSOL=$PBSOL LD_LIBRARY_PATH=$BOOST_LIB" ;;
    *) envs="FDN=$FDN PICATPATH=lib2" ;;
    esac
    t0=$(date +%s%N)
    out=$(eval env $envs timeout "$TIMEOUT" "$PICAT" "$ex" "$sz" 2>/dev/null)
    rc=$?
    t1=$(date +%s%N)
    ms=$(( (t1 - t0) / 1000000 ))
    if [ $rc -eq 124 ]; then
        res="TIMEOUT"
    elif [ $rc -ne 0 ]; then
        res="ERROR"
    else
        res=$(printf '%s' "$out" | head -1)
    fi
    printf '%s %s %s %s %s\n' "$fam" "$sz" "$solver" "$ms" "$res"
}

# swap the import line into a temp copy per solver.  The pattern has
# no line number: the import line sits below the comment header (an
# earlier `1s/...` targeting line 1 matched the header comment and
# swapped nothing, silently re-running pb for the cp/sat columns).
for fam in php_card pmodd mchess evencol subsetcard; do
    sed 's/^import pb,/import cp,/'  "exs/pb/$fam.pi" > "$WORK/${fam}_cp.pi"
    sed 's/^import pb,/import sat,/' "exs/pb/$fam.pi" > "$WORK/${fam}_sat.pi"
    grep -q "^import cp,\|^import sat," "$WORK/${fam}_cp.pi" "$WORK/${fam}_sat.pi" || {
        echo "SWAP FAILED for $fam" >&2; exit 1; }
done

for sz in $PHP; do
    for solver in pb cp sat; do
        ex="exs/pb/php_card.pi"; [ $solver = pb ] || ex="$WORK/php_card_${solver}.pi"
        run_one php_card "$sz" "$solver" "$ex"
    done
done
for sz in $PMODD; do
    for solver in pb cp sat; do
        ex="exs/pb/pmodd.pi"; [ $solver = pb ] || ex="$WORK/pmodd_${solver}.pi"
        run_one pmodd "$sz" "$solver" "$ex"
    done
done
for sz in $MCHESS; do
    for solver in pb cp sat; do
        ex="exs/pb/mchess.pi"; [ $solver = pb ] || ex="$WORK/mchess_${solver}.pi"
        run_one mchess "$sz" "$solver" "$ex"
    done
done
for sz in $EVENCOL; do
    for solver in pb cp sat; do
        ex="exs/pb/evencol.pi"; [ $solver = pb ] || ex="$WORK/evencol_${solver}.pi"
        run_one evencol "$sz" "$solver" "$ex"
    done
done
for sz in $SUBSETCARD; do
    for solver in pb cp sat; do
        ex="exs/pb/subsetcard.pi"; [ $solver = pb ] || ex="$WORK/subsetcard_${solver}.pi"
        run_one subsetcard "$sz" "$solver" "$ex"
    done
done
