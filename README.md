# Experimental branch — what is new versus `main`

This branch (`experimental`) adds features on top of `main`. Each area
has its own README where noted.

## 1. External SAT solvers and the SAT solver portfolio (satext)

`solve`/`solve_all` can now be served by an external SAT solver
(kissat, CaDiCaL, MiniSat, CryptoMiniSat, or any DIMACS/IPASIR solver —
the input protocol is picked from the solver name, probed once and
cached for unknown names). Models come back exactly as with the
built-in solver, so `solve_all`, `findall` and blocking of enumerated
solutions work unchanged. Several solvers may be named at once: they
are **raced** on the same CNF and the first decisive answer wins (the
rest are killed) — the portfolio is capped at 8 solvers per solve. The
built-in solver can itself be one of the racers (the reserved name
`builtin`), running in-process; when an external selection is active
the built-in is **never re-run as a fallback** — a failed or `unknown`
external run fails the solve with `St=0` (`c_satext_last_status`).

How to select a solver (live surfaces; the `$solver` option is
documented but inert in the current build — see below):

- from code: `bp.c_satext_set_solver(Spec)` (e.g. in `main`, before
  the constraints/solve; `Spec` = an atom/path, an argv list such as
  `["cryptominisat", "@file"]`, or a list of argv lists for a
  portfolio; `nil`/`false` restores the built-in)
- environment: `SATEXT_SOLVER=...` (below)
- low level: `import satext.` (needs `PICATPATH=<picat>/lib2`) —
  `satext.solve(Spec, Clauses, Status, Model)`, `satext.cnf_info`,
  `satext.write_dimacs`; and `bp.c_satext_last_status(St)` reports how
  the last `solve` resolved (1 SAT / 2 UNSAT / 0 unknown-or-abandoned).

`Spec` is a bare atom (`kissat`, a path, ...; PATH-resolved
when it has no `/`), a list of atoms/strings forming the solver's argv
(a `@file` token is replaced by a generated CNF file, for solvers that
read the input file as an argument), or a **list of such argv lists**
to race them as a first-wins portfolio. `nil`/`false` selects the
built-in solver again.

Note: the `Spec` grammar distinguishes strings from argv lists, so an
in-program selection is a bare atom (`kissat`) or a list of atoms/
strings — a top-level Picat string such as `"kissat"` is a list of
characters and would be parsed as a six-element argv (`"k"`, `"i"`,
...), which protocol detection rejects with a warning.

The `$solver(Spec)` option of `solve(Options,Vars)` is documented in
`lib/sat.pi`, but it is **inert in the current build**: `import sat`
loads the embedded copy of the standard library (`emu/picat_bc.h`),
which predates the option, and a module named `sat` on `PICATPATH`
cannot override an embedded one. No generator for `emu/picat_bc.h`
exists in the repository, so the option stays inert until the standard
library is re-embedded. The environment
variable and `bp.c_satext_set_solver/1` above reach the same C state
without going through `sat.pi`, so they work today.

Environment variables (all read by the satext layer):

- `SATEXT_SOLVER` — solver selection for the standard `import sat`
  flow. One solver: whitespace-separated argv — first token =
  executable (name or path), the rest = extra arguments, e.g.
  `SATEXT_SOLVER="kissat -t 4"`. A portfolio: `|` separates several
  such argv strings, e.g. `SATEXT_SOLVER="kissat|cryptominisat"`; the
  solvers are raced and the first decisive answer wins (up to 8). A
  part may be the reserved name `builtin`, which races the built-in
  kissat solver in-process (e.g. `SATEXT_SOLVER="kissat|builtin"`);
  `builtin` alone is just the default built-in engine. `nil`/`false`
  selects the built-in solver.
- `SATEXT_PRT_MIN` — estimated CNF size in bytes below which a
  portfolio collapses to its first solver (default 64 KiB).
- `SATEXT_PRT_BUDGET_MS` — portfolio wall budget in ms per solve
  (default 60000; `0` = no budget). On expiry the race (including any
  `builtin` racer) is killed and the solve **fails with `St=0`** — the
  built-in is never re-run as a fallback. **Danger:** a failure here
  is *indistinguishable from an UNSAT at the Picat level* — both
  simply fail the `solve` goal — so a caller that treats a failed
  solve as "no solution" silently concludes a false infeasibility
  (e.g. a B&B loop reads it as "bound proven optimal"). Check
  `c_satext_last_status(St)` after a failed solve (2 = UNSAT, 0 = no
  verdict), or set `0` for no budget. The engine prints an explicit
  `satext: WARNING:` line to stderr whenever this happens.
- `SATEXT_PRT_STATS` — non-empty: print a per-solve line to stderr
  with each racer's wall time and the winner.
- `SATEXT_SHIM` — path of the `satshim` helper, used to hand a large
  formula to a solver via a file descriptor; default: next to the
  `picat` binary.
- `SATEXT_SHIM_MIN` — estimated CNF size in bytes above which the shim
  path is used instead of a pipe (default 4 MiB).
- `SATEXT_TMPDIR` — directory for generated CNF files (default:
  `/dev/shm`, else `/tmp`).

Usage notes: the comments of `lib/sat.pi` / `lib2/satext.pi` and
`exs/satext/bench_satext.sh`; examples: `exs/satext/`.

## 2. Picat in a browser — new folder `webasm/`

A wasm32 (emscripten) build of the interpreter plus an
editor-and-terminal web page: any Picat program runs in a browser tab,
repeatedly, in one runtime. `import cp.` (CLP/FD) and `import sat.`
(with the built-in kissat solver) work; what does not: external SAT
solvers (fork/exec), `mip`/`smt` (external binaries), `thread`
(pthreads). 100 example programs (packed from `exs/`) are preloaded.

```sh
cd webasm && make && make serve   # needs emsdk + a native build (lib2)
# -> http://localhost:8000 : editor left, terminal right, example select
```

Performance versus the native build (median of 3 runs, 384-core
x86_64, 32-way parallel measurements; the full 100-example table,
methodology and runnable harness are in
[webasm/headless/README.md](webasm/headless/README.md)):

- overall, the geomean of (wasm run time)/(native wall time) is
  **1.06x** (median 1.19x) — small programs are at parity, because
  the native number includes ~25–30 ms of process startup while the
  wasm side pays only a one-time ~60 ms instantiate + ~35 ms boot
  per session;
- genuinely compute-heavy programs (search, SAT, FANN) run
  **~2–4x slower** on wasm — the honest figure for interpreter
  overhead (e.g. `sat_queens` 2.2x, `planner_klotski` 2.3x; worst
  verified: `cp_pigeon_hole` 4.5x);
- a few heavy programs are actually **faster on wasm**:
  `sat_vmtl` 7.0 s → 2.3 s (0.31x), `euler_p48` 1.8 s → 0.8 s
  (0.44x), `sat_magic_square` 2.8 s → 2.4 s (0.84x).

Details (build, run loop, exclusions, example set, headless runs):
[webasm/README.md](webasm/README.md)

## 3. Structured concurrency for Picat — `parblock` (new modules `parblock`, `parblock_pvm`, `parblock_pvm_dyn`)

A structured-concurrency construct family: every combinator has one
sequential semantics core and one or two PVM backends that fork()-ed
copy-on-write workers must honour (validated by two-way batteries,
not by wall-clock outcomes). The modules live in `lib2/`, so run with
`PICATPATH=<picat>/lib2 picat yourfile.pi`.

List of calls (sequential core, `import parblock.` — everything runs
in written order; the contracts the async backends must honour):

- `par_run(Tasks) = Rs` — all tasks run, results in task order; a task
  exception aborts the whole par
- `map_par(F, Xs) = Ys` — ordered parallel map (== `map` today)
- `race(F, Xs) = (W, Y)` — the first `X` in written order whose
  `F(X)` terminates with a value wins; a throwing candidate drops
  out; all-but-none throwing throws `$parblock_race_empty(Xs)`
- `map_race(Fs, Xs) = Ys` — per element `X`, the first `F` in `Fs`
  (written order) to terminate with `F(X)` wins
- `par_any(P, Xs) = [sat, X] | none` — first `X` with `P(X)`
- `par_all(P, Xs) = ok | [fail, X]` — fail-fast predicate check
- `effect0(F)` / `effect1(F, A)` — runs AT MOST ONCE per key for the
  life of the program, even when the surrounding search backtracks
  and re-executes the call
- `detach0(F) = H` / `detach1(F, A) = H` / `collect(H) = V` — the
  task's value, or the rethrown exception, exactly once

PVM backends (`import parblock_pvm.` — the same contracts, over up to
4096 fork()-ed workers whose only inter-process state is one POSIX
shared-memory block; `NT = 0` falls back to the serial core):

- `par_run(NT, Tasks) = Rs` — the fixed-chunk split; results in task
  order regardless of which worker ran what
- `race(NT, F, Xs)`, `race_res(NT, F, Xs)`, `par_any(NT, P, Xs)`,
  `par_all(NT, P, Xs)`, `map_par(NT, F, Xs)`, `map_race(NT, Fs, Xs)` —
  the mode-1 portfolio: candidates are forked at choice points under
  the pool size `NT`; the first to COMPLETE wins, the rest are killed
- the block forms — the statement-level versions of the above:
  `race_begin(NT)`, followed by a plain disjunction of
  `race_cl(I, F)` clauses with a trailing `true` (the
  exhaustion/found landing), then `race_end(R)` with
  `R = [won, I, V]` (every clause dropped after the collect throws
  `$parblock_raceb_empty`); `par_begin(NT)` / `par_cl(I, F)` /
  `par_end(Rs)` is the statement-level `par_run` (a serial
  registration walk, then the mode-2 split)
- `import parblock_pvm_dyn.` — `par_run_dyn(NT, Tasks) = Rs`: the
  dynamic shared-cursor backend. Every worker claims its NEXT free
  task index from a session-wide cursor (`bp.pvm_claim`) instead of
  receiving a fixed chunk, so uneven task costs don't strand a whole
  chunk's fast siblings behind one slow task

Minimal examples (from `exs/parallel/parblock/`):

```picat
% the smallest perfect number > 496:  race_res, the mode-1 portfolio
module race_perfect.
import parblock_pvm.

cand(X) = Y =>                     % total-or-throw: the race discipline
    ( isperfect(X), X > 496, Y = X
    ; throw($parbat_notperf(X)) ).

main =>
    S = race_res(4, cand, [6, 28, 496, 8128]),
    S = [won, W, Y],               % W = 8128 (the winning candidate), Y = 8128
    println(Y).
```

```picat
% the same race as a block form:  race_begin / race_cl / race_end
main =>
    race_begin(4),
    ( race_cl(1, sol_mersenne)     % candidates are clauses, tried in
    ; race_cl(2, sol_sigma)        % parallel; the first to COMPLETE
    ; race_cl(3, sol_sequence)     % wins
    ; true ),
    race_end(R),
    R = [won, I, V],               % I = the winning clause index
    println(V).                    % 8128
```

```picat
% par_run over fork()-ed workers:  results in task order, a throwing
% task aborts the whole par with the serial exception term
module demo_run.
import parblock_pvm.

sq(X) = X * X.
boom(A) = Y => throw($demo_boom(A)).

main =>
    Rs = par_run(4, [{sq, 1}, {sq, 2}, {sq, 3}]),
    Rs = [1, 4, 9],                % in task order, whatever ran where
    println(Rs),
    catch((Rs2 = par_run(4, [{sq, 4}, {boom, 7}]), println(Rs2)),
          E, println(E)),          % $demo_boom(7)
    true.
```

Notes: task names in `{F}` / `{F, A}` must be UNQUALIFIED references
visible in the calling program — a module-qualified dynamic dispatch
of a function hangs (engine bug, repros `q1/q2.pi`). The PVM
acceleration matrix for the underlying `pvm` substrate and the full
example set:
[exs/parallel/README.md](exs/parallel/README.md),
[exs/parallel/pvm/README.md](exs/parallel/pvm/README.md).

## 4. Picat syntax checker — new folder `lsp/`

`lsp/picat_syntax_check.py FILE.pi [MORE ...]` reports
line/column-accurate syntax diagnostics (unbalanced delimiters,
unterminated strings/quoted atoms/comments, line-spanning string
literals, ...) where the Picat parser would only say `error` — useful
for editor/LSP integration. Self-test:
`python3 lsp/run_mutation_tests.py`.

## 5. User-defined functions in constraints — very experimental (`udf`)

The `udf` module (`lib2/udf.pi`) lets you use *your own* functions inside
constraint expressions, which the built-in solvers do not support.
**This is very experimental and the syntax is not yet ideal.** In
particular, `f(X)` in a normal (non-constraint) expression is an
ordinary function call, so `udf.define($f(X), ...)` registers the
function as a *term* and every use must be written `$f(X)`.

**Why an operator file is needed.** A solver's own `#=`, `#>=`, … treat
expressions as terms, so they cannot expand a user function. The feature
therefore ships one small *operator file* per solver — `udf_ops.pi`
(`cp`), `udf_ops_sat.pi`, `udf_ops_mip.pi`, `udf_ops_smt.pi` —
that you `include` in *your own* module: it redefines `#=`, `#!=`,
`#<`, `#=<`, `#<=`, `#>`, `#>=` (and the Boolean `#~ #/\ #^ #\/ #=> #<=>`)
to first expand the user functions and then delegate back to the solver.

Tiny example (`cp`), using normal constraint notation (this is
`exs/cp/udf_test.pi`):

```picat
import cp.
import udf.
include "udf_ops.pi".        % defines the operator shims (see above)

main =>
    % register the user function used in constraints:
    udf.define($inc(N), $(N+1)),
    X :: 0..10,
    $inc(X) #= 6,            % inc(X) = X+1 = 6  ->  X = 5
    solve([X]),
    println(X),               % 5
    println(udf.evaluate($inc(X))).   % 6
```

The `udf` documentation lives in `doc/udf.tex`.
The `udf` examples are:
`exs/cp/udf_functions.pi`,
`exs/cp/udf_ops_schedule.pi`,
`exs/sat/udf_functions.pi`,
`exs/mip/udf_functions.pi`,
`exs/smt/udf_functions.pi`.

Solver twins: `import cp2.` / `import sat2.` (in `lib/`, not
preloaded — run with `PICATPATH` including `lib`, e.g.
`PICATPATH=lib2:lib` when combined with `lib2` modules such as
`udf`) re-export the whole `cp`/`sat` surface plus
`count_all_solve(Opts, Vars) = C` — `count_all(solve(Opts, Vars))`
with the zero-slice trap handled in one place (a failed or thrown
count is the count 0; everything else is the exact count), which is
the library-level replacement for the per-callsite `; C = 0`
fallbacks in counting and branch-and-bound loops that also post
`udf` constraints.

__Current version 3.9#12.__

Picat is a simple, and yet powerful, logic-based
multi-paradigm programming language aimed for
general-purpose applications. Picat is a rule-based
language, in which predicates, functions, and actors are
defined with pattern-matching rules. Picat incorporates
many declarative language features for better productivity
of software development, including explicit
non-determinism, explicit unification, functions, list
comprehensions, constraints, and tabling. Picat also
provides imperative language constructs, such as
assignments and loops, for programming everyday things. The
Picat implementation, which is based on a well-designed
virtual machine and incorporates a memory manager that
garbage-collects and expands the stacks and data areas when
needed, is efficient and scalable. Picat can be used for
not only symbolic computations, which is a traditional
application domain of declarative languages, but also for
scripting and modeling tasks. 

Compared with functional and scripting languages, the
support of explicit unification, explicit non-determinism,
tabling, and constraints makes Picat more suitable for
symbolic computations. Compared with Prolog, Picat is
arguably more expressive and scalable: it is not rare to
find problems for which Picat requires an order of
magnitude fewer lines of code to describe than Prolog and
Picat can be significantly faster than Prolog because
pattern-matching facilitates indexing of rules. 

The Picat system is written in both C and Picat. The
package has the following folders and files:
- Picat/README  --   this file
- Picat/doc     --   documentation
- Picat/exs     --   program examples
- Picat/emu     --   the C/C++ source code of the engine
- Picat/lib     --   library modules
- Picat/lib2  --   library modules, not preloaded

The folder "Picat/emu" contains the C source code needed to
make Picat's standalone executable. It also contains the C
source code of Espresso, SAT solvers (maple and lingeling),
which are used by Picat. This folder also contains make files
for different platforms.

The folder "Picat/lib" contains library modules. There are
three types of library modules:

pre-loaded and pre-imported: 
---------------------------
The symbols defined in this type of modules are directly
available to applications, and it's unnecessary to import
them. This type includes: "basic.pi", "io.pi", "math.pi",
and "sys.pi".

pre-loaded but not pre-imported: 
-------------------------------
These modules are included in the executable. However,
applications need to import them in order to access the
functions, predicates, and constraints defined in the
modules. No setting of the environment variable PICATPATH is
required. This type includes: "cp.pi", "mip.pi", "nn.pi",
"os.pi", "planner.pi", "sat.pi", "smt.pi", and "util.pi".

not pre-loaded, and not pre-imported: 
-------------------------------------
These modules, which are mainly developed by third parties,
are not included in the executable. In order to import any
of these modules, applications must set the environment
variable PICATPATH to include the folder, in which the module
resides, or start picat with the option "-path" set.

Please contact:
- picat@picat-lang.org
- picat-lang@googlegroups.com 
