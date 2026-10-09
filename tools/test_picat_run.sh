#!/bin/sh
# Tests for tools/picat-run, the wrapper that runs Picat files with a "#!"
# first line as executable scripts (./prog args).
#
# The example scripts are written into a temporary directory, each case is
# run, and every case prints PASS or FAIL. The exit code is 0 only if all
# cases pass.
#
# Usage:
#   tools/test_picat_run.sh              # picat must be on PATH
#   PICAT=/path/to/picat tools/test_picat_run.sh
#   tools/test_picat_run.sh --fdn        # also test the fdn build (../emu/picat);
#                                        # without --fdn it is tested only if built
set -u

here=$(cd "$(dirname "$0")" && pwd) || exit 2
run=$here/picat-run
if [ ! -x "$run" ]; then
    echo "test_picat_run: $run not found or not executable" >&2
    exit 2
fi

fdn=0
if [ "${1:-}" = "--fdn" ]; then
    fdn=1
fi
fdnbin=$here/../emu/picat

pass=0
fail=0

# 1. An extensionless script with main(Args), run directly like ./prog,
#    with one argument containing a space.
#    Expected: "args = [a,b c]" on stdout, exit code 0.
work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT HUP INT TERM

cat > "$work/hello" <<'EOF'
#!/usr/bin/env picat-run
main(Args) => println(args=Args).
EOF
chmod +x "$work/hello"

out=$(PATH="$here:$PATH" "$work/hello" a "b c" 2>&1)
if [ $? -eq 0 ] && [ "$out" = "args = [a,b c]" ]; then
    echo "PASS  hello a 'b c' prints args = [a,b c]"
    pass=$((pass+1))
else
    echo "FAIL  hello a 'b c' prints args = [a,b c] (got: $out)"
    fail=$((fail+1))
fi

# 2. A syntax error is reported at the original line numbers: the broken
#    line is line 3 of bad.pi, and the "#!" line is not counted away.
#    Expected: the error position (3-4), exit code nonzero.
cat > "$work/bad.pi" <<'EOF'
#!/usr/bin/env picat-run
% line 2
this line is broken (
EOF

out=$("$run" "$work/bad.pi" 2>&1)
if [ $? -ne 0 ] && echo "$out" | grep -q '(3-4)'; then
    echo "PASS  bad.pi syntax error reported at (3-4), exit nonzero"
    pass=$((pass+1))
else
    echo "FAIL  bad.pi syntax error reported at (3-4), exit nonzero (got: $out)"
    fail=$((fail+1))
fi

# 3. "import helper." finds helper.pi next to the script, whatever the
#    current directory is: run the script from its parent directory.
#    Expected: "from helper" on stdout, exit code 0.
mkdir -p "$work/sub"
cat > "$work/sub/useit" <<'EOF'
#!/usr/bin/env picat-run
import helper.
main => go.
EOF
cat > "$work/sub/helper.pi" <<'EOF'
module helper.

go =>
    println('from helper').
EOF
chmod +x "$work/sub/useit"

out=$(cd "$work" && PATH="$here:$PATH" sub/useit 2>&1)
if [ $? -eq 0 ] && [ "$out" = "from helper" ]; then
    echo "PASS  sub/useit imports the module next to the script"
    pass=$((pass+1))
else
    echo "FAIL  sub/useit imports the module next to the script (got: $out)"
    fail=$((fail+1))
fi

# 4. The fdn build runs through the wrapper: an FD program (queens-12)
#    solves and prints its runtime.
#    Expected: "STAT runtime_ms=" on stdout, exit code 0.
if [ $fdn -eq 1 ] && [ ! -x "$fdnbin" ]; then
    echo "FAIL  fdn queens-12: --fdn given but $fdnbin is not built"
    fail=$((fail+1))
elif [ -x "$fdnbin" ]; then
    cat > "$work/fdqueens" <<'EOF'
#!/usr/bin/env picat-run
import cp.
main([A]) => N=to_int(A), Qs=new_array(N), Qs :: 1..N,
    foreach (I in 1..N-1, J in I+1..N) Qs[I] #!= Qs[J], abs(Qs[I]-Qs[J]) #!= J-I end,
    solve([ff],Qs), statistics(runtime,[T,_]), printf("STAT runtime_ms=%w%n",T).
EOF
    out=$("$run" "$work/fdqueens" 12 2>&1)
    if [ $? -eq 0 ] && echo "$out" | grep -q 'STAT runtime_ms='; then
        echo "PASS  fdn queens-12 solves through the wrapper"
        pass=$((pass+1))
    else
        echo "FAIL  fdn queens-12 solves through the wrapper (got: $out)"
        fail=$((fail+1))
    fi
fi

echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
