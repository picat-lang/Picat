#!/bin/sh
# err_msg_scan.sh -- inventory of the interpreter's parse-error messages.
#
# For each synthetic malformed construct (one defect per file) this runs the
# picat binary, records the first "*** SYNTAX ERROR ***" line and whether the
# <<HERE>> marker appeared, and prints a table.  Classification used in the
# analysis:
#   A = the <<HERE>> marker appears (tokenizer-level failure, exact position)
#   B = the message names the offending construct ("wrong iterators in
#       foreach") but gives no position
#   C = a vague one-phrase verdict with no position ("wrong list format")
#
# Usage:  sh test/err_msg_scan.sh [path-to-picat-binary]
# Default binary: <repo>/emu/picat.  Scratch files go to
# $PICAT_SCRATCH (default <repo>/scratch/errmsgs).

BIN="${1:-$(dirname "$0")/../emu/picat}"
case "$BIN" in
    /*) ;;
    *) BIN="$(pwd)/$BIN" ;;
esac
SCRATCH="${PICAT_SCRATCH:-$(dirname "$0")/../scratch/errmsgs}"
mkdir -p "$SCRATCH"

run_case() {
    name="$1"; shift
    cat > "$SCRATCH/$name.pi"
    out=$(cd "$SCRATCH" && timeout 20 "$BIN" "$name.pi" 2>&1)
    err=$(printf '%s' "$out" | grep -m1 'SYNTAX ERROR')
    here=$(printf '%s' "$out" | grep -c 'HERE')
    other=$(printf '%s' "$out" | grep -m1 '^\*\*\* error\|Free variable\|Undefined procedure')
    if [ -n "$err" ]; then
        msg=$err
    elif [ -n "$other" ]; then
        msg=$other
    else
        msg="(no error reported)"
    fi
    mark=""
    [ "$here" -gt 0 ] && mark=" [HERE]"
    printf '%-24s | %s%s\n' "$name" "$(printf '%s' "$msg" | cut -c1-72)" "$mark"
}

run_case foreach_missing_in <<'EOF'
main =>
    foreach(I 1..3)
        writeln(I)
    end.
EOF

run_case foreach_missing_end <<'EOF'
main =>
    foreach(I in 1..3)
        writeln(I).
EOF

run_case foreach_bad_iter_range <<'EOF'
main =>
    foreach(I in 1)
        writeln(I)
    end.
EOF

run_case foreach_dup_iterator_var <<'EOF'
main =>
    foreach(I in 1..3, I in 2..4)
        writeln(I)
    end.
EOF

run_case while_with_iterator <<'EOF'
main =>
    I = 0,
    while (I in 1..3)
        writeln(I)
    end.
EOF

run_case while_missing_end <<'EOF'
main =>
    T = 3,
    while (T > 0)
        T := T - 1.
EOF

run_case if_missing_then <<'EOF'
main =>
    X = 1,
    if X > 0 writeln("pos") else writeln("neg") end.
EOF

run_case if_missing_end <<'EOF'
main =>
    X = 1,
    if X > 0 then writeln("pos").
EOF

run_case list_missing_comma <<'EOF'
main =>
    L = [1 2 3],
    writeln(L).
EOF

run_case list_double_comma <<'EOF'
main =>
    L = [1,,2],
    writeln(L).
EOF

run_case list_bad_tail <<'EOF'
main =>
    L = [1|2,3],
    writeln(L).
EOF

run_case list_unclosed <<'EOF'
main =>
    L = [1,2,
    writeln(L).
EOF

run_case comprehension_missing_colon <<'EOF'
main =>
    L = [I*2 I in 1..3],
    writeln(L).
EOF

run_case comprehension_bad_iters <<'EOF'
main =>
    L = [I*2 : I 1..3],
    writeln(L).
EOF

run_case map_bad_arrow <<'EOF'
main =>
    M = new_map([a->1, b->2]),
    writeln(M).
EOF

run_case expr_double_operator <<'EOF'
main =>
    X = 1 + * 2,
    writeln(X).
EOF

run_case asp_list_bad_element <<'EOF'
main =>
    asp($[node(17), edge9,10)]),
    writeln("ok").
EOF

run_case call_missing_paren <<'EOF'
main =>
    writeln "hello".
EOF

run_case bad_number <<'EOF'
main =>
    X = 3.14.15,
    writeln(X).
EOF

run_case bad_module_qualifier <<'EOF'
main =>
    X = no_such_mod::f(1),
    writeln(X).
EOF

run_case unclosed_atom_quote <<'EOF'
main =>
    writeln('unclosed).
EOF

run_case unclosed_string <<'EOF'
main =>
    writeln("unclosed).
EOF

run_case table_bad_index <<'EOF'
main =>
    A = new_array(3),
    A[1,,
    writeln(A).
EOF
