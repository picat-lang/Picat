# parblock — process-parallel block combinators

`parblock` is a small family of parallel combinators over two substrates:

* **phase 0 (serial)** — module `parblock` (`lib2/parblock.pi`). The
  contracts (task order, winner-by-satisfaction, fail-fast, drop-out,
  effect-once, detach/collect) are pinned here and are the reference
  that every PVM backend is validated *two-way* against.
* **PVM (process-parallel)** — modules `parblock_pvm` and
  `parblock_pvm_dyn` (`lib2/parblock_pvm.pi`, `lib2/parblock_pvm_dyn.pi`).
  These run the same contracts on the `emu/parvm.c` fork-pool:
  `par_run*` on **mode 2** (fixed-chunk / dynamic-cursor partition) and
  the race family on **mode 1** (first-solution portfolio).

Run everything from the **repo root**:

```
PICATPATH=lib2 emu/picat exs/parallel/parblock/<file>.pi [args]
```

The `_tasks` / `_race` batteries are the two-way tests; `demo`
is a short runnable tour; the `queens_*` / `ramsey_*` files are real
workloads. See `docs/pvmbugs_report.tex` (sibling dir) for the engine
bugs each battery pins and the adopted workarounds.

## Files

| file | what it is | run |
|---|---|---|
| `semantics.pi` | two-way test of the **phase-0** family | `main()` |
| `demo.pi` | runnable tour of the phase-0 family | `main()` |
| `pvm_tasks.pi` | `par_run(NT,Tasks)` contract battery (5 cases) | `main([C, NT])` |
| `pvm_dyn_tasks.pi` | `par_run_dyn` vs fixed `par_run` vs serial (6 cases) | `main([C, NT])` |
| `pvm_race.pi` | raw **mode-1** portfolio substrate (`pvm_fork`/`pvm_delegate`/`pvm_report`/`pvm_collect` + `race_clause`) | `main([C, NT])` |
| `race_pvm_tasks.pi` | mode-1 race family vs serial (13 cases) | `main =>` (full NT matrix) |
| `race_begin_tasks.pi` | phase-2 **race block forms** (8 cases) | `main =>` (full NT matrix) |
| `par_begin_tasks.pi` | phase-3 **par block forms** (9 cases) | `main =>` (full NT matrix) |
| `queens_count{,2,_multi}.pi` | n-queens counting on `par_run(NT,Tasks)` | `main([N, NT])` |
| `ramsey_m2{,_cp}.pi` | (K,K)-Ramsey counting on `par_run` / `par_run_dyn` (SAT / CP) | `main =>` + env |
| `parblock_env.pi` | shared helper for the ramsey files: `from_env(Name, Def) = V` (the int value of `$Name`, else `Def`) | module |

`ramsey_m2` knobs come from the environment: `DYN=1` selects
`par_run_dyn` (shared pool, cursor-claimed), `RESET` (default `1`)
applies the upstream `sat.reset_store()` workaround for engine Bug F
at every task boundary — `RESET=0` opts out, after which only one
count per process is sound (`NT = T`); `NT`, `K`, `N`, `T`, `M`, `O`
as shown by its header.

Probes (bug reproducers, not API demos): `bugf_probe.pi`
(SAT Bug F zero-count poisoning), `cp_posting_probe.pi`
(CP constraint-posting Shapes A/B/C; drive a shape with
`emu/picat -path lib2 <file> -g pA()`).

## Single-API illustrations

One tiny, **self-contained** example per API (no shared helper; each
defines its own number-theory work). The PVM ones run the full
`NT ∈ {0,1,2,4}` matrix (NT = 0 is the phase-0 serial fallback); the
serial-only `effect` / `detach` examples run once. All are green.

| file | API | problem (self-contained) |
|---|---|---|
| `par_run_primes.pi` | `par_run(NT, Tasks)` | prime count in disjoint ranges `[1..10]..[31..40]` → `[4,4,2,2]`, sum = π(40) = 12 |
| `race_perfect.pi` | `race_res(NT, F, Xs)` | smallest perfect number > 496 among candidates → 8128 (OEIS A000396) |
| `map_par_totient.pi` | `map_par(NT, F, Xs)` | Euler totient φ(1..16) → `[1,1,2,2,4,2,6,4,6,4,10,4,12,6,8,8]` |
| `map_race_mersenne.pi` | `map_race(NT, Fs, Xs)` | is `2^P-1` prime? trial-division vs Lucas-Lehmer, `P ∈ [3,5,7,11,13,19,31]` |
| `par_any_twinprime.pi` | `par_any(NT, P, Xs)` | first twin-prime pair in `[1000..1030]` → (1019, 1021) |
| `par_all_primegap.pi` | `par_all(NT, P, Xs)` | the 23↔29 prime gap: interior `[24..28]` composite (`ok`), 29 closes it (`[fail,29]`) |
| `effect_twinprime.pi` | `effect1(F, A)` | report each twin pair in `[1..200]` once across two passes → 15 (not 30) |
| `detach_primecount.pi` | `detach1(F, A)` / `collect` | deferred π(100000) = 9592 vs foreground π(100) = 25 |
| `race_block_perfect.pi` | `race_begin` / `race_cl` / `race_end` | 3 solvers (Mersenne / σ-scan / sequence) for the next perfect > 496 → 8128 |
| `par_block_counts.pi` | `par_begin` / `par_cl` / `par_end` | π(100), twin pairs ≤ 100, perfect numbers ≤ 100 → `[25, 8, 2]` |
| `par_run_dyn_primes.pi` | `par_run_dyn(NT, Tasks)` | uneven prime counts `[1..50000]` + three lights → `[5133, 25, 46, 62]` |

---

## Module `parblock` (phase 0, serial)

Canonical schedule is **written order**; the PVM twins are validated
against these contracts, not against wall-clock outcomes. `F` names a
module-visible function; a task is `{F}` (zero-arg) or `{F, A}`
(one-arg). A candidate that **throws or fails drops out**; function
candidates are total-or-throw by language semantics.

| entry | contract | illustrated by |
|---|---|---|
| `par_run(Tasks) = Rs` | results in **task order**; a throwing task aborts the par with that task's exception term | `semantics` t01/t02; `demo` §2c; `par_run_primes` |
| `race(F, Xs) = P` | `P = (W, Y)`: first `X` (written order) on which `F(X)` completes; `Y` its value | `semantics` t03; `demo` §2 |
| `race_res(F, Xs) = S` | `S = [won, W, Y]` \| `exhausted` (total form of `race`) | `semantics` t04 (all-throw → `exhausted`); `race_perfect` |
| `map_par(F, Xs) = Ys` | ordered parallel map, results in **element order** | `semantics` t05; `demo` §1; `map_par_totient` |
| `map_race(Fs, Xs) = Ys` | per-element portfolio over the function list `Fs`; all-throw element throws `$parblock_mrace_empty(X)` | `semantics` t09; `demo` §2b; `map_race_mersenne` |
| `par_any(P, Xs) = S` | `S = [sat, X]` first satisfying `X` (written order) \| `none`; a throwing `P(X)` drops out | `semantics` t06; `demo` §3; `par_any_twinprime` |
| `par_all(P, Xs) = S` | `S = ok` \| `[fail, X]` **fail-fast**; a throwing `P(X)` propagates | `semantics` t07; `demo` §4; `par_all_primegap` |
| `effect0(F)` / `effect1(F, A)` | `F()`/`F(A)` run **at most once** per key for the program's life (key marked *before* the call) | `semantics` (effect8 counter); `demo` §5; `effect_twinprime` |
| `detach0(F) = H` / `detach1(F, A) = H` / `collect(H) = V` | phase 0: computed immediately, `H = [done, V]` \| `[dead, Term]`; `collect` returns `V` or rethrows `Term` | `semantics` (detach/collect); `detach_primecount` |

---

## Module `parblock_pvm` (PVM backend)

Same names as `parblock`, plus a leading `NT` (pool size). `NT = 0`
falls back to the phase-0 serial combinator. The **satext rule** is the
validation contract for the race family, named for the first-wins
satext SAT-solver portfolio in `emu/satext.c`, which has the same
shape: candidates race and the first decisive answer wins, so a winner
is a *model* — validate the reported value against the winner's
contract and **never assert which candidate won** (a scheduling
outcome, like which model a CDCL solver returns). "Agreement" is
about the *value*: a value may be asserted *exactly* only where every
completer that could win reports the same value, so the number is
independent of the schedule (`case_maprace`: `fa` and `fb` both `= I`,
the third variant throws → `Ys = [1,2,3]` asserted exact); where
completers report different values, assert agreement with *some*
completer instead — a disjunction over the completers' values
(`case_maprace_agree`: `fa2 = I`, `fb2 = 2*I` → `Z1 = 1 ; Z1 = 2`) or
the value's contract (`case_race`: two queens solvers → `valid8(Y)`,
never a specific placement).

| entry | substrate | illustrated by |
|---|---|---|
| `par_run(NT, Tasks) = Rs` | mode 2, fixed-chunk split; task-ordered results, first-thrower rethrown after collect | `pvm_tasks` (5 cases); `queens_count{,2,_multi}`; `ramsey_m2`; `par_run_primes` |
| `race(NT, F, Xs) = P` | mode 1 portfolio (`race_res` wrap) | `race_pvm_tasks` case_race/case_race2 |
| `race_res(NT, F, Xs) = S` | mode 1 portfolio | `race_pvm_tasks` case_race / case_race_exhaust; `race_perfect` |
| `map_par(NT, F, Xs) = Ys` | mode 2 (ordered map) | `race_pvm_tasks` case_mappar; `map_par_totient` |
| `map_race(NT, Fs, Xs) = Ys` | serial element loop, one mode-1 session per element | `race_pvm_tasks` case_maprace*; `map_race_mersenne` |
| `par_any(NT, P, Xs) = S` | mode 1 | `race_pvm_tasks` case_parany*; `par_any_twinprime` |
| `par_all(NT, P, Xs) = S` | **mode 2** (all elements must be tested, fail-fast term) | `race_pvm_tasks` case_parall*; `par_all_primegap` |
| `race_begin(NT)` / `race_cl(I, F)` / `race_cl(I, F, A)` / `race_end(R)` | mode 1 **block forms** (phase 2) | `race_begin_tasks` (8 cases); raw substrate in `pvm_race`; `race_block_perfect` |
| `par_begin(NT)` / `par_cl(I, F)` / `par_cl(I, F, A)` / `par_end(Rs)` | **par block forms** (phase 3): serial registration walk + mode-2 `par_run` | `par_begin_tasks` (9 cases); `par_block_counts` |

### Race block forms (phase 2)

The candidates are the *clauses* of a user-written disjunction between
`race_begin(NT)` and `race_end(R)`, instead of `{F, X}` pairs. The block
is **mode-uniform**: `pvm_fork` rejects `NT < 1`, so `NT = 0` is a
pool-1 session — no child is forked, the root walks the clauses in
written order, and the first to complete reports first, which *is* the
serial written-order race. `race_end(R) = [won, I, V]` (value carried by
value in the pool report); every clause dropped out throws
`$parblock_raceb_empty` **after** collect. The trailing `; true`
disjunct is **required** (root: the found/exhaustion landing; child: the
fall-through into `race_end`, where the engine exits the worker
natively). Names are collision-free, so no `parblock_pvm.` qualifier is
needed.

```picat
import parblock_pvm.
race_begin(NT),
( race_cl(1, sol_a)
; race_cl(2, sol_b)
; true ),
race_end(R).                     % R = [won, I, V]
```

See `race_begin_tasks.pi` for root-win / child-win /
grandchild-tail-win schedules, throw-dropping, all-throw, the one-arg
form, and the misuse-raises case across `NT ∈ {0,1,2,4}`.

### Par block forms (phase 3)

The statement-level `par_run`: the *tasks* are the clauses of a
user-written disjunction between `par_begin(NT)` and `par_end(Rs)`.
Each `par_cl(I, F)` / `par_cl(I, F, A)` **registers** the task `F()` /
`F(A)` at index `I` (a C-side append that survives the clause's
trailing `false`) so the disjunction walks every clause in written
order; `par_end` then runs the registered tasks through the proven
**mode-2** fixed-chunk `par_run` and returns `Rs = [V1, …, VM]` in
**index order** (independent of `NT` and of which worker ran which
chunk — *not* completion order). A task exception aborts the par with
the first throwing task **in index order**'s term, rethrown after
collect. The trailing `; true` disjunct is **required** (every `par_cl`
fails after registering, so the disjunction needs a final succeeding
disjunct). `NT = 0` runs the serial `parblock.par_run`.

```picat
import parblock_pvm.
par_begin(NT),
( par_cl(1, task_a)
; par_cl(2, task_b, Arg)
; true ),
par_end(Rs).                    % Rs = [Va, Vb]  in index order
```

> Why not a disjunction *pool partition*: the engine forks pool
> children **at CONFIRM** — after a frame's first value has been walked
> — so a block-level disjunction would partition *serially* (clause 1
> runs to completion before clause 2's worker is even forked). The par
> block therefore splits into a serial registration walk + the mode-2
> run. (An earlier mode-4 "par pool" attempt — a run-all-disjuncts
> partition — was built and then *reverted*: that same CONFIRM
> serialization made it a no-op for parallelism, so it shipped no
> benefit and was removed to keep the engine lean.)

See `par_begin_tasks.pi` for exact-value, staggered (index-order-not
completion-order), three-clause, thrower / all-throw (first-thrower-in
index order), single, genuine-completion (8-queens), empty, and
misuse-raises cases across `NT ∈ {0,1,2,4}`.

---

## Module `parblock_pvm_dyn`

| entry | substrate | illustrated by |
|---|---|---|
| `par_run_dyn(NT, Tasks) = Rs` | mode 2 with the **dynamic cursor** (`bp.pvm_claim`): workers share the pool and each claims the next slice, so cost-skewed pools don't strand fast workers; task-ordered results, first-thrower rethrown | `pvm_dyn_tasks` (6 cases); `ramsey_m2{,_cp}` with `DYN=1`; `par_run_dyn_primes` |

---

## Standing disciplines (all PVM backends)

* **Total-or-throw**: a function candidate either completes with a value
  or throws (a call with no matching rule throws, never silently fails).
  In-window this is what keeps a search from *exhausting* inside a
  delegate window (engine Bug C) — a dropping search must **throw**, not
  fail its goal.
* **By-value reports**: index-tagged `(I, V)` **tuple** reports (the
  codec is exact for tuples/arrays of ground terms, truncated for lists
  consed from window-exit values — Bug B).
* **No mid-session root exception**: every combinator routes all possible
  throws onto supported paths *before* the root may throw (a live-session
  root throw dirties the next `pvm_fork`).
* **Stale `.qi`**: the runtime prefers a `.qi` over a newer `.pi` — delete
  the `.qi` after editing a `lib2` module.
