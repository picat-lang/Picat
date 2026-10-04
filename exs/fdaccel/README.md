# exs/fdaccel: the scaled cp benchmark suite for picat-fdn

These are **scaled variants of the `exs/cp` examples**: the stock instances
there run in 16–32 ms, i.e. just process start-up, so nothing about solver
performance can be measured from them.  Here the instance size (or the
number of repeated solves) is raised so the wall time is dominated by
constraint solving and search, which makes stock-vs-native speedups
meaningful.

## What is being compared

- **stock**: the emulator's interpreted CLP(FD) path — the same code path as
  release Picat 3.9#12.  Run with `FDN=0`.
- **picat-fdn**: the experimental build's transparent native multicore FD
  solver behind `cp`'s `solve/1,2` (engages by default; see
  `Picat-src/emu/fdn.c`, `fdn_solver.cpp`, `fdn_hook.pi`).

Both run the *same binary*, so the only difference is the solver.  All
benchmarks print `STAT <name> runtime_ms=<ms> backtracks=<n>` (the `stat`
module in this directory); the solution results (solution counts,
sat/unsat) are identical between the two paths — only the search order and
timings differ.

## The benchmarks

| file | model | what it exercises |
|---|---|---|
| `qall.pi N` | N-queens, all solutions | enumeration with forward checking (`#!=` + `abs`); count all solutions |
| `qff.pi N` | N-queens, first solution | search-heavy, `ff` variable selection |
| `qffsplit.pi N` | N-queens | separates posting time from search time (`report(posted)` before solving) |
| `qpost.pi N` | N-queens | posting only (cubic in N), no search |
| `pigeon.pi M` | M+1 pigeons / M holes, 0/1 matrix | linear sums, unsat: proof by exhaustive search |
| `pigeon_ad.pi M` | same, `all_distinct` list model | fails at posting (no search at all) |
| `pigeon_sat.pi M` | same 0/1 model | `import sat` path |
| `knight.pi N` | knight's tour as `asp([$size(N)])` | the `asp` interface |
| `knight_sat.pi` | knight's tour | `import sat` path |
| `sudoku25.pi` | 25x25 sudoku | `all_different`: > 600 s (genuinely hard, both sides) |
| `sudoku25d.pi` | 25x25 sudoku | `all_distinct` variant |
| `sudoku25_sat.pi` | 25x25 sudoku | `import sat` path |
| `kakuroN.pi R` | kakuru puzzle, R repeated solves | posting-dominated; the ×1000 loop |
| `zebraN.pi R` | Lewis Carroll's zebra puzzle, R repeated solves | posting-dominated; the ×2000 loop |
| `ppm2.pi` | permutation pattern matching | ASP competition benchmark |
| `seq.pi N` | global constraint `sequence` | decomposition |

## Running

From the repo root, with the experimental binary built
(`cd Picat-src/emu && make -f Makefile.linux64 picat`):

```sh
# the whole suite, each benchmark on both solver paths:
sh exs/fdaccel/run_bench.sh

# a single benchmark:
sh exs/fdaccel/run_bench.sh          # (runner runs all; pass a binary as $1)
Picat-src/emu/picat exs/fdaccel/qff.pi 600            # picat-fdn
FDN=0 Picat-src/emu/picat exs/fdaccel/qff.pi 600      # stock
```

The runner accepts an optional binary path as its first argument
(`sh exs/fdaccel/run_bench.sh /path/to/picat`) and honours `TIMEOUT`
(default 300 s per side).  With `PROFILE=1` (in the original
`scratch/cpeval/run_bench.sh` form) it also records a perf flat profile.

**Thread tuning matters**: the native solver parallelises the search
across `FDN_THREADS` workers, defaulting to `thread::hardware_concurrency()`.
On a shared or cgroup-limited box the full count can OVER-subscribe and
collapse the parallel gain (e.g. pigeon 11/10: 0.93 s with the default
384 threads on this container vs 0.19–0.23 s at 32–64 threads vs 3.3 s
single-threaded).  Set `FDN_THREADS=32..64` for benchmarking here:

```sh
FDN_THREADS=32 Picat-src/emu/picat exs/fdaccel/pigeon.pi 10
```

## Reference results

Whole-program wall time; each column shows the measured time with the
acceleration vs stock Picat 3.9#12 underneath.  Measured 2026-10 on x86
(AMD EPYC 9654, shared cgroup-limited container) with the current
experimental build — complete runs, identical solution and backtrack
counts on both sides:

| program | stock 3.9#12 | FDN=0 | th=1 | th=4 | th=16 | th=64 |
|---|---|---|---|---|---|---|
| kakuro x1000 | 7.91 s | 8.81 s (0.9x) | 0.24 s (**33x**) | 0.25 s (**32x**) | 0.25 s (**32x**) | 0.25 s (**32x**) |
| pigeon 12/11 (unsat) | 17.85 s | 18.61 s (1.0x) | 35.55 s (0.5x) | 12.57 s (1.4x) | 2.79 s (**6.4x**) | 0.89 s (**20x**) |
| pigeon 11/10 (unsat) | 1.59 s | 1.72 s (0.9x) | 3.22 s (0.5x) | 0.98 s (1.6x) | 0.29 s (**5.5x**) | 0.23 s (**6.9x**) |
| queens-600, first solution | 144.12 s | 145.94 s (1.0x) | 18.66 s (**7.7x**) | 18.78 s (**7.7x**) | 18.81 s (**7.7x**) | 18.97 s (**7.6x**) |
| queens-14, count all | 3.15 s | 3.55 s (0.9x) | 3.57 s (0.9x) | 1.63 s (1.9x) | 1.33 s (**2.4x**) | 1.55 s (2.0x) |

Reading the numbers:

- **kakuro x1000: 33x even single-threaded** — 1000 small searches; the
  win is posting cost, not parallelism.
- **pigeon scales with cores** — but the single-threaded fdn is 2x
  SLOWER than stock (0.5x); it only wins with >= 16 threads.  Its value
  at small sizes is entirely the multicore behaviour.
- **queens-600: 7.7x completely flat** — a first-solution search does
  not benefit from more workers; the win is the native propagation.
- **queens-14, count all: 2.4x at th=16** — modest scaling; the stock's
  delay machinery is competitive at this size.
- **FDN=0 matches stock (0.9-1.0x)** — the transparency holds
  throughout.

For comparison, the fdaccel development phase
(`scratch/aa/fdaccel/RESULTS.md`, less-contended machine, default
thread count) measured: kakuro x1000 41x, pigeon 12/11 24x, pigeon
11/10 16x, queens-600 7.9x, queens-14 2.3x — consistent with the table
above modulo machine contention and thread counts.
