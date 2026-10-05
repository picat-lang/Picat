# picat-fdn: transparent native multicore FD solver

The engine in `Picat-src/emu/fdn.c`, `fdn.h` and `fdn_solver.cpp` is the
unmodified Picat 3.9#12 emulator plus a native C++ search engine for CP models.
Programs run unchanged: `picat prog.pi args`. Every `solve/1,2` of the `cp`
module checks the constraint network that is live at that moment:

- **Supported** (see below): the whole search runs natively on all cores.
  The solutions come back to Picat one at a time, in exactly the order
  Picat's own labeling would produce them, and `statistics(backtracks)`
  advances by exactly Picat's count.
- **Anything else:** Picat's own labeling runs, unchanged.

A bare cp `solve` inside `count_all` is counted natively, in parallel,
without handing solutions to Picat; only the number goes back
(`FDN_COUNT=0` reverts to the old path).

## Build, run, test

```sh
cd emu && make -f Makefile.linux64 -j8 picat FDN_ARCH=-mpopcnt
cd .. && emu/picat exs/fd_native_mt/mixed.pi        # transparency test
emu/picat exs/cp/queens.pi                          # any cp program
FDN=0 emu/picat exs/cp/queens.pi                    # hook off: exactly Picat 3.9#12
sh exs/fd_native_mt/run_bench.sh                    # the benchmark suite
```

`FDN_ARCH=-mpopcnt` is for x86_64; omit it on other platforms. `fdn_hook.pi`
must sit next to the binary (it does, in `emu/`); `FDN_HOOK=path` overrides
this.

Environment variables:

| variable | effect |
|---|---|
| `FDN=0` | do not install the hook (exactly Picat 3.9#12) |
| `FDN_COUNT=0` | do not repoint `count_all/2` (solutions through Picat again) |
| `FDN_THREADS=n` | threads per search (default: all cores; see the tuning note below) |
| `FDN_VERBOSE=1` | report each `solve` on stderr: native search (size) or the fallback reason |
| `FDN_SPAWN=n` | start the thread pool after n nodes (default 20000; small searches stay on the calling thread) |

## Supported propagators

| Picat source | native propagator |
|---|---|
| `X #!= Y`, `X #!= Y+C`, `abs(X-Y) #!= C` | forward checking; the edges are read from the `'$combined_neq'` frame, which is what Picat runs after a unification |
| `all_different` | forward checking |
| `all_distinct` | FC + Hall check anchored at the changed variable |
| `all_distinct` on a permutation | + primal/dual channelling |
| `sum`, `#=`, `#>=`, `#=<` (linear) | Picat's single ordered propagation pass (`nary_interval_consistent_eq/ge`) with Picat's wake rules; ARC sums reach arc consistency once ≤ 2 variables are unbound |
| `X #= Y+C`, `X #= C-Y` | value mapping (arc consistent) + the `'$v_in_*_int'` bounds frames on a unification |
| `abs(X-Y) #= C` | domain filtering + bounds + FC per removal |
| `X*Y #= Z` | no propagation while all three unbound; one operand fixed → 2-var ARC; result fixed → sign+interval+sound-div factoring; exact square-root filtering for `Y*Y` |
| `X div y #= Z`, `X mod y #= Z` (fixed divisor) | arc rule on Z's removals / full domain filtering (skip when max(X) ≥ 3000) |
| `R #= min([...])`, `R #= max([...])` | bound-event rule replay |
| `B <=> X=c`, `B <=> X!=c`, `B <=> X=Y`, `B <=> X!=Y`, `B <=> X>=Y` | reification (incl. the `#\/` expansion of `alldifferent_except_0`) |
| `B => X=c`, ... | entailment (one-directional) |
| `element(I, [...], V)` | unification maintenance (I fixed), value→range table (V fixed) + FC both directions |
| `global_cardinality` (via gcc decomposition) | elements + reified equalities + linear sums |
| lex chains (`e$$cp$$watch_lex_lt/le`) | watch bounds on the leading pairs, arc-consistent last pair |

Labeling: heuristics `[]`/`leftmost`, `ff`, `min`, `max`, `ffd`, `degree`,
`constr`, `ffc`; value strategies `up`, `down`, `split`, `reverse_split`.
Still falling back to Picat's labeling: `updown`, the non-atom options
(`minimize`/`maximize`/`$report`), `inout`, `forward`, `rand*`, other
attributes, value ranges wider than 4096.

## `mixed.pi`

The transparency test: each block prints solutions / counts and the
`backtracks` statistic, which must be identical to Picat 3.9#12. Run it with
and without `FDN=0` and compare. It covers findall / count_all
enumeration, ARC and general linear constraints, binary equalities, `ffc`,
partially instantiated label lists, `element` with a constant index, and a
fallback case (`circuit`).

## The scaled benchmark suite

The `*.pi` files here (except `mixed.pi`, `stat.pi`) are **scaled variants
of the `exs/cp` examples**: the Picat 3.9#12 instances there run in 16–32 ms, i.e.
just process start-up, so nothing about solver performance can be measured
from them.  Here the instance size (or the number of repeated solves) is
raised so the wall time is dominated by constraint solving and search.

- **Picat 3.9#12**: the interpreted CLP(FD) path (`FDN=0`), the same code path as
  release Picat 3.9#12.
- **picat-fdn**: the native multicore solver (the default).

Both run the *same binary*.  The `stat` module prints
`STAT <name> runtime_ms=<ms> backtracks=<n>`; the solution results
(solution counts, sat/unsat) are identical between the two paths.

| file | model | what it exercises |
|---|---|---|
| `qall.pi N` | N-queens, all solutions | `count_all(solve(..))`, counted natively; enumeration with forward checking (`#!=` + `abs`) |
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

**Thread tuning matters**: the native solver parallelises the search
across `FDN_THREADS` workers, defaulting to `thread::hardware_concurrency()`.
On a shared or cgroup-limited box the full count can OVER-subscribe and
collapse the parallel gain (e.g. pigeon 11/10: 0.93 s with the default
384 threads on this container vs 0.19–0.23 s at 32–64 threads vs 3.3 s
single-threaded).  Set `FDN_THREADS=32..64` for benchmarking here:

```sh
FDN_THREADS=32 ../../emu/picat pigeon.pi 10
```

## Reference results

Whole-program wall time; each column shows the measured time with the
acceleration vs release Picat 3.9#12 underneath.  Measured 2026-10 on x86
(AMD EPYC 9654, shared cgroup-limited container) with the current
experimental build — complete runs, identical solution and backtrack
counts on both sides:

| program | Picat 3.9#12 | FDN=0 | th=1 | th=4 | th=16 | th=64 |
|---|---|---|---|---|---|---|
| kakuro x1000 | 7.91 s | 8.81 s (0.9x) | 0.24 s (**33x**) | 0.25 s (**32x**) | 0.25 s (**32x**) | 0.25 s (**32x**) |
| queens-15, count all | 19.94 s | — | — | — | — | 0.28 s (**71x**) |
| queens-14, count all | 3.28 s | — | — | — | 0.17 s (**19x**, th=32) | 0.20 s (**16x**) |
| pigeon 12/11 (unsat) | 17.85 s | 18.61 s (1.0x) | 29.4 s (0.6x) | 12.57 s (1.4x) | 2.79 s (**6.4x**) | 0.89 s (**20x**) |
| pigeon 13/12 (unsat) | > 300 s | — | — | — | — | 10.68 s (**>28x**) |
| pigeon 11/10 (unsat) | 1.59 s | 1.72 s (0.9x) | 2.7 s (0.6x) | 0.98 s (1.6x) | 0.29 s (**5.5x**) | 0.23 s (**6.9x**) |
| queens-600, first solution | 144.12 s | 145.94 s (1.0x) | 18.5 s (**7.8x**) | 18.78 s (**7.7x**) | 18.81 s (**7.7x**) | 18.97 s (**7.6x**) |

Reading the numbers:

- **kakuro x1000: 33x even single-threaded** — 1000 small searches; the
  win is posting cost, not parallelism.
- **queens count all: 71x at 64 threads (queens-15), 19x at 32 threads
  (queens-14)** — the native `count_all` counts inside the search; no
  solution ever reaches Picat.  The backtrack total is exact for any
  split.
- **pigeon 13/12: Picat 3.9#12 does not finish within 300 s; the
  fdn proves unsat in 10.7 s at 64 threads** (>28x).  Measured against
  the official 3.9#12 release binary.
- **pigeon scales with cores** — but the single-threaded fdn is ~1.7x
  SLOWER than Picat 3.9#12 (0.6x); it only wins with >= 16 threads.  Its value
  at small sizes is entirely the multicore behaviour.
- **queens-600: ~7.7x completely flat** — a first-solution search does
  not benefit from more workers; the win is the native propagation.
- **FDN=0 matches Picat 3.9#12 (0.9-1.0x)** — the transparency holds
  throughout.

For comparison, the fdaccel development phase
(`scratch/aa/fdaccel/RESULTS.md`, less-contended machine, default
thread count) measured: kakuro x1000 41x, pigeon 12/11 24x, pigeon
11/10 16x, queens-600 7.9x, queens-15 count all 71x — consistent with
the table above modulo machine contention and thread counts.

## Limits and known differences

- Solutions are transferred to Picat one at a time by unification, which
  runs Picat's own propagators. That keeps every non-label variable exactly
  as Picat 3.9#12 leaves it, but it costs a few µs per solution.
- First-solution searches gain from native code only (up to ~8× observed);
  threads help when the solution lies right of large failing subtrees.
- Bool-sum heavy models (pigeon 0/1) are about 1.7× slower than Picat on
  one thread (the native per-node bookkeeping: full-state trail, event
  queue, sum-bound deltas); the parallel speedup more than makes up for it.
- After each solution of the label list, Picat 3.9#12's labeling labels the
  constrained variables left in the whole store once (`retrieve_frozen_dvars`).
  picat-fdn checks (`c_fdn_alone`) whether anything else is live: if so, it
  runs Picat's own code after each native solution, and `count_all` falls
  back — otherwise that step is skipped (nothing to label).  Matching Picat 3.9#12
  exactly, including the `backtracks` statistic (the fdaccel regression
  set 13/13).
- Loading `fdn_hook.pi` at start-up shifts the internal heap and symbol
  layout. Flows that never call `labeling/2` (e.g. `sat`/`sat2` programs)
  are unaffected logically, but a solver that returns one of several valid
  solutions may return a different one (observed on
  `exs/sat/marriage_roman_sat.pi`: a different, equally valid stable
  matching; on a MIP model, the generated LP's variable enumeration order —
  `exs/fd_native_mt`-family checks against the official binary cover this).
  `FDN=0` removes the load entirely.
