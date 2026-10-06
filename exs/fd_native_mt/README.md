# picat-fdn: transparent native multicore FD solver

The fdn engine — `Picat-src/emu/fdn.c`, `fdn.h` and `fdn_solver.cpp` — is a
native C++ search engine for CP models, yet another extension of the
experimental branch's emulator (which is 3.9#13-based and extended itself,
not release Picat 3.9#12).
Programs run unchanged: `picat prog.pi args`. Every `solve/1,2` of the `cp`
module checks the constraint network that is live at that moment:

- **Supported** (see below): the whole search runs natively in parallel.
  The solutions come back to Picat one at a time, in exactly the order
  Picat's own labeling would produce them, and `statistics(backtracks)`
  advances by Picat's count (one known trigger-order exception, see
  "Limits and known differences").
- **Anything else:** Picat's own labeling runs, unchanged.

A bare cp `solve` inside `count_all` is counted natively, in parallel,
without handing solutions to Picat; only the number goes back
(`FDN_COUNT=0` reverts to the old path).

## Build, run, test

```sh
cd emu && make -f Makefile.linux64 -j8 picat FDN_ARCH=-mpopcnt
cd .. && emu/picat exs/fd_native_mt/mixed.pi        # transparency test
emu/picat exs/cp/queens.pi                          # any cp program
FDN=0 emu/picat exs/cp/queens.pi                    # hook off: the experimental build's own interpreted path
sh exs/fd_native_mt/run_bench.sh                    # the benchmark suite
```

`FDN_ARCH=-mpopcnt` is for x86_64; omit it on other platforms. `fdn_hook.pi`
must sit next to the binary (it does, in `emu/`); `FDN_HOOK=path` overrides
this.

Environment variables:

| variable | effect |
|---|---|
| `FDN=0` | do not install the hook (the experimental build's own interpreted CLP(FD) path) |
| `FDN_COUNT=0` | do not repoint `count_all/2` (solutions through Picat again) |
| `FDN_THREADS=n` | threads per search (default: `min(cores, 64)`; explicit values uncapped; see the tuning note below) |
| `FDN_VERBOSE=1` | report each `solve` on stderr: native search (size) or the fallback reason |
| `FDN_SPAWN=n` | start the thread pool after n nodes (default 20000; small searches stay on the calling thread) |

## Supported propagators

| Picat source | native propagator |
|---|---|
| `X #!= Y`, `X #!= Y+C`, `abs(X-Y) #!= C` | forward checking; the edges are read from the `'$combined_neq'` frame, which is what Picat runs after a unification |
| `all_different` | forward checking |
| `all_distinct` | FC + Hall check anchored at the changed variable |
| `all_distinct` on a permutation | + primal/dual channelling |
| `sum`, `#=`, `#>=`, `#=<` (linear) | Picat's single ordered propagation pass (`nary_interval_consistent_eq/ge`) with Picat's wake rules; ARC sums reach arc consistency once ≤ 2 variables are unbound — through an O(1) bound-intersect fast path when the congruence is vacuous (one coefficient divides the other and the constant) and the supporting domain is contiguous, exactly equivalent to the value-by-value pass otherwise |
| `X #= Y+C`, `X #= C-Y` | value mapping (arc consistent) + the `'$v_in_*_int'` bounds frames on a unification |
| `abs(X-Y) #= C` | domain filtering + bounds + FC per removal |
| `X*Y #= Z` | falls back to Picat's own bounds propagation (see below) |
| `X div y #= Z`, `X mod y #= Z` (fixed divisor) | arc rule on Z's removals / full domain filtering (skip when max(X) ≥ 3000) |
| `R #= min([...])`, `R #= max([...])` | bound-event rule replay |
| `B <=> X=c`, `B <=> X!=c`, `B <=> X=Y`, `B <=> X!=Y`, `B <=> X>=Y` | reification (incl. the `#\/` expansion of `alldifferent_except_0`) |
| `B => X=c`, ... | entailment (one-directional) |
| `element(I, [...], V)` | unification maintenance (I fixed), value→range table (V fixed) + FC both directions |
| `global_cardinality` (via gcc decomposition) | elements + reified equalities + linear sums |
| lex chains (`e$$cp$$watch_lex_lt/le`) | watch bounds on the leading pairs, arc-consistent last pair |

Labeling: heuristics `[]`/`leftmost`, `ff`, `min`, `max`, `ff_min`, `ff_max`,
`ffd`, `degree`, `constr`, `ffc`; value strategies `up`, `down`, `updown`,
`split`, `reverse_split`; reorderings `backward`, `inout`; also `forward`.
Branch and bound: `$min(O)`, `$max(O)`, `$minimize(O)`, `$maximize(O)`, with
`$report(G)` and `limit(N)` next to an objective (see the next section).
Still falling back to Picat's labeling: any model with a multiply
constraint (`X*Y #= Z`, quadratics — the 2-var ARC rule is value-by-value
on the operands' domains and superlinear in the width, so Picat's own
bounds propagation wins at every measured width), `rand*`, `label(_)`,
`time_out(..)`, `limit(N)` without an objective, a non-FD objective,
`split`/`reverse_split` on a negative minimum (Picat 3.9#12 itself loops
forever there), other attributes, value ranges wider than 65536.

## Native branch and bound

`solve($[min(O)], Vars)` (and `$max(O)`, `$minimize(O)`, `$maximize(O)`, with
`$report(G)` and `limit(N)` next to an objective) runs Picat's own `minof/3`
loop: each round's first solution is searched natively in parallel, on the
network extracted once, with the objective variable narrowed to ≤ Best−1 per
round.  The report sequence, the solutions and `statistics(backtracks)` are
exactly Picat 3.9#12's (a round's search is Picat's own labeling order), so
the whole loop is transparent.  The options compose with the labeling
strategies (`ff`, `ffc`, `updown`, `split`, ...) which are also native.

Weighted N-queens (`picat qopt.pi N ff`; the `qopt.pi` here,
minimise `sum(I*Q[I])`, `FDN_THREADS=64`, median of 3 whole-program runs,
output byte-identical incl. `backtracks`):

| program | Picat 3.9#12 | fdn | speedup |
|---|---|---|---|
| qopt 14 ff (31 rounds, 9.13M backtracks) | 11.04 s | 1.17 s | **9.5x** |
| qopt 13 ff (23 rounds, 1.96M backtracks) | 2.28 s | 0.43 s | **5.3x** |
| qopt 13 updown max | 0.63 s | 0.23 s | **2.7x** |
| qopt 13 ffc max | 0.91 s | 0.46 s | **2.0x** |
| qopt 12 split (0 backtracks) | 0.39 s | 0.23 s | **1.7x** |
| qopt 12 ff / leftmost | 0.32 / 0.37 s | 0.25 / 0.28 s | **1.3x** |

- **The gain is the parallel rounds, above all the last, exhaustive one** —
  Picat re-runs the whole search per round; the fdn runs every round natively
  in parallel on one network, with the objective bound narrowed per round.
  qopt 14 at 1 thread is about stock's single-thread time (the work per node
  is Picat's); the parallel rounds are the win.
- **Every option list goes native now** — before this phase only `[]`, `ff`
  and the counting options did; `$min(O)`/`updown`/`split`/`ffc` fell back to
  stock code.
- **The default thread count** (capped at 64 on this box) is the tuned
  value for branch and bound here; set `FDN_THREADS` explicitly on other
  machines (uncapped).

## `mixed.pi`

The transparency test: each block prints solutions / counts and the
`backtracks` statistic, which must be identical with and without the fdn
(and is, byte for byte; the fdn-off path is output-verified against
release Picat 3.9#12 on the FD families). Run it with
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
Only programs that the fdn actually accelerates are kept — every file
below was measured with `FDN_THREADS=64` vs `FDN=0` and wins; programs
that go parity or slower natively (the `import sat` variants, the
`circuit` fallback case, posting-only models, the `sequence` and zebra
decompositions) were removed.

- **FDN=0**: the experimental build's own interpreted CLP(FD) path (the hook off) —
  output-verified against release Picat 3.9#12 on the FD families.
- **picat-fdn**: the native multicore solver (the default).

These two run the *same binary*.  Where the reference tables below say
"Picat 3.9#12", that column was measured against the stock release binary
(`/home/jovyan/bin/picat312`).  The `stat` module prints
`STAT <name> runtime_ms=<ms> backtracks=<n>`; the solution results
(solution counts, sat/unsat, `backtracks`) are identical between the two
paths of the same binary.

| file | model | what it exercises |
|---|---|---|
| `qall.pi N` | N-queens, all solutions | `count_all(solve(..))` counted natively in parallel — no solution ever reaches Picat; enumeration with forward checking (`#!=` + `abs`) |
| `qff.pi N` | N-queens, first solution | search-heavy first-solution run, `ff` variable selection; gains from native code only (threads don't help) |
| `pigeon.pi M` | M+1 pigeons / M holes, 0/1 matrix | linear sums, unsat: proof by exhaustive search; the one example that scales with cores |
| `kakuroN.pi R` | kakuro puzzle, R repeated solves | 1000 small searches; the win is posting cost, not parallelism |
| `qopt.pi N [S] [D]` | weighted N-queens, minimise/maximise `sum(I*Q[I])` | branch and bound (native B&B, see the section above); the option lists compose with the native labeling strategies |
| `mixed.pi` | transparency test | byte-identical outputs incl. `backtracks` vs Picat 3.9#12 (run with and without `FDN=0`) |
| `stat.pi` | STAT helper | the module the benchmarks import; prints `runtime_ms` + `backtracks` |

**Thread tuning matters**: the native solver parallelises the search
across `FDN_THREADS` workers, defaulting to `min(thread::hardware_concurrency(), 64)`
(explicit `FDN_THREADS` values are uncapped).
On a shared or cgroup-limited box the full count can OVER-subscribe and
collapse the parallel gain (e.g. pigeon 11/10: 0.93 s when the thread
count was uncapped at 384 on this container vs 0.19–0.23 s at 32–64
threads vs 3.3 s single-threaded — which is why the default is capped
at 64).  Set `FDN_THREADS=32..64` for benchmarking here:

```sh
FDN_THREADS=32 ../../emu/picat pigeon.pi 10
```

## Reference results

Whole-program wall time; each column shows the measured time with the
acceleration vs release Picat 3.9#12 underneath.  Measured 2026-10 on x86
(AMD EPYC 9654, shared cgroup-limited container) with the experimental
build of that date — complete runs, identical solution and backtrack
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

Confirmed 2026-10-05 on the current build under a heavily loaded box
(load average 57, shared cgroup-limited container) — same examples, same
acceleration factors modulo contention, `FDN_THREADS=64` vs `FDN=0`,
whole-program wall, median of 3, outputs byte-identical on both paths:

| program | picat-fdn (th=64) | FDN=0 | speedup |
|---|---|---|---|
| qall 14 (count all) | 0.20 s | 3.6 s | **18x** |
| qall 15 (count all) | 0.41 s | 21.6 s | **52x** |
| qff 600 (first solution) | 18.4 s | 142.3 s | **7.7x** |
| pigeon 12/11 (unsat) | 0.98 s | 18.7 s | **19x** |
| kakuro x1000 | 0.61 s | 8.6 s | **14x** |
| qopt 14 ff (branch and bound) | 1.12 s | 12.4 s | **11x** |

Parity or slower natively (measured with the current build, hence removed):
`qff`/`qffsplit` at N=300 (posting-dominated at that size), `qpost`
(posting only), `pigeon_ad` (fails at posting), `ppm2` (instance too
small), `knight` (circuit fallback), `zebraN` 2000 (1.2x slower),
`seq` 40 (the `sequence` decomposition's many small overlapping ARC
sums, 1.9x slower — the ac2 fast path cut it from ~20x, the remaining
gap is the native per-node cost on that dense graph), `sudoku25` /
`sudoku25d` (> 120 s on both paths, no demonstrated gain), and the
`import sat` variants (the fdn is not involved).

## Limits and known differences

- `statistics(backtracks)` advances by Picat's count on every model tested
  except one known fuzz case (a linear + `all_different` model where a
  propagation trigger fires in a different order and the total differs by
  6 of ~940; the solutions are identical). The solution order and the
  solution sets are identical everywhere tested.
- Solutions are transferred to Picat one decision at a time (a unification,
  or a decision-by-decision replay when Picat's rest step follows — the same
  calls Picat's own labeling makes, so Picat's propagators run after every
  decision). That keeps every non-label variable exactly
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
