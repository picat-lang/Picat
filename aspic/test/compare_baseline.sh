#!/usr/bin/env bash
# compare_baseline.sh: run every shipped aspic example with the CURRENT aspic
# sources and with the UNMODIFIED upstream sources (git HEAD), and compare the
# answers.
#
# Purpose: verify that a transpiler change (e.g. the v0.4.0 udf rewrite of the
# generated code) did not change the semantics of the examples.
#
# Methodology (per example):
#   .lp files  : the battery's proven flow - the transpiler generates the
#                program on stderr (gen.txt), the program is extracted with
#                awk (everything from the first "import sat/cp/aspic_runtime."
#                line) and run with a fixed seed; the ANSWER is the first
#                "solution" line each run prints (an optimization example's
#                line carries its objective value too), or the first UNSAT
#                marker when the run has no solution.
#   .pi files  : the PICASP embedded flow - aspic_prep pre-transpiles the
#                file (writing out.pi and aspic_runtime.pi into the run dir)
#                and the generated program runs; the FULL stdout of both runs
#                is compared.
#   money_fd_converted.pi is a static converted program: aspic_runtime.pi is
#                generated from the template of each source version (the only
#                substitution the prep does is ASPIC_CONSTRAINTS_LIB) and the
#                file runs directly.
#
# Usage: bash test/compare_baseline.sh
#   (runnable from anywhere; expects the Picat binary "picat" in the PATH and
#   the git repo; the repo's lib2 is put on PICATPATH for the udf layer of
#   the v0.4.0 generated programs; each run gets 120s; the run dirs live
#   under the repo's scratch/ folder and are removed at exit)
#
# Output: one MATCH/DIFF/SKIP line per example and a summary; exit status 0
# iff no example DIFFers (SKIP = neither version produced an answer, e.g. a
# construct neither transpiler supports).

set -u
ASPIC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
REPO_DIR="$(cd "$ASPIC_DIR/.." && pwd)"
cd "$ASPIC_DIR" || exit 1

if ! command -v picat >/dev/null 2>&1; then
    echo "compare_baseline: picat not found in PATH" >&2
    exit 1
fi

# the generated programs import udf (the v0.4.0 runtime's udf layer), which
# lives in the repo's lib2 one level above this folder
export PICATPATH="$REPO_DIR/lib2${PICATPATH:+:$PICATPATH}"

WORK="$(mktemp -d "$REPO_DIR/scratch/excmp_XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# the unmodified upstream sources (git HEAD) into the baseline folder
BASE="$WORK/baseline"
mkdir -p "$BASE" || exit 1
PIFILES="aspic.pi aspic_transpiler.pi aspic_gen.pi aspic_prep.pi aspic_runtime_template.pi"
for f in $PIFILES; do
    git show "HEAD:aspic/$f" > "$BASE/$f" || exit 1
done

TMO=120

pass=0; fail=0; skip=0; failed=""
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); failed="$failed
  $1"; }
skp() { skip=$((skip+1)); }

# run_lp DIR SRCDIR FILE: transpile + extract + execute (the battery's flow);
# run_out.txt holds the run output
run_lp() {
    local dir="$1" src="$2" file="$3"
    mkdir -p "$dir"
    for f in $PIFILES; do cp "$src/$f" "$dir/" 2>/dev/null; done
    cp "examples/$file" "$dir/"
    [ -f examples/edges.txt ] && cp examples/edges.txt "$dir/"
    ( cd "$dir" \
      && timeout $TMO picat -log aspic_transpiler.pi run "$file" >/dev/null 2>gen.txt \
      && awk '/^import (sat2|cp2|sat|cp|aspic_runtime)\./{found=1} found' gen.txt > gen.pi \
      && timeout $TMO picat -log -s 1234567890 gen.pi >run_out.txt 2>&1 )
}

# run_pi DIR SRCDIR FILE: the embedded flow; run_out.txt holds the run output
# (stdout without the -log compile logging; the runtime's solution report
# goes to stderr, so it is merged in with 2>&1)
run_pi() {
    local dir="$1" src="$2" file="$3"
    mkdir -p "$dir"
    for f in $PIFILES; do cp "$src/$f" "$dir/" 2>/dev/null; done
    cp "examples/$file" "$dir/"
    ( cd "$dir" \
      && timeout $TMO picat aspic_prep.pi pre "$file" . out.pi aspic_runtime_template.pi sat >prep_out.txt 2>&1 \
      && timeout $TMO picat -s 1234567890 out.pi >run_out.txt 2>&1 )
}

# run_static DIR SRCDIR FILE: generate aspic_runtime.pi from the template of
# the given source version and run the static converted program directly
run_static() {
    local dir="$1" src="$2" file="$3"
    mkdir -p "$dir"
    cp "examples/$file" "$dir/"
    sed 's/ASPIC_CONSTRAINTS_LIB/sat/' "$src/aspic_runtime_template.pi" > "$dir/aspic_runtime.pi"
    ( cd "$dir" && timeout $TMO picat -s 1234567890 "$file" >run_out.txt 2>&1 )
}

# answer FILE: the first solution line, or the first UNSAT marker when the
# run has no solution line
answer() {
    local s
    s=$(grep -m1 'solution' "$1" 2>/dev/null)
    if [ -n "$s" ]; then
        echo "$s"
    else
        grep -m1 'UNSAT' "$1" 2>/dev/null
    fi
}

echo "compare_baseline: .lp examples (current sources vs git HEAD sources)"
for f in examples/*.lp; do
    name=$(basename "$f")
    run_lp "$WORK/lp_new_$name" "$ASPIC_DIR" "$name"
    run_lp "$WORK/lp_old_$name" "$BASE" "$name"
    a1=$(answer "$WORK/lp_new_$name/run_out.txt")
    a2=$(answer "$WORK/lp_old_$name/run_out.txt")
    if [ -z "$a1" ] && [ -z "$a2" ]; then
        echo "  SKIP $name (no answer from either version)"
        skp
    else
        # the objective values must match; when the program has no optimize
        # directive the answer is just "a solution was found" - which of the
        # (possibly many) solutions the solver returns is timing-dependent
        # (the satext portfolio races its solvers), so the specific solution
        # is not compared
        o1=$(echo "$a1" | grep -oE '(optimization|maximization)\([0-9-]+\)' | head -1)
        o2=$(echo "$a2" | grep -oE '(optimization|maximization)\([0-9-]+\)' | head -1)
        if [ -n "$o1" ] || [ -n "$o2" ]; then
            if [ "$o1" = "$o2" ] && [ -n "$o1" ]; then
                echo "  MATCH $name (objective $o1)"
                ok
            else
                echo "  DIFF $name: new objective='${o1:-none}' old objective='${o2:-none}'"
                bad "$name: new objective='${o1:-none}' old objective='${o2:-none}'"
            fi
        elif [ -n "$a1" ] && [ -n "$a2" ]; then
            echo "  MATCH $name (both found a solution)"
            ok
        else
            echo "  DIFF $name: new='${a1:-none}' old='${a2:-none}'"
            bad "$name: new='${a1:-none}' old='${a2:-none}'"
        fi
    fi
done

echo "compare_baseline: .pi examples (the PICASP embedded flow)"
for f in examples/picasp_backpack.pi examples/asp_embedded_in_picat.pi examples/asp_embedded_in_picat-queens.pi; do
    [ -e "$f" ] || continue
    name=$(basename "$f")
    run_pi "$WORK/pi_new_$name" "$ASPIC_DIR" "$name"
    run_pi "$WORK/pi_old_$name" "$BASE" "$name"
    # the semantic output only: drop the non-deterministic noise (the satext
    # portfolio racing lines with their timestamps, the lint diagnostics whose
    # line numbers shift with the emitted shim line, the auxiliary-variable
    # counts which differ by design - the new emitter introduces fewer -, and
    # the timestamp lines)
    filt() { grep -vE '^(satext: |\*\*\* lint:|cpvars = |cprules = |date_time|Compiling::|\*\* Warning)' "$1" 2>/dev/null; }
    # the semantic output only, order-insensitive: a multi-solution example
    # may print the same solutions in a different order between the versions
    # (the backtracking order varies with the solver's search)
    filt "$WORK/pi_new_$name/run_out.txt" | sort > "$WORK/pi_new_$name/filt.txt"
    filt "$WORK/pi_old_$name/run_out.txt" | sort > "$WORK/pi_old_$name/filt.txt"
    if diff -q "$WORK/pi_new_$name/filt.txt" "$WORK/pi_old_$name/filt.txt" >/dev/null 2>&1; then
        echo "  MATCH $name"
        ok
    else
        echo "  DIFF $name (filtered stdout differs; first differences:)"
        diff "$WORK/pi_new_$name/filt.txt" "$WORK/pi_old_$name/filt.txt" | head -8 | sed 's/^/    /'
        bad "$name: the embedded flow's filtered stdout differs between the versions"
    fi
done

echo "compare_baseline: the static converted example (money_fd_converted.pi)"
run_static "$WORK/st_new" "$ASPIC_DIR" money_fd_converted.pi
run_static "$WORK/st_old" "$BASE" money_fd_converted.pi
if diff -q "$WORK/st_new/run_out.txt" "$WORK/st_old/run_out.txt" >/dev/null 2>&1; then
    echo "  MATCH money_fd_converted.pi"
    ok
else
    echo "  DIFF money_fd_converted.pi (full stdout differs; first differences:)"
    diff "$WORK/st_new/run_out.txt" "$WORK/st_old/run_out.txt" | head -8 | sed 's/^/    /'
    bad "money_fd_converted.pi: the static example's stdout differs between the versions"
fi

echo "compare_baseline: $pass matched, $fail differ, $skip skipped"
if [ "$fail" -gt 0 ]; then
    echo "differing examples:$failed"
    exit 1
fi
exit 0
