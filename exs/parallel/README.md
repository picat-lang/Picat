# Parallel Picat — examples and benchmarks

This folder holds the parallel-search example sets, by branch:

* **`parallel` branch** (threads in the engine) — the examples in this
  folder's root: `par_*`, `threads_*`, `pp_*`, `bench_*`. See the
  sections below ("What the branch adds" ... "Measured numbers").
* **`parsearch` branch** (fork-based OR-parallel CP search,
  `bp.pvm_fork/report/collect/chunk/worker_id`) — the complete source
  of every model used in the `parsearch` engineering report
  (docs/ report "OR-Parallel Constraint Search in Picat 3.9"), in
  **`pvm/`**. One file per configuration; the filename encodes
  `instance_modechunk/workers`.

## parsearch examples, in `pvm/`

Builtins: `bp.pvm_fork(NT, Mode, C)` arms the session, and
`bp.pvm_report(S)` / `bp.pvm_collect(R)` / `bp.pvm_solution(S)`
finish it. Mode 1 = the $C=1$ OR split; mode 2 = static value-chunk
all-results (`bp.pvm_worker_id(I)` + `bp.pvm_chunk(Lo, Hi)` in each
worker; each worker `pvm_report`s its result -- any ground term -- and
the root's `pvm_collect(R)` returns the LIST of all reported results,
in report order, which the parent then uses explicitly, e.g. sums);
mode 3 = first-solution with value chunks of size $C$. For
modes 1/3 the solution is **reported by value**: the finding process
calls `pvm_report(Sol)` with the solution term (any ground term:
integers, atoms, lists, compounds, arrays — floats and bigints ride
along as their `$float`/`$bigint` PSCs), the root's `pvm_collect(R)`
returns 1, and `pvm_solution(S)` (root side) binds `S` to a fresh term
of the reported shape. The term crosses the process boundary as a
flat, address-free word buffer (the codec in `emu/parvm.c`). On
single-engine targets (`PAR_THREADS = 0`, e.g. the webasm build) there
is no PVM at all: the builtins arm a serial session, `pvm_report`
keeps the term by value in the one engine (no encoding, no shared
block) and `pvm_solution` returns that same value. When the root's
own process parks on a delegated disjunction, it waits for the
delegated region's outcome and then resumes its own (COW-isolated)
view of the disjunction exactly as a serial run would — the delegated
chunk is never re-searched. The post-found kill sweeps wait for the
solution's completion marker before killing children
(`pvm_wait_marker`, commit `7e2e650`), so a completed report is never
lost in the sweep; a `found` without a marker is refused at
`pvm_collect` as a hard error.
Mode 2's `pvm_report(T)` appends T (any ground term) to the
session's result buffer; `pvm_collect` hands back the list of all
reported terms and the parent consumes it explicitly (e.g. sums it).

### Builtin reference (`bp.*`)

All PVM entry points in one table; every call except `pvm_fork`
itself requires an armed session (otherwise `invalid_argument`). On
single-engine targets (`PAR_THREADS = 0`, e.g. the webasm build) the
same names run the no-PVM serial fallback described above.

| predicate | meaning |
|---|---|
| `bp.pvm_fork(NT, Mode, C)` | Arms the session; a finished session may be re-armed (a new `pvm_fork` after its `pvm_collect` returns) with changed bounds/program state — the engine clears the per-session records at each re-arm; CONCURRENT (nested) sessions error; NT 1..4096. Modes 1/3: the pool grows through branch forks up to NT live workers (the root is worker 0; freed seats flow to the deepest active frontier). Mode 2: forks its static workers at this call. Third argument: **mode 3** = chunk size C values; **mode 2** = total number of values to partition among the workers; **mode 1** = ignored (the split is the C=1 OR split). |
| `bp.pvm_delegate(On)` | Opens (`On > 0`) / closes (`On = 0`) the delegation window, counted so nested solves compose. Only the value disjunctions of frames *inside* the window are delegable; outer loop frames stay serial (a worker's trivial fallback success would be indistinguishable from exhaustion). Silent no-op outside a session. |
| `bp.pvm_worker_id(I)` | Mode 2: binds the caller's worker id — 0 in the root (the collector), 1..N in the workers. |
| `bp.pvm_chunk(Lo, Hi)` | Mode 2: binds the caller's statically assigned value range [Lo, Hi] over the total given to `pvm_fork`. |
| `bp.pvm_report(T)` | Mode 1/3: report the solution **by value** — T may be any ground term (ints, atoms, lists, compounds, arrays; floats/bigints ride as their `$float`/`$bigint` PSCs); the first reporter wins by CAS, a second reporter is a no-op. Mode 2: any ground term, appended to the session's result buffer (as many times as it likes). |
| `bp.pvm_collect(R)` | Ends the session: SIGKILL sweep of the pool (after waiting for the solution's completion marker), blocking reaps, copy-out and unmap of the shared block. Mode 1/3: R = 1 iff a solution was reported, else 0. Mode 2: R = the list of all reported terms, in report order (the parent consumes/sums it explicitly). Refuses the session as a hard `run_time_error` on a crashed worker or a `found` without a completed marker. A forked child reaching this in a finished mode-1/3 session exits quietly; a mode-2 worker reaching it (its report branch failed upstream) instead flags the session and drops out -- the real root's collect then refuses with `run_time_error` rather than serving a silent undercount or letting the worker run a second collect against the shared block. |
| `bp.pvm_solution(S)` | Root side, after a mode-1/3 `pvm_collect` that returned 1: binds S to a fresh term of the reported shape, materialized from the address-free encoding (shape/bounds/offsets validated, heap headroom checked first). On no-PVM targets: returns the stored by-value term. Errors if nothing was reported. |

A complete session has the shape (modes 1/3):

```
bp.pvm_fork(NT, Mode, C),
( model, bp.pvm_report(Sol) ; true ),   % report where search succeeds
bp.pvm_collect(R),
( R = 1, bp.pvm_solution(S), ...verify/use S... ; true ).
```
Run with a `parsearch` build, e.g.
`picat exs/parallel/pvm/<model>.pi` (the counting model counts with
`count_all`, so no process ever holds a solution list and no sized
arena is needed, not even at N=16). The Ram
models read `K`/`N` from the environment (e.g. `K=4 N=16`); their
base solver is `exs/satext/ramsey_ps.pi` with the `pvm` calls
inserted.

Four parametrized programs cover every configuration used in the
report (one file per problem, not one per benchmark cell), plus one
capability check:

| file | args | usage from the report |
|------|------|-----------------------|
| `pvm/queens_first.pi` | `[N] [NT] [MODE] [C] [PIN]` (defaults 10 0 3 1 0) | `queens_first.pi 10 4 3 2 4` (worked example), `queens_first.pi 10 4 1 1` (mode-1 family), `queens_first.pi 479` (N=479 serial baseline), `queens_first.pi 479 16 3 64` (mode-3 grid cells) |
| `pvm/queens_count.pi` | `[N] [NT]` (defaults 10, serial) | the counting matrix: `queens_count.pi 16 16`, `queens_count.pi 13 8`, `queens_count.pi 10 4` (must print 724); OEIS A000170 totals are quoted in the header |
| `pvm/queens_count2.pi` | `[N] [NT]` (defaults 10, serial) | the counting matrix partitioned over the joint value `Q[1] + N*(Q[2]-1)` in `1..N*N` (first two queens), so up to `N*N` workers are useful -- `queens_count2.pi 8 35` must print `b = 92` once. Note the `slice_count` guard: `count_all` sometimes *fails* (instead of returning 0) on a zero-solution slice, and an unguarded failure would backtrack the worker into the root branch |
| `pvm/ramsey_pvm.pi` | env `K= N= T=` (T = workers, 0 = serial) | `K=4 N=16 T=8 picat pvm/ramsey_pvm.pi`; `K=4 N=18` is the UNSAT whole-tree case ($R(4,4)=18$) |
| `pvm/term_report.pi` | none | any-ground-term report: a worker reports a mixed nested term (compounds, array, float, bigint), the root materializes it and the parent verifies it field by field — prints `1 / <term> / pass`; runs identically on native and webasm |
 | `pvm/backpack_pvm.pi` (sat encoding; serial twin `exs/satext/backpack_ps.pi`) | env `N=` `NT=` (defaults 20, 0) | the **mode-4** pattern at user level: bound-lifted 0/1-knapsack B&B where each iteration is a re-armed mode-1 session racing `model + P #>= B+1`; the first reporter lifts the bound by value, the exhausting session proves optimality, one serial solve re-derives the final model (`NT=0` = the built-in `$max` reference). The first example that re-arms a session in one process — it drove the re-arm/tombstone/hand-off engine fixes (`ae73e03`); verified N=10..100 x NT=0..16 vs the knapsack DP optima |
  | `pvm/backpack_band.pi` (sat encoding; same problem as `backpack_pvm.pi`) | env `N=` `NT=` (defaults 20, 0) | the **mode-4 variant that splits the lifting**: instead of one mode-1 session racing a single bound per iteration, each round is one re-armed **mode-2** session that partitions the value range `B+1..U` into static chunks; a worker either finds a model in its band (lifts `B`) or proves its band empty (tightens `U`), and an all-empty round proves `B` optimal. `NT=1` degenerates to `backpack_pvm`'s loop shape. Mode-2 forks `ceil(W/ceil(W/NT))` workers (fewer than `NT` when the range is small; the driver checks that count). The round runs `pvm_fork_lb`: a band whose top falls at or below the max value another band finds stops at its next choice point (claims the empty report for its band), so a lifting round no longer pays for doomed bands; on this family the saving is within measurement noise (the hard work is the final all-empty proving round). A thin round (`W <= NT`) can also partition the labeling tree (`PVM_TREE=1`, opt-in): `2^m` prefix-pattern slices, each worker proving its patterns model-free over the whole `B+1..U`; measured 1.0x-5.4x slower than the value-band round whenever it fires on this family (a `P=v` band proof is the cheap proof, and the band round already runs `W` workers), so it stays off by default. On this machine the driver wins 1.7x at N=80 and 1.5x at N=100 over the built-in `$max` reference |
 
Measured on this machine (best of 3, this tree, -O3), first solution
of the N=479 distinct-differences instance: serial 7.71 s, mode 3
$C=8$/NT8 6.25 s (1.23x), mode 3 $C=16$/NT8 5.56 s (1.39x);
mode 1 ($C=1$) is slower than serial (per-value OR-split overhead).
Re-verified 2026-08-24 on a 27-run battery (serial + the mode-3
NT4/8/16 x C8/16/64 grid, 3 reps each), after the marker-wait fix
(`7e2e650`): serial 6.90-7.03 s, best cell 4.92-5.49 s (1.14-1.40x),
every run with the identical reference solution and the exact
parent-side sum (114960). Validation battery: queens N=10 mode 1/3
first-solution cells, `queens_count` N=10/12/13 (724 / 14200 /
73712), the ramsey $K=3$ $N=5/6$, $K=4$ $N=15$ SAT/UNSAT matrix at
T=2/8/16, and `term_report.pi` on native and webasm all pass.

Example:

```
picat exs/parallel/pvm/queens_count.pi 16 16
# 14772512
```

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
