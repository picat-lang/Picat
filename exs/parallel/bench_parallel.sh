#!/usr/bin/env bash
# bench_parallel.sh - run all parallel-branch examples and the size
# sweep. Requires the rebuilt engine (emu/picat) and PICATPATH=lib2.
#
# Usage:  exs/parallel/bench_parallel.sh

set -u
ROOT="$(cd "$(dirname "$0")/../../" && pwd)"
PICAT="$ROOT/emu/picat"
export PICATPATH="$ROOT/lib2"

if [ ! -x "$PICAT" ]; then
    echo "engine not found: $PICAT"
    echo "build it with:  cd $ROOT/emu && make -f Makefile.linux64 par.o thread.o picat"
    exit 1
fi

E="$ROOT/exs/parallel"

run() {
    echo
    echo "==================================================================="
    echo "=== $1"
    echo "==================================================================="
    timeout 600 "$PICAT" "$E/$2"
    rc=$?
    if [ $rc -ne 0 ]; then
        echo "!!! $2 FAILED (rc=$rc)"
        exit $rc
    fi
}

run "C1: data-parallel reductions"            par_reduce.pi
run "C2: scan / scale / parallel fib array"   par_scan_fib.pi
run "C3: fork-join over real pthreads"        threads_fork_join.pi
run "C3: shared-counter race vs mutex"        threads_mutex_race.pi
run "C4: high-level pp layer"                 pp_layer.pi
run "size sweep (par fib array + sequential)" bench_pi.pi

echo
echo "all parallel examples passed"
