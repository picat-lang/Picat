#!/bin/sh
# err_classify.sh -- classify the error cases in err_report_fuzz reports.
#
# Reads one or more report files (as produced by test/err_report_fuzz.sh;
# per-case blocks start with "=== ") and prints the count per class plus the
# marker coverage for parse errors and one example file per class.
#
# Classes (the corrected classification, 2026-10-17):
#   parse-here    parse error, <<HERE>> marker present (exact position)
#   parse-span    parse error, no marker (rule span only)
#   contiguous    "Predicate or function is not defined contiguously."
#   undef-proc    runtime "Undefined procedure" (intercepted by the exit lint)
#   misleading    free-variable / instantiation errors (operator blamed)
#   typed-err     typed compile error without a span (collection_expected..)
#   oddity        runtime oddity from a malformed constant (type_args..)
#   other-err     any other rc=1/2 output
#   non-error     rc=0 or timeout (no error to classify)
#
# Usage: sh test/err_classify.sh report1.txt [report2.txt ...]

SCRATCH="${PICAT_SCRATCH:-$(dirname "$0")/../scratch/errclassify}"
mkdir -p "$SCRATCH"

awk '
/^=== / {                      # new case: flush the previous block
    if (n > 0) print blk > (dir "/b" n)
    n++
    blk = $0 "\n"
    next
}
{ blk = blk $0 "\n" }
END { if (n > 0) print blk > (dir "/b" n) }
' dir="$SCRATCH" "$@" 2>/dev/null

classify() {
    blk="$1"
    if printf '%s' "$blk" | grep -q 'SYNTAX ERROR'; then
        if printf '%s' "$blk" | grep -q 'HERE'; then echo parse-here
        elif printf '%s' "$blk" | grep -q 'contiguously'; then echo contiguous
        else echo parse-span; fi
    elif printf '%s' "$blk" | grep -q 'Undefined procedure'; then echo undef-proc
    elif printf '%s' "$blk" | grep -q 'Free variable\|instantiation_error'; then echo misleading
    elif printf '%s' "$blk" | grep -q 'type_args'; then echo oddity
    elif printf '%s' "$blk" | grep -q 'collection_expected\|type_error\|domain_error\|existence_error'; then echo typed-err
    elif printf '%s' "$blk" | grep -qE 'rc=(1|2)'; then echo other-err
    else echo non-error; fi
}

: > "$SCRATCH/classes"
for b in "$SCRATCH"/b*; do
    [ -e "$b" ] || continue
    blk=$(cat "$b")
    cls=$(classify "$blk")
    fex=$(grep -m1 '^=== ' "$b" | awk '{print $2}')
    echo "$cls $(basename "$fex" 2>/dev/null)" >> "$SCRATCH/classes"
done

echo "== class counts (n cases) =="
awk '{print $1}' "$SCRATCH/classes" | sort | uniq -c | sort -rn
echo
echo "== parse-error marker coverage =="
ph=$(grep -c '^parse-here' "$SCRATCH/classes"); ps=$(grep -c '^parse-span' "$SCRATCH/classes")
echo "parse-here=$ph parse-span=$ps"
echo
echo "== one example per class =="
for cls in parse-here parse-span contiguous undef-proc misleading typed-err oddity other-err; do
    ex=$(awk -v c="$cls" '$1 == c {print $2; exit}' "$SCRATCH/classes")
    [ -n "$ex" ] && printf '%-12s: %s\n' "$cls" "$ex"
done