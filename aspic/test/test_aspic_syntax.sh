#!/usr/bin/env bash
# Regression tests for the aspic ASP-to-Picat transpiler (picat/aspic).
#
# Covers:
#   1. Syntax battery (test/cases/*.lp):
#        ok_*  : must parse (stdout reports "notparsed = []"); generated code
#                must not crash ("*** error(" in stderr fails the case).
#        err_  : must be reported as unparsed ("notparsed = [ ... ]"), with a
#                clean diagnostic and no crash. These are constructs aspic
#                does not support (see the README "Not implemented" section):
#                the point is that they fail LOUDLY, not silently.
#        run_* : transpiled and executed end-to-end; output is checked.
#   2. Shipped examples: every examples/*.lp must still parse.
#   3. Embedded-ASP flow: aspic_prep pre-transpiles
#      examples/asp_embedded_in_picat-queens.pi and the result must run.
#
# Usage: bash test/test_aspic_syntax.sh
# (runnable from anywhere; expects the Picat binary "picat" in the PATH)
#
# Exit status: 0 iff all tests pass.

set -u
ASPIC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ASPIC_DIR"

if ! command -v picat >/dev/null 2>&1; then
    echo "test_aspic_syntax: picat not found in PATH" >&2
    exit 1
fi

TMP="$(mktemp -d "$ASPIC_DIR/test/tmp_XXXXXX")"
trap 'rm -rf "$TMP"; rm -f t.pi t2.pi' EXIT

pass=0
fail=0
failed=""

ok() {
    pass=$((pass+1))
}

bad() {
    fail=$((fail+1))
    failed="$failed
  $1"
}

# run_case FILE: transpile FILE, stdout->out.txt, stderr->err.txt
run_case() {
    picat -log aspic_transpiler.pi run "$1" >"$TMP/out.txt" 2>"$TMP/err.txt"
}

# notparsed status: "parse_ok", "parse_err" or "crash"
status() {
    if grep -q '^\*\*\* error(' "$TMP/err.txt" 2>/dev/null; then
        echo crash
    elif grep -q 'notparsed = \[\]' "$TMP/out.txt" 2>/dev/null; then
        echo parse_ok
    else
        echo parse_err
    fi
}

# ---- 1. ok_* cases ---------------------------------------------------------

for f in test/cases/ok_*.lp; do
    name=$(basename "$f")
    run_case "$f"
    st=$(status)
    if [ "$st" != "parse_ok" ]; then
        bad "$name: expected parse_ok, got $st ($(grep -m1 'notparsed\|error' "$TMP/out.txt" "$TMP/err.txt" | head -1))"
        continue
    fi
    case "$name" in
    ok_minimize.lp)
        if ! grep -q 'ASPIC_OPT#=sum(' "$TMP/err.txt"; then
            bad "$name: generated code lacks the #minimize objective"
            continue
        fi ;;
    ok_maximize.lp)
        if ! grep -q 'aspic_solve(ASPIC_SEARCH,ASPIC_OPT,max)' "$TMP/err.txt"; then
            bad "$name: generated code lacks the maximize direction in aspic_solve"
            continue
        fi
        if ! grep -q 'aspic_solve_dir(max)' "$TMP/err.txt"; then
            bad "$name: generated code lacks aspic_solve_dir(max)"
            continue
        fi ;;
    ok_not_paren.lp)
        if ! grep -q 'aspic_not(aspic_var(q))' "$TMP/err.txt"; then
            bad "$name: not(q) did not become negation (aspic_not) in the generated code"
            continue
        fi
        if grep -q '\$not(' "$TMP/err.txt"; then
            bad "$name: not(q) became a spurious positive atom \$not(...)"
            continue
        fi ;;
    ok_show_tolerated.lp)
        if ! grep -q '% aspic: WARNING: #show' "$TMP/err.txt"; then
            bad "$name: #show was not reported as an ignored line (warning comment expected)"
            continue
        fi ;;
    ok_weak.lp)
        if ! grep -q 'ASPIC_OPT#=sum(' "$TMP/err.txt"; then
            bad "$name: the weak constraint did not produce the penalty objective"
            continue
        fi
        if ! grep -q 'aspic_solve(ASPIC_SEARCH,ASPIC_OPT,min)' "$TMP/err.txt"; then
            bad "$name: the weak constraint did not minimize (aspic_solve(...,min) expected)"
            continue
        fi ;;
    ok_weak_multi.lp)
        if ! grep -q 'ASPIC_OPT#=sum(.*)+sum(' "$TMP/err.txt"; then
            bad "$name: the two weak constraints were not merged into one summed objective"
            continue
        fi ;;
    ok_weak_not.lp)
        if ! grep -q 'aspic_not' "$TMP/err.txt"; then
            bad "$name: negation in the weak constraint body did not become aspic_not"
            continue
        fi ;;
    esac
    ok
done

# ---- 2. err_* cases --------------------------------------------------------

for f in test/cases/err_*.lp; do
    name=$(basename "$f")
    run_case "$f"
    st=$(status)
    if [ "$st" != "parse_err" ]; then
        bad "$name: expected parse_err, got $st"
        continue
    fi
    case "$name" in
    err_maximize.lp|err_include.lp|err_heuristic.lp|err_bare_hash.lp)
        if ! grep -q 'unsupported ASP directive' "$TMP/out.txt"; then
            bad "$name: expected 'unsupported ASP directive' in the diagnostic"
            continue
        fi ;;
    err_const_string.lp)
        if ! grep -q 'invalid #const' "$TMP/out.txt"; then
            bad "$name: expected 'invalid #const' in the diagnostic"
            continue
        fi ;;
    err_multiline_minimize.lp)
        if ! grep -q 'invalid #minimize' "$TMP/out.txt"; then
            bad "$name: expected 'invalid #minimize' in the diagnostic"
            continue
        fi ;;
    err_weak_invalid.lp)
        if ! grep -q 'invalid weak constraint' "$TMP/out.txt"; then
            bad "$name: expected 'invalid weak constraint' in the diagnostic"
            continue
        fi ;;
    esac
    ok
done

# ---- 3. run_* cases (transpile + execute) ----------------------------------

for f in test/cases/run_*.lp; do
    name=$(basename "$f")
    picat -log aspic_transpiler.pi run "$f" >/dev/null 2>"$TMP/gen.txt"
    if grep -q '^\*\*\* error(' "$TMP/gen.txt"; then
        bad "$name: transpile crashed"
        continue
    fi
    # keep only the generated program (drop picat compile messages)
    awk '/^import (sat|cp)\./{found=1} found' "$TMP/gen.txt" > "$TMP/gen.pi"
    picat -log -s 1234567890 "$TMP/gen.pi" >"$TMP/run_out.txt" 2>&1
    case "$name" in
    run_negation.lp)
        sol=$(grep 'solution' "$TMP/run_out.txt" | head -1)
        if [ -z "$sol" ]; then
            bad "$name: no solution printed"
        elif ! echo "$sol" | grep -q 'p'; then
            bad "$name: p missing from the solution ($sol)"
        elif echo "$sol" | grep -q 'q'; then
            bad "$name: q should be false, got $sol"
        else
            ok
        fi ;;
    run_two_models.lp)
        if grep -q 'solution' "$TMP/run_out.txt"; then
            ok
        else
            bad "$name: no solution printed (one of the two stable models expected)"
        fi ;;
    run_maximize.lp)
        sol=$(grep -oE 'maximization\([0-9-]+\)' "$TMP/run_out.txt" | head -1)
        if [ "$sol" != "maximization(13)" ]; then
            bad "$name: expected maximization(13), got ${sol:-nothing}"
        else
            ok
        fi ;;
    run_firstpos.lp)
        sol=$(grep -oE 'maximization\([0-9-]+\)' "$TMP/run_out.txt" | head -1)
        if [ "$sol" != "maximization(13)" ]; then
            bad "$name: expected maximization(13) with the choice rule in the first position, got ${sol:-nothing}"
        else
            ok
        fi ;;
    run_weak.lp)
        sol=$(grep -oE 'optimization\([0-9-]+\)' "$TMP/run_out.txt" | head -1)
        if [ "$sol" != "optimization(2)" ]; then
            bad "$name: expected the soft weighted optimum optimization(2), got ${sol:-nothing}"
        else
            ok
        fi ;;
    run_aggregate_atoms.lp)
        sol=$(grep 'solution' "$TMP/run_out.txt" | head -1)
        if [ -z "$sol" ]; then
            bad "$name: no solution printed"
        elif ! echo "$sol" | grep -q 'q'; then
            bad "$name: q should hold (the atom aggregate >= 1 forces at least one p), got $sol"
        elif ! echo "$sol" | grep -q 'p('; then
            bad "$name: at least one p should be chosen, got $sol"
        else
            ok
        fi ;;
    run_island_spaces.lp)
        sol=$(grep 'solution' "$TMP/run_out.txt" | head -1)
        if [ -z "$sol" ]; then
            bad "$name: no solution printed"
        elif ! echo "$sol" | grep -q 'r'; then
            bad "$name: r should hold (the spaced island evaluates to 1), got $sol"
        else
            ok
        fi ;;
    run_condlit.lp)
        sol=$(grep 'solution' "$TMP/run_out.txt" | head -1)
        if [ -z "$sol" ]; then
            bad "$name: no solution printed"
        elif ! echo "$sol" | grep -q 'all'; then
            bad "$name: all should hold (the conditional literal is forced), got $sol"
        elif ! echo "$sol" | grep -q 'p(1)' || ! echo "$sol" | grep -q 'p(2)' || ! echo "$sol" | grep -q 'p(3)'; then
            bad "$name: all three p should be chosen (all <-> the universal conditional literal), got $sol"
        else
            ok
        fi ;;
    run_condlit_neg.lp)
        sol=$(grep 'solution' "$TMP/run_out.txt" | head -1)
        if [ -z "$sol" ]; then
            bad "$name: no solution printed"
        elif ! echo "$sol" | grep -q 's'; then
            bad "$name: s should hold (the instance X=2 is violated: dom(2) holds, p(2) false), got $sol"
        elif echo "$sol" | grep -q 'p(2)'; then
            bad "$name: p(2) should be false (not derivable), got $sol"
        else
            ok
        fi ;;
    run_condlit_unsat.lp)
        if grep -q 'solution' "$TMP/run_out.txt"; then
            bad "$name: expected UNSAT (the universal cannot hold with p(2) forbidden), got a solution"
        else
            ok
        fi ;;
    run_aggregate_neg.lp)
        sol=$(grep 'solution' "$TMP/run_out.txt" | head -1)
        if [ -z "$sol" ]; then
            bad "$name: no solution printed"
        elif ! echo "$sol" | grep -q 'q'; then
            bad "$name: q should hold (the negated element forces at least one false p), got $sol"
        elif echo "$sol" | grep -q 'p(1)' && echo "$sol" | grep -q 'p(2)'; then
            bad "$name: not both p(1) and p(2) should be chosen (at least one p must be false), got $sol"
        else
            ok
        fi ;;
    run_aggregate_neg_all.lp)
        sol=$(grep 'solution' "$TMP/run_out.txt" | head -1)
        if [ -z "$sol" ]; then
            bad "$name: no solution printed"
        elif echo "$sol" | grep -q 'q'; then
            bad "$name: q should not hold (no p is false, the negated sum is 0), got $sol"
        else
            ok
        fi ;;
    run_mixed_minmax.lp)
        sol=$(grep -oE 'optimization\([0-9-]+\)' "$TMP/run_out.txt" | head -1)
        if [ "$sol" != "optimization(-13)" ]; then
            bad "$name: expected the unified objective optimization(-13), got ${sol:-nothing}"
        elif grep 'solution' "$TMP/run_out.txt" | head -1 | grep -q 'q'; then
            bad "$name: q should be false (it costs 1), got a solution with q"
        else
            ok
        fi ;;
    run_weak_max_mix.lp)
        sol=$(grep -oE 'optimization\([0-9-]+\)' "$TMP/run_out.txt" | head -1)
        if [ "$sol" != "optimization(-13)" ]; then
            bad "$name: expected the unified objective optimization(-13), got ${sol:-nothing}"
        else
            ok
        fi ;;
    esac
done

# ---- 3b. run_*.pi cases (embedded flow: aspic_prep + execute) --------------

for f in test/cases/run_*.pi; do
    [ -e "$f" ] || continue
    name=$(basename "$f")
    mkdir -p "$TMP/embcase"
    if ! picat aspic_prep.pi pre "$f" "$TMP/embcase" "$TMP/embcase/out.pi" aspic_runtime_template.pi sat >/dev/null 2>&1 \
       || [ ! -f "$TMP/embcase/out.pi" ]; then
        bad "$name: aspic_prep failed to pre-transpile"
        continue
    fi
    ( cd "$TMP/embcase" && picat -log -s 1234567890 out.pi >run_out.txt 2>&1 )
    case "$name" in
    run_emb_maximize.pi)
        if grep -q '^10$' "$TMP/embcase/run_out.txt"; then
            ok
        else
            bad "$name: expected the maximized objective 10 on stdout, got: $(tail -1 "$TMP/embcase/run_out.txt")"
        fi ;;
    run_emb_weak.pi)
        if grep -q '^1$' "$TMP/embcase/run_out.txt"; then
            ok
        else
            bad "$name: expected the soft weighted optimum 1 on stdout, got: $(tail -1 "$TMP/embcase/run_out.txt")"
        fi ;;
    run_emb_comp.pi)
        if ! grep -q '^s = 0$' "$TMP/embcase/run_out.txt"; then
            bad "$name: s should be false (the body q,p(2) fails with p(2) forced false by the completion), got: $(grep '^s' "$TMP/embcase/run_out.txt")"
        elif ! grep -q '^p2 = 0$' "$TMP/embcase/run_out.txt"; then
            bad "$name: p(2) should be forced false by the completion (defined by no rule), got: $(grep '^p2' "$TMP/embcase/run_out.txt")"
        elif ! grep -q '^q = 1$' "$TMP/embcase/run_out.txt"; then
            bad "$name: q should hold (the rule direction works), got: $(grep '^q' "$TMP/embcase/run_out.txt")"
        else
            ok
        fi ;;
    *)
        ok ;;
    esac
done

# ---- 4. shipped examples regression ----------------------------------------

for f in examples/*.lp; do
    name=$(basename "$f")
    run_case "$f"
    st=$(status)
    if [ "$st" != "parse_ok" ]; then
        bad "examples/$name: expected parse_ok, got $st"
        continue
    fi
    ok
done

# ---- 5. embedded-ASP flow end-to-end ---------------------------------------

mkdir -p "$TMP/emb"
if picat aspic_prep.pi pre examples/asp_embedded_in_picat-queens.pi "$TMP/emb" "$TMP/emb/out.pi" aspic_runtime_template.pi sat >/dev/null 2>&1 \
   && [ -f "$TMP/emb/out.pi" ] && [ -f "$TMP/emb/aspic_runtime.pi" ]; then
    ( cd "$TMP/emb" && picat -log -s 1234567890 out.pi >run_out.txt 2>&1 )
    if grep -q -e '-----------------------------------' "$TMP/emb/run_out.txt"; then
        ok
    else
        bad "embedded flow: generated program produced no solution output"
    fi
else
    bad "embedded flow: aspic_prep failed to pre-transpile asp_embedded_in_picat-queens.pi"
fi

# ---- report -----------------------------------------------------------------

echo "test_aspic_syntax: $pass passed, $fail failed"
if [ "$fail" -gt 0 ]; then
    echo "failed:$failed"
    exit 1
fi
exit 0
