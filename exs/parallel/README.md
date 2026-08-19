# Parallel Picat — examples and benchmarks

This folder contains the examples for the `parallel` branch, which
makes Picat parallel in the open-source 3.9 tree (which ships without
a `thread` module and runs a single-threaded VM).

## What the branch adds

| commit | feature | entry points |
|--------|---------|--------------|
| C1 | data-parallel reductions, pthread fork-join engine (`emu/par.c`) | `par.par_sum/prod/min/max` |
| C2 | parallel prefix scan, elementwise map, parallel fast-doubling fib array | `par.par_scan`, `par.par_scale`, `par.par_fib_fast` |
| C3 | real-OS-thread module: `T = new_thread(Task, Args); T.start(); join(T); R = result(T)`, mutexes, semaphores, counters (`emu/thread.c`) | module `thread` |
| C4 | high-level functional API + pure-Picat sequential baselines | module `pp` (`psum`, `pfib`, `fanout_sum`, `fib_linear`, `fib_doubling`) |

Design constraint: the VM is single-threaded (only the toam-loop
thread may execute Picat code, and the arena only grows at
instruction boundaries). Therefore:

* the C data-parallel ops run pthread workers on plain C buffers and
  build the result list on the VM thread after the join;
* `thread` workers run registered C tasks (never the VM) and hand
  results back through a mailbox read after `join()`;
* large results that don't fit the inline integer range are returned
  as exact bigints (use `%w`, not `%d`, to print them — `%d` shows
  1-word ints truncated to 32 bits).

## Build

```
cd emu
make -f Makefile.linux64 par.o thread.o picat
```

## Run

```
export PICATPATH=$PWD/lib2          # from the repo root
./emu/picat exs/parallel/<example>.pi
```

or simply:

```
./exs/parallel/bench_parallel.sh
```

## Examples

* `par_reduce.pi` (C1) — correctness checks for the reductions,
  including mod-2^64 wraparound and exact big sums.
* `par_scan_fib.pi` (C2) — scan/scale/fib checks; `fib(90)` exact;
  self-timed `par_fib_fast(10^5)` / `par_scan(10^6)`.
* `threads_fork_join.pi` (C3) — NT pthreads sum disjoint ranges of
  1..10^8; wall-clock speedup 1 thread vs 8 threads.
* `threads_mutex_race.pi` (C3) — shared-counter race: unprotected
  bumps lose updates; mutex-guarded bumps are exact.
* `pp_layer.pi` (C4) — the `pp` API; cross-checks three independent
  fib implementations (parallel C, pure-Picat O(log n), pure-Picat
  O(n)); fork-join through the `pp` layer.
* `bench_pi.pi` — size sweep of `par_fib_fast` vs `fib_linear`.

## Measured numbers (384-core x86-64, this tree, -O3)

| benchmark | time |
|-----------|------|
| `par_fib_fast(10^5)` | ~12 ms |
| `par_fib_fast(10^6)` | ~47 ms |
| `par_fib_fast(10^7)` | ~320 ms |
| `par_fib_fast(10^8)` | ~3.0 s |
| `fib_linear(10^6)` pure Picat, sequential | ~250 ms |
| `fib_linear(10^7)` pure Picat, sequential | ~2.0 s |
| fork-join sum 1..10^8, 1 thread / 8 threads | 29 / 8 ms |
| mutex race, 8 threads x 2e5 bumps | unprotected loses ~50%; guarded exact |

Comparison with the Futhark reference built earlier on the same
machine (`map` of fib over 10^6 elements): Futhark C 13719 ms,
Futhark multicore 284 ms, Futhark H100 222 ms — Picat's
`par_fib_fast(10^6)` at ~50 ms is ~5-275x faster (it computes each
fib(1..N) in parallel by O(log n) fast doubling, whereas the Futhark
kernel maps an O(n) fibonacci).
