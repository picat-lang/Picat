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

The `_tasks` / `_race` batteries are the two-way tests; `parblock_demo`
is a short runnable tour; the `queens_*` / `ramsey_*` files are real
workloads. See `docs/pvmbugs_report.tex` (sibling dir) for the engine
bugs each battery pins and the adopted workarounds.

## Files

| file | what it is | run |
|---|---|---|
| `parblock_semantics.pi` | two-way test of the **phase-0** family | `main()` |
| `parblock_demo.pi` | runnable tour of the phase-0 family | `main()` |
| `parblock_pvm_tasks.pi` | `par_run(NT,Tasks)` contract battery (5 cases) | `main([C, NT])` |
| `parblock_pvm_dyn_tasks.pi` | `par_run_dyn` vs fixed `par_run` vs serial (6 cases) | `main([C, NT])` |
| `parblock_pvm_race.pi` | raw **mode-1** portfolio substrate (`pvm_fork`/`pvm_delegate`/`pvm_report`/`pvm_collect` + `race_clause`) | `main([C, NT])` |
| `parblock_race_pvm_tasks.pi` | mode-1 race family vs serial (13 cases) | `main =>` (full NT matrix) |
| `parblock_race_begin_tasks.pi` | phase-2 **race block forms** (8 cases) | `main =>` (full NT matrix) |
| `parblock_queens_count{,2,_multi}.pi` | n-queens counting on `par_run(NT,Tasks)` | `main([N, NT])` |
| `parblock_ramsey_m2{,_cp}.pi` | (K,K)-Ramsey counting on `par_run` / `par_run_dyn` (SAT / CP) | `main =>` + env |
| `parblock_env.pi` | shared helper for the ramsey files: `from_env(Name, Def) = V` (the int value of `$Name`, else `Def`) | module |

`ramsey_m2` knobs come from the environment: `DYN=1` selects
`par_run_dyn` (shared pool, cursor-claimed), `RESET=1` applies the
upstream `sat.reset_store()` workaround for engine Bug F; `NT`, `K`,
`N`, `T`, `M`, `O` as shown by its header.

Probes (bug reproducers, not API demos): `parblock_bugf_probe.pi`
(SAT Bug F zero-count poisoning), `parblock_cp_posting_probe.pi`
(CP constraint-posting Shapes A/B/C; drive a shape with
`emu/picat -path lib2 <file> -g pA()`).

---

## Module `parblock` (phase 0, serial)

Canonical schedule is **written order**; the PVM twins are validated
against these contracts, not against wall-clock outcomes. `F` names a
module-visible function; a task is `{F}` (zero-arg) or `{F, A}`
(one-arg). A candidate that **throws or fails drops out**; function
candidates are total-or-throw by language semantics.

| entry | contract | illustrated by |
|---|---|---|
| `par_run(Tasks) = Rs` | results in **task order**; a throwing task aborts the par with that task's exception term | `parblock_semantics` t01/t02; `parblock_demo` §2c |
| `race(F, Xs) = P` | `P = (W, Y)`: first `X` (written order) on which `F(X)` completes; `Y` its value | `parblock_semantics` t03; `parblock_demo` §2 |
| `race_res(F, Xs) = S` | `S = [won, W, Y]` \| `exhausted` (total form of `race`) | `parblock_semantics` t04 (all-throw → `exhausted`) |
| `map_par(F, Xs) = Ys` | ordered parallel map, results in **element order** | `parblock_semantics` t05; `parblock_demo` §1 |
| `map_race(Fs, Xs) = Ys` | per-element portfolio over the function list `Fs`; all-throw element throws `$parblock_mrace_empty(X)` | `parblock_semantics` t09; `parblock_demo` §2b |
| `par_any(P, Xs) = S` | `S = [sat, X]` first satisfying `X` (written order) \| `none`; a throwing `P(X)` drops out | `parblock_semantics` t06; `parblock_demo` §3 |
| `par_all(P, Xs) = S` | `S = ok` \| `[fail, X]` **fail-fast**; a throwing `P(X)` propagates | `parblock_semantics` t07; `parblock_demo` §4 |
| `effect0(F)` / `effect1(F, A)` | `F()`/`F(A)` run **at most once** per key for the program's life (key marked *before* the call) | `parblock_semantics` (effect8 counter); `parblock_demo` §5 |
| `detach0(F) = H` / `detach1(F, A) = H` / `collect(H) = V` | phase 0: computed immediately, `H = [done, V]` \| `[dead, Term]`; `collect` returns `V` or rethrows `Term` | `parblock_semantics` (detach/collect) |

---

## Module `parblock_pvm` (PVM backend)

Same names as `parblock`, plus a leading `NT` (pool size). `NT = 0`
falls back to the phase-0 serial combinator. The **satext rule** is the
validation contract for the race family: a portfolio winner is a
*model* — validate the reported value against the winner's contract and
**never assert which candidate won** (a scheduling outcome, like which
model a CDCL solver returns); assert the value *exactly* only where all
completers agree.

| entry | substrate | illustrated by |
|---|---|---|
| `par_run(NT, Tasks) = Rs` | mode 2, fixed-chunk split; task-ordered results, first-thrower rethrown after collect | `parblock_pvm_tasks` (5 cases); `queens_count{,2,_multi}`; `ramsey_m2` |
| `race(NT, F, Xs) = P` | mode 1 portfolio (`race_res` wrap) | `parblock_race_pvm_tasks` case_race/case_race2 |
| `race_res(NT, F, Xs) = S` | mode 1 portfolio | `parblock_race_pvm_tasks` case_race / case_race_exhaust |
| `map_par(NT, F, Xs) = Ys` | mode 2 (ordered map) | `parblock_race_pvm_tasks` case_mappar |
| `map_race(NT, Fs, Xs) = Ys` | serial element loop, one mode-1 session per element | `parblock_race_pvm_tasks` case_maprace* |
| `par_any(NT, P, Xs) = S` | mode 1 | `parblock_race_pvm_tasks` case_parany* |
| `par_all(NT, P, Xs) = S` | **mode 2** (all elements must be tested, fail-fast term) | `parblock_race_pvm_tasks` case_parall* |
| `race_begin(NT)` / `race_cl(I, F)` / `race_cl(I, F, A)` / `race_end(R)` | mode 1 **block forms** (phase 2) | `parblock_race_begin_tasks` (8 cases); raw substrate in `parblock_pvm_race` |

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

See `parblock_race_begin_tasks.pi` for root-win / child-win /
grandchild-tail-win schedules, throw-dropping, all-throw, the one-arg
form, and the misuse-raises case across `NT ∈ {0,1,2,4}`.

---

## Module `parblock_pvm_dyn`

| entry | substrate | illustrated by |
|---|---|---|
| `par_run_dyn(NT, Tasks) = Rs` | mode 2 with the **dynamic cursor** (`bp.pvm_claim`): workers share the pool and each claims the next slice, so cost-skewed pools don't strand fast workers; task-ordered results, first-thrower rethrown | `parblock_pvm_dyn_tasks` (6 cases); `ramsey_m2{,_cp}` with `DYN=1` |

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
